/*
 * extents.c -- extent map resolution for advfs-fuse
 *
 * Reads BSR_XTNTS (type=1) from a primary mcell and follows the
 * chain to BSR_XTRA_XTNTS (type=5) or BSR_SHADOW_XTNTS (type=6)
 * overflow mcells to build a flat extent map. Resolves bitfile
 * page numbers to disk blocks.
 *
 * Version differences (ADVFS_FIRST_XTNT_IN_PRIM_MCELL_VERSION = 4):
 *
 *   V3: for non-reserved files the primary BSR_XTNTS first_xtnt
 *       entries are stubs; the real descriptors live in chained
 *       BSR_SHADOW_XTNTS (type=6) mcells. The stubs are discarded
 *       when the first shadow record is encountered.
 *
 *   V4: the primary BSR_XTNTS first_xtnt entries are real extent
 *       descriptors; overflow continues in chained BSR_XTRA_XTNTS
 *       (type=5) mcells. No shadow records appear, so no version
 *       flag is needed -- trying XTRA first and falling back to
 *       shadow handles both layouts.
 *
 * Multi-volume domains: mcell chains may cross volumes. The first
 * hop is named by BSR_XTNTS chain_vd_index; later hops by each
 * mcell's next_vd_index (0 = same volume). Extent descriptors in an
 * mcell always describe storage on the volume holding that mcell,
 * so every collected extent is tagged with the mcell's vd_index.
 * The legacy single-volume entry points keep their old behavior
 * (everything resolved against one volume, vd tags left 0).
 *
 * Striped bitfiles (BSXMT_STRIPE): each BSR_SHADOW_XTNTS record in
 * the chain starts a new stripe column; BSR_XTRA_XTNTS records
 * continue the current column. File pages round-robin across the
 * columns in runs of segment_size pages; the per-column maps are
 * flattened into a file-page-relative map by
 * advfs_extents_stripe_interleave().
 *
 * Copyright (C) 2026 Armas Spann
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _XOPEN_SOURCE 500  /* pread() */

#include "extents.h"
#include "bmt.h"
#include "util.h"

#include <errno.h>
#include <string.h>

/* Maximum chain hops to prevent infinite loops on corrupt metadata. */
#define MAX_CHAIN_HOPS  64

/* Maximum stripe columns we track (>= any sane stripe width). */
#define MAX_STRIPE_COLS  16

/*
 * Metadata source for the chain walk: either a full domain context
 * (multi-volume, vd-aware) or a single volume plus its BMT map
 * (legacy mode -- vd indices are ignored and extents are tagged 0).
 */
typedef struct {
    advfs_domain_t   *domain;        /* NULL = legacy single-volume mode */
    advfs_volume_t   *vol;           /* legacy: the only volume */
    const adv_xtnt_t *bmt_map;       /* legacy: its BMT extent map */
    int               bmt_map_count;
} xtnt_src_t;

/* Read BMT-internal page bmt_pg_num of volume vd through the source. */
static int src_read_bmt_page(const xtnt_src_t *src, uint32_t vd,
                             uint32_t bmt_pg_num, adv_bmt_pg_t *pg)
{
    if (src->domain) {
        return advfs_domain_read_bmt_page_vd(src->domain, vd,
                                             bmt_pg_num, pg);
    }

    uint32_t vol_page;
    int err = advfs_bmt_resolve_page(src->bmt_map, src->bmt_map_count,
                                     bmt_pg_num, &vol_page);
    if (err) {
        advfs_err("extents: BMT page %u not in extent map", bmt_pg_num);
        return err;
    }
    return advfs_bmt_read_page(src->vol, vol_page, pg);
}

/* Append one extent descriptor to the output array. */
static int append_extent(advfs_extent_t *out, int *count, int max,
                         uint32_t bs_page, uint32_t vd_blk, uint32_t vd)
{
    if (*count >= max) {
        advfs_err("extents: overflow, more than %d extents", max);
        return -EOVERFLOW;
    }
    out[*count].bs_page = bs_page;
    out[*count].vd_blk = vd_blk;
    out[*count].vd_index = vd;
    (*count)++;
    return 0;
}

