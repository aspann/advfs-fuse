/*
 * dir.c -- AdvFS directory entry parsing for advfs-fuse
 *
 * Reads directory data pages and parses variable-length directory
 * entries within 512-byte (DIRBLKSIZ) blocks. Entries do not cross
 * block boundaries. Each 8 KB page contains 16 such blocks.
 *
 * Data flow:
 *   1. Look up dir_tag in the fileset's tag directory
 *   2. Follow the tag map to the mcell, find BSR_XTNTS
 *   3. Read the directory's extent map
 *   4. Read each data page and walk entries per 512-byte block
 *
 * Copyright (C) 2026 Armas Spann
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _XOPEN_SOURCE 500  /* pread() */

#include "dir.h"
#include "bmt.h"
#include "fileset.h"  /* advfs_fileset_tag_extents (clone fallback) */
#include "util.h"

#include <errno.h>
#include <string.h>

/* Maximum directory data pages to scan.
 * Large directories (e.g. spool, mail) can span many pages.
 * 16384 pages = 128 MB max directory size. */
#define MAX_DIR_PAGES  16384

/* Maximum mcell chain hops when searching for BSR_XTNTS. */
#define MAX_XTNTS_HOPS  16

/* Maximum file name length in a directory entry. */
#define MAX_DIR_NAME  255

/* Minimum directory entry size guard. Valid V3 entries are at least 20
 * (8 header + 4 padded name + 8 trailing tag), but deleted/gap entries
 * may be as small as 8 (header only) or 12 (header + 4 gap bytes).
 * Keep this at the header size to avoid breaking on valid gap entries. */
#define MIN_DIR_ENTRY_SZ  ADVFS_DIR_ENTRY_HDR_SZ

/*
 * Find the mcell containing BSR_XTNTS for a tag's primary mcell.
 *
 * In ODS V3, BSR_XTNTS may not be in the primary mcell but in a
 * chained mcell. This function checks the primary first, then
 * follows the chain (which may cross volumes via next_vd_index).
 *
 * On success, sets *xtnt_vd, *xtnt_pg and *xtnt_cell to the volume,
 * BMT-internal page and cell of the mcell containing BSR_XTNTS.
 * Returns 0 on success, -errno on failure.
 */
static int find_xtnts_mcell(advfs_domain_t *domain,
                            uint32_t vd, uint32_t bmt_pg_num, uint32_t cell,
                            uint32_t *xtnt_vd, uint32_t *xtnt_pg,
                            uint32_t *xtnt_cell)
{
    adv_bmt_pg_t bmt_pg;
    int err = advfs_domain_read_bmt_page_vd(domain, vd, bmt_pg_num,
                                            &bmt_pg);
    if (err) {
        advfs_err("dir: cannot read vd %u BMT page %u", vd, bmt_pg_num);
        return err;
    }

    adv_mcell_t *mc = advfs_bmt_get_mcell(&bmt_pg, cell);
    if (!mc) {
        advfs_err("dir: invalid cell %u on BMT page %u", cell, bmt_pg_num);
        return -EINVAL;
    }

    /* Check primary mcell for BSR_XTNTS */
    if (advfs_bmt_find_rec(mc, ADVFS_BSR_XTNTS, NULL)) {
        *xtnt_vd = vd;
        *xtnt_pg = bmt_pg_num;
        *xtnt_cell = cell;
        return 0;
    }

    /* Follow the mcell chain to find BSR_XTNTS (V3 layout) */
    adv_mcell_id_t next = mc->next_mcid;
    uint32_t next_vd = (mc->next_vd_index != 0) ? mc->next_vd_index : vd;
    int hops = 0;

    while (next.raw != 0 && hops < MAX_XTNTS_HOPS) {
        uint32_t np = ADV_MCID_PAGE(next);
        uint32_t nc = ADV_MCID_CELL(next);
        uint32_t nvd = next_vd;

        err = advfs_domain_read_bmt_page_vd(domain, nvd, np, &bmt_pg);
        if (err) {
            advfs_dbg("dir: chain: cannot read vd %u BMT page %u",
                      nvd, np);
            return err;
        }

        mc = advfs_bmt_get_mcell(&bmt_pg, nc);
        if (!mc) {
            advfs_dbg("dir: chain: invalid cell %u on BMT page %u",
                      nc, np);
            return -EINVAL;
        }

        if (advfs_bmt_find_rec(mc, ADVFS_BSR_XTNTS, NULL)) {
            advfs_dbg("dir: BSR_XTNTS found at chain hop %d "
                      "(vd %u BMT p%u.c%u)", hops + 1, nvd, np, nc);
            *xtnt_vd = nvd;
            *xtnt_pg = np;
            *xtnt_cell = nc;
            return 0;
        }

        next = mc->next_mcid;
        next_vd = (mc->next_vd_index != 0) ? mc->next_vd_index : nvd;
        hops++;
    }

    advfs_err("dir: BSR_XTNTS not found in mcell chain");
    return -ENODATA;
}

