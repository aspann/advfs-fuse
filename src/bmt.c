/*
 * bmt.c -- BMT page reading and mcell record walking for advfs-fuse
 *
 * Reads BMT/RBMT pages, provides access to mcells and their typed
 * records. All reads go through the volume layer (pread-based).
 *
 * Copyright (C) 2026 Armas Spann
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _XOPEN_SOURCE 500  /* pread() */

#include "bmt.h"
#include "util.h"

#include <errno.h>
#include <string.h>

/* Read and validate a BMT page from disk. */
int advfs_bmt_read_page(advfs_volume_t *vol, uint32_t page_num,
                        adv_bmt_pg_t *bmt_pg)
{
    if (!vol || !bmt_pg) {
        return -EINVAL;
    }

    int err = advfs_volume_read_page(vol, page_num, bmt_pg);
    if (err) {
        advfs_err("bmt: failed to read page %u: %s", page_num,
                  strerror(-err));
        return err;
    }

    /* Validate the ODS version in the page header */
    uint32_t version = ADV_BMT_PG_VERSION(*bmt_pg);
    if (version < ADVFS_FIRST_VALID_VERSION ||
        version > ADVFS_LAST_VALID_VERSION) {
        advfs_err("bmt: page %u has invalid ODS version %u",
                  page_num, version);
        return -EILSEQ;
    }

    return 0;
}

/* Get a pointer to mcell at cell_idx within a BMT page. */
adv_mcell_t *advfs_bmt_get_mcell(adv_bmt_pg_t *bmt_pg, uint32_t cell_idx)
{
    if (!bmt_pg || cell_idx >= ADVFS_BSPG_CELLS) {
        return NULL;
    }
    return &bmt_pg->mcells[cell_idx];
}

/* Find the first record of a given type within an mcell. */
void *advfs_bmt_find_rec(adv_mcell_t *mcell, uint8_t rec_type,
                         adv_rec_hdr_t *hdr_out)
{
    if (!mcell) {
        return NULL;
    }

    char *base = mcell->records;
    char *end = base + ADVFS_BSC_R_SZ;
    char *ptr = base;

    while (ptr + ADVFS_REC_HDR_SZ <= end) {
        adv_rec_hdr_t hdr;
        memcpy(&hdr, ptr, sizeof(hdr));

        if (ADV_REC_IS_NIL(hdr)) {
            break;
        }

        uint16_t bcnt = ADV_REC_BCNT(hdr);
        uint8_t type = ADV_REC_TYPE(hdr);

        /* Sanity: bCnt includes the 4-byte header, minimum valid is 4 */
        if (bcnt < ADVFS_REC_HDR_SZ || ptr + bcnt > end) {
            break;
        }

        if (type == rec_type) {
            if (hdr_out) {
                *hdr_out = hdr;
            }
            return ptr + ADVFS_REC_HDR_SZ;
        }

        /* Advance: bCnt includes header, round up to 4-byte boundary */
        uint32_t advance = ADV_REC_NEXT_OFF(bcnt);
        ptr += advance;
    }

    return NULL;
}

/* Advance to the next record header after current_rec. */
adv_rec_hdr_t *advfs_bmt_next_rec(adv_mcell_t *mcell,
                                  adv_rec_hdr_t *current_rec)
{
    if (!mcell || !current_rec) {
        return NULL;
    }

    char *base = mcell->records;
    char *end = base + ADVFS_BSC_R_SZ;
    char *ptr = (char *)current_rec;

    /* Validate that current_rec is within the record area */
    if (ptr < base || ptr + ADVFS_REC_HDR_SZ > end) {
        return NULL;
    }

    adv_rec_hdr_t hdr;
    memcpy(&hdr, ptr, sizeof(hdr));

    if (ADV_REC_IS_NIL(hdr)) {
        return NULL;
    }

    uint16_t bcnt = ADV_REC_BCNT(hdr);
    if (bcnt < ADVFS_REC_HDR_SZ) {
        return NULL;
    }

    /* Advance: bCnt includes 4-byte header, round up to 4-byte boundary */
    uint32_t advance = ADV_REC_NEXT_OFF(bcnt);
    char *next = ptr + advance;

    if (next + ADVFS_REC_HDR_SZ > end) {
        return NULL;
    }

    adv_rec_hdr_t next_hdr;
    memcpy(&next_hdr, next, sizeof(next_hdr));

    if (ADV_REC_IS_NIL(next_hdr)) {
        return NULL;
    }

    return (adv_rec_hdr_t *)next;
}
