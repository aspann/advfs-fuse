/*
 * test_stripe.c -- stripe-domain tests against volume 1 of a
 * striped AdvFS domain
 *
 * Usage: test_stripe <vdisk-path-of-volume-1> [expected-ods-version]
 *
 * Runs against volume 1 of the two-volume test domain "test_stripe"
 * (fileset "striped_fs"). The expected ODS version defaults to 3 and
 * can be overridden with the optional second argument (e.g. 4 for
 * the V4 stripe image). The driver reads single volumes only, so
 * these tests verify DETECTION and basic metadata reading, not
 * cross-volume data integrity:
 *   1. Volume 1 is detected as AdvFS with the expected ODS version
 *   2. The domain opens on volume 1
 *   3. The domain reports 2 volumes (BSR_DMN_MATTR vd_cnt)
 *   4. Fileset "striped_fs" is enumerated
 *   5. stripe_test.bin is present in the root directory
 *   6. The file's BSR_XTNTS record resolves and its extents map all
 *      pages of the 32768-byte file
 *   7. Stripe detection fires: a BSR_XTNTS record with
 *      type == ADVFS_BSXMT_STRIPE (2) is recognized by
 *      advfs_extents_read() and emits the stripe warning
 *
 * Note on test 7: on the current test image the file data was
 * placed entirely on volume 1 and the on-disk BSR_XTNTS type field
 * is BSXMT_APPEND (0), i.e. the fixture file is not genuinely
 * striped (test 6 reports the actual on-disk type). To exercise the
 * driver's stripe detection path regardless, test 7 crafts a
 * synthetic volume with a BSXMT_STRIPE extent record, following the
 * synthetic-fixture approach of test_edge_cases.c.
 *
 * Each test prints "[PASS] name: description" or
 * "[FAIL] name: reason". Exit code 0 if all pass, 1 otherwise.
 *
 * Copyright (C) 2026 Armas Spann
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _XOPEN_SOURCE 500  /* pread(), mkstemp() */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/volume.h"
#include "../src/domain.h"
#include "../src/bmt.h"
#include "../src/extents.h"
#include "../src/fileset.h"
#include "../src/dir.h"
#include "../src/util.h"

/* Expected properties of the striped test domain. */
#define DEFAULT_ODS_VERSION   3
#define EXPECTED_VD_CNT       2
#define EXPECTED_FILESET      "striped_fs"
#define STRIPE_FILE_NAME      "stripe_test.bin"
#define STRIPE_FILE_SIZE      32768u
#define STRIPE_FILE_PAGES     (STRIPE_FILE_SIZE / ADVFS_PGSZ)

/* Maximum mcell chain hops when searching for BSR_XTNTS. */
#define MAX_CHAIN_HOPS  16

/* Test result counters. */
static int tests_passed;
static int tests_failed;

/* Expected ODS version of the image under test (argv[2], default 3). */
static int expected_ods_version = DEFAULT_ODS_VERSION;

/* Record a passed test. */
static void test_pass(const char *name, const char *fmt, ...)
{
    va_list ap;
    printf("[PASS] %s: ", name);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    tests_passed++;
}

/* Record a failed test. */
static void test_fail(const char *name, const char *fmt, ...)
{
    va_list ap;
    printf("[FAIL] %s: ", name);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    tests_failed++;
}

/* Storage for the fileset matched during enumeration. */
typedef struct {
    advfs_fileset_info_t info;
    int found;
    int total;
} fileset_search_t;

/* Callback: capture the fileset named EXPECTED_FILESET. */
static int capture_striped_fileset(const advfs_fileset_info_t *info,
                                   void *user_data)
{
    fileset_search_t *fs = (fileset_search_t *)user_data;
    fs->total++;
    if (strcmp(info->name, EXPECTED_FILESET) == 0) {
        fs->info = *info;
        fs->found = 1;
    }
    return 0;  /* keep going to count all filesets */
}

/* Storage for the directory entry matched during listing. */
typedef struct {
    adv_bf_tag_t tag;
    int found;
    int total;
} dir_search_t;

