/*
 * fileset.c -- fileset enumeration for advfs-fuse
 *
 * Reads the BFSDIR bitfile's extents, then walks the tag directory
 * pages to find all in-use tags. For each in-use tag, follows the
 * tag map to the mcell and looks for BSR_BFS_ATTR records to
 * discover filesets.
 *
 * Copyright (C) 2026 Armas Spann
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _XOPEN_SOURCE 500  /* pread() */

#include "fileset.h"
#include "bmt.h"
#include "extents.h"
#include "util.h"

#include <errno.h>
#include <string.h>

/* Maximum tag directory pages we will scan.
 * Each page holds 1022 tags; 16384 pages = ~16.7M tags per fileset. */
#define MAX_TAG_DIR_PAGES  16384

/* Try to extract fileset info from an mcell's BSR_BFS_ATTR record. */
static int try_fileset_from_mcell(adv_mcell_t *mc, advfs_fileset_info_t *info)
{
    adv_rec_hdr_t rhdr;
    adv_bf_set_attr_t *bfs = advfs_bmt_find_rec(mc, ADVFS_BSR_BFS_ATTR,
                                                  &rhdr);
    if (!bfs) {
        return -1;
    }

    if (ADV_REC_DATA_SZ(rhdr) < sizeof(adv_bf_set_attr_t)) {
        advfs_dbg("fileset: BSR_BFS_ATTR too small (%u < %zu)",
                  ADV_REC_DATA_SZ(rhdr), sizeof(adv_bf_set_attr_t));
        return -1;
    }

    memset(info, 0, sizeof(*info));
    memcpy(info->name, bfs->set_name, ADVFS_BS_SET_NAME_SZ);
    info->name[ADVFS_BS_SET_NAME_SZ - 1] = '\0';
    info->dir_tag = mc->tag;
    info->set_id = bfs->bf_set_id;
    info->state = bfs->state;
    info->fs_dev = bfs->fs_dev;
    info->clone_id = bfs->clone_id;
    info->clone_cnt = bfs->clone_cnt;
    info->orig_set_tag = bfs->orig_set_tag;
    info->next_clone_set_tag = bfs->next_clone_set_tag;
    return 0;
}

/* Log the clone status of a fileset. */
void advfs_fileset_log_clone(const advfs_fileset_info_t *info)
{
    if (!info) {
        return;
    }

    if (info->clone_id != 0) {
        advfs_dbg("fileset: '%s' is a clone of tag %u -- permanent-hole "
                  "pages fall back to the original fileset",
                  info->name, info->orig_set_tag.num);
        return;
    }

    if (info->clone_cnt != 0) {
        advfs_dbg("fileset: '%s' has %u clone(s), next clone set tag %u",
                  info->name, info->clone_cnt,
                  info->next_clone_set_tag.num);
    } else {
        advfs_dbg("fileset: '%s' is not a clone and has no clones",
                  info->name);
    }
}

