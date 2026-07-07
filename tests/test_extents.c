/*
 * test_extents.c -- edge-case tests for extent map resolution
 *
 * Usage: test_extents <vdisk-path>
 *
 * Runs against a real AdvFS vdisk (ODS v3 or v4). Verifies:
 *   1. advfs_extents_page_count() matches the final TERM boundary
 *   2. Reading a page past EOF returns -ENOENT
 *   3. The domain BMT extent map is complete and resolvable
 *   4. Root directory extents resolve through BSR_SHADOW_XTNTS on
 *      ODS v3, or through the primary BSR_XTNTS (no shadow) on v4
 *
 * Each test prints "[PASS] name: description" or
 * "[FAIL] name: reason". Exit code 0 if all pass, 1 otherwise.
 *
 * Copyright (C) 2026 Armas Spann
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _XOPEN_SOURCE 500  /* pread() */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/volume.h"
#include "../src/domain.h"
#include "../src/bmt.h"
#include "../src/extents.h"
#include "../src/fileset.h"
#include "../src/util.h"

/* Test result counters. */
static int tests_passed;
static int tests_failed;

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

/* Storage for the first fileset found during enumeration. */
typedef struct {
    advfs_fileset_info_t info;
    int found;
} first_fileset_t;

/* Callback: capture the first fileset and stop enumeration. */
static int capture_first_fileset(const advfs_fileset_info_t *info,
                                 void *user_data)
{
    first_fileset_t *fs = (first_fileset_t *)user_data;
    fs->info = *info;
    fs->found = 1;
    return 1;  /* stop after first */
}

/* Find the highest TERM boundary bs_page in an extent array, or 0. */
static uint32_t max_term_page(const advfs_extent_t *extents, int count,
                              int *term_found)
{
    uint32_t max_pg = 0;
    *term_found = 0;
    for (int i = 0; i < count; i++) {
        if (extents[i].vd_blk == ADVFS_XTNT_TERM &&
            extents[i].bs_page >= max_pg) {
            max_pg = extents[i].bs_page;
            *term_found = 1;
        }
    }
    return max_pg;
}

/* Find the highest TERM boundary bs_page in a raw BMT extent map. */
static uint32_t max_term_page_raw(const adv_xtnt_t *xtnts, int count,
                                  int *term_found)
{
    uint32_t max_pg = 0;
    *term_found = 0;
    for (int i = 0; i < count; i++) {
        if (xtnts[i].vd_blk == ADVFS_XTNT_TERM &&
            xtnts[i].bs_page >= max_pg) {
            max_pg = xtnts[i].bs_page;
            *term_found = 1;
        }
    }
    return max_pg;
}

/* Read the BFSDIR extent map for the domain. */
static int read_bfsdir_extents(advfs_volume_t *vol, advfs_domain_t *domain,
                               advfs_extent_t *extents, int max_extents,
                               int *count_out)
{
    uint32_t bfsdir_cell = (domain->ods_version >= ADVFS_FIRST_RBMT_VERSION)
                            ? ADVFS_BFM_BFSDIR : ADVFS_BFM_BFSDIR_V3;

    return advfs_extents_read(vol, domain->bmt_map, domain->bmt_map_count,
                              domain->bmt_page, bfsdir_cell,
                              extents, max_extents, count_out);
}

/*
 * Resolve the fileset tag directory extents from BFSDIR.
 * Same walk as the FUSE path: BFSDIR tag lookup, then mcell chain
 * until an mcell with BSR_XTNTS yields a usable extent map.
 */
