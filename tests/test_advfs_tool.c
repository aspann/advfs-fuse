/*
 * test_advfs_tool.c -- tests for advfs-tool features
 *
 * Covers the recursive directory walk (-r / --recursive) against
 * the V3 test_single.vdisk image with known file structure, and the
 * BSD disklabel parser against a synthetic binary fixture.
 *
 * Usage: test_advfs_tool <v3_test_single.vdisk>
 *
 * Copyright (C) 2026 Armas Spann
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _XOPEN_SOURCE 500  /* mkstemp() */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/dir.h"
#include "../src/domain.h"
#include "../src/extents.h"
#include "../src/filedata.h"
#include "../src/fileset.h"
#include "../src/ods.h"
#include "../src/util.h"
#include "../src/volume.h"

static int passed = 0;
static int failed = 0;

#define CHECK(cond, msg) do { \
    if (cond) { \
        printf("[PASS] %s\n", (msg)); \
        passed++; \
    } else { \
        printf("[FAIL] %s\n", (msg)); \
        failed++; \
    } \
} while (0)

/* Tru64 st_mode file type bits. */
#define T_S_IFMT   0170000u
#define T_S_IFDIR  0040000u
#define T_S_IFREG  0100000u
#define T_S_IFLNK  0120000u

/* ================================================================
 * Recursive file count
 * ================================================================ */

/* Counters for the recursive walk. */
typedef struct {
    int files;
    int dirs;
    int symlinks;
    int other;
} rcount_t;

/* Walk context passed through user_data. */
typedef struct {
    advfs_volume_t *vol;
    advfs_domain_t *domain;
    adv_bf_tag_t    fs_tag;
    advfs_extent_t  tag_extents[ADVFS_MAX_EXTENTS];
    int             num_tag_extents;
    int             depth;
    rcount_t       *cnt;
} walk_ctx_t;

static void walk_dir(walk_ctx_t *ctx, adv_bf_tag_t dir_tag, int depth);

/* Directory entry callback: count and recurse into subdirectories. */
static int walk_cb(const char *name, adv_bf_tag_t tag, void *user_data)
{
    walk_ctx_t *ctx = (walk_ctx_t *)user_data;

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return 0;
    }

    adv_fs_stat_t st;
    int err = advfs_filedata_stat(ctx->vol, ctx->domain, ctx->tag_extents,
                                  ctx->num_tag_extents, ctx->fs_tag, tag,
                                  &st);
    if (err) {
        return 0;
    }

    uint32_t ftype = st.st_mode & T_S_IFMT;

    if (ftype == T_S_IFDIR) {
        ctx->cnt->dirs++;
        walk_dir(ctx, tag, ctx->depth + 1);
    } else if (ftype == T_S_IFREG) {
        ctx->cnt->files++;
    } else if (ftype == T_S_IFLNK) {
        ctx->cnt->symlinks++;
    } else {
        ctx->cnt->other++;
    }

    return 0;
}

/* Walk a directory recursively via advfs_dir_read_full. */
static void walk_dir(walk_ctx_t *ctx, adv_bf_tag_t dir_tag, int depth)
{
    if (depth > 32) {
        return;
    }

    /* Thread the depth through the context so walk_cb can pass
     * depth + 1; restore it afterwards for the caller's siblings. */
    int prev_depth = ctx->depth;
    ctx->depth = depth;

    advfs_dir_read_full(ctx->vol, ctx->domain, ctx->tag_extents,
                        ctx->num_tag_extents, ctx->fs_tag, dir_tag,
                        walk_cb, NULL, ctx);

    ctx->depth = prev_depth;
}

/* ================================================================
 * BSD disklabel parser (happy path, synthetic fixture)
 * ================================================================ */

/* Fixture layout constants (mirror the parser in src/volume.c). */
#define DL_MAGIC          0x82564557u
#define DL_LABEL_SIZE     404   /* 148-byte header + 16 x 16-byte entries */
#define DL_SECSIZE_OFS    40
#define DL_MAGIC2_OFS     132
#define DL_CKSUM_OFS      136
#define DL_NPARTS_OFS     138
#define DL_PARTS_OFS      148
#define DL_PART_ENTRY_SZ  16
#define DL_FILE_OFS       64    /* Alpha: label at byte 64 of the disk */

