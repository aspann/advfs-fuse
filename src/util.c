/*
 * util.c -- shared helpers for advfs-fuse
 *
 * Copyright (C) 2026 Armas Spann
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "util.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>

/* Resolve a BMT page number to a volume page via the extent map. */
int advfs_bmt_resolve_page(const adv_xtnt_t *bmt_map, int bmt_map_count,
                           uint32_t bmt_page_num, uint32_t *vol_page_out)
{
    if (!vol_page_out) {
        return -EINVAL;
    }

    /* Fallback: if no map loaded, page 0 is at the fixed location */
    if (bmt_map_count <= 0 || !bmt_map) {
        if (bmt_page_num == 0) {
            *vol_page_out = ADVFS_RESERVED_BLKS / ADVFS_BLKS_PER_PG;
            return 0;
        }
        return -ENOENT;
    }

    /*
     * Find the extent containing bmt_page_num.
     *
     * Entry i covers pages [bs_page[i], bs_page[i+1]). The map may
     * contain boundary entries (vd_blk == ADVFS_XTNT_TERM): the final
     * one marks the end of the BMT; intermediate ones come from
     * record chaining and cover empty ranges. The BMT has no sparse
     * holes -- resolving into a TERM range is an error.
     */
    int found = -1;
    for (int i = 0; i < bmt_map_count; i++) {
        if (bmt_page_num < bmt_map[i].bs_page) {
            continue;
        }
        if (i + 1 < bmt_map_count) {
            if (bmt_page_num < bmt_map[i + 1].bs_page) {
                found = i;
                break;
            }
        } else {
            /* Last entry: a trailing TERM bounds the BMT; a trailing
             * mapped extent (no terminator collected) is treated as
             * an unbounded run. */
            if (bmt_map[i].vd_blk == ADVFS_XTNT_TERM) {
                break;
            }
            found = i;
            break;
        }
    }

    /* The BMT has no sparse holes: TERM and PERM_HOLE entries are
     * boundaries, never mapped storage. */
    if (found < 0 || bmt_map[found].vd_blk == ADVFS_XTNT_TERM ||
        bmt_map[found].vd_blk == ADVFS_PERM_HOLE) {
        return -ENOENT;
    }

    /* 64-bit math so a corrupt map cannot wrap to a wrong page. */
    uint32_t page_offset = bmt_page_num - bmt_map[found].bs_page;
    uint64_t disk_block = (uint64_t)bmt_map[found].vd_blk +
                          (uint64_t)page_offset * ADVFS_BLKS_PER_PG;
    if (disk_block > UINT32_MAX) {
        return -ERANGE;
    }
    *vol_page_out = (uint32_t)(disk_block / ADVFS_BLKS_PER_PG);
    return 0;
}

/* Write a log message to stderr with a severity prefix. */
void advfs_log_msg(const char *level, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "advfs: %s: ", level);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