/* Callback: capture the tag of STRIPE_FILE_NAME. */
static int capture_stripe_file(const char *name, adv_bf_tag_t tag,
                               void *user_data)
{
    dir_search_t *ds = (dir_search_t *)user_data;
    ds->total++;
    if (strcmp(name, STRIPE_FILE_NAME) == 0) {
        ds->tag = tag;
        ds->found = 1;
    }
    return 0;
}

/*
 * Find the mcell containing BSR_XTNTS for a tag's primary mcell,
 * following the next_mcid chain (in the V3 layout BSR_XTNTS may sit
 * in a chained mcell rather than the primary one).
 *
 * On success, writes the volume page and cell index of the mcell
 * holding the record and returns 0. Returns -errno on failure.
 */
static int find_xtnts_mcell(advfs_volume_t *vol, advfs_domain_t *domain,
                            uint32_t bmt_pg_num, uint32_t cell,
                            uint32_t *vol_page_out, uint32_t *cell_out)
{
    uint32_t vol_page;
    int err = advfs_bmt_resolve_page(domain->bmt_map,
                                     domain->bmt_map_count,
                                     bmt_pg_num, &vol_page);
    if (err) {
        return err;
    }

    int hops = 0;
    while (hops < MAX_CHAIN_HOPS) {
        adv_bmt_pg_t bmt_pg;
        err = advfs_bmt_read_page(vol, vol_page, &bmt_pg);
        if (err) {
            return err;
        }

        adv_mcell_t *mc = advfs_bmt_get_mcell(&bmt_pg, cell);
        if (!mc) {
            return -EINVAL;
        }

        if (advfs_bmt_find_rec(mc, ADVFS_BSR_XTNTS, NULL)) {
            *vol_page_out = vol_page;
            *cell_out = cell;
            return 0;
        }

        adv_mcell_id_t next = mc->next_mcid;
        if (next.raw == 0) {
            break;
        }

        cell = ADV_MCID_CELL(next);
        err = advfs_bmt_resolve_page(domain->bmt_map,
                                     domain->bmt_map_count,
                                     ADV_MCID_PAGE(next), &vol_page);
        if (err) {
            return err;
        }
        hops++;
    }

    return -ENODATA;
}

/*
 * Resolve the fileset tag directory extents from BFSDIR.
 *
 * Looks up the fileset's dir_tag in the BFSDIR tag directory,
 * follows the mcell chain to BSR_XTNTS, and reads the extent map.
 *
 * Returns 0 on success, -errno on failure.
 */
static int resolve_fileset_tag_extents(advfs_volume_t *vol,
                                       advfs_domain_t *domain,
                                       adv_bf_tag_t fs_dir_tag,
                                       advfs_extent_t *extents_out,
                                       int max_extents,
                                       int *count_out)
{
    uint32_t bfsdir_cell = (domain->ods_version >= ADVFS_FIRST_RBMT_VERSION)
                            ? ADVFS_BFM_BFSDIR : ADVFS_BFM_BFSDIR_V3;

    advfs_extent_t bfsdir_extents[ADVFS_MAX_EXTENTS];
    int bfsdir_num_extents = 0;

    int err = advfs_extents_read(vol, domain->bmt_map,
                                 domain->bmt_map_count,
                                 domain->bmt_page, bfsdir_cell,
                                 bfsdir_extents, ADVFS_MAX_EXTENTS,
                                 &bfsdir_num_extents);
    if (err) {
        return err;
    }

    /* Look up the fileset dir_tag in the BFSDIR tag directory. */
    uint32_t tag_page = fs_dir_tag.num / ADVFS_TAGS_PER_PG;
    uint32_t tag_idx = fs_dir_tag.num % ADVFS_TAGS_PER_PG;

    adv_tag_dir_pg_t tag_pg;
    err = advfs_extents_read_data(vol, bfsdir_extents, bfsdir_num_extents,
                                  tag_page, &tag_pg);
    if (err) {
        return err;
    }

    adv_tag_map_t *tm = &tag_pg.maps[tag_idx];
    if (!ADV_TMAP_IN_USE(*tm)) {
        return -ENOENT;
    }

    uint32_t vol_page, cell;
    err = find_xtnts_mcell(vol, domain,
                           ADV_MCID_PAGE(tm->u.s3.mcid),
                           ADV_MCID_CELL(tm->u.s3.mcid),
                           &vol_page, &cell);
    if (err) {
        return err;
    }

    return advfs_extents_read(vol, domain->bmt_map,
                              domain->bmt_map_count,
                              vol_page, cell,
                              extents_out, max_extents, count_out);
}