/* Maximum BMT pages to scan in the brute-force fallback. */
#define MAX_BMT_SCAN_PAGES  4096

/* Compute a BMT scan range from one BMT extent map. */
static uint32_t bmt_scan_pages(const adv_xtnt_t *map, int count)
{
    /*
     * The final terminator entry carries the exact BMT page count;
     * if the map has no terminator (truncated), estimate past the
     * last extent.
     */
    uint32_t max_pages = 0;
    for (int i = 0; i < count; i++) {
        uint32_t cand;
        if (map[i].vd_blk == ADVFS_XTNT_TERM) {
            cand = map[i].bs_page;
        } else {
            cand = map[i].bs_page + ((i + 1 < count) ? 1 : 64);
        }
        if (cand > max_pages) {
            max_pages = cand;
        }
    }
    if (max_pages > MAX_BMT_SCAN_PAGES) {
        max_pages = MAX_BMT_SCAN_PAGES;
    }
    if (max_pages == 0) {
        max_pages = 16;
    }
    return max_pages;
}

/*
 * Find a tag's mcell by scanning BMT pages (brute-force fallback).
 *
 * Used when the fileset's tag directory is dead/uninitialized. Scans
 * the BMT of every attached volume looking for an mcell whose
 * bf_set_tag matches fs_tag and whose tag.num matches target_tag_num.
 *
 * On success, sets *vd_out, *bmt_pg_out and *cell_out to the volume,
 * BMT page number and cell index. Returns 0 on success, -ENOENT if
 * not found.
 */
static int find_tag_by_bmt_scan(advfs_domain_t *domain,
                                adv_bf_tag_t fs_tag,
                                uint32_t target_tag_num,
                                uint32_t *vd_out,
                                uint32_t *bmt_pg_out, uint32_t *cell_out)
{
    /* Legacy contexts without a vd table scan the primary only. */
    int nvds = (domain->vd_count > 0) ? domain->vd_count : 1;

    for (int v = 0; v < nvds; v++) {
        uint32_t vd;
        const adv_xtnt_t *map;
        int map_count;

        if (domain->vd_count > 0) {
            vd = domain->vds[v].vd_index;
            map = domain->vds[v].bmt_map;
            map_count = domain->vds[v].bmt_map_count;
        } else {
            vd = 0;
            map = domain->bmt_map;
            map_count = domain->bmt_map_count;
        }

        uint32_t max_pages = bmt_scan_pages(map, map_count);

        advfs_dbg("dir: BMT scan for tag %u (bf_set_tag=%u) on vd %u, "
                  "scanning %u pages",
                  target_tag_num, fs_tag.num, vd, max_pages);

        for (uint32_t pg = 0; pg < max_pages; pg++) {
            adv_bmt_pg_t bmt_pg;
            int err = advfs_domain_read_bmt_page_vd(domain, vd, pg,
                                                    &bmt_pg);
            if (err) {
                continue;  /* skip unreadable pages */
            }

            for (uint32_t c = 0; c < ADVFS_BSPG_CELLS; c++) {
                adv_mcell_t *mc = advfs_bmt_get_mcell(&bmt_pg, c);
                if (!mc || mc->tag.num == 0) {
                    continue;
                }

                if (mc->tag.num == target_tag_num &&
                    mc->bf_set_tag.num == fs_tag.num) {
                    advfs_dbg("dir: BMT scan found tag %u at "
                              "vd %u p%u.c%u (bf_set_tag=%u.%u)",
                              target_tag_num, vd, pg, c,
                              mc->bf_set_tag.num, mc->bf_set_tag.seq);
                    *vd_out = vd;
                    *bmt_pg_out = pg;
                    *cell_out = c;
                    return 0;
                }
            }
        }
    }

    return -ENOENT;
}

