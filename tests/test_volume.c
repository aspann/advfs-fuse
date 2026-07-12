/*
 * test_volume.c -- verify AdvFS volume I/O, magic detection,
 *                  offset support, and disklabel parsing
 *
 * Usage: test_volume <vdisk-path>
 *
 * Copyright (C) 2026 Armas Spann
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/volume.h"
#include "../src/domain.h"
#include "../src/dir.h"
#include "../src/extents.h"
#include "../src/filedata.h"
#include "../src/fileset.h"
#include "../src/util.h"

/* Basic open + magic detection (original test). */
static int test_basic_open(const char *vdisk)
{
    advfs_volume_t *vol = NULL;
    int err = advfs_volume_open(vdisk, &vol);
    if (err) {
        fprintf(stderr, "Not an AdvFS volume (or open failed): %s\n", vdisk);
        return 1;
    }

    printf("AdvFS detected: %s\n", advfs_volume_path(vol));
    printf("  Size: %lu bytes (%lu pages)\n",
           (unsigned long)advfs_volume_size(vol),
           (unsigned long)advfs_volume_page_count(vol));

    int version = advfs_volume_detect_version(vol);
    if (version > 0) {
        printf("  ODS version: %d", version);
        if (version == 3) {
            printf(" (Digital UNIX V4.x / Tru64 V5.x with -V3)");
        } else if (version == 4) {
            printf(" (Tru64 UNIX V5.x)");
        }
        printf("\n");
    } else {
        printf("  ODS version: detection failed (%d)\n", version);
    }

    advfs_volume_close(vol);
    return 0;
}

/* open_at(offset=0) must succeed and match open(). */
static int test_open_at_zero(const char *vdisk)
{
    advfs_volume_t *vol = NULL;
    int err = advfs_volume_open_at(vdisk, 0, 0, &vol);
    if (err != 0 || vol == NULL) {
        printf("[FAIL] open_at(offset=0) failed\n");
        return 1;
    }

    uint64_t pages = advfs_volume_page_count(vol);
    if (pages == 0) {
        printf("[FAIL] open_at(offset=0) reports zero pages\n");
        advfs_volume_close(vol);
        return 1;
    }

    printf("[PASS] open_at(offset=0): %lu pages\n", (unsigned long)pages);
    advfs_volume_close(vol);
    return 0;
}

/* open_at() with offset beyond file size must fail. */
static int test_open_at_bad_offset(const char *vdisk)
{
    advfs_volume_t *vol = NULL;
    int err = advfs_volume_open_at(vdisk, (uint64_t)1 << 40, 0, &vol);
    if (err == 0) {
        printf("[FAIL] open_at(1TB) should have failed\n");
        advfs_volume_close(vol);
        return 1;
    }
    printf("[PASS] open_at(1TB) correctly rejected\n");
    return 0;
}

/* open_at() with an explicit size must clamp the usable bytes. */
static int test_open_at_size_clamp(const char *vdisk)
{
    advfs_volume_t *full = NULL, *clamped = NULL;

    if (advfs_volume_open_at(vdisk, 0, 0, &full) != 0) {
        printf("[FAIL] open_at size clamp: baseline open failed\n");
        return 1;
    }

    uint64_t full_size = advfs_volume_size(full);
    uint64_t half = full_size / 2;
    advfs_volume_close(full);

    if (advfs_volume_open_at(vdisk, 0, half, &clamped) != 0) {
        printf("[FAIL] open_at size clamp: clamped open failed\n");
        return 1;
    }

    uint64_t got = advfs_volume_size(clamped);
    advfs_volume_close(clamped);

    if (got != half) {
        printf("[FAIL] open_at size clamp: expected %lu bytes, got %lu\n",
               (unsigned long)half, (unsigned long)got);
        return 1;
    }
    printf("[PASS] open_at(size=%lu) clamps volume size\n",
           (unsigned long)half);
    return 0;
}

/* open_at(0) must produce the same domain ID as open(). */
static int test_open_at_domain_match(const char *vdisk)
{
    advfs_volume_t *v1 = NULL, *v2 = NULL;
    advfs_domain_t d1, d2;

    if (advfs_volume_open(vdisk, &v1) != 0 ||
        advfs_domain_open(v1, &d1) != 0) {
        printf("[FAIL] open_at domain match: baseline open() failed\n");
        if (v1) advfs_volume_close(v1);
        return 1;
    }

    if (advfs_volume_open_at(vdisk, 0, 0, &v2) != 0 ||
        advfs_domain_open(v2, &d2) != 0) {
        printf("[FAIL] open_at domain match: open_at(0) failed\n");
        if (v2) advfs_volume_close(v2);
        advfs_domain_close(&d1);
        advfs_volume_close(v1);
        return 1;
    }

    int ok = (d1.domain_id.id_sec == d2.domain_id.id_sec &&
              d1.domain_id.id_usec == d2.domain_id.id_usec &&
              d1.ods_version == d2.ods_version);

    advfs_domain_close(&d2);
    advfs_volume_close(v2);
    advfs_domain_close(&d1);
    advfs_volume_close(v1);

    if (ok) {
        printf("[PASS] open_at(0) produces identical domain\n");
    } else {
        printf("[FAIL] open_at(0) domain mismatch\n");
    }
    return ok ? 0 : 1;
}