/*
 * Look up a file tag in the fileset's tag directory and return the
 * location of the mcell holding its BSR_XTNTS record plus the
 * record's type and segment_size fields.
 *
 * Returns 0 on success, -errno on failure.
 */
static int lookup_file_xtnts(advfs_volume_t *vol, advfs_domain_t *domain,
                             const advfs_extent_t *fs_tag_extents,
                             int fs_tag_num_extents, adv_bf_tag_t file_tag,
                             uint32_t *vol_page_out, uint32_t *cell_out,
                             uint32_t *xtnt_type_out,
                             uint32_t *segment_size_out)
{
    uint32_t tag_page = file_tag.num / ADVFS_TAGS_PER_PG;
    uint32_t tag_idx = file_tag.num % ADVFS_TAGS_PER_PG;

    adv_tag_dir_pg_t tag_pg;
    int err = advfs_extents_read_data(vol, fs_tag_extents,
                                      fs_tag_num_extents,
                                      tag_page, &tag_pg);
    if (err) {
        return err;
    }

    adv_tag_map_t *tm = &tag_pg.maps[tag_idx];
    if (!ADV_TMAP_IN_USE(*tm)) {
        return -ENOENT;
    }

    uint32_t vol_page, cell;
    err = find_xtnts_mcell(vol, domain,
                           ADV_MCID_PAGE(tm->u.s3.mcid),
                           ADV_MCID_CELL(tm->u.s3.mcid),
                           &vol_page, &cell);
    if (err) {
        return err;
    }

    adv_bmt_pg_t bmt_pg;
    err = advfs_bmt_read_page(vol, vol_page, &bmt_pg);
    if (err) {
        return err;
    }

    adv_mcell_t *mc = advfs_bmt_get_mcell(&bmt_pg, cell);
    if (!mc) {
        return -EINVAL;
    }

    adv_rec_hdr_t rhdr;
    adv_xtnt_rec_t *xr = advfs_bmt_find_rec(mc, ADVFS_BSR_XTNTS, &rhdr);
    if (!xr) {
        return -ENODATA;
    }
    if (ADV_REC_DATA_SZ(rhdr) < sizeof(adv_xtnt_rec_t)) {
        return -EILSEQ;
    }

    *vol_page_out = vol_page;
    *cell_out = cell;
    *xtnt_type_out = xr->type;
    *segment_size_out = xr->segment_size;
    return 0;
}

/* ------------------------------------------------------------------
 * Synthetic stripe fixture for test 7.
 *
 * Builds a 4-page temp volume:
 *   page 0-1: zeros, with the AdvFS magic patched in
 *   page 2:   crafted BMT page (ODS v3 header) whose mcell 0 holds
 *             a BSR_XTNTS record with type == ADVFS_BSXMT_STRIPE
 *   page 3:   file data area referenced by the extent record
 * ------------------------------------------------------------------ */