/*
 * Collect extent descriptors from a raw descriptor array.
 *
 * ALL entries are kept, including those with vd_blk == ADVFS_XTNT_TERM
 * or ADVFS_PERM_HOLE. A TERM/HOLE entry followed by another entry is a
 * hole boundary (the pages up to the next entry's bs_page are sparse);
 * the final TERM entry marks the end of the map and its bs_page is the
 * bitfile's total page count. Dropping these entries would lose the
 * file's end boundary and misplace all extents after a sparse hole.
 */
static int collect_array(const adv_xtnt_t *xtnts, uint16_t x_cnt,
                         uint16_t x_max, advfs_extent_t *out,
                         int *count, int max, uint32_t vd)
{
    if (x_cnt > x_max) {
        x_cnt = x_max;
    }

    for (uint16_t i = 0; i < x_cnt; i++) {
        int err = append_extent(out, count, max,
                                xtnts[i].bs_page, xtnts[i].vd_blk, vd);
        if (err) {
            return err;
        }
    }
    return 0;
}

/*
 * Core extent-chain walk. prim_pg is the already-read BMT page
 * holding the primary mcell at cell_idx; prim_vd is the vd_index of
 * the volume it came from (0 in legacy mode).
 */
static int extents_read_core(const xtnt_src_t *src, uint32_t prim_vd,
                             adv_bmt_pg_t *prim_pg, uint32_t cell_idx,
                             advfs_extent_t *extents_out, int max_extents,
                             int *count_out)
{
    *count_out = 0;

    adv_mcell_t *mc = advfs_bmt_get_mcell(prim_pg, cell_idx);
    if (!mc) {
        advfs_err("extents: invalid cell %u on BMT page", cell_idx);
        return -EINVAL;
    }

    /* Find BSR_XTNTS (type=1) in the primary mcell */
    adv_rec_hdr_t rhdr;
    adv_xtnt_rec_t *xr = advfs_bmt_find_rec(mc, ADVFS_BSR_XTNTS, &rhdr);
    if (!xr) {
        advfs_err("extents: no BSR_XTNTS in cell %u", cell_idx);
        return -ENODATA;
    }

    if (ADV_REC_DATA_SZ(rhdr) < sizeof(adv_xtnt_rec_t)) {
        advfs_err("extents: BSR_XTNTS too small (%u < %zu)",
                  ADV_REC_DATA_SZ(rhdr), sizeof(adv_xtnt_rec_t));
        return -EILSEQ;
    }

    advfs_dbg("extents: BSR_XTNTS in cell %u: type=%u, "
              "chain_vd=%u, chain_mcid=p%u.c%u, x_cnt=%u",
              cell_idx, xr->type,
              xr->chain_vd_index,
              ADV_MCID_PAGE(xr->chain_mcid),
              ADV_MCID_CELL(xr->chain_mcid),
              xr->first_xtnt.x_cnt);

    int is_stripe = (xr->type == ADVFS_BSXMT_STRIPE);
    uint32_t seg_sz = xr->segment_size;

    /*
     * Striping in legacy (single-volume) mode: stripe interleave
     * needs all column volumes attached, which the legacy entry
     * point cannot provide. Fall back to reading the columns
     * sequentially, which returns data in the wrong order for a
     * genuinely striped file (but does not crash).
     */
    if (is_stripe && !src->domain) {
        advfs_warn("extents: striped file detected (segment_size=%u "
                   "pages) -- stripe interleave not implemented, "
                   "reading sequential only", seg_sz);
        is_stripe = 0;
    }

    /*
     * Collect all extents into a scratch array, remembering where
     * each shadow record started (stripe column boundaries).
     */
    advfs_extent_t raw[ADVFS_MAX_EXTENTS];
    int raw_cnt = 0;
    int col_start[MAX_STRIPE_COLS + 1];
    int ncols = 0;

    int err = collect_array(xr->first_xtnt.xtnts, xr->first_xtnt.x_cnt,
                            ADVFS_BMT_XTNTS, raw, &raw_cnt,
                            ADVFS_MAX_EXTENTS, prim_vd);
    if (err) {
        return err;
    }

    /* Follow chain to overflow mcells. The first hop's volume comes
     * from chain_vd_index; later hops from each mcell's
     * next_vd_index (0 = stay on the current volume). Volume hints
     * are only honored in domain mode. */
    adv_mcell_id_t chain_mcid = xr->chain_mcid;
    uint32_t chain_vd = prim_vd;
    if (src->domain && xr->chain_vd_index != 0) {
        chain_vd = xr->chain_vd_index;
    }
    int hops = 0;
    int found_shadow = 0;

    while (chain_mcid.raw != 0 && hops < MAX_CHAIN_HOPS) {
        uint32_t chain_bmt_pg_num = ADV_MCID_PAGE(chain_mcid);
        uint32_t chain_cell = ADV_MCID_CELL(chain_mcid);

        advfs_dbg("extents: chain hop %d -> vd %u BMT page %u, cell %u",
                  hops, chain_vd, chain_bmt_pg_num, chain_cell);

        adv_bmt_pg_t chain_pg;
        err = src_read_bmt_page(src, chain_vd, chain_bmt_pg_num,
                                &chain_pg);
        if (err) {
            advfs_err("extents: failed to read chain BMT page %u "
                      "(vd %u)", chain_bmt_pg_num, chain_vd);
            return err;
        }

        adv_mcell_t *chain_mc = advfs_bmt_get_mcell(&chain_pg, chain_cell);
        if (!chain_mc) {
            advfs_err("extents: invalid chain cell %u on BMT page %u",
                      chain_cell, chain_bmt_pg_num);
            return -EILSEQ;
        }

        /* Look for BSR_XTRA_XTNTS (type=5) first -- the V4 overflow
         * record (also used for reserved-file overflow on V3) */
        adv_rec_hdr_t xhdr;
        adv_xtra_xtnt_rec_t *xtra = advfs_bmt_find_rec(chain_mc,
            ADVFS_BSR_XTRA_XTNTS, &xhdr);
        if (xtra) {
            if (ADV_REC_DATA_SZ(xhdr) < 4) {
                advfs_err("extents: BSR_XTRA_XTNTS too small (%u)",
                          ADV_REC_DATA_SZ(xhdr));
                return -EILSEQ;
            }

            advfs_dbg("extents: BSR_XTRA_XTNTS in cell %u: x_cnt=%u",
                      chain_cell, xtra->x_cnt);

            err = collect_array(xtra->xtnts, xtra->x_cnt,
                                ADVFS_BMT_XTRA_XTNTS, raw, &raw_cnt,
                                ADVFS_MAX_EXTENTS, chain_vd);
            if (err) {
                return err;
            }
        } else {
            /* Try BSR_SHADOW_XTNTS (type=6) -- used in ODS V3 */
            adv_rec_hdr_t shdr;
            adv_shadow_xtnt_rec_t *shadow = advfs_bmt_find_rec(chain_mc,
                ADVFS_BSR_SHADOW_XTNTS, &shdr);
            if (!shadow) {
                advfs_dbg("extents: no BSR_XTRA_XTNTS or "
                          "BSR_SHADOW_XTNTS in chain cell %u, stopping",
                          chain_cell);
                break;
            }

            if (ADV_REC_DATA_SZ(shdr) < 8) {
                advfs_err("extents: BSR_SHADOW_XTNTS too small (%u)",
                          ADV_REC_DATA_SZ(shdr));
                return -EILSEQ;
            }

            /*
             * In ODS V3, BSR_SHADOW_XTNTS holds the real extent
             * descriptors. The primary BSR_XTNTS first_xtnt entries
             * are stubs for non-reserved files (they contain range
             * metadata, not actual block mappings). Discard any
             * primary extents on the first shadow encounter.
             */
            if (!found_shadow) {
                advfs_dbg("extents: discarding %d primary stub extent(s) "
                          "in favor of shadow data", raw_cnt);
                raw_cnt = 0;
                found_shadow = 1;
            }

            /* Each shadow record starts a stripe column. */
            if (ncols >= MAX_STRIPE_COLS) {
                if (is_stripe) {
                    advfs_err("extents: more than %d stripe columns",
                              MAX_STRIPE_COLS);
                    return -EILSEQ;
                }
                /* APPEND files just chain many shadows; boundary
                 * tracking is irrelevant, stop recording them. */
            } else {
                col_start[ncols++] = raw_cnt;
            }

            advfs_dbg("extents: BSR_SHADOW_XTNTS in cell %u: x_cnt=%u, "
                      "alloc_vd=%u (column %d)",
                      chain_cell, shadow->x_cnt, shadow->alloc_vd_index,
                      ncols - 1);

            err = collect_array(shadow->xtnts, shadow->x_cnt,
                                ADVFS_BMT_SHADOW_XTNTS, raw, &raw_cnt,
                                ADVFS_MAX_EXTENTS, chain_vd);
            if (err) {
                return err;
            }
        }

        /* Follow the mcell's own chain link for further overflow */
        if (src->domain && chain_mc->next_vd_index != 0) {
            chain_vd = chain_mc->next_vd_index;
        }
        chain_mcid = chain_mc->next_mcid;
        hops++;
    }

    /* Striped bitfile: interleave the columns. */
    if (is_stripe) {
        if (ncols >= 2) {
            col_start[ncols] = raw_cnt;
            if (seg_sz == 0) {
                advfs_err("extents: striped file with segment_size 0");
                return -EILSEQ;
            }
            err = advfs_extents_stripe_interleave(raw, col_start, ncols,
                                                  seg_sz, extents_out,
                                                  max_extents, count_out);
            if (err) {
                return err;
            }
            advfs_dbg("extents: striped bitfile: %d column(s), "
                      "segment_size=%u, %d interleaved extent(s)",
                      ncols, seg_sz, *count_out);
            return 0;
        }
        /* 0 or 1 column: nothing to interleave; sequential IS the
         * correct order. */
        advfs_dbg("extents: striped bitfile with %d column(s), "
                  "no interleave needed", ncols);
    }

    /* APPEND (or degenerate stripe): flat concatenation. */
    for (int i = 0; i < raw_cnt; i++) {
        err = append_extent(extents_out, count_out, max_extents,
                            raw[i].bs_page, raw[i].vd_blk,
                            raw[i].vd_index);
        if (err) {
            return err;
        }
    }

    advfs_dbg("extents: collected %d extent(s) total", *count_out);
    return 0;
}

