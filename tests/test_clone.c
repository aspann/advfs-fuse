/*
 * test_clone.c -- verify clone (copy-on-write snapshot) file data reading.
 *
 * Usage: test_clone <clone-vdisk-path>
 *
 * Opens an AdvFS vdisk that contains a clone fileset, selects the
 * clone (clone_id != 0) via advfs_fileset_list(), resolves its tag
 * directory extents, and reads each known clone test file's data
 * through advfs_filedata_read_clone(). Files in the clone consist
 * mostly of PERM_HOLE (copy-on-write) pages that fall back to the
 * original fileset; the clone-aware reader must reconstruct the
 * original data. Each file's POSIX cksum (1003.2 CRC) and size are
 * compared against values recorded from Tru64's cksum command.
 *
 * Expected values are selected by ODS version (the V3 and V4 test
 * images contain different random data).
 *
 * Copyright (C) 2026 Armas Spann
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _XOPEN_SOURCE 500  /* pread() */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/volume.h"
#include "../src/domain.h"
#include "../src/bmt.h"
#include "../src/extents.h"
#include "../src/fileset.h"
#include "../src/dir.h"
#include "../src/filedata.h"
#include "../src/util.h"

/* ------------------------------------------------------------------
 * POSIX cksum -- 1003.2 CRC.
 *
 * CRC-32 with polynomial 0x04C11DB7, MSB-first, initial value 0.
 * After the data, the file length is fed in as bytes (least
 * significant byte first, stopping once the remaining length is 0),
 * and the result is complemented. Matches the output of Tru64's
 * (and GNU coreutils') cksum command.
 * ------------------------------------------------------------------ */
static uint32_t posix_cksum(const uint8_t *buf, uint64_t len)
{
    static uint32_t table[256];
    static int table_init = 0;

    if (!table_init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i << 24;
            for (int j = 0; j < 8; j++) {
                c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : (c << 1);
            }
            table[i] = c;
        }
        table_init = 1;
    }

    uint32_t crc = 0;

    for (uint64_t i = 0; i < len; i++) {
        crc = (crc << 8) ^ table[((crc >> 24) ^ buf[i]) & 0xFFu];
    }

    /* Append the length, LSB first, until no bits remain. */
    for (uint64_t n = len; n != 0; n >>= 8) {
        crc = (crc << 8) ^ table[((crc >> 24) ^ (n & 0xFFu)) & 0xFFu];
    }

    return ~crc;
}

/* ------------------------------------------------------------------
 * Known-good values from Tru64 cksum with CMD_ENV=xpg4 (POSIX CRC),
 * taken from the clone_snap fileset of each test_clone image. Every
 * file here is served from PERM_HOLE pages via the clone fallback to
 * the original ('primary') fileset.
 * ------------------------------------------------------------------ */
typedef struct {
    const char *dir;     /* NULL = root, otherwise subdirectory name */
    const char *name;
    uint32_t    cksum;
    uint64_t    size;
} expected_file_t;

static const expected_file_t expected_v3[] = {
    { NULL, "original_data.bin",  531619334u, 16384 },
    { NULL, "unchanged.bin",     3870428147u,  1024 },
};

static const expected_file_t expected_v4[] = {
    { NULL, "original_data.bin", 2741767821u, 16384 },
    { NULL, "unchanged.bin",     3578312582u,  1024 },
};

#define NUM_EXPECTED  2

/* ------------------------------------------------------------------
 * Directory entry collection.
 * ------------------------------------------------------------------ */
#define MAX_ENTRIES  64
#define MAX_NAME     255

typedef struct {
    char         name[MAX_NAME + 1];
    adv_bf_tag_t tag;
} dir_entry_t;

typedef struct {
    dir_entry_t entries[MAX_ENTRIES];
    int         count;
} dir_listing_t;

static int collect_entry(const char *name, adv_bf_tag_t tag, void *user_data)
{
    dir_listing_t *dl = (dir_listing_t *)user_data;
    if (dl->count < MAX_ENTRIES) {
        snprintf(dl->entries[dl->count].name, sizeof(dl->entries[0].name),
                 "%s", name);
        dl->entries[dl->count].tag = tag;
        dl->count++;
    }
    return 0;
}