/*
 * Check a directory's mcell chain for a BMTR_FS_UNDEL_DIR record.
 * Returns 1 if found (and warns), 0 if not, -errno on read failure.
 */
int advfs_dir_has_undelete(advfs_volume_t *vol, advfs_domain_t *domain,
                           uint32_t vd, uint32_t bmt_pg_num, uint32_t cell)
{
    if (!vol || !domain) {
        return -EINVAL;
    }

    adv_bmt_pg_t bmt_pg;
    int err = advfs_domain_read_bmt_page_vd(domain, vd, bmt_pg_num,
                                            &bmt_pg);
    if (err) {
        return err;
    }

    adv_mcell_t *mc = advfs_bmt_get_mcell(&bmt_pg, cell);
    if (!mc) {
        return -EINVAL;
    }

    /* Check the primary mcell, then follow the chain (which may
     * cross volumes via next_vd_index). */
    int hops = 0;

    while (mc) {
        if (advfs_bmt_find_rec(mc, ADVFS_BMTR_FS_UNDEL_DIR, NULL)) {
            advfs_warn("dir: undelete directory detected -- "
                       "not yet implemented");
            return 1;
        }

        adv_mcell_id_t next = mc->next_mcid;
        if (next.raw == 0 || hops >= MAX_XTNTS_HOPS) {
            break;
        }

        uint32_t np = ADV_MCID_PAGE(next);
        uint32_t nc = ADV_MCID_CELL(next);
        if (mc->next_vd_index != 0) {
            vd = mc->next_vd_index;
        }

        err = advfs_domain_read_bmt_page_vd(domain, vd, np, &bmt_pg);
        if (err) {
            break;  /* chain leaves the map: treat as not found */
        }

        mc = advfs_bmt_get_mcell(&bmt_pg, nc);
        hops++;
    }

    return 0;
}

/*
 * Parse directory entries within one 512-byte block.
 *
 * Calls cb for each valid (non-deleted) entry. Entries with
 * tag_num == 0 (deleted or never-used gap space) are counted in
 * *deleted_count for debug reporting; those that still carry a
 * plausible preserved name and trailing tag (i.e. real deleted
 * entries, not gap space) are additionally reported to deleted_cb.
 *
 * Either callback may be NULL to skip that class of entry.
 * Returns 0 if all entries processed, 1 if a callback requested stop.
 */