/* Read the full extent map for an mcell (legacy single-volume API). */
int advfs_extents_read(advfs_volume_t *vol,
                       const adv_xtnt_t *bmt_map, int bmt_map_count,
                       uint32_t bmt_vol_page, uint32_t cell_idx,
                       advfs_extent_t *extents_out, int max_extents,
                       int *count_out)
{
    if (!vol || !extents_out || !count_out || max_extents <= 0) {
        return -EINVAL;
    }

    /* Read the BMT page containing the primary mcell */
    adv_bmt_pg_t bmt_pg;
    int err = advfs_bmt_read_page(vol, bmt_vol_page, &bmt_pg);
    if (err) {
        advfs_err("extents: failed to read BMT page %u", bmt_vol_page);
        return err;
    }

    xtnt_src_t src = {
        .domain = NULL,
        .vol = vol,
        .bmt_map = bmt_map,
        .bmt_map_count = bmt_map_count,
    };
    return extents_read_core(&src, 0, &bmt_pg, cell_idx,
                             extents_out, max_extents, count_out);
}

/* Read the full extent map for an mcell (multi-volume API). */
int advfs_extents_read_dom(advfs_domain_t *domain, uint32_t vd_index,
                           uint32_t bmt_pg_num, uint32_t cell_idx,
                           advfs_extent_t *extents_out, int max_extents,
                           int *count_out)
{
    if (!domain || !extents_out || !count_out || max_extents <= 0) {
        return -EINVAL;
    }

    adv_bmt_pg_t bmt_pg;
    int err = advfs_domain_read_bmt_page_vd(domain, vd_index,
                                            bmt_pg_num, &bmt_pg);
    if (err) {
        return err;
    }

    /* Normalize the "default volume" alias to the real vd_index so
     * extent tags always name a concrete volume. */
    uint32_t prim_vd = vd_index;
    if (prim_vd == 0 && domain->vd_count > 0) {
        prim_vd = domain->vds[0].vd_index;
    }

    xtnt_src_t src = {
        .domain = domain,
        .vol = NULL,
        .bmt_map = NULL,
        .bmt_map_count = 0,
    };
    return extents_read_core(&src, prim_vd, &bmt_pg, cell_idx,
                             extents_out, max_extents, count_out);
}