static int make_striped_volume(advfs_volume_t **vol_out, char *path_out,
                               size_t path_sz)
{
    char path[] = "/tmp/advfs_stripe_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        return -1;
    }

    uint8_t page[ADVFS_PGSZ];
    memset(page, 0, sizeof(page));

    /* Pages 0 and 1: zeros (magic patched below). */
    for (int i = 0; i < 2; i++) {
        if (write(fd, page, sizeof(page)) != (ssize_t)sizeof(page)) {
            goto fail;
        }
    }

    /* Page 2: BMT page with the striped BSR_XTNTS record. */
    adv_bmt_pg_t bmt_pg;
    memset(&bmt_pg, 0, sizeof(bmt_pg));
    /* The synthetic fixture is always crafted in the V3 on-disk
     * layout, independent of the image under test. */
    bmt_pg.pg_id_ver = (uint32_t)3 << 27;  /* ODS v3, pageId 0 */

    adv_mcell_t *mc = &bmt_pg.mcells[0];
    mc->tag.num = 100;
    mc->tag.seq = 1;
    mc->bf_set_tag.num = 1;
    mc->bf_set_tag.seq = 1;

    adv_rec_hdr_t hdr;
    hdr.raw = ((uint32_t)ADVFS_BSR_XTNTS << 16)
              | (uint32_t)(ADVFS_REC_HDR_SZ + sizeof(adv_xtnt_rec_t));

    adv_xtnt_rec_t xr;
    memset(&xr, 0, sizeof(xr));
    xr.type = ADVFS_BSXMT_STRIPE;
    xr.blks_per_page = ADVFS_BLKS_PER_PG;
    xr.segment_size = 8;                    /* stripe segment pages */
    xr.first_xtnt.mcell_cnt = 1;
    xr.first_xtnt.x_cnt = 2;
    xr.first_xtnt.xtnts[0].bs_page = 0;
    xr.first_xtnt.xtnts[0].vd_blk = 3 * ADVFS_BLKS_PER_PG;  /* page 3 */
    xr.first_xtnt.xtnts[1].bs_page = 1;
    xr.first_xtnt.xtnts[1].vd_blk = ADVFS_XTNT_TERM;

    memcpy(mc->records, &hdr, sizeof(hdr));
    memcpy(mc->records + sizeof(hdr), &xr, sizeof(xr));
    /* Nil terminator record: records[] is already zeroed. */

    if (write(fd, &bmt_pg, sizeof(bmt_pg)) != (ssize_t)sizeof(bmt_pg)) {
        goto fail;
    }

    /* Page 3: file data. */
    memset(page, 0x5A, sizeof(page));
    if (write(fd, page, sizeof(page)) != (ssize_t)sizeof(page)) {
        goto fail;
    }

    /* Patch in the AdvFS magic. */
    uint32_t magic = ADVFS_MAGIC;
    if (pwrite(fd, &magic, sizeof(magic),
               (off_t)ADVFS_MAGIC_ABS_OFFSET) != (ssize_t)sizeof(magic)) {
        goto fail;
    }
    close(fd);

    if (advfs_volume_open(path, vol_out)) {
        unlink(path);
        return -1;
    }

    snprintf(path_out, path_sz, "%s", path);
    return 0;

fail:
    close(fd);
    unlink(path);
    return -1;
}