/* Fixture geometry: partition 'g' = 2048 sectors at sector 4096. */
#define DL_FIX_SECSIZE    512u
#define DL_FIX_NPARTS     8u
#define DL_FIX_G_SIZE     2048u
#define DL_FIX_G_OFFSET   4096u

/* Build a minimal valid Alpha/BSD disklabel into buf (404 bytes). */
static void build_disklabel_fixture(uint8_t buf[DL_LABEL_SIZE])
{
    uint32_t v32;
    uint16_t v16;

    memset(buf, 0, DL_LABEL_SIZE);

    v32 = DL_MAGIC;
    memcpy(buf, &v32, 4);
    v32 = DL_FIX_SECSIZE;
    memcpy(buf + DL_SECSIZE_OFS, &v32, 4);
    v32 = DL_MAGIC;
    memcpy(buf + DL_MAGIC2_OFS, &v32, 4);
    v16 = DL_FIX_NPARTS;
    memcpy(buf + DL_NPARTS_OFS, &v16, 2);

    /* Partition entry 'g' (index 6): [p_size(4), p_offset(4), ...]. */
    uint8_t *pent = buf + DL_PARTS_OFS + 6 * DL_PART_ENTRY_SZ;
    v32 = DL_FIX_G_SIZE;
    memcpy(pent + 0, &v32, 4);
    v32 = DL_FIX_G_OFFSET;
    memcpy(pent + 4, &v32, 4);

    /* d_checksum: XOR of all 16-bit words over the header plus
     * d_npartitions entries must be zero, so store the residue. */
    uint16_t cksum = 0;
    for (size_t i = 0;
         i + 2 <= DL_PARTS_OFS + DL_FIX_NPARTS * DL_PART_ENTRY_SZ;
         i += 2) {
        uint16_t word;
        memcpy(&word, buf + i, 2);
        cksum ^= word;
    }
    memcpy(buf + DL_CKSUM_OFS, &cksum, 2);
}

/* Happy-path test: write the fixture to a temp file and parse it. */
static void test_disklabel_fixture(void)
{
    uint8_t label[DL_LABEL_SIZE];
    uint8_t zeros[DL_FILE_OFS];
    char path[] = "/tmp/advfs_disklabel_XXXXXX";

    build_disklabel_fixture(label);
    memset(zeros, 0, sizeof(zeros));

    int fd = mkstemp(path);
    if (fd < 0) {
        printf("[FAIL] disklabel fixture: mkstemp failed\n");
        failed++;
        return;
    }

    int wr_ok = (write(fd, zeros, sizeof(zeros)) == (ssize_t)sizeof(zeros) &&
                 write(fd, label, sizeof(label)) == (ssize_t)sizeof(label));
    close(fd);

    if (!wr_ok) {
        printf("[FAIL] disklabel fixture: cannot write temp file\n");
        failed++;
        unlink(path);
        return;
    }

    uint64_t offset = 0, size = 0;
    int err = advfs_volume_read_disklabel(path, 'g', &offset, &size);

    CHECK(err == 0, "disklabel fixture: partition 'g' parsed");
    CHECK(offset == (uint64_t)DL_FIX_G_OFFSET * DL_FIX_SECSIZE,
          "disklabel fixture: partition 'g' byte offset");
    CHECK(size == (uint64_t)DL_FIX_G_SIZE * DL_FIX_SECSIZE,
          "disklabel fixture: partition 'g' byte size");

    /* Partition index beyond d_npartitions must be rejected. */
    err = advfs_volume_read_disklabel(path, 'i', &offset, &size);
    CHECK(err == -ENOENT,
          "disklabel fixture: partition beyond npartitions rejected");

    unlink(path);
}