/*
 * Column-map lookup for the stripe interleaver: resolve
 * column-relative page xm to a disk block (or hole marker) plus the
 * number of contiguous pages remaining in the same extent/hole.
 */
static void col_lookup(const advfs_extent_t *ce, int cn, uint32_t xm,
                       uint32_t *blk_out, uint32_t *vd_out,
                       uint32_t *run_out)
{
    for (int i = 0; i < cn; i++) {
        if (xm < ce[i].bs_page) {
            continue;
        }
        uint32_t end = (i + 1 < cn) ? ce[i + 1].bs_page : UINT32_MAX;
        if (xm >= end) {
            continue;
        }

        *vd_out = ce[i].vd_index;
        *run_out = end - xm;

        if (ce[i].vd_blk == ADVFS_XTNT_TERM ||
            ce[i].vd_blk == ADVFS_PERM_HOLE) {
            *blk_out = ce[i].vd_blk;
            return;
        }

        uint64_t blk64 = (uint64_t)ce[i].vd_blk +
                         (uint64_t)(xm - ce[i].bs_page) *
                         ADVFS_BLKS_PER_PG;
        if (blk64 > UINT32_MAX) {
            advfs_warn("extents: stripe column page %u maps past "
                       "addressable blocks, treating as hole", xm);
            *blk_out = ADVFS_XTNT_TERM;
            return;
        }
        *blk_out = (uint32_t)blk64;
        return;
    }

    /* Beyond the column map: unmapped hole to end. */
    *blk_out = ADVFS_XTNT_TERM;
    *vd_out = 0;
    *run_out = UINT32_MAX;
}