/* Enumerate filesets via BFSDIR extent map and tag directory pages. */
int advfs_fileset_list(advfs_volume_t *vol, advfs_domain_t *domain,
                       advfs_fileset_cb cb, void *user_data)
{
    if (!vol || !domain || !cb) {
        return -EINVAL;
    }

    /*
     * Step 1: Determine BFSDIR cell index.
     * For V3: cell 2 on BMT page 0.
     * For V4: cell 2 on RBMT page 0.
     */
    uint32_t bfsdir_cell;
    if (domain->ods_version >= ADVFS_FIRST_RBMT_VERSION) {
        bfsdir_cell = ADVFS_BFM_BFSDIR;
    } else {
        bfsdir_cell = ADVFS_BFM_BFSDIR_V3;
    }

    advfs_dbg("fileset: BFSDIR cell = %u, BMT base page = %u",
              bfsdir_cell, domain->bmt_page);

    /*
     * Step 2: Read the extent map for the BFSDIR bitfile.
     *
     * The BFSDIR primary mcell sits on the page-0 metadata page
     * (domain->bmt_page) for both versions. On V4 that page is the
     * RBMT, and any chained extent mcells of this reserved file are
     * RBMT cells -- their page numbers must be resolved through the
     * RBMT's own extent map, not the BMT's.
     */
    const adv_xtnt_t *meta_map = domain->bmt_map;
    int meta_map_count = domain->bmt_map_count;
    if (domain->ods_version >= ADVFS_FIRST_RBMT_VERSION) {
        meta_map = domain->rbmt_map;
        meta_map_count = domain->rbmt_map_count;
    }

    advfs_extent_t extents[ADVFS_MAX_EXTENTS];
    int num_extents = 0;

    int err = advfs_extents_read(vol, meta_map, meta_map_count,
                                 domain->bmt_page, bfsdir_cell, extents,
                                 ADVFS_MAX_EXTENTS, &num_extents);
    if (err) {
        advfs_err("fileset: failed to read BFSDIR extents");
        return err;
    }

    if (num_extents == 0) {
        advfs_dbg("fileset: BFSDIR has no extents");
        return 0;
    }

    advfs_dbg("fileset: BFSDIR has %d extent(s)", num_extents);

    /*
     * Step 3: Determine how many data pages the BFSDIR has.
     *
     * The extent map's final terminator entry carries the exact
     * page count. We cap at MAX_TAG_DIR_PAGES to avoid runaway on
     * corrupt data.
     */
    uint32_t max_bf_page = advfs_extents_page_count(extents, num_extents);

    if (max_bf_page > MAX_TAG_DIR_PAGES) {
        advfs_dbg("fileset: capping BFSDIR pages from %u to %u",
                  max_bf_page, MAX_TAG_DIR_PAGES);
        max_bf_page = MAX_TAG_DIR_PAGES;
    }

    advfs_dbg("fileset: scanning %u BFSDIR page(s)", max_bf_page);

    /*
     * Step 4: Read each tag directory page and process in-use tags.
     */
    int found = 0;
    adv_tag_dir_pg_t tag_pg;

    for (uint32_t pg = 0; pg < max_bf_page; pg++) {
        err = advfs_extents_read_data(vol, extents, num_extents,
                                      pg, &tag_pg);
        if (err) {
            advfs_dbg("fileset: failed to read BFSDIR page %u (err=%d), "
                      "skipping", pg, err);
            continue;
        }

        advfs_dbg("fileset: tag dir page %u: curr_page=%u, "
                  "next_free_page=%u, num_alloc=%u",
                  pg, tag_pg.hdr.curr_page,
                  tag_pg.hdr.next_free_page,
                  tag_pg.hdr.num_alloc_tmaps);

        /*
         * Walk tag map entries. Entry 0 on page 0 is the free list
         * header, not a real tag -- skip it.
         */
        uint32_t start = (pg == 0) ? 1 : 0;

        for (uint32_t i = start; i < ADVFS_TAGS_PER_PG; i++) {
            adv_tag_map_t *tm = &tag_pg.maps[i];

            if (!ADV_TMAP_IN_USE(*tm)) {
                continue;
            }

            adv_mcell_id_t mcid = tm->u.s3.mcid;
            uint32_t tag_num = (uint32_t)(pg * ADVFS_TAGS_PER_PG + i);
            (void)tag_num;  /* referenced only by debug logging */

            advfs_dbg("fileset: tag %u in-use: vd=%u, mcid=p%u.c%u",
                      tag_num, tm->u.s3.vd_index,
                      ADV_MCID_PAGE(mcid), ADV_MCID_CELL(mcid));

            /*
             * Follow the tag map to the mcell on the volume named
             * by the tag map's vd_index.
             */
            uint32_t mcell_vd = tm->u.s3.vd_index;
            uint32_t mcell_bmt_pg = ADV_MCID_PAGE(mcid);
            uint32_t mcell_cell = ADV_MCID_CELL(mcid);

            adv_bmt_pg_t bmt_pg;
            err = advfs_domain_read_bmt_page_vd(domain, mcell_vd,
                                                mcell_bmt_pg, &bmt_pg);
            if (err) {
                advfs_dbg("fileset: failed to read vd %u BMT page %u "
                          "for tag %u", mcell_vd, mcell_bmt_pg, tag_num);
                continue;
            }

            adv_mcell_t *mc = advfs_bmt_get_mcell(&bmt_pg, mcell_cell);
            if (!mc) {
                advfs_dbg("fileset: invalid cell %u on BMT page %u "
                          "for tag %u", mcell_cell, mcell_bmt_pg, tag_num);
                continue;
            }

            /* Try to get BSR_BFS_ATTR from this mcell */
            advfs_fileset_info_t info;
            if (try_fileset_from_mcell(mc, &info) == 0) {
                advfs_dbg("fileset: found '%s' at tag %u",
                          info.name, tag_num);
                advfs_fileset_log_clone(&info);
                found++;
                int ret = cb(&info, user_data);
                if (ret != 0) {
                    return 0;  /* callback requested stop */
                }
                continue;
            }

            /*
             * BSR_BFS_ATTR might be in a chained mcell. Walk the
             * chain up to 8 hops looking for it.
             */
            adv_mcell_id_t next = mc->next_mcid;
            uint32_t next_vd = (mc->next_vd_index != 0)
                               ? mc->next_vd_index : mcell_vd;
            int hops = 0;

            while (next.raw != 0 && hops < 8) {
                uint32_t np = ADV_MCID_PAGE(next);
                uint32_t nc = ADV_MCID_CELL(next);

                adv_bmt_pg_t next_pg;
                err = advfs_domain_read_bmt_page_vd(domain, next_vd,
                                                    np, &next_pg);
                if (err) {
                    break;
                }

                adv_mcell_t *nmc = advfs_bmt_get_mcell(&next_pg, nc);
                if (!nmc) {
                    break;
                }

                if (try_fileset_from_mcell(nmc, &info) == 0) {
                    advfs_dbg("fileset: found '%s' at tag %u "
                              "(chain hop %d)", info.name, tag_num,
                              hops + 1);
                    advfs_fileset_log_clone(&info);
                    found++;
                    int ret = cb(&info, user_data);
                    if (ret != 0) {
                        return 0;
                    }
                    break;
                }

                if (nmc->next_vd_index != 0) {
                    next_vd = nmc->next_vd_index;
                }
                next = nmc->next_mcid;
                hops++;
            }
        }
    }

    if (found == 0) {
        advfs_dbg("fileset: no filesets found in BFSDIR");
    } else {
        advfs_dbg("fileset: found %d fileset(s)", found);
    }

    return 0;
}