static int walk_dir_block(const uint8_t *block, advfs_dir_entry_cb cb,
                          advfs_dir_entry_cb deleted_cb,
                          void *user_data, uint32_t *deleted_count)
{
    uint32_t off = 0;

    while (off + ADVFS_DIR_ENTRY_HDR_SZ <= ADVFS_BLKSZ) {
        /* Read entry header via memcpy to avoid alignment issues */
        adv_dir_entry_t hdr;
        memcpy(&hdr, block + off, sizeof(hdr));

        uint16_t entry_size = hdr.entry_size;

        /* Guard: entry_size == 0 would cause infinite loop */
        if (entry_size == 0) {
            break;
        }

        /* Guard: entry too small to be valid */
        if (entry_size < MIN_DIR_ENTRY_SZ) {
            advfs_warn("dir: entry at offset %u has invalid size %u",
                       off, entry_size);
            break;
        }

        /* Guard: entry would cross block boundary */
        if (off + entry_size > ADVFS_BLKSZ) {
            advfs_warn("dir: entry at offset %u (size %u) crosses "
                       "block boundary", off, entry_size);
            break;
        }

        /* Count and skip deleted/empty entries (tag_num == 0). The
         * count includes never-used gap entries, which share the
         * tag_num == 0 marker and cannot be told apart reliably. */
        if (hdr.tag_num == 0) {
            (*deleted_count)++;

            /*
             * Deletion zeroes the header's tag_num but preserves the
             * entry size, name and trailing full tag. Report entries
             * that still look like real deleted entries (valid name
             * geometry, non-empty name, non-zero trailing tag);
             * anything else is never-used gap space.
             */
            if (deleted_cb) {
                uint16_t name_count = hdr.name_count;

                if (name_count > 0 && entry_size >= 20 &&
                    ADVFS_DIR_ENTRY_HDR_SZ + name_count +
                        sizeof(adv_bf_tag_t) <= entry_size) {
                    adv_bf_tag_t trailing_tag;
                    memcpy(&trailing_tag,
                           block + off + entry_size - sizeof(adv_bf_tag_t),
                           sizeof(adv_bf_tag_t));

                    char name_buf[MAX_DIR_NAME + 1];
                    uint16_t copy_len = name_count;
                    if (copy_len > MAX_DIR_NAME) {
                        copy_len = MAX_DIR_NAME;
                    }
                    memcpy(name_buf,
                           block + off + ADVFS_DIR_ENTRY_HDR_SZ, copy_len);
                    name_buf[copy_len] = '\0';

                    if (trailing_tag.num != 0 && name_buf[0] != '\0') {
                        int ret = deleted_cb(name_buf, trailing_tag,
                                             user_data);
                        if (ret != 0) {
                            return 1;  /* callback requested stop */
                        }
                    }
                }
            }
        } else {
            uint16_t name_count = hdr.name_count;
            const char *name_ptr = (const char *)(block + off +
                                                   ADVFS_DIR_ENTRY_HDR_SZ);

            /*
             * V3 layout: 8-byte header, padded name, then a trailing
             * adv_bf_tag_t (8 bytes) at the end of the entry.
             * Minimum valid entry: 8 + 4 + 8 = 20 bytes.
             */
            if (name_count > 0 && entry_size >= 20 &&
                ADVFS_DIR_ENTRY_HDR_SZ + name_count +
                    sizeof(adv_bf_tag_t) <= entry_size) {
                /* Extract trailing tag (last 8 bytes of entry) */
                adv_bf_tag_t trailing_tag;
                memcpy(&trailing_tag,
                       block + off + entry_size - sizeof(adv_bf_tag_t),
                       sizeof(adv_bf_tag_t));

                char name_buf[MAX_DIR_NAME + 1];
                uint16_t copy_len = name_count;
                if (copy_len > MAX_DIR_NAME) {
                    copy_len = MAX_DIR_NAME;
                }
                memcpy(name_buf, name_ptr, copy_len);
                name_buf[copy_len] = '\0';

                if (cb) {
                    int ret = cb(name_buf, trailing_tag, user_data);
                    if (ret != 0) {
                        return 1;  /* callback requested stop */
                    }
                }
            } else {
                advfs_warn("dir: entry at offset %u has invalid "
                           "name_count %u (entry_size=%u)",
                           off, name_count, entry_size);
            }
        }

        off += entry_size;
    }

    return 0;
}

/*
 * Resolve a directory tag to its extent map within one fileset.
 *
 * Performs the tag-directory lookup (with BMT-scan fallback for dead
 * tag dirs), locates the mcell holding BSR_XTNTS, and reads the
 * directory's extent map into extents_out/num_out. When prim_vd/
 * prim_pg/prim_cell are non-NULL they receive the primary mcell
 * location (for undelete detection).
 *
 * Returns 0 on success, -ENOENT if the tag cannot be located, or
 * -errno on failure.
 */