static const dir_entry_t *find_entry(const dir_listing_t *dl,
                                     const char *name)
{
    for (int i = 0; i < dl->count; i++) {
        if (strcmp(dl->entries[i].name, name) == 0) {
            return &dl->entries[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------
 * Clone fileset discovery: collect the first fileset whose clone_id
 * is non-zero (the copy-on-write snapshot).
 * ------------------------------------------------------------------ */
typedef struct {
    advfs_fileset_info_t info;
    int found;
} clone_fileset_t;

static int capture_clone_fileset(const advfs_fileset_info_t *info,
                                 void *user_data)
{
    clone_fileset_t *fs = (clone_fileset_t *)user_data;
    if (info->clone_id != 0 && !fs->found) {
        fs->info = *info;
        fs->found = 1;
        return 1;  /* stop after the first clone */
    }
    return 0;
}

/* ------------------------------------------------------------------
 * Per-file check: read data through the clone-aware reader, cksum,
 * compare. Returns 0 on PASS, 1 on FAIL.
 * ------------------------------------------------------------------ */
static int check_file(advfs_volume_t *vol, advfs_domain_t *domain,
                      const advfs_extent_t *fs_tag_extents,
                      int fs_tag_num_extents, adv_bf_tag_t fs_tag,
                      const advfs_clone_ctx_t *clone,
                      const expected_file_t *exp, adv_bf_tag_t file_tag)
{
    const char *dir = exp->dir ? exp->dir : "";
    const char *sep = exp->dir ? "/" : "";

    uint8_t *buf = NULL;
    uint64_t size = 0;

    int err = advfs_filedata_read_clone(vol, domain, fs_tag_extents,
                                        fs_tag_num_extents, fs_tag, file_tag,
                                        clone, &buf, &size);
    if (err) {
        printf("[FAIL] %s%s%s: read failed (err=%d)\n",
               dir, sep, exp->name, err);
        return 1;
    }

    uint32_t ck = posix_cksum(buf, size);
    free(buf);

    if (size != exp->size) {
        printf("[FAIL] %s%s%s: size=%llu expected=%llu\n",
               dir, sep, exp->name,
               (unsigned long long)size,
               (unsigned long long)exp->size);
        return 1;
    }
    if (ck != exp->cksum) {
        printf("[FAIL] %s%s%s: cksum=%u expected=%u (size=%llu)\n",
               dir, sep, exp->name, ck, exp->cksum,
               (unsigned long long)size);
        return 1;
    }

    printf("[PASS] %s%s%s: cksum=%u size=%llu\n",
           dir, sep, exp->name, ck, (unsigned long long)size);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <clone-vdisk-path>\n", argv[0]);
        return 1;
    }

    /*
     * Self-check the cksum implementation against a known vector:
     * POSIX cksum of "123456789" (9 bytes) is 930766865.
     */
    if (posix_cksum((const uint8_t *)"123456789", 9) != 930766865u) {
        fprintf(stderr, "cksum self-check failed\n");
        return 1;
    }

    advfs_volume_t *vol = NULL;
    int err = advfs_volume_open(argv[1], &vol);
    if (err) {
        fprintf(stderr, "Failed to open volume: %s (err=%d)\n",
                argv[1], err);
        return 1;
    }

    advfs_domain_t domain;
    err = advfs_domain_open(vol, &domain);
    if (err) {
        fprintf(stderr, "Failed to open domain (err=%d)\n", err);
        advfs_volume_close(vol);
        return 1;
    }

    printf("ODS version: %d\n", domain.ods_version);

    const expected_file_t *expected =
        (domain.ods_version >= 4) ? expected_v4 : expected_v3;

    /* Find the clone fileset (clone_id != 0). */
    clone_fileset_t clone_fs;
    clone_fs.found = 0;

    err = advfs_fileset_list(vol, &domain, capture_clone_fileset, &clone_fs);
    if (err || !clone_fs.found) {
        fprintf(stderr, "No clone fileset found (err=%d)\n", err);
        advfs_domain_close(&domain);
        advfs_volume_close(vol);
        return 1;
    }

    printf("Clone fileset: %s (tag=%u.%u) clone_of=%u.%u\n",
           clone_fs.info.name,
           clone_fs.info.dir_tag.num, clone_fs.info.dir_tag.seq,
           clone_fs.info.orig_set_tag.num, clone_fs.info.orig_set_tag.seq);

    /* Clone fallback context for the clone-aware read path. */
    advfs_clone_ctx_t clone = {
        .is_clone = (clone_fs.info.clone_id != 0),
        .orig_set_tag = clone_fs.info.orig_set_tag,
    };

    /* Resolve the clone fileset's tag directory extents. */
    advfs_extent_t fs_tag_extents[ADVFS_MAX_EXTENTS];
    int fs_tag_num_extents = 0;

    err = advfs_fileset_tag_extents(vol, &domain, clone_fs.info.dir_tag,
                                    fs_tag_extents, ADVFS_MAX_EXTENTS,
                                    &fs_tag_num_extents);
    if (err) {
        fprintf(stderr, "Failed to resolve clone fileset tag dir extents "
                "(err=%d)\n", err);
        advfs_domain_close(&domain);
        advfs_volume_close(vol);
        return 1;
    }

    adv_bf_tag_t fs_tag = clone_fs.info.dir_tag;

    /* List the clone's root directory (tag 2). This exercises the
     * clone-aware directory reader too. */
    adv_bf_tag_t root_tag = { .num = 2, .seq = 0 };
    dir_listing_t root_listing;
    root_listing.count = 0;

    err = advfs_dir_read_clone(vol, &domain, fs_tag_extents,
                               fs_tag_num_extents, fs_tag, root_tag,
                               collect_entry, &clone, &root_listing);
    if (err) {
        fprintf(stderr, "Failed to read clone root directory (err=%d)\n",
                err);
        advfs_domain_close(&domain);
        advfs_volume_close(vol);
        return 1;
    }

    printf("Clone root directory: %d entries\n\n", root_listing.count);

    int failures = 0;

    for (int i = 0; i < NUM_EXPECTED; i++) {
        const expected_file_t *exp = &expected[i];
        const dir_entry_t *entry = find_entry(&root_listing, exp->name);

        if (!entry) {
            printf("[FAIL] %s: not found in clone root directory\n",
                   exp->name);
            failures++;
            continue;
        }

        failures += check_file(vol, &domain, fs_tag_extents,
                               fs_tag_num_extents, fs_tag, &clone, exp,
                               entry->tag);
    }

    printf("\nTotal: %d/%d clone file checks passed\n",
           NUM_EXPECTED - failures, NUM_EXPECTED);

    advfs_domain_close(&domain);
    advfs_volume_close(vol);
    return failures ? 1 : 0;
}