/* Maximum mcell chain hops when searching for BSR_XTNTS. */
#define MAX_XTNTS_CHAIN_HOPS  16

/*
 * Read the extent map for the mcell at (vd, bmt_pg_num, cell),
 * following the mcell chain when BSR_XTNTS is not in the primary
 * (V3 layout). Chains may cross volumes via next_vd_index.
 * Returns 0 on success, -errno on failure.
 */
static int read_extents_via_chain(advfs_domain_t *domain,
                                  uint32_t vd, uint32_t bmt_pg_num,
                                  uint32_t cell,
                                  advfs_extent_t *extents_out,
                                  int max_extents, int *count_out)
{
    int hops = 0;

    for (;;) {
        int err = advfs_extents_read_dom(domain, vd, bmt_pg_num, cell,
                                         extents_out, max_extents,
                                         count_out);
        if (err == 0) {
            return 0;
        }
        if (err != -ENODATA) {
            return err;
        }

        /* No BSR_XTNTS in this mcell -- follow the chain (V3). */
        adv_bmt_pg_t bmt_pg;
        err = advfs_domain_read_bmt_page_vd(domain, vd, bmt_pg_num,
                                            &bmt_pg);
        if (err) {
            return err;
        }

        adv_mcell_t *mc = advfs_bmt_get_mcell(&bmt_pg, cell);
        if (!mc) {
            return -EINVAL;
        }

        adv_mcell_id_t next = mc->next_mcid;
        if (next.raw == 0 || hops >= MAX_XTNTS_CHAIN_HOPS) {
            advfs_dbg("fileset: no BSR_XTNTS in mcell chain "
                      "(hops=%d)", hops);
            return -ENODATA;
        }

        if (mc->next_vd_index != 0) {
            vd = mc->next_vd_index;
        }
        bmt_pg_num = ADV_MCID_PAGE(next);
        cell = ADV_MCID_CELL(next);
        hops++;
    }
}