/* Bad-checksum test: a corrupt d_checksum must warn but still parse. */
static void test_disklabel_bad_checksum(void)
{
    uint8_t label[DL_LABEL_SIZE];
    uint8_t zeros[DL_FILE_OFS];
    char path[] = "/tmp/advfs_disklabel_XXXXXX";

    build_disklabel_fixture(label);
    /* Corrupt the stored checksum so the XOR residue is nonzero. */
    label[DL_CKSUM_OFS] ^= 0x55;
    memset(zeros, 0, sizeof(zeros));

    int fd = mkstemp(path);
    if (fd < 0) {
        printf("[FAIL] disklabel bad checksum: mkstemp failed\n");
        failed++;
        return;
    }

    int wr_ok = (write(fd, zeros, sizeof(zeros)) == (ssize_t)sizeof(zeros) &&
                 write(fd, label, sizeof(label)) == (ssize_t)sizeof(label));
    close(fd);

    if (!wr_ok) {
        printf("[FAIL] disklabel bad checksum: cannot write temp file\n");
        failed++;
        unlink(path);
        return;
    }

    /* The parser prints a "checksum mismatch" warning on stderr (the
     * test runner greps for it) and must still return the partition. */
    uint64_t offset = 0, size = 0;
    int err = advfs_volume_read_disklabel(path, 'g', &offset, &size);

    CHECK(err == 0, "disklabel bad checksum: still parses");
    CHECK(offset == (uint64_t)DL_FIX_G_OFFSET * DL_FIX_SECSIZE,
          "disklabel bad checksum: partition 'g' byte offset intact");
    CHECK(size == (uint64_t)DL_FIX_G_SIZE * DL_FIX_SECSIZE,
          "disklabel bad checksum: partition 'g' byte size intact");

    unlink(path);
}

/* Fileset collector: grab the first non-clone fileset. */
static int grab_first_fs(const advfs_fileset_info_t *info, void *user_data)
{
    advfs_fileset_info_t *dst = (advfs_fileset_info_t *)user_data;

    if (info->clone_id == 0 && dst->dir_tag.num == 0) {
        *dst = *info;
    }
    return 0;
}

/* Run the recursive count and verify expected values. */
static void test_recursive_count(const char *vdisk)
{
    advfs_volume_t *vol = NULL;
    advfs_domain_t domain;

    if (advfs_volume_open(vdisk, &vol) != 0) {
        printf("[FAIL] recursive count: cannot open vdisk\n");
        failed++;
        return;
    }

    if (advfs_domain_open(vol, &domain) != 0) {
        printf("[FAIL] recursive count: cannot open domain\n");
        failed++;
        advfs_volume_close(vol);
        return;
    }

    advfs_fileset_info_t fsinfo;
    memset(&fsinfo, 0, sizeof(fsinfo));

    if (advfs_fileset_list(vol, &domain, grab_first_fs, &fsinfo) != 0 ||
        fsinfo.dir_tag.num == 0) {
        printf("[FAIL] recursive count: no fileset found\n");
        failed++;
        advfs_domain_close(&domain);
        advfs_volume_close(vol);
        return;
    }

    advfs_extent_t tag_ext[ADVFS_MAX_EXTENTS];
    int num_ext = 0;

    if (advfs_fileset_tag_extents(vol, &domain, fsinfo.dir_tag,
                                  tag_ext, ADVFS_MAX_EXTENTS,
                                  &num_ext) != 0) {
        printf("[FAIL] recursive count: cannot resolve tag extents\n");
        failed++;
        advfs_domain_close(&domain);
        advfs_volume_close(vol);
        return;
    }

    rcount_t cnt = { 0, 0, 0, 0 };
    walk_ctx_t ctx = {
        .vol = vol,
        .domain = &domain,
        .fs_tag = fsinfo.dir_tag,
        .num_tag_extents = num_ext,
        .cnt = &cnt,
    };
    memcpy(ctx.tag_extents, tag_ext, sizeof(tag_ext));

    adv_bf_tag_t root = { .num = 2, .seq = 0 };
    walk_dir(&ctx, root, 0);

    printf("  counted: %d files, %d dirs, %d symlinks, %d other\n",
           cnt.files, cnt.dirs, cnt.symlinks, cnt.other);

    CHECK(cnt.files == 6,  "recursive count: 6 regular files");
    CHECK(cnt.dirs == 2,   "recursive count: 2 directories");
    CHECK(cnt.symlinks == 0, "recursive count: 0 symlinks");
    CHECK(cnt.files + cnt.dirs + cnt.symlinks + cnt.other > 0,
          "recursive count: total > 0");

    advfs_domain_close(&domain);
    advfs_volume_close(vol);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <v3_test_single.vdisk>\n", argv[0]);
        return 2;
    }

    printf("=== advfs-tool tests ===\n\n");
    test_disklabel_fixture();
    test_disklabel_bad_checksum();
    test_recursive_count(argv[1]);
    printf("\n=== %d passed, %d failed ===\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