/* ================================================================
 * Composite-offset test (M-NEW-1)
 *
 * The composite fixture is <padding> + <vdisk> concatenated into one
 * file. Opening it at offset=<padding size> must produce the same
 * domain identity and the same recursive entry counts as opening the
 * original vdisk at offset 0, exercising the part_offset + offset
 * arithmetic in vol_pread() with real AdvFS data.
 * ================================================================ */

/* Tru64 st_mode file type bits (mirrors advfs_tool.c). */
#define TV_S_IFMT   0170000u
#define TV_S_IFDIR  0040000u
#define TV_S_IFREG  0100000u
#define TV_S_IFLNK  0120000u

/* Domain identity plus recursive entry counts from one probe. */
typedef struct {
    int      ods_version;
    int      id_sec;
    int      id_usec;
    uint64_t files;
    uint64_t dirs;
    uint64_t symlinks;
    uint64_t other;
} probe_t;

/* Recursive walk context for the composite-offset probe. */
typedef struct {
    advfs_volume_t *vol;
    advfs_domain_t *domain;
    adv_bf_tag_t    fs_tag;
    advfs_extent_t  tag_extents[ADVFS_MAX_EXTENTS];
    int             num_tag_extents;
    int             depth;
    probe_t        *probe;
} count_ctx_t;

static void count_dir(count_ctx_t *ctx, adv_bf_tag_t dir_tag, int depth);

/* Directory entry callback: classify by FS_STAT, recurse into dirs. */
static int count_cb(const char *name, adv_bf_tag_t tag, void *user_data)
{
    count_ctx_t *ctx = (count_ctx_t *)user_data;

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return 0;
    }

    adv_fs_stat_t st;
    if (advfs_filedata_stat(ctx->vol, ctx->domain, ctx->tag_extents,
                            ctx->num_tag_extents, ctx->fs_tag, tag,
                            &st) != 0) {
        return 0;
    }

    uint32_t ftype = st.st_mode & TV_S_IFMT;
    if (ftype == TV_S_IFDIR) {
        ctx->probe->dirs++;
        count_dir(ctx, tag, ctx->depth + 1);
    } else if (ftype == TV_S_IFREG) {
        ctx->probe->files++;
    } else if (ftype == TV_S_IFLNK) {
        ctx->probe->symlinks++;
    } else {
        ctx->probe->other++;
    }
    return 0;
}

/* Walk one directory recursively, bounded by a fixed depth. */
static void count_dir(count_ctx_t *ctx, adv_bf_tag_t dir_tag, int depth)
{
    if (depth > 32) {
        return;
    }
    int prev_depth = ctx->depth;
    ctx->depth = depth;
    advfs_dir_read_full(ctx->vol, ctx->domain, ctx->tag_extents,
                        ctx->num_tag_extents, ctx->fs_tag, dir_tag,
                        count_cb, NULL, ctx);
    ctx->depth = prev_depth;
}

/* Fileset collector: keep the first non-clone fileset. */
static int probe_first_fs(const advfs_fileset_info_t *info, void *user_data)
{
    advfs_fileset_info_t *dst = (advfs_fileset_info_t *)user_data;
    if (info->clone_id == 0 && dst->dir_tag.num == 0) {
        *dst = *info;
    }
    return 0;
}

/* Open path at offset, discover the domain, count entries recursively. */
static int probe_domain(const char *path, uint64_t offset, probe_t *out)
{
    memset(out, 0, sizeof(*out));

    advfs_volume_t *vol = NULL;
    if (advfs_volume_open_at(path, offset, 0, &vol) != 0) {
        return -1;
    }

    advfs_domain_t domain;
    if (advfs_domain_open(vol, &domain) != 0) {
        advfs_volume_close(vol);
        return -1;
    }

    out->ods_version = domain.ods_version;
    out->id_sec = domain.domain_id.id_sec;
    out->id_usec = domain.domain_id.id_usec;

    advfs_fileset_info_t fsinfo;
    memset(&fsinfo, 0, sizeof(fsinfo));
    if (advfs_fileset_list(vol, &domain, probe_first_fs, &fsinfo) != 0 ||
        fsinfo.dir_tag.num == 0) {
        advfs_domain_close(&domain);
        advfs_volume_close(vol);
        return -1;
    }

    count_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.vol = vol;
    ctx.domain = &domain;
    ctx.fs_tag = fsinfo.dir_tag;
    ctx.probe = out;

    if (advfs_fileset_tag_extents(vol, &domain, fsinfo.dir_tag,
                                  ctx.tag_extents, ADVFS_MAX_EXTENTS,
                                  &ctx.num_tag_extents) != 0) {
        advfs_domain_close(&domain);
        advfs_volume_close(vol);
        return -1;
    }

    adv_bf_tag_t root = { .num = 2, .seq = 0 };
    count_dir(&ctx, root, 0);

    advfs_domain_close(&domain);
    advfs_volume_close(vol);
    return 0;
}

