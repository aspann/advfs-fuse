/*
 * bmt.h -- BMT page reading and mcell record walking for advfs-fuse
 *
 * Reads BMT/RBMT pages from disk, accesses individual mcells within
 * a page, and iterates over typed records within an mcell.
 *
 * Copyright (C) 2026 Armas Spann
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef ADVFS_BMT_H
#define ADVFS_BMT_H

#include "ods.h"
#include "volume.h"

/*
 * Read and validate a BMT page from disk.
 *
 * page_num: absolute page number on the volume (NOT the BMT-internal
 *           page index). For V3, BMT page 0 is at volume page 2
 *           (ADVFS_RESERVED_BLKS / ADVFS_BLKS_PER_PG).
 *
 * Returns 0 on success, -errno on failure.
 */
int advfs_bmt_read_page(advfs_volume_t *vol, uint32_t page_num,
                        adv_bmt_pg_t *bmt_pg);

/*
 * Get a pointer to the mcell at cell_idx within a BMT page.
 * Returns NULL if cell_idx is out of range.
 */
adv_mcell_t *advfs_bmt_get_mcell(adv_bmt_pg_t *bmt_pg, uint32_t cell_idx);

/*
 * Find the first record of a given type within an mcell.
 * Returns a pointer to the record data (past the header), or NULL
 * if no record of that type exists. If hdr_out is non-NULL, the
 * record header is written there.
 */
void *advfs_bmt_find_rec(adv_mcell_t *mcell, uint8_t rec_type,
                         adv_rec_hdr_t *hdr_out);

/*
 * Advance to the next record after current_rec within the mcell.
 * current_rec must point to a record header inside mcell->records[].
 * Returns a pointer to the next record header, or NULL if there are
 * no more records (hit nil terminator or end of record area).
 */
adv_rec_hdr_t *advfs_bmt_next_rec(adv_mcell_t *mcell,
                                  adv_rec_hdr_t *current_rec);

#endif /* ADVFS_BMT_H */