/* Build a flat file-relative map for a striped bitfile. */
int advfs_extents_stripe_interleave(const advfs_extent_t *raw,
                                    const int *col_start, int ncols,
                                    uint32_t segment_size,
                                    advfs_extent_t *extents_out,
                                    int max_extents, int *count_out)
{
    if (!raw || !col_start || ncols < 1 || ncols > MAX_STRIPE_COLS ||
        segment_size == 0 || !extents_out || !count_out ||
        max_extents <= 0) {
        return -EINVAL;
    }

    *count_out = 0;

    /* Total file pages = sum of per-column page counts. */
    uint64_t total64 = 0;
    for (int c = 0; c < ncols; c++) {
        total64 += advfs_extents_page_count(raw + col_start[c],
                                            col_start[c + 1] -
                                            col_start[c]);
    }
    if (total64 > UINT32_MAX) {
        return -ERANGE;
    }
    uint32_t total = (uint32_t)total64;

    /*
     * Walk file pages, mapping each run to its stripe column
     * (Tru64 BFPAGE_TO_XMPAGE): file segment s = fpage/segment_size
     * lives on column s % ncols at column-relative page
     * (s / ncols) * segment_size + (fpage % segment_size).
     * Emit one extent per contiguous run, merging where possible.
     */
    uint32_t fpage = 0;
    while (fpage < total) {
        uint32_t seg = fpage / segment_size;
        uint32_t in_seg = fpage % segment_size;
        int col = (int)(seg % (uint32_t)ncols);
        uint32_t xm = (seg / (uint32_t)ncols) * segment_size + in_seg;

        uint32_t blk, vd, run;
        col_lookup(raw + col_start[col],
                   col_start[col + 1] - col_start[col], xm,
                   &blk, &vd, &run);

        uint32_t n = segment_size - in_seg;    /* rest of this segment */
        if (run < n) {
            n = run;
        }
        if (total - fpage < n) {
            n = total - fpage;
        }
        if (n == 0) {
            n = 1;    /* safety: always make progress */
        }

        /* Merge with the previous entry when contiguous. */
        int merged = 0;
        if (*count_out > 0) {
            advfs_extent_t *prev = &extents_out[*count_out - 1];
            int prev_hole = (prev->vd_blk == ADVFS_XTNT_TERM ||
                             prev->vd_blk == ADVFS_PERM_HOLE);
            int cur_hole = (blk == ADVFS_XTNT_TERM ||
                            blk == ADVFS_PERM_HOLE);
            if (prev_hole && cur_hole && prev->vd_blk == blk) {
                merged = 1;    /* hole continues */
            } else if (!prev_hole && !cur_hole &&
                       prev->vd_index == vd &&
                       (uint64_t)prev->vd_blk +
                       (uint64_t)(fpage - prev->bs_page) *
                       ADVFS_BLKS_PER_PG == blk) {
                merged = 1;    /* physically contiguous mapped run */
            }
        }

        if (!merged) {
            int err = append_extent(extents_out, count_out, max_extents,
                                    fpage, blk, vd);
            if (err) {
                return err;
            }
        }

        fpage += n;
    }

    /* Final terminator: bs_page == total page count. */
    return append_extent(extents_out, count_out, max_extents,
                         total, ADVFS_XTNT_TERM, 0);
}