/* Composite at offset must match the original vdisk at offset 0. */
static int test_open_at_composite(const char *orig, const char *composite,
                                  uint64_t offset)
{
    probe_t a, b;

    if (probe_domain(orig, 0, &a) != 0) {
        printf("[FAIL] composite offset: probe of original failed\n");
        return 1;
    }
    if (probe_domain(composite, offset, &b) != 0) {
        printf("[FAIL] composite offset: probe at offset %llu failed\n",
               (unsigned long long)offset);
        return 1;
    }

    if (a.ods_version != b.ods_version ||
        a.id_sec != b.id_sec || a.id_usec != b.id_usec) {
        printf("[FAIL] composite offset: domain mismatch "
               "(v%d %d.%06d vs v%d %d.%06d)\n",
               a.ods_version, a.id_sec, a.id_usec,
               b.ods_version, b.id_sec, b.id_usec);
        return 1;
    }
    if (a.files != b.files || a.dirs != b.dirs ||
        a.symlinks != b.symlinks || a.other != b.other) {
        printf("[FAIL] composite offset: recursive count mismatch "
               "(%llu/%llu/%llu/%llu vs %llu/%llu/%llu/%llu)\n",
               (unsigned long long)a.files, (unsigned long long)a.dirs,
               (unsigned long long)a.symlinks, (unsigned long long)a.other,
               (unsigned long long)b.files, (unsigned long long)b.dirs,
               (unsigned long long)b.symlinks, (unsigned long long)b.other);
        return 1;
    }

    printf("[PASS] composite offset: domain and recursive count match "
           "offset-0 (%llu files, %llu dirs at offset %llu)\n",
           (unsigned long long)b.files, (unsigned long long)b.dirs,
           (unsigned long long)offset);
    return 0;
}

/* read_disklabel on nonexistent file must fail. */
static int test_disklabel_no_file(void)
{
    uint64_t offset = 0, size = 0;
    int err = advfs_volume_read_disklabel("/no/such/file", 'a',
                                          &offset, &size);
    if (err == 0) {
        printf("[FAIL] read_disklabel on nonexistent file should fail\n");
        return 1;
    }
    printf("[PASS] read_disklabel on nonexistent file returns error\n");
    return 0;
}

/* read_disklabel with invalid letter must return -EINVAL. */
static int test_disklabel_bad_letter(const char *vdisk)
{
    uint64_t offset = 0, size = 0;
    int err = advfs_volume_read_disklabel(vdisk, 'z', &offset, &size);
    if (err != -EINVAL) {
        printf("[FAIL] read_disklabel('z') expected -EINVAL, got %d\n", err);
        return 1;
    }
    printf("[PASS] read_disklabel('z') returns -EINVAL\n");
    return 0;
}

/* read_disklabel with NULL path must return -EINVAL. */
static int test_disklabel_null(void)
{
    uint64_t offset = 0, size = 0;
    int err = advfs_volume_read_disklabel(NULL, 'a', &offset, &size);
    if (err != -EINVAL) {
        printf("[FAIL] read_disklabel(NULL) expected -EINVAL, got %d\n", err);
        return 1;
    }
    printf("[PASS] read_disklabel(NULL) returns -EINVAL\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2 && argc != 4) {
        fprintf(stderr, "usage: %s <vdisk-path> "
                "[<composite-path> <offset>]\n", argv[0]);
        return 1;
    }

    const char *vdisk = argv[1];
    int failures = 0;

    /* Original basic test. */
    failures += test_basic_open(vdisk);

    /* v1.0.2: open_at tests. */
    failures += test_open_at_zero(vdisk);
    failures += test_open_at_bad_offset(vdisk);
    failures += test_open_at_size_clamp(vdisk);
    failures += test_open_at_domain_match(vdisk);

    /* v1.0.2: disklabel tests. */
    failures += test_disklabel_no_file();
    failures += test_disklabel_bad_letter(vdisk);
    failures += test_disklabel_null();

    /* v1.0.2: end-to-end offset test against a composite fixture. */
    if (argc == 4) {
        uint64_t offset = strtoull(argv[3], NULL, 0);
        failures += test_open_at_composite(vdisk, argv[2], offset);
    }

    return failures > 0 ? 1 : 0;
}