static int dir_resolve_extents(advfs_domain_t *domain,
                               const advfs_extent_t *fs_tag_extents,
                               int fs_tag_num_extents,
                               adv_bf_tag_t fs_tag, adv_bf_tag_t dir_tag,
                               advfs_extent_t *extents_out, int max_extents,
                               int *num_out,
                               uint32_t *prim_vd, uint32_t *prim_pg,
                               uint32_t *prim_cell)
{
    /*
     * Step 1: Look up dir_tag in the fileset's tag directory.
     *
     * Tag number determines the page and index within the tag dir:
     *   page  = tag_num / ADVFS_TAGS_PER_PG
     *   index = tag_num % ADVFS_TAGS_PER_PG
     */
    uint32_t tag_page = dir_tag.num / ADVFS_TAGS_PER_PG;
    uint32_t tag_idx = dir_tag.num % ADVFS_TAGS_PER_PG;

    uint32_t mcell_vd = 0;
    uint32_t bmt_pg_num = 0;
    uint32_t mcell_cell = 0;
    int have_mcell = 0;

    adv_tag_dir_pg_t tag_pg;
    int err = advfs_extents_read_data_dom(domain, fs_tag_extents,
                                          fs_tag_num_extents,
                                          tag_page, &tag_pg);
    if (err) {
        advfs_dbg("dir: failed to read tag dir page %u for tag %u "
                  "(err=%d), trying BMT scan fallback",
                  tag_page, dir_tag.num, err);
    } else {
        adv_tag_map_t *tm = &tag_pg.maps[tag_idx];
        if (ADV_TMAP_IN_USE(*tm) && tm->u.s3.seq_no != 0xFFFF) {
            adv_mcell_id_t mcid = tm->u.s3.mcid;
            mcell_vd = tm->u.s3.vd_index;
            bmt_pg_num = ADV_MCID_PAGE(mcid);
            mcell_cell = ADV_MCID_CELL(mcid);
            have_mcell = 1;

            advfs_dbg("dir: tag %u -> vd=%u, mcid=p%u.c%u",
                      dir_tag.num, mcell_vd,
                      bmt_pg_num, mcell_cell);
        } else {
            /*
             * Tag directory entry is dead or uninitialized (seq_no ==
             * 0xFFFF or not in use).
             */
            advfs_dbg("dir: tag %u not in use in tag dir (seq=%u), "
                      "trying BMT scan fallback",
                      dir_tag.num, tm->u.s3.seq_no);
        }
    }

    if (!have_mcell) {
        /*
         * Fall back to scanning BMT pages for an mcell with matching
         * bf_set_tag and tag number.
         */
        err = find_tag_by_bmt_scan(domain, fs_tag, dir_tag.num,
                                    &mcell_vd, &bmt_pg_num, &mcell_cell);
        if (err) {
            advfs_err("dir: tag %u not found via tag dir or BMT scan",
                      dir_tag.num);
            return -ENOENT;
        }

        advfs_dbg("dir: tag %u found via BMT scan at vd %u p%u.c%u",
                  dir_tag.num, mcell_vd, bmt_pg_num, mcell_cell);
    }

    if (prim_vd)   *prim_vd = mcell_vd;
    if (prim_pg)   *prim_pg = bmt_pg_num;
    if (prim_cell) *prim_cell = mcell_cell;

    /*
     * Step 2: Find the mcell containing BSR_XTNTS.
     *
     * For V3 (pre-ADVFS_FIRST_XTNT_IN_PRIM_MCELL_VERSION), BSR_XTNTS
     * may not be in the primary mcell. find_xtnts_mcell() checks the
     * primary first, then follows the chain.
     */
    uint32_t xtnt_vd, xtnt_pg, xtnt_cell;
    err = find_xtnts_mcell(domain, mcell_vd, bmt_pg_num, mcell_cell,
                           &xtnt_vd, &xtnt_pg, &xtnt_cell);
    if (err) {
        advfs_err("dir: failed to find BSR_XTNTS for tag %u", dir_tag.num);
        return err;
    }

    /*
     * Step 3: Read the directory's extent map.
     */
    err = advfs_extents_read_dom(domain, xtnt_vd, xtnt_pg, xtnt_cell,
                                 extents_out, max_extents, num_out);
    if (err) {
        advfs_err("dir: failed to read extents for tag %u", dir_tag.num);
        return err;
    }

    return 0;
}