/*
 * Shared page-read logic. When domain is non-NULL the read goes to
 * the volume named by the matched extent's vd_index; otherwise to
 * the caller-supplied single volume.
 *
 * perm_hole_out (optional): when non-NULL, a permanent hole (PERM_HOLE)
 * page is reported through *perm_hole_out = 1 and the buffer is
 * zero-filled WITHOUT a warning -- the caller is expected to supply the
 * real data via clone fallback. When NULL, a permanent hole keeps the
 * legacy behavior (warn, then zero-fill), so callers that predate clone
 * fallback are unaffected.
 */
static int read_data_core(advfs_volume_t *legacy_vol,
                          const advfs_domain_t *domain,
                          const advfs_extent_t *extents, int num_extents,
                          uint32_t page_num, void *buf, int *perm_hole_out)
{
    if (perm_hole_out) {
        *perm_hole_out = 0;
    }

    if (!extents || !buf || num_extents <= 0) {
        return -EINVAL;
    }

    /*
     * Find the extent that contains page_num.
     *
     * Extents are sorted by bs_page. Entry i describes the pages
     * [bs_page[i], bs_page[i+1]). An entry with vd_blk == XTNT_TERM
     * or PERM_HOLE that is followed by another entry is a sparse
     * hole; the final TERM entry is the end-of-map boundary (its
     * bs_page is the bitfile's page count) and covers no pages.
     * Boundary duplicates from record chaining produce empty ranges
     * [p, p), which match no page and are skipped naturally.
     */
    int found = -1;

    for (int i = 0; i < num_extents; i++) {
        if (page_num < extents[i].bs_page) {
            continue;
        }
        if (i + 1 < num_extents) {
            if (page_num < extents[i + 1].bs_page) {
                found = i;
                break;
            }
        } else {
            /*
             * Last entry. A trailing TERM/HOLE is the end-of-map
             * boundary: page_num is beyond the end of the bitfile.
             * A trailing mapped extent (no terminator collected,
             * e.g. truncated map) is treated as an unbounded run.
             */
            if (extents[i].vd_blk == ADVFS_XTNT_TERM ||
                extents[i].vd_blk == ADVFS_PERM_HOLE) {
                break;
            }
            found = i;
            break;
        }
    }

    if (found < 0) {
        advfs_dbg("extents: page %u not in extent map (%d extents)",
                  page_num, num_extents);
        return -ENOENT;
    }

    /*
     * Permanent hole: this page belongs to a clone fileset and was
     * never copy-on-write'd -- the real data lives in the ORIGINAL
     * fileset's extent map. The page is always zero-filled here; a
     * caller that passed perm_hole_out is told about the hole so it can
     * overlay the original fileset's data (clone fallback). Callers
     * that pass NULL get the legacy warn-and-zero-fill behavior.
     *
     * Two on-disk encodings mean "clone hole, read from original":
     *   - ADVFS_PERM_HOLE (-2): ODS V4 permanent-hole marker.
     *   - vd_blk == 0: ODS V3 clone stub. A copy-on-write file/dir that
     *     was never modified in the clone carries a stub BSR_XTNTS
     *     (x_cnt == 0xFFFF, no BSR_SHADOW_XTNTS) whose sole descriptor
     *     maps to disk block 0. Block 0 is the volume's reserved boot
     *     area and is never valid bitfile data, so a data extent that
     *     resolves to block 0 is unambiguously a clone stub, not real
     *     storage. Treat it exactly like a permanent hole.
     */
    if (extents[found].vd_blk == ADVFS_PERM_HOLE ||
        extents[found].vd_blk == 0) {
        memset(buf, 0, ADVFS_PGSZ);
        if (perm_hole_out) {
            *perm_hole_out = 1;
        } else {
            advfs_warn("extents: page %u is a clone permanent hole -- "
                       "clone fallback not available, zero-filling",
                       page_num);
        }
        return 0;
    }

    /* Sparse hole: the page is allocated no storage -- read as zeros */
    if (extents[found].vd_blk == ADVFS_XTNT_TERM) {
        advfs_dbg("extents: page %u is in a hole, zero-filling", page_num);
        memset(buf, 0, ADVFS_PGSZ);
        return 0;
    }

    /* Pick the volume holding this extent's blocks. */
    advfs_volume_t *vol = legacy_vol;
    if (domain) {
        vol = advfs_domain_vd_vol(domain, extents[found].vd_index);
        if (!vol) {
            advfs_warn("extents: page %u lives on vd %u, which is "
                       "not attached -- pass all domain volumes",
                       page_num, extents[found].vd_index);
            return -ENODEV;
        }
    }
    if (!vol) {
        return -EINVAL;
    }

    /* Calculate the disk block for this page. Use 64-bit math so a
     * corrupt extent map cannot silently wrap around and read the
     * wrong volume page. */
    uint32_t page_offset = page_num - extents[found].bs_page;
    uint64_t disk_block64 = (uint64_t)extents[found].vd_blk +
                            (uint64_t)page_offset * ADVFS_BLKS_PER_PG;
    if (disk_block64 > UINT32_MAX) {
        advfs_err("extents: page %u maps past addressable blocks "
                  "(extent %d, vd_blk=%u)", page_num, found,
                  extents[found].vd_blk);
        return -ERANGE;
    }
    uint32_t disk_block = (uint32_t)disk_block64;

    /* Convert disk block to volume page and read */
    uint32_t vol_page = advfs_block_to_page(disk_block);

    advfs_dbg("extents: page %u -> extent %d (bs_page=%u, vd_blk=%u, "
              "vd=%u) -> disk block %u -> vol page %u",
              page_num, found, extents[found].bs_page,
              extents[found].vd_blk, extents[found].vd_index,
              disk_block, vol_page);

    return advfs_volume_read_page(vol, vol_page, buf);
}

