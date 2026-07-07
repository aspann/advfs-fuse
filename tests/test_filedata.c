/*
 * test_filedata.c -- verify file data reading + hash verification
 *
 * Usage: test_filedata <vdisk-path>
 *
 * Opens an AdvFS vdisk, lists the root directory of the first
 * fileset, reads each known test file's data via
 * advfs_filedata_read(), computes its POSIX cksum (1003.2 CRC),
 * and compares checksum and size against values recorded from
 * Tru64's cksum command.
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
 * Known-good values from Tru64 cksum with CMD_ENV=xpg4 (POSIX CRC).
 * NOTE: Tru64 V5.1B default cksum uses a BSD algorithm, NOT POSIX.
 * Set CMD_ENV=xpg4 before running cksum to get POSIX 1003.2 output.
 * ------------------------------------------------------------------ */
typedef struct {
    const char *dir;     /* NULL = root, otherwise subdirectory name */
    const char *name;
    uint32_t    cksum;
    uint64_t    size;
} expected_file_t;

static const expected_file_t expected_v3[] = {
    { NULL,     "random_1k.bin",   1540616566u,  1024 },
    { NULL,     "random_4k.bin",    428563433u,  4096 },
    { "subdir", "nested_8k.bin",    690244400u,  8192 },
    { NULL,     "sparse_test.bin", 2881231933u, 66560 },
};

static const expected_file_t expected_v4[] = {
    { NULL,     "random_1k.bin",   2943379816u,  1024 },
    { NULL,     "random_4k.bin",   1965366879u,  4096 },
    { "subdir", "nested_8k.bin",   3329289784u,  8192 },
    { NULL,     "sparse_test.bin", 2421383984u, 66560 },
};

#define NUM_EXPECTED  4

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
 * Fileset discovery (same flow as test_dir.c).
 * ------------------------------------------------------------------ */
typedef struct {
    advfs_fileset_info_t info;
    int found;
} first_fileset_t;

static int capture_first_fileset(const advfs_fileset_info_t *info,
                                 void *user_data)
{
    first_fileset_t *fs = (first_fileset_t *)user_data;
    fs->info = *info;
    fs->found = 1;
    return 1;  /* stop after first */
}

/*
 * Resolve the fileset tag directory extents from BFSDIR.
 * Same logic as test_dir.c: read BFSDIR extents, look up the
 * fileset's dir_tag, follow the mcell chain to BSR_XTNTS.
 */
static int resolve_fileset_tag_extents(advfs_volume_t *vol,
                                       advfs_domain_t *domain,
                                       adv_bf_tag_t fs_dir_tag,
                                       advfs_extent_t *extents_out,
                                       int max_extents,
                                       int *count_out)
{
    uint32_t bfsdir_cell = (domain->ods_version >= 4)
                            ? ADVFS_BFM_BFSDIR : ADVFS_BFM_BFSDIR_V3;

    advfs_extent_t bfsdir_extents[ADVFS_MAX_EXTENTS];
    int bfsdir_num_extents = 0;

    int err = advfs_extents_read(vol, domain->bmt_map, domain->bmt_map_count,
                                 domain->bmt_page, bfsdir_cell,
                                 bfsdir_extents, ADVFS_MAX_EXTENTS,
                                 &bfsdir_num_extents);
    if (err) {
        fprintf(stderr, "Failed to read BFSDIR extents (err=%d)\n", err);
        return err;
    }

    uint32_t tag_page = fs_dir_tag.num / ADVFS_TAGS_PER_PG;
    uint32_t tag_idx = fs_dir_tag.num % ADVFS_TAGS_PER_PG;

    adv_tag_dir_pg_t bfsdir_tag_pg;
    err = advfs_extents_read_data(vol, bfsdir_extents, bfsdir_num_extents,
                                  tag_page, &bfsdir_tag_pg);
    if (err) {
        fprintf(stderr, "Failed to read BFSDIR tag page %u (err=%d)\n",
                tag_page, err);
        return err;
    }

    adv_tag_map_t *tm = &bfsdir_tag_pg.maps[tag_idx];
    if (!ADV_TMAP_IN_USE(*tm)) {
        fprintf(stderr, "Fileset tag %u not in use in BFSDIR\n",
                fs_dir_tag.num);
        return -ENOENT;
    }

    uint32_t bmt_pg_num = ADV_MCID_PAGE(tm->u.s3.mcid);
    uint32_t mcell_cell = ADV_MCID_CELL(tm->u.s3.mcid);

    uint32_t vol_page;
    err = advfs_bmt_resolve_page(domain->bmt_map, domain->bmt_map_count,
                                 bmt_pg_num, &vol_page);
    if (err) {
        fprintf(stderr, "Failed to resolve BMT page %u (err=%d)\n",
                bmt_pg_num, err);
        return err;
    }

    err = advfs_extents_read(vol, domain->bmt_map, domain->bmt_map_count,
                             vol_page, mcell_cell,
                             extents_out, max_extents, count_out);
    if (err == 0) {
        return 0;
    }
    if (err != -ENODATA) {
        fprintf(stderr, "Failed to read fileset extents (err=%d)\n", err);
        return err;
    }

    /* V3: BSR_XTNTS not in the primary mcell -- follow the chain. */
    adv_bmt_pg_t bmt_pg;
    err = advfs_bmt_read_page(vol, vol_page, &bmt_pg);
    if (err) {
        return err;
    }

    adv_mcell_t *mc = advfs_bmt_get_mcell(&bmt_pg, mcell_cell);
    if (!mc) {
        return -EINVAL;
    }

    adv_mcell_id_t next = mc->next_mcid;
    int hops = 0;

    while (next.raw != 0 && hops < 16) {
        uint32_t np = ADV_MCID_PAGE(next);
        uint32_t nc = ADV_MCID_CELL(next);
        uint32_t nvp;

        err = advfs_bmt_resolve_page(domain->bmt_map,
                                     domain->bmt_map_count, np, &nvp);
        if (err) {
            return err;
        }

        err = advfs_extents_read(vol, domain->bmt_map,
                                 domain->bmt_map_count,
                                 nvp, nc, extents_out, max_extents,
                                 count_out);
        if (err == 0) {
            return 0;
        }
        if (err != -ENODATA) {
            return err;
        }

        adv_bmt_pg_t next_pg;
        err = advfs_bmt_read_page(vol, nvp, &next_pg);
        if (err) {
            return err;
        }

        adv_mcell_t *nmc = advfs_bmt_get_mcell(&next_pg, nc);
        if (!nmc) {
            return -EINVAL;
        }

        next = nmc->next_mcid;
        hops++;
    }

    fprintf(stderr, "Failed to find BSR_XTNTS in fileset mcell chain\n");
    return -ENODATA;
}