/* Clone-aware directory walk (deleted entries optional). */
int advfs_dir_read_full_clone(advfs_volume_t *vol, advfs_domain_t *domain,
                              const advfs_extent_t *fs_tag_extents,
                              int fs_tag_num_extents,
                              adv_bf_tag_t fs_tag, adv_bf_tag_t dir_tag,
                              advfs_dir_entry_cb cb,
                              advfs_dir_entry_cb deleted_cb,
                              const advfs_clone_ctx_t *clone,
                              void *user_data)
{
    if (!vol || !domain || !fs_tag_extents || (!cb && !deleted_cb)) {
        return -EINVAL;
    }
    if (fs_tag_num_extents <= 0 || dir_tag.num == 0) {
        return -EINVAL;
    }

    advfs_dbg("dir: reading directory tag %u.%u", dir_tag.num, dir_tag.seq);

    /* Steps 1-3: resolve the (clone's) directory extent map. */
    advfs_extent_t dir_extents[ADVFS_MAX_EXTENTS];
    int num_dir_extents = 0;
    uint32_t prim_vd = 0, prim_pg = 0, prim_cell = 0;

    int err = dir_resolve_extents(domain, fs_tag_extents, fs_tag_num_extents,
                                  fs_tag, dir_tag, dir_extents,
                                  ADVFS_MAX_EXTENTS, &num_dir_extents,
                                  &prim_vd, &prim_pg, &prim_cell);
    if (err) {
        return err;
    }

    /*
     * Feature detection: does this directory have an undelete
     * (trashcan) directory attached? Detection only -- restoring
     * deleted files is not implemented.
     */
    (void)advfs_dir_has_undelete(vol, domain, prim_vd, prim_pg, prim_cell);

    /*
     * Clone fallback: a clone fileset's directory pages are usually
     * copy-on-write holes that must be read from the SAME directory
     * tag in the original fileset. Resolve the original's fileset tag
     * directory, then the original directory's extent map.
     */
    int clone_active = (clone && clone->is_clone &&
                        clone->orig_set_tag.num != 0);
    advfs_extent_t orig_dir_extents[ADVFS_MAX_EXTENTS];
    int orig_dir_num = 0;
    int orig_ok = 0;

    if (clone_active) {
        advfs_extent_t orig_fs_extents[ADVFS_MAX_EXTENTS];
        int orig_fs_num = 0;

        int e = advfs_fileset_tag_extents(vol, domain, clone->orig_set_tag,
                                          orig_fs_extents, ADVFS_MAX_EXTENTS,
                                          &orig_fs_num);
        if (!e && orig_fs_num > 0) {
            e = dir_resolve_extents(domain, orig_fs_extents, orig_fs_num,
                                    clone->orig_set_tag, dir_tag,
                                    orig_dir_extents, ADVFS_MAX_EXTENTS,
                                    &orig_dir_num, NULL, NULL, NULL);
            if (!e && orig_dir_num > 0) {
                orig_ok = 1;
            }
        }
        if (!orig_ok) {
            advfs_dbg("dir: clone tag %u: no original fallback available "
                      "(err=%d)", dir_tag.num, e);
        }
    }

    if (num_dir_extents == 0 && !orig_ok) {
        advfs_dbg("dir: tag %u has no extents (empty directory)",
                  dir_tag.num);
        return 0;
    }

    advfs_dbg("dir: tag %u has %d extent(s)%s", dir_tag.num, num_dir_extents,
              orig_ok ? " (+clone fallback)" : "");

    /*
     * Step 4: Determine how many data pages to read.
     *
     * The extent array preserves the terminator entry, whose bs_page
     * is the directory's exact page count. A clone's own extent map is
     * a copy-on-write stub -- empty (page_count 0, V4) or degenerate
     * (page_count 0xFFFFFFFF, V3) -- so when a fallback exists the
     * original fileset is the authority on the true page count.
     */
    uint32_t max_page;
    if (orig_ok) {
        uint32_t clone_pages = advfs_extents_page_count(dir_extents,
                                                        num_dir_extents);
        uint32_t orig_pages = advfs_extents_page_count(orig_dir_extents,
                                                       orig_dir_num);
        if (clone_pages > MAX_DIR_PAGES) {
            max_page = orig_pages;  /* clone map is a bogus stub (V3) */
        } else {
            max_page = (clone_pages > orig_pages) ? clone_pages : orig_pages;
        }
    } else {
        max_page = advfs_extents_page_count(dir_extents, num_dir_extents);
    }
    if (max_page > MAX_DIR_PAGES) {
        advfs_warn("dir: capping directory pages from %u to %u",
                   max_page, MAX_DIR_PAGES);
        max_page = MAX_DIR_PAGES;
    }

    advfs_dbg("dir: scanning %u page(s)", max_page);

    /*
     * Step 5: Read each data page and walk directory entries.
     *
     * Each 8 KB page contains 16 x 512-byte directory blocks.
     * Entries are variable-length and do not cross block boundaries.
     *
     * A clone page that is a permanent hole (copy-on-write, never
     * modified) or is absent from the clone's stub extent map is read
     * from the original fileset instead. A page that is a hole in both
     * (a nested clone, or genuinely sparse) is skipped.
     */
    uint8_t page_buf[ADVFS_PGSZ];
    uint32_t deleted_count = 0;

    for (uint32_t pg = 0; pg < max_page; pg++) {
        int have_page = 0;

        if (num_dir_extents > 0) {
            int perm_hole = 0;
            err = advfs_extents_read_data_dom_ex(domain, dir_extents,
                                                 num_dir_extents, pg,
                                                 page_buf, &perm_hole);
            if (err == 0 && !perm_hole) {
                have_page = 1;
            } else if (err && err != -ENOENT) {
                advfs_err("dir: failed to read page %u", pg);
                return err;
            }
        }

        if (!have_page && orig_ok) {
            int orig_hole = 0;
            int e = advfs_extents_read_data_dom_ex(domain, orig_dir_extents,
                                                   orig_dir_num, pg,
                                                   page_buf, &orig_hole);
            if (e == 0 && !orig_hole) {
                have_page = 1;
            } else if (e && e != -ENOENT) {
                advfs_err("dir: failed to read original page %u", pg);
                return e;
            }
        }

        if (!have_page) {
            advfs_dbg("dir: page %u not mapped (hole), skipping", pg);
            continue;
        }

        /* Walk 16 x 512-byte blocks per page */
        for (int blk = 0; blk < ADVFS_BLKS_PER_PG; blk++) {
            const uint8_t *block = page_buf + blk * ADVFS_BLKSZ;
            int stop = walk_dir_block(block, cb, deleted_cb, user_data,
                                      &deleted_count);
            if (stop) {
                if (deleted_count > 0) {
                    advfs_dbg("dir: %u deleted entries skipped "
                              "(partial walk)", deleted_count);
                }
                return 0;  /* callback requested stop */
            }
        }
    }

    if (deleted_count > 0) {
        advfs_dbg("dir: %u deleted entries skipped", deleted_count);
    }

    return 0;
}

