/*
 * extents.h -- extent map resolution for advfs-fuse
 *
 * Reads the extent chain for a bitfile (primary BSR_XTNTS plus
 * overflow BSR_XTRA_XTNTS mcells) and resolves bitfile page
 * numbers to physical disk blocks.
 *
 * Copyright (C) 2026 Armas Spann
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef ADVFS_EXTENTS_H
#define ADVFS_EXTENTS_H

#include "ods.h"
#include "volume.h"
#include "domain.h"

/* Maximum extents we collect from a single mcell chain.
 * Production Tru64 systems (30+ year uptime, no defrag) routinely
 * exceed 4096 extents -- frag files and large C-ISAM databases on
 * production volumes reached 5000+ extents per file.
 * 32768 entries = ~384 KB per stack array; worst-case ~1.5 MB total
 * per FUSE read call, well within the default 8 MB thread stack. */
#define ADVFS_MAX_EXTENTS  32768

/* One resolved extent descriptor: bitfile page -> disk block.
 * vd_index selects the domain volume holding the blocks (1-based);
 * 0 means "the default volume" (single-volume compatibility). */
typedef struct {
    uint32_t bs_page;   /* starting bitfile page */
    uint32_t vd_blk;    /* starting disk block (XTNT_TERM = end marker) */
    uint32_t vd_index;  /* volume holding the blocks (0 = default) */
} advfs_extent_t;

/*
 * Read the extent map for the mcell at (bmt_vol_page, cell_idx).
 *
 * Reads BSR_XTNTS (type=1) from the given mcell, then follows the
 * chain_mcid/chain_vd_index to read BSR_XTRA_XTNTS (type=5) overflow
 * records. Produces a flat array of extent descriptors.
 *
 * The array preserves boundary entries (vd_blk == ADVFS_XTNT_TERM or
 * ADVFS_PERM_HOLE): a boundary followed by another entry is a sparse
 * hole; the final TERM entry's bs_page is the bitfile's page count.
 *
 * bmt_map:       BMT extent map (from domain->bmt_map). Maps BMT page
 *                numbers to disk block numbers. Used to resolve chain
 *                mcell BMT page references to volume pages.
 * bmt_map_count: number of valid entries in bmt_map.
 * bmt_vol_page:  absolute volume page of the BMT page containing the mcell.
 * cell_idx:      cell index within that BMT page.
 * extents_out:   caller-provided array of at least max_extents entries.
 * max_extents:   capacity of extents_out.
 * count_out:     on success, set to the number of valid entries written.
 *
 * Returns 0 on success, -errno on failure.
 */
int advfs_extents_read(advfs_volume_t *vol,
                       const adv_xtnt_t *bmt_map, int bmt_map_count,
                       uint32_t bmt_vol_page, uint32_t cell_idx,
                       advfs_extent_t *extents_out, int max_extents,
                       int *count_out);

/*
 * Domain-aware variant of advfs_extents_read() for multi-volume
 * domains.
 *
 * The primary mcell lives at BMT-internal page bmt_pg_num, cell
 * cell_idx, on the volume registered under vd_index (0 = primary).
 * Chain mcells may live on OTHER volumes (chain_vd_index in
 * BSR_XTNTS, next_vd_index in each chain mcell); they are resolved
 * through the owning volume's own BMT map. Every produced extent is
 * tagged with the vd_index of the volume holding its blocks (extents
 * live on the same volume as the mcell whose record describes them).
 *
 * BSXMT_STRIPE bitfiles are interleaved: each BSR_SHADOW_XTNTS
 * record in the chain starts a new stripe column, and file pages
 * round-robin across columns in runs of segment_size pages. The
 * returned map is flat (file-page-relative), ready for
 * advfs_extents_read_data_dom().
 *
 * Returns 0 on success, -errno on failure.
 */
int advfs_extents_read_dom(advfs_domain_t *domain, uint32_t vd_index,
                           uint32_t bmt_pg_num, uint32_t cell_idx,
                           advfs_extent_t *extents_out, int max_extents,
                           int *count_out);

/*
 * Read a specific bitfile page by resolving it through the extent map.
 *
 * Given the extent array from advfs_extents_read(), maps bitfile
 * page_num to a physical disk block and reads ADVFS_PGSZ bytes
 * into buf.
 *
 * Pages inside a sparse hole read as zeros.
 *
 * Returns 0 on success, -ENOENT if the page is not in the extent map,
 * or -errno on I/O failure.
 */
int advfs_extents_read_data(advfs_volume_t *vol,
                            const advfs_extent_t *extents, int num_extents,
                            uint32_t page_num, void *buf);

/*
 * Domain-aware variant of advfs_extents_read_data(): the disk read
 * goes to the volume named by the matched extent's vd_index (0 =
 * primary volume). Returns -ENODEV if that volume is not attached.
 */
int advfs_extents_read_data_dom(const advfs_domain_t *domain,
                                const advfs_extent_t *extents,
                                int num_extents,
                                uint32_t page_num, void *buf);

/*
 * Domain-aware page read that reports permanent (copy-on-write) holes.
 *
 * Behaves like advfs_extents_read_data_dom(), except that when the
 * page maps to a permanent hole (ADVFS_PERM_HOLE -- a clone page that
 * was never copied) the buffer is zero-filled, *perm_hole_out is set
 * to 1, and NO warning is logged. For every other outcome
 * *perm_hole_out is set to 0. This lets the file-data reader fall back
 * to the original fileset for such pages instead of returning zeros.
 *
 * perm_hole_out must be non-NULL. Returns 0 on success (including the
 * permanent-hole case), -ENOENT if the page is beyond the extent map,
 * or -errno on I/O failure.
 */
int advfs_extents_read_data_dom_ex(const advfs_domain_t *domain,
                                   const advfs_extent_t *extents,
                                   int num_extents,
                                   uint32_t page_num, void *buf,
                                   int *perm_hole_out);

/*
 * Build a flat file-page-relative extent map for a striped bitfile
 * from its per-column extent maps.
 *
 * raw holds all column extents back to back; column c occupies
 * raw[col_start[c] .. col_start[c+1]) with col_start[ncols] == raw_cnt.
 * File pages round-robin across the ncols columns in runs of
 * segment_size pages (Tru64 BFPAGE_TO_XMPAGE mapping): file segment
 * s maps to column (s % ncols), column-relative pages
 * [(s / ncols) * segment_size ...).
 *
 * Exposed separately so the interleave math is unit-testable.
 * Returns 0 on success, -errno on failure.
 */
int advfs_extents_stripe_interleave(const advfs_extent_t *raw,
                                    const int *col_start, int ncols,
                                    uint32_t segment_size,
                                    advfs_extent_t *extents_out,
                                    int max_extents, int *count_out);

/*
 * Compute the bitfile page count from an extent array.
 *
 * Uses the final terminator entry's bs_page when present (exact);
 * otherwise falls back to the highest mapped extent start plus one
 * (lower bound, e.g. for truncated maps).
 */
uint32_t advfs_extents_page_count(const advfs_extent_t *extents,
                                  int num_extents);

#endif /* ADVFS_EXTENTS_H */