/* Read a bitfile page by resolving through the extent map. */
int advfs_extents_read_data(advfs_volume_t *vol,
                            const advfs_extent_t *extents, int num_extents,
                            uint32_t page_num, void *buf)
{
    if (!vol) {
        return -EINVAL;
    }
    return read_data_core(vol, NULL, extents, num_extents, page_num, buf,
                          NULL);
}

/* Read a bitfile page, dispatching to the extent's volume. */
int advfs_extents_read_data_dom(const advfs_domain_t *domain,
                                const advfs_extent_t *extents,
                                int num_extents,
                                uint32_t page_num, void *buf)
{
    if (!domain) {
        return -EINVAL;
    }
    return read_data_core(NULL, domain, extents, num_extents,
                          page_num, buf, NULL);
}

/*
 * Like advfs_extents_read_data_dom(), but reports whether the page was
 * a permanent (copy-on-write) hole through *perm_hole_out. On a
 * permanent hole the buffer is zero-filled and *perm_hole_out is set to
 * 1 without emitting a warning, so the caller can substitute the
 * original fileset's data (clone fallback).
 */
int advfs_extents_read_data_dom_ex(const advfs_domain_t *domain,
                                   const advfs_extent_t *extents,
                                   int num_extents,
                                   uint32_t page_num, void *buf,
                                   int *perm_hole_out)
{
    if (!domain) {
        return -EINVAL;
    }
    return read_data_core(NULL, domain, extents, num_extents,
                          page_num, buf, perm_hole_out);
}

/* Compute the bitfile page count from an extent array. */
uint32_t advfs_extents_page_count(const advfs_extent_t *extents,
                                  int num_extents)
{
    uint32_t max_page = 0;

    if (!extents) {
        return 0;
    }

    for (int i = 0; i < num_extents; i++) {
        uint32_t cand;
        if (extents[i].vd_blk == ADVFS_XTNT_TERM ||
            extents[i].vd_blk == ADVFS_PERM_HOLE) {
            /* Boundary entry: bs_page is the exact end of the
             * preceding run (for the final TERM: the page count). */
            cand = extents[i].bs_page;
        } else {
            /* Mapped run: covers at least one page. */
            cand = extents[i].bs_page + 1;
        }
        if (cand > max_page) {
            max_page = cand;
        }
    }

    return max_page;
}
