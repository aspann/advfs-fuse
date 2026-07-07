/*
 * util.h -- shared helpers for advfs-fuse
 *
 * Error logging, page/block math, endian conversion.
 *
 * Copyright (C) 2026 Armas Spann
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef ADVFS_UTIL_H
#define ADVFS_UTIL_H

#include <stdint.h>
#include "ods.h"

/* ================================================================
 * Error logging
 *
 * All output goes to stderr. Debug messages compile out unless
 * ADVFS_DEBUG is defined.
 * ================================================================ */

/* Log a message at the given severity level. */
void advfs_log_msg(const char *level, const char *fmt, ...);

#define advfs_err(...)   advfs_log_msg("error", __VA_ARGS__)
#define advfs_warn(...)  advfs_log_msg("warn",  __VA_ARGS__)

#ifdef ADVFS_DEBUG
#define advfs_dbg(...)   advfs_log_msg("debug", __VA_ARGS__)
#else
#define advfs_dbg(...)   ((void)0)
#endif

/* ================================================================
 * Page/block conversion helpers
 * ================================================================ */

/* Convert a page number to an absolute byte offset. */
static inline uint64_t advfs_page_to_offset(uint32_t page)
{
    return (uint64_t)page * ADVFS_PGSZ;
}

/* Convert a block number to an absolute byte offset. */
static inline uint64_t advfs_block_to_offset(uint32_t block)
{
    return (uint64_t)block * ADVFS_BLKSZ;
}

/* Convert a block number to a page number. */
static inline uint32_t advfs_block_to_page(uint32_t block)
{
    return block / ADVFS_BLKS_PER_PG;
}

/* Convert a page number to the first block of that page. */
static inline uint32_t advfs_page_to_block(uint32_t page)
{
    return page * ADVFS_BLKS_PER_PG;
}

/* ================================================================
 * BMT extent map resolution
 *
 * The BMT is a bitfile with its own extent map. BMT page N may not
 * be at volume page (bmt_base + N). This helper resolves BMT page
 * numbers to absolute volume pages using the BMT's extent map.
 * ================================================================ */

/*
 * Resolve a BMT page number to an absolute volume page.
 *
 * bmt_map/bmt_map_count: extent map from the BMT's BSR_XTNTS record.
 *   Each entry maps a starting BMT page (bs_page) to a starting disk
 *   block (vd_blk). Entries with vd_blk == ADVFS_XTNT_TERM are
 *   boundaries: the final one marks the end of the BMT (its bs_page
 *   is the BMT page count); intermediate ones cover empty ranges.
 *
 * Returns 0 on success with *vol_page_out set, or -ENOENT if the
 * page is not covered by the map.
 */
int advfs_bmt_resolve_page(const adv_xtnt_t *bmt_map, int bmt_map_count,
                           uint32_t bmt_page_num, uint32_t *vol_page_out);

/* ================================================================
 * Endian conversion helpers
 *
 * AdvFS on-disk format is LITTLE-ENDIAN (Alpha architecture).
 * Linux x86_64 is also little-endian, so these are identity
 * functions. Included for documentation and future portability
 * to big-endian hosts.
 * ================================================================ */

/* Read a little-endian 16-bit value. */
static inline uint16_t advfs_le16(uint16_t v) { return v; }

/* Read a little-endian 32-bit value. */
static inline uint32_t advfs_le32(uint32_t v) { return v; }

/* Read a little-endian 64-bit value. */
static inline uint64_t advfs_le64(uint64_t v) { return v; }

#endif /* ADVFS_UTIL_H */