static int resolve_fileset_tag_extents(advfs_volume_t *vol,
                                       advfs_domain_t *domain,
                                       adv_bf_tag_t fs_dir_tag,
                                       advfs_extent_t *extents_out,
                                       int max_extents, int *count_out)
{
    advfs_extent_t bfsdir_extents[ADVFS_MAX_EXTENTS];
    int bfsdir_num_extents = 0;

    int err = read_bfsdir_extents(vol, domain, bfsdir_extents,
                                  ADVFS_MAX_EXTENTS, &bfsdir_num_extents);
    if (err) {
        return err;
    }

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

    uint32_t bmt_pg_num = ADV_MCID_PAGE(tm->u.s3.mcid);
    uint32_t mcell_cell = ADV_MCID_CELL(tm->u.s3.mcid);

    uint32_t vol_page;
    err = advfs_bmt_resolve_page(domain->bmt_map, domain->bmt_map_count,
                                 bmt_pg_num, &vol_page);
    if (err) {
        return err;
    }

    err = advfs_extents_read(vol, domain->bmt_map, domain->bmt_map_count,
                             vol_page, mcell_cell,
                             extents_out, max_extents, count_out);
    if (err == 0) {
        return 0;
    }
    if (err != -ENODATA) {
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

    return -ENODATA;
}

/*
 * Locate the mcell holding BSR_XTNTS for a tag in a fileset.
 * Follows the tag directory, then the mcell chain (V3 layout).
 * On success sets *xtnt_vol_page and *xtnt_cell and, if a chained mcell
 * carries BSR_SHADOW_XTNTS, sets *shadow_seen.
 */
static int find_tag_xtnts_mcell(advfs_volume_t *vol, advfs_domain_t *domain,
                                const advfs_extent_t *fs_tag_extents,
                                int fs_tag_num_extents, uint32_t tag_num,
                                uint32_t *xtnt_vol_page, uint32_t *xtnt_cell,
                                int *shadow_seen)
{
    *shadow_seen = 0;

    uint32_t tag_page = tag_num / ADVFS_TAGS_PER_PG;
    uint32_t tag_idx = tag_num % ADVFS_TAGS_PER_PG;

    adv_tag_dir_pg_t tag_pg;
    int err = advfs_extents_read_data(vol, fs_tag_extents,
                                      fs_tag_num_extents, tag_page, &tag_pg);
    if (err) {
        return err;
    }

    adv_tag_map_t *tm = &tag_pg.maps[tag_idx];
    if (!ADV_TMAP_IN_USE(*tm)) {
        return -ENOENT;
    }

    uint32_t bmt_pg_num = ADV_MCID_PAGE(tm->u.s3.mcid);
    uint32_t cell = ADV_MCID_CELL(tm->u.s3.mcid);

    /* Walk the mcell chain until BSR_XTNTS is found. */
    int hops = 0;

    while (hops < 16) {
        uint32_t vol_page;
        err = advfs_bmt_resolve_page(domain->bmt_map,
                                     domain->bmt_map_count,
                                     bmt_pg_num, &vol_page);
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
        if (xr && ADV_REC_DATA_SZ(rhdr) >= sizeof(adv_xtnt_rec_t)) {
            *xtnt_vol_page = vol_page;
            *xtnt_cell = cell;

            /* Inspect the chain mcell for BSR_SHADOW_XTNTS (V3). */
            adv_mcell_id_t chain = xr->chain_mcid;
            int chops = 0;

            while (chain.raw != 0 && chops < 16) {
                uint32_t cp = ADV_MCID_PAGE(chain);
                uint32_t cc = ADV_MCID_CELL(chain);
                uint32_t cvp;

                if (advfs_bmt_resolve_page(domain->bmt_map,
                                           domain->bmt_map_count,
                                           cp, &cvp) != 0) {
                    break;
                }

                adv_bmt_pg_t chain_pg;
                if (advfs_bmt_read_page(vol, cvp, &chain_pg) != 0) {
                    break;
                }

                adv_mcell_t *cmc = advfs_bmt_get_mcell(&chain_pg, cc);
                if (!cmc) {
                    break;
                }

                if (advfs_bmt_find_rec(cmc, ADVFS_BSR_SHADOW_XTNTS,
                                       NULL)) {
                    *shadow_seen = 1;
                    break;
                }

                chain = cmc->next_mcid;
                chops++;
            }

            return 0;
        }

        if (mc->next_mcid.raw == 0) {
            return -ENODATA;
        }
        bmt_pg_num = ADV_MCID_PAGE(mc->next_mcid);
        cell = ADV_MCID_CELL(mc->next_mcid);
        hops++;
    }

    return -ENODATA;
}

/* Test 1: page count matches the extent map's final TERM boundary. */
static void test_page_count(advfs_volume_t *vol, advfs_domain_t *domain)
{
    const char *name = "page_count_matches_term";

    advfs_extent_t extents[ADVFS_MAX_EXTENTS];
    int num_extents = 0;

    int err = read_bfsdir_extents(vol, domain, extents,
                                  ADVFS_MAX_EXTENTS, &num_extents);
    if (err || num_extents == 0) {
        test_fail(name, "cannot read BFSDIR extents (err=%d, count=%d)",
                  err, num_extents);
        return;
    }

    int term_found = 0;
    uint32_t term_page = max_term_page(extents, num_extents, &term_found);
    if (!term_found) {
        test_fail(name, "BFSDIR extent map has no TERM boundary "
                  "(%d extents)", num_extents);
        return;
    }
    if (term_page == 0) {
        test_fail(name, "final TERM bs_page is 0 (empty bitfile?)");
        return;
    }

    uint32_t count = advfs_extents_page_count(extents, num_extents);
    if (count != term_page) {
        test_fail(name, "page_count=%u but final TERM bs_page=%u",
                  count, term_page);
        return;
    }

    test_pass(name, "BFSDIR page count %u matches final TERM boundary "
              "(%d extents)", count, num_extents);
}

/* Test 2: reading a page past EOF returns -ENOENT. */
static void test_past_eof(advfs_volume_t *vol, advfs_domain_t *domain)
{
    const char *name = "past_eof_read_enoent";

    advfs_extent_t extents[ADVFS_MAX_EXTENTS];
    int num_extents = 0;

    int err = read_bfsdir_extents(vol, domain, extents,
                                  ADVFS_MAX_EXTENTS, &num_extents);
    if (err || num_extents == 0) {
        test_fail(name, "cannot read BFSDIR extents (err=%d)", err);
        return;
    }

    uint32_t count = advfs_extents_page_count(extents, num_extents);
    uint8_t buf[ADVFS_PGSZ];

    /* First page past EOF */
    err = advfs_extents_read_data(vol, extents, num_extents, count, buf);
    if (err != -ENOENT) {
        test_fail(name, "page %u (first past EOF) returned %d, "
                  "expected -ENOENT (%d)", count, err, -ENOENT);
        return;
    }

    /* Far past EOF */
    err = advfs_extents_read_data(vol, extents, num_extents,
                                  count + 100000, buf);
    if (err != -ENOENT) {
        test_fail(name, "page %u (far past EOF) returned %d, "
                  "expected -ENOENT (%d)", count + 100000, err, -ENOENT);
        return;
    }

    test_pass(name, "pages %u and %u past EOF both return -ENOENT",
              count, count + 100000);
}

/* Test 3: BMT extent map completeness and full-range resolution. */
static void test_bmt_map(advfs_volume_t *vol, advfs_domain_t *domain)
{
    const char *name = "bmt_map_completeness";

    if (domain->bmt_map_count <= 0) {
        test_fail(name, "domain BMT map is empty");
        return;
    }

    /* The map must carry a TERM boundary bounding the BMT. */
    int term_found = 0;
    uint32_t bmt_pages = max_term_page_raw(domain->bmt_map,
                                           domain->bmt_map_count,
                                           &term_found);
    if (!term_found || bmt_pages == 0) {
        test_fail(name, "BMT map has no final TERM boundary "
                  "(%d entries)", domain->bmt_map_count);
        return;
    }

    /* BMT page 0 must resolve. On V3 the BMT starts at the fixed
     * reserved location (vol page 2). On V4 that location holds the
     * RBMT instead: the BMT is a separate bitfile elsewhere, so BMT
     * page 0 must NOT alias the RBMT page and must read back as a
     * valid BMT page. */
    uint32_t vol_page = 0;
    int err = advfs_bmt_resolve_page(domain->bmt_map,
                                     domain->bmt_map_count, 0, &vol_page);
    if (err) {
        test_fail(name, "BMT page 0 does not resolve (err=%d)", err);
        return;
    }
    uint32_t rsvd_page = ADVFS_RESERVED_BLKS / ADVFS_BLKS_PER_PG;
    if (domain->ods_version < ADVFS_FIRST_RBMT_VERSION) {
        if (vol_page != rsvd_page) {
            test_fail(name, "BMT page 0 resolves to vol page %u, "
                      "expected %u", vol_page, rsvd_page);
            return;
        }
    } else {
        if (vol_page == rsvd_page) {
            test_fail(name, "V4 BMT page 0 aliases the RBMT page (%u)",
                      rsvd_page);
            return;
        }
        adv_bmt_pg_t pg;
        err = advfs_bmt_read_page(vol, vol_page, &pg);
        if (err) {
            test_fail(name, "V4 BMT page 0 (vol page %u) is not a "
                      "valid BMT page (err=%d)", vol_page, err);
            return;
        }
    }

    /* Every BMT page inside the range must resolve. */
    for (uint32_t pg = 0; pg < bmt_pages; pg++) {
        err = advfs_bmt_resolve_page(domain->bmt_map,
                                     domain->bmt_map_count, pg, &vol_page);
        if (err) {
            test_fail(name, "BMT page %u of %u does not resolve (err=%d)",
                      pg, bmt_pages, err);
            return;
        }
    }

    /* The first page past the TERM boundary must NOT resolve. */
    err = advfs_bmt_resolve_page(domain->bmt_map, domain->bmt_map_count,
                                 bmt_pages, &vol_page);
    if (err != -ENOENT) {
        test_fail(name, "BMT page %u (past TERM) returned %d, "
                  "expected -ENOENT (%d)", bmt_pages, err, -ENOENT);
        return;
    }

    test_pass(name, "all %u BMT pages resolve, page %u past TERM "
              "returns -ENOENT (%d map entries)",
              bmt_pages, bmt_pages, domain->bmt_map_count);
}

/*
 * Test 4: root directory extents resolve through the version-correct
 * record: chained BSR_SHADOW_XTNTS on ODS v3, real extents in the
 * primary BSR_XTNTS (no shadow record) on ODS v4.
 */
static void test_shadow_extents(advfs_volume_t *vol, advfs_domain_t *domain)
{
    const char *name = "shadow_extents_root_dir";

    /*
     * Find the first fileset. On test_single.vdisk the parent fileset
     * "primary" is enumerated first (its clone "primary_snap" follows).
     */
    first_fileset_t first_fs;
    memset(&first_fs, 0, sizeof(first_fs));

    int err = advfs_fileset_list(vol, domain, capture_first_fileset,
                                 &first_fs);
    if (err || !first_fs.found) {
        test_fail(name, "no fileset found (err=%d)", err);
        return;
    }

    /* Resolve the fileset tag directory extents. */
    advfs_extent_t fs_tag_extents[ADVFS_MAX_EXTENTS];
    int fs_tag_num_extents = 0;

    err = resolve_fileset_tag_extents(vol, domain, first_fs.info.dir_tag,
                                      fs_tag_extents, ADVFS_MAX_EXTENTS,
                                      &fs_tag_num_extents);
    if (err) {
        test_fail(name, "cannot resolve fileset tag dir extents (err=%d)",
                  err);
        return;
    }

    /* Locate the root directory's (tag 2) BSR_XTNTS mcell. */
    uint32_t xtnt_vol_page = 0, xtnt_cell = 0;
    int shadow_seen = 0;

    err = find_tag_xtnts_mcell(vol, domain, fs_tag_extents,
                               fs_tag_num_extents, 2,
                               &xtnt_vol_page, &xtnt_cell, &shadow_seen);
    if (err) {
        test_fail(name, "cannot locate BSR_XTNTS for root tag 2 (err=%d)",
                  err);
        return;
    }

    if (domain->ods_version >= ADVFS_FIRST_XTNT_IN_PRIM_MCELL_VERSION) {
        /* V4: real extents live in the primary BSR_XTNTS; shadow
         * records must not appear. */
        if (shadow_seen) {
            test_fail(name, "root tag 2 chain has a BSR_SHADOW_XTNTS "
                      "record (unexpected on ODS v%d)",
                      domain->ods_version);
            return;
        }
    } else if (!shadow_seen) {
        test_fail(name, "root tag 2 chain has no BSR_SHADOW_XTNTS record "
                  "(expected on ODS v3)");
        return;
    }

    /* Read the root directory's extent map through the chain. */
    advfs_extent_t dir_extents[ADVFS_MAX_EXTENTS];
    int num_dir_extents = 0;

    err = advfs_extents_read(vol, domain->bmt_map, domain->bmt_map_count,
                             xtnt_vol_page, xtnt_cell,
                             dir_extents, ADVFS_MAX_EXTENTS,
                             &num_dir_extents);
    if (err || num_dir_extents == 0) {
        test_fail(name, "cannot read root dir extents (err=%d, count=%d)",
                  err, num_dir_extents);
        return;
    }

    /* Read data page 0 and verify the first entry is "." for tag 2. */
    uint8_t page_buf[ADVFS_PGSZ];
    err = advfs_extents_read_data(vol, dir_extents, num_dir_extents,
                                  0, page_buf);
    if (err) {
        test_fail(name, "cannot read root dir page 0 (err=%d)", err);
        return;
    }

    adv_dir_entry_t hdr;
    memcpy(&hdr, page_buf, sizeof(hdr));

    if (hdr.tag_num != 2 || hdr.name_count != 1 ||
        page_buf[ADVFS_DIR_ENTRY_HDR_SZ] != '.') {
        test_fail(name, "root dir page 0 first entry is not \".\" "
                  "(tag_num=%u, name_count=%u, name[0]=0x%02x)",
                  hdr.tag_num, hdr.name_count,
                  page_buf[ADVFS_DIR_ENTRY_HDR_SZ]);
        return;
    }

    test_pass(name, "root tag 2 resolved via %s, "
              "%d extent(s), page 0 starts with \".\" entry",
              shadow_seen ? "BSR_SHADOW_XTNTS" : "primary BSR_XTNTS",
              num_dir_extents);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <vdisk-path>\n", argv[0]);
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

    test_page_count(vol, &domain);
    test_past_eof(vol, &domain);
    test_bmt_map(vol, &domain);
    test_shadow_extents(vol, &domain);

    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);

    advfs_domain_close(&domain);
    advfs_volume_close(vol);
    return tests_failed > 0 ? 1 : 0;
}