/* Read all entries in a directory, optionally reporting deleted ones. */
int advfs_dir_read_full(advfs_volume_t *vol, advfs_domain_t *domain,
                        const advfs_extent_t *fs_tag_extents,
                        int fs_tag_num_extents,
                        adv_bf_tag_t fs_tag, adv_bf_tag_t dir_tag,
                        advfs_dir_entry_cb cb, advfs_dir_entry_cb deleted_cb,
                        void *user_data)
{
    return advfs_dir_read_full_clone(vol, domain, fs_tag_extents,
                                     fs_tag_num_extents, fs_tag, dir_tag,
                                     cb, deleted_cb, NULL, user_data);
}

/* Read all entries in a directory (deleted entries skipped). */
int advfs_dir_read(advfs_volume_t *vol, advfs_domain_t *domain,
                   const advfs_extent_t *fs_tag_extents, int fs_tag_num_extents,
                   adv_bf_tag_t fs_tag, adv_bf_tag_t dir_tag,
                   advfs_dir_entry_cb cb, void *user_data)
{
    if (!cb) {
        return -EINVAL;
    }
    return advfs_dir_read_full(vol, domain, fs_tag_extents,
                               fs_tag_num_extents, fs_tag, dir_tag,
                               cb, NULL, user_data);
}

/* Clone-aware variant of advfs_dir_read() (deleted entries skipped). */
int advfs_dir_read_clone(advfs_volume_t *vol, advfs_domain_t *domain,
                         const advfs_extent_t *fs_tag_extents,
                         int fs_tag_num_extents,
                         adv_bf_tag_t fs_tag, adv_bf_tag_t dir_tag,
                         advfs_dir_entry_cb cb,
                         const advfs_clone_ctx_t *clone, void *user_data)
{
    if (!cb) {
        return -EINVAL;
    }
    return advfs_dir_read_full_clone(vol, domain, fs_tag_extents,
                                     fs_tag_num_extents, fs_tag, dir_tag,
                                     cb, NULL, clone, user_data);
}