/* Resolve the extent map of a fileset's tag directory. */
int advfs_fileset_tag_extents(advfs_volume_t *vol, advfs_domain_t *domain,
                              adv_bf_tag_t fs_dir_tag,
                              advfs_extent_t *extents_out, int max_extents,
                              int *count_out)
{
    if (!vol || !domain || !extents_out || !count_out) {
        return -EINVAL;
    }
    if (max_extents <= 0 || fs_dir_tag.num == 0) {
        return -EINVAL;
    }

    uint32_t bfsdir_cell = (domain->ods_version >= ADVFS_FIRST_RBMT_VERSION)
                           ? ADVFS_BFM_BFSDIR : ADVFS_BFM_BFSDIR_V3;

    /* Step 1: BFSDIR's own extent map from the reserved mcell.
     *
     * As in advfs_fileset_list(): the BFSDIR primary mcell sits on the
     * page-0 metadata page for both versions, but on V4 that page is
     * the RBMT and any chained extent mcells of this reserved file are
     * RBMT cells -- their page numbers must be resolved through the
     * RBMT's own extent map, not the BMT's. */
    const adv_xtnt_t *meta_map = domain->bmt_map;
    int meta_map_count = domain->bmt_map_count;
    if (domain->ods_version >= ADVFS_FIRST_RBMT_VERSION) {
        meta_map = domain->rbmt_map;
        meta_map_count = domain->rbmt_map_count;
    }

    advfs_extent_t bfsdir_extents[ADVFS_MAX_EXTENTS];
    int bfsdir_num_extents = 0;

    int err = advfs_extents_read(vol, meta_map, meta_map_count,
                                 domain->bmt_page, bfsdir_cell,
                                 bfsdir_extents, ADVFS_MAX_EXTENTS,
                                 &bfsdir_num_extents);
    if (err) {
        advfs_err("fileset: cannot read BFSDIR extents (err=%d)", err);
        return err;
    }

    /* Step 2: look up the fileset's tag in the BFSDIR tag directory. */
    uint32_t tag_page = fs_dir_tag.num / ADVFS_TAGS_PER_PG;
    uint32_t tag_idx = fs_dir_tag.num % ADVFS_TAGS_PER_PG;

    adv_tag_dir_pg_t bfsdir_tag_pg;
    err = advfs_extents_read_data(vol, bfsdir_extents, bfsdir_num_extents,
                                  tag_page, &bfsdir_tag_pg);
    if (err) {
        advfs_err("fileset: cannot read BFSDIR tag page %u (err=%d)",
                  tag_page, err);
        return err;
    }

    adv_tag_map_t *tm = &bfsdir_tag_pg.maps[tag_idx];
    if (!ADV_TMAP_IN_USE(*tm)) {
        advfs_err("fileset: tag %u not in use in BFSDIR", fs_dir_tag.num);
        return -ENOENT;
    }

    /* Step 3: fileset's primary mcell -> BSR_XTNTS extent map. */
    uint32_t mcell_vd = tm->u.s3.vd_index;
    uint32_t bmt_pg_num = ADV_MCID_PAGE(tm->u.s3.mcid);
    uint32_t mcell_cell = ADV_MCID_CELL(tm->u.s3.mcid);

    err = read_extents_via_chain(domain, mcell_vd, bmt_pg_num, mcell_cell,
                                 extents_out, max_extents, count_out);
    if (err) {
        advfs_err("fileset: cannot read tag dir extents for fileset "
                  "tag %u (err=%d)", fs_dir_tag.num, err);
    }
    return err;
}