/* Test 7: stripe detection fires on a BSXMT_STRIPE BSR_XTNTS. */
static void test_stripe_detection_fires(void)
{
    const char *name = "stripe_warn";

    advfs_volume_t *svol = NULL;
    char spath[64];
    if (make_striped_volume(&svol, spath, sizeof(spath))) {
        test_fail(name, "cannot create synthetic striped volume");
        return;
    }

    /* Minimal BMT map; unused because the record has no chain. */
    adv_xtnt_t bmt_map[2];
    bmt_map[0].bs_page = 0;
    bmt_map[0].vd_blk = 2 * ADVFS_BLKS_PER_PG;
    bmt_map[1].bs_page = 1;
    bmt_map[1].vd_blk = ADVFS_XTNT_TERM;

    advfs_extent_t extents[ADVFS_MAX_EXTENTS];
    int num_extents = 0;

    /* advfs_extents_read() must recognize type == BSXMT_STRIPE and
     * emit the "striped file detected" warning (the test runner
     * greps the captured output for it), then still return the
     * extents without failing. */
    int err = advfs_extents_read(svol, bmt_map, 2, 2, 0,
                                 extents, ADVFS_MAX_EXTENTS,
                                 &num_extents);

    advfs_volume_close(svol);
    unlink(spath);

    if (err) {
        test_fail(name, "advfs_extents_read failed on striped "
                  "record (err=%d)", err);
        return;
    }
    if (num_extents != 2) {
        test_fail(name, "expected 2 extent entries, got %d",
                  num_extents);
        return;
    }
    if (advfs_extents_page_count(extents, num_extents) != 1) {
        test_fail(name, "expected page count 1, got %u",
                  advfs_extents_page_count(extents, num_extents));
        return;
    }

    test_pass(name, "BSXMT_STRIPE recognized, warning emitted, "
              "%d extent entries read", num_extents);
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s <vdisk-path-of-volume-1> "
                "[expected-ods-version]\n", argv[0]);
        return 1;
    }

    if (argc == 3) {
        expected_ods_version = atoi(argv[2]);
        if (expected_ods_version <= 0) {
            fprintf(stderr, "invalid expected ODS version: %s\n", argv[2]);
            return 1;
        }
    }

    /* --- Test 1: AdvFS detection and ODS version --- */
    advfs_volume_t *vol = NULL;
    int err = advfs_volume_open(argv[1], &vol);
    if (err) {
        test_fail("stripe_detect", "failed to open volume %s (err=%d)",
                  argv[1], err);
        printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
        return 1;
    }

    int version = advfs_volume_detect_version(vol);
    if (version == expected_ods_version) {
        test_pass("stripe_detect", "AdvFS detected on volume 1, ODS v%d",
                  version);
    } else {
        test_fail("stripe_detect", "expected ODS v%d, got %d",
                  expected_ods_version, version);
    }

    /* --- Test 2: domain open on volume 1 --- */
    advfs_domain_t domain;
    err = advfs_domain_open(vol, &domain);
    if (err) {
        test_fail("stripe_domain", "advfs_domain_open failed (err=%d)",
                  err);
        advfs_volume_close(vol);
        printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
        return 1;
    }

    if (domain.ods_version == expected_ods_version) {
        test_pass("stripe_domain", "domain %d.%06d opened, ODS v%d",
                  domain.domain_id.id_sec, domain.domain_id.id_usec,
                  domain.ods_version);
    } else {
        test_fail("stripe_domain", "domain ODS version %d, expected %d",
                  domain.ods_version, expected_ods_version);
    }

    /* --- Test 3: domain reports 2 volumes (BSR_DMN_MATTR) --- */
    if (!domain.has_mattr) {
        test_fail("stripe_vd_cnt", "BSR_DMN_MATTR not found on volume 1");
    } else if (domain.mattr.vd_cnt == EXPECTED_VD_CNT) {
        test_pass("stripe_vd_cnt", "domain has %u volumes (vd_index=%u)",
                  domain.mattr.vd_cnt,
                  domain.has_vd_attr ? domain.vd_attr.vd_index : 0);
    } else {
        test_fail("stripe_vd_cnt", "vd_cnt=%u, expected %u",
                  domain.mattr.vd_cnt, EXPECTED_VD_CNT);
    }

    /* --- Test 4: fileset "striped_fs" is enumerated --- */
    fileset_search_t fs;
    memset(&fs, 0, sizeof(fs));

    err = advfs_fileset_list(vol, &domain, capture_striped_fileset, &fs);
    if (err) {
        test_fail("stripe_fileset", "advfs_fileset_list failed (err=%d)",
                  err);
    } else if (fs.found) {
        test_pass("stripe_fileset",
                  "fileset '%s' found (tag=%u.%u, %d fileset(s) total)",
                  fs.info.name, fs.info.dir_tag.num, fs.info.dir_tag.seq,
                  fs.total);
    } else {
        test_fail("stripe_fileset",
                  "fileset '%s' not found (%d fileset(s) enumerated)",
                  EXPECTED_FILESET, fs.total);
    }

    /* The remaining vdisk-based tests need the fileset. */
    if (!fs.found) {
        advfs_domain_close(&domain);
        advfs_volume_close(vol);
        test_stripe_detection_fires();
        printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
        return 1;
    }

    /* --- Test 5: stripe_test.bin is in the root directory --- */
    advfs_extent_t fs_tag_extents[ADVFS_MAX_EXTENTS];
    int fs_tag_num_extents = 0;

    err = resolve_fileset_tag_extents(vol, &domain, fs.info.dir_tag,
                                      fs_tag_extents, ADVFS_MAX_EXTENTS,
                                      &fs_tag_num_extents);
    dir_search_t ds;
    memset(&ds, 0, sizeof(ds));

    if (err) {
        test_fail("stripe_root_dir",
                  "failed to resolve fileset tag dir extents (err=%d)",
                  err);
    } else {
        adv_bf_tag_t root_tag = { .num = 2, .seq = 0 };
        err = advfs_dir_read(vol, &domain, fs_tag_extents,
                             fs_tag_num_extents, fs.info.dir_tag,
                             root_tag, capture_stripe_file, &ds);
        if (err) {
            test_fail("stripe_root_dir",
                      "advfs_dir_read failed (err=%d)", err);
        } else if (ds.found) {
            test_pass("stripe_root_dir",
                      "%s found (tag=%u.%u, %d entries total)",
                      STRIPE_FILE_NAME, ds.tag.num, ds.tag.seq,
                      ds.total);
        } else {
            test_fail("stripe_root_dir", "%s not found (%d entries)",
                      STRIPE_FILE_NAME, ds.total);
        }
    }

    /* --- Test 6: file BSR_XTNTS resolves; extents cover the file --- */
    if (!ds.found) {
        test_fail("stripe_file_extents",
                  "skipped: %s not found", STRIPE_FILE_NAME);
    } else {
        uint32_t file_vol_page = 0;
        uint32_t file_cell = 0;
        uint32_t xtnt_type = 0;
        uint32_t segment_size = 0;

        err = lookup_file_xtnts(vol, &domain, fs_tag_extents,
                                fs_tag_num_extents, ds.tag,
                                &file_vol_page, &file_cell,
                                &xtnt_type, &segment_size);
        if (err) {
            test_fail("stripe_file_extents",
                      "failed to locate BSR_XTNTS for tag %u (err=%d)",
                      ds.tag.num, err);
        } else {
            if (xtnt_type != ADVFS_BSXMT_STRIPE) {
                printf("NOTE: on-disk BSR_XTNTS type=%u (BSXMT_APPEND) "
                       "for %s -- the fixture file is not genuinely "
                       "striped; all extents live on volume 1\n",
                       xtnt_type, STRIPE_FILE_NAME);
            }

            advfs_extent_t extents[ADVFS_MAX_EXTENTS];
            int num_extents = 0;
            err = advfs_extents_read(vol, domain.bmt_map,
                                     domain.bmt_map_count,
                                     file_vol_page, file_cell,
                                     extents, ADVFS_MAX_EXTENTS,
                                     &num_extents);
            if (err) {
                test_fail("stripe_file_extents",
                          "advfs_extents_read failed (err=%d)", err);
            } else {
                uint32_t pages = advfs_extents_page_count(extents,
                                                          num_extents);
                if (pages == STRIPE_FILE_PAGES) {
                    test_pass("stripe_file_extents",
                              "BSR_XTNTS type=%u, %d extent entries, "
                              "%u pages (%u bytes)",
                              xtnt_type, num_extents, pages,
                              pages * ADVFS_PGSZ);
                } else {
                    test_fail("stripe_file_extents",
                              "expected %u pages, got %u",
                              (unsigned)STRIPE_FILE_PAGES, pages);
                }
            }
        }
    }

    advfs_domain_close(&domain);
    advfs_volume_close(vol);

    /* --- Test 7: stripe detection fires (synthetic fixture) --- */
    test_stripe_detection_fires();

    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed ? 1 : 0;
}