/* ------------------------------------------------------------------
 * Per-file check: read data, cksum, compare.
 * Returns 0 on PASS, 1 on FAIL.
 * ------------------------------------------------------------------ */
static int check_file(advfs_volume_t *vol, advfs_domain_t *domain,
                      const advfs_extent_t *fs_tag_extents,
                      int fs_tag_num_extents, adv_bf_tag_t fs_tag,
                      const expected_file_t *exp, adv_bf_tag_t file_tag)
{
    const char *dir = exp->dir ? exp->dir : "";
    const char *sep = exp->dir ? "/" : "";

    uint8_t *buf = NULL;
    uint64_t size = 0;

    int err = advfs_filedata_read(vol, domain, fs_tag_extents,
                                  fs_tag_num_extents, fs_tag, file_tag,
                                  &buf, &size);
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
        fprintf(stderr, "usage: %s <vdisk-path>\n", argv[0]);
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

    /* Find the first fileset. */
    first_fileset_t first_fs;
    first_fs.found = 0;

    err = advfs_fileset_list(vol, &domain, capture_first_fileset, &first_fs);
    if (err || !first_fs.found) {
        fprintf(stderr, "No filesets found (err=%d)\n", err);
        advfs_domain_close(&domain);
        advfs_volume_close(vol);
        return 1;
    }

    printf("Fileset: %s (tag=%u.%u)\n",
           first_fs.info.name,
           first_fs.info.dir_tag.num, first_fs.info.dir_tag.seq);

    /* Resolve the fileset's tag directory extents. */
    advfs_extent_t fs_tag_extents[ADVFS_MAX_EXTENTS];
    int fs_tag_num_extents = 0;

    err = resolve_fileset_tag_extents(vol, &domain, first_fs.info.dir_tag,
                                      fs_tag_extents, ADVFS_MAX_EXTENTS,
                                      &fs_tag_num_extents);
    if (err) {
        fprintf(stderr, "Failed to resolve fileset tag dir extents "
                "(err=%d)\n", err);
        advfs_domain_close(&domain);
        advfs_volume_close(vol);
        return 1;
    }

    adv_bf_tag_t fs_tag = first_fs.info.dir_tag;

    /* List the root directory (tag 2). */
    adv_bf_tag_t root_tag = { .num = 2, .seq = 0 };
    dir_listing_t root_listing;
    root_listing.count = 0;

    err = advfs_dir_read(vol, &domain, fs_tag_extents, fs_tag_num_extents,
                         fs_tag, root_tag, collect_entry, &root_listing);
    if (err) {
        fprintf(stderr, "Failed to read root directory (err=%d)\n", err);
        advfs_domain_close(&domain);
        advfs_volume_close(vol);
        return 1;
    }

    printf("Root directory: %d entries\n\n", root_listing.count);

    int failures = 0;

    for (int i = 0; i < NUM_EXPECTED; i++) {
        const expected_file_t *exp = &expected[i];
        const dir_entry_t *entry;

        if (exp->dir == NULL) {
            /* File in the root directory. */
            entry = find_entry(&root_listing, exp->name);
            if (!entry) {
                printf("[FAIL] %s: not found in root directory\n",
                       exp->name);
                failures++;
                continue;
            }
        } else {
            /* File in a subdirectory: list it to find the tag. */
            const dir_entry_t *subdir = find_entry(&root_listing, exp->dir);
            if (!subdir) {
                printf("[FAIL] %s/%s: directory '%s' not found in root\n",
                       exp->dir, exp->name, exp->dir);
                failures++;
                continue;
            }

            dir_listing_t sub_listing;
            sub_listing.count = 0;

            err = advfs_dir_read(vol, &domain, fs_tag_extents,
                                 fs_tag_num_extents, fs_tag, subdir->tag,
                                 collect_entry, &sub_listing);
            if (err) {
                printf("[FAIL] %s/%s: cannot list directory (err=%d)\n",
                       exp->dir, exp->name, err);
                failures++;
                continue;
            }

            entry = find_entry(&sub_listing, exp->name);
            if (!entry) {
                printf("[FAIL] %s/%s: not found in '%s'\n",
                       exp->dir, exp->name, exp->dir);
                failures++;
                continue;
            }
        }

        failures += check_file(vol, &domain, fs_tag_extents,
                               fs_tag_num_extents, fs_tag, exp,
                               entry->tag);
    }

    printf("\nTotal: %d/%d file checks passed\n",
           NUM_EXPECTED - failures, NUM_EXPECTED);

    advfs_domain_close(&domain);
    advfs_volume_close(vol);
    return failures ? 1 : 0;
}
