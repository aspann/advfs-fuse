/*
 * domain.c -- AdvFS domain discovery for advfs-fuse
 *
 * Reads domain metadata from the BMT/RBMT and enumerates filesets.
 * For ODS V3, there is no separate RBMT -- the BMT at page 2 holds
 * all reserved metadata cells.
 *
 * Copyright (C) 2026 Armas Spann
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _XOPEN_SOURCE 500  /* pread() */

#include "domain.h"
#include "bmt.h"
#include "fileset.h"
#include "util.h"

#include <errno.h>
#include <string.h>

/*
 * Scan all mcells on BMT page 0 for a record of the given type.
 * Returns a pointer to the record data (within bmt_pg) and sets
 * *cell_out to the mcell index where it was found. Returns NULL
 * if not found.
 */
static void *find_rec_in_page(adv_bmt_pg_t *bmt_pg, uint8_t rec_type,
                              uint32_t *cell_out, adv_rec_hdr_t *hdr_out)
{
    for (uint32_t i = 0; i < ADVFS_BSPG_CELLS; i++) {
        adv_mcell_t *mc = advfs_bmt_get_mcell(bmt_pg, i);
        if (!mc) {
            continue;
        }
        void *rec = advfs_bmt_find_rec(mc, rec_type, hdr_out);
        if (rec) {
            if (cell_out) {
                *cell_out = i;
            }
            return rec;
        }
    }
    return NULL;
}

/*
 * Build a reserved metadata bitfile's extent map from its primary
 * mcell on (R)BMT page 0.
 *
 * pg0 is the already-read metadata page holding the primary mcell at
 * cell_idx. Primary extents from the BSR_XTNTS record are collected
 * first, then the chain of overflow mcells (BSR_XTRA_XTNTS on V4,
 * BSR_SHADOW_XTNTS on V3) is followed.
 *
 * Chain mcell page numbers are (R)BMT-internal and are resolved
 * through resolve_map/resolve_count. Passing resolve_map == NULL
 * resolves through the map being built (self-bootstrap): the chain
 * mcells of the BMT (V3) and the RBMT (V4) live on pages covered by
 * extents already collected -- guaranteed by construction, since the
 * kernel must be able to find its own metadata. For the V4 BMT, the
 * chain mcells live in the RBMT instead, so the caller passes the
 * previously-built RBMT map.
 *
 * Boundary entries (vd_blk == ADVFS_XTNT_TERM) are kept: the final
 * one bounds the bitfile (its bs_page is the page count);
 * intermediate ones mark where the map continues in the next
 * chained record.
 *
 * Returns 0 on success, -errno on failure.
 */
static int build_meta_extent_map(advfs_volume_t *vol, adv_bmt_pg_t *pg0,
                                 uint32_t cell_idx, const char *label,
                                 const adv_xtnt_t *resolve_map,
                                 int resolve_count,
                                 adv_xtnt_t *map_out, int *count_out)
{
    adv_mcell_t *mc = advfs_bmt_get_mcell(pg0, cell_idx);
    if (!mc) {
        advfs_err("domain: %s metadata cell %u not found", label, cell_idx);
        return -EILSEQ;
    }

    adv_rec_hdr_t xhdr;
    adv_xtnt_rec_t *xr = advfs_bmt_find_rec(mc, ADVFS_BSR_XTNTS, &xhdr);
    if (!xr) {
        advfs_err("domain: no BSR_XTNTS in %s cell %u", label, cell_idx);
        return -EILSEQ;
    }

    if (ADV_REC_DATA_SZ(xhdr) < sizeof(adv_xtnt_rec_t)) {
        advfs_err("domain: BSR_XTNTS in %s cell %u too small (%u < %zu)",
                  label, cell_idx, ADV_REC_DATA_SZ(xhdr),
                  sizeof(adv_xtnt_rec_t));
        return -EILSEQ;
    }

    /* Collect primary extents (up to 2 entries). */
    *count_out = 0;
    uint16_t x_cnt = xr->first_xtnt.x_cnt;
    if (x_cnt > ADVFS_BMT_XTNTS) {
        x_cnt = ADVFS_BMT_XTNTS;
    }

    for (uint16_t i = 0; i < x_cnt; i++) {
        if (*count_out < ADVFS_MAX_BMT_EXTENTS) {
            map_out[*count_out] = xr->first_xtnt.xtnts[i];
            (*count_out)++;
        }
    }

    advfs_dbg("domain: %s primary extents: x_cnt=%u, collected=%d, "
              "chain_mcid=p%u.c%u",
              label, xr->first_xtnt.x_cnt, *count_out,
              ADV_MCID_PAGE(xr->chain_mcid),
              ADV_MCID_CELL(xr->chain_mcid));

    /*
     * Follow the chain to overflow mcells. Each overflow mcell adds
     * up to 32 extent entries (31 for shadow records).
     */
    adv_mcell_id_t chain = xr->chain_mcid;
    int hops = 0;

    while (chain.raw != 0 && hops < 64) {
        uint32_t cpg = ADV_MCID_PAGE(chain);
        uint32_t ccl = ADV_MCID_CELL(chain);

        /* Resolve the chain page through the caller-supplied map,
         * or through the partial map built so far (self-bootstrap). */
        const adv_xtnt_t *rmap = resolve_map ? resolve_map : map_out;
        int rcnt = resolve_map ? resolve_count : *count_out;

        uint32_t cvp;
        int rerr = advfs_bmt_resolve_page(rmap, rcnt, cpg, &cvp);
        if (rerr) {
            advfs_dbg("domain: %s extent chain: cannot resolve "
                      "page %u (map has %d entries)", label, cpg, rcnt);
            break;
        }

        adv_bmt_pg_t chain_pg;
        int cerr = advfs_bmt_read_page(vol, cvp, &chain_pg);
        if (cerr) {
            advfs_dbg("domain: %s extent chain: failed to read "
                      "vol page %u for page %u", label, cvp, cpg);
            break;
        }

        adv_mcell_t *chain_mc = advfs_bmt_get_mcell(&chain_pg, ccl);
        if (!chain_mc) {
            break;
        }

        /* Try BSR_XTRA_XTNTS (type=5) first, then BSR_SHADOW_XTNTS (type=6) */
        adv_rec_hdr_t xtra_hdr;
        adv_xtra_xtnt_rec_t *xtra = advfs_bmt_find_rec(chain_mc,
            ADVFS_BSR_XTRA_XTNTS, &xtra_hdr);

        uint16_t ext_cnt = 0;
        uint16_t ext_max = 0;
        adv_xtnt_t *ext_array = NULL;

        if (xtra) {
            ext_cnt = xtra->x_cnt;
            ext_max = ADVFS_BMT_XTRA_XTNTS;
            ext_array = xtra->xtnts;
        } else {
            adv_rec_hdr_t shdr;
            adv_shadow_xtnt_rec_t *shadow = advfs_bmt_find_rec(chain_mc,
                ADVFS_BSR_SHADOW_XTNTS, &shdr);
            if (!shadow) {
                break;
            }
            ext_cnt = shadow->x_cnt;
            ext_max = ADVFS_BMT_SHADOW_XTNTS;
            ext_array = shadow->xtnts;
        }

        if (ext_cnt > ext_max) {
            ext_cnt = ext_max;
        }

        /* Keep boundary entries (see primary extent collection above) */
        for (uint16_t i = 0; i < ext_cnt; i++) {
            if (*count_out < ADVFS_MAX_BMT_EXTENTS) {
                map_out[*count_out] = ext_array[i];
                (*count_out)++;
            }
        }

        /* Follow mcell chain for further overflow */
        chain = chain_mc->next_mcid;
        hops++;
    }

    if (*count_out >= ADVFS_MAX_BMT_EXTENTS) {
        advfs_warn("domain: %s extent map full (%d entries); "
                   "later %s pages may be unresolvable",
                   label, *count_out, label);
    }

    advfs_dbg("domain: %s extent map: %d entry(ies)", label, *count_out);
    for (int i = 0; i < *count_out; i++) {
        if (map_out[i].vd_blk == ADVFS_XTNT_TERM) {
            advfs_dbg("domain:   [%d] bs_page=%u TERM (boundary)",
                      i, map_out[i].bs_page);
        } else {
            advfs_dbg("domain:   [%d] bs_page=%u -> vd_blk=%u (vol_page=%u)",
                      i, map_out[i].bs_page,
                      map_out[i].vd_blk,
                      map_out[i].vd_blk / ADVFS_BLKS_PER_PG);
        }
    }

    return 0;
}

/*
 * Discover domain metadata from BMT (V3) / RBMT (V4) page 0 of the
 * primary volume. Fills the legacy single-volume fields of *domain;
 * the caller (advfs_domain_open_multi) registers the per-vd entry.
 */
static int domain_open_primary(advfs_volume_t *vol, advfs_domain_t *domain)
{
    if (!vol || !domain) {
        return -EINVAL;
    }

    memset(domain, 0, sizeof(*domain));
    domain->vol = vol;

    /* Detect ODS version */
    int version = advfs_volume_detect_version(vol);
    if (version < 0) {
        return version;
    }
    domain->ods_version = version;

    /* The page-0 metadata page is at absolute volume page 2 for both
     * versions: BMT page 0 on V3, RBMT page 0 on V4. */
    domain->bmt_page = ADVFS_RESERVED_BLKS / ADVFS_BLKS_PER_PG;

    /* Read BMT page 0 */
    adv_bmt_pg_t bmt_pg;
    int err = advfs_bmt_read_page(vol, domain->bmt_page, &bmt_pg);
    if (err) {
        advfs_err("domain: failed to read BMT page 0");
        return err;
    }

    advfs_dbg("domain: BMT page 0 at volume page %u, ODS v%d, "
              "pageId=%u, %u free mcells",
              domain->bmt_page, version,
              ADV_BMT_PG_ID(bmt_pg), bmt_pg.free_mcell_cnt);

    /*
     * For V3: the reserved mcells on BMT page 0 are:
     *   cell 0 = BMT itself
     *   cell 1 = SBM
     *   cell 2 = Bitfile Set Directory
     *   cell 3 = Transaction Log
     *   cell 4 = BMT extension
     *   cell 5 = Misc (fake superblock)
     *
     * For V4: the reserved mcells on RBMT page 0 are:
     *   cell 0 = RBMT itself
     *   cell 1 = SBM
     *   cell 2 = Bitfile Set Directory
     *   cell 3 = Transaction Log
     *   cell 4 = BMT (a separate bitfile on V4)
     *   cell 5 = Misc (fake superblock)
     *   cell 6 = RBMT extension mcell
     *
     * Domain attrs (BSR_DMN_ATTR=4) and VD attrs (BSR_VD_ATTR=3)
     * can be records attached to any of these mcells: typically in
     * cell 0 (BMT) or in a chain from cell 0 on V3, and in cell 6
     * (RBMT extension) on V4. find_rec_in_page() scans all cells,
     * which covers both layouts.
     */

    /* Find BSR_DMN_ATTR (type 4) -- required */
    uint32_t cell_idx = 0;
    adv_rec_hdr_t rhdr;
    adv_dmn_attr_t *dmn_attr = find_rec_in_page(&bmt_pg,
                                                 ADVFS_BSR_DMN_ATTR,
                                                 &cell_idx, &rhdr);
    if (!dmn_attr) {
        advfs_err("domain: BSR_DMN_ATTR not found in BMT page 0");
        return -EILSEQ;
    }

    /* Validate record size */
    if (ADV_REC_DATA_SZ(rhdr) < sizeof(adv_dmn_attr_t)) {
        advfs_err("domain: BSR_DMN_ATTR too small (%u < %zu)",
                  ADV_REC_DATA_SZ(rhdr), sizeof(adv_dmn_attr_t));
        return -EILSEQ;
    }

    memcpy(&domain->domain_id, &dmn_attr->bf_domain_id,
           sizeof(domain->domain_id));
    domain->max_vds = dmn_attr->max_vds;
    domain->bf_set_dir_tag = dmn_attr->bf_set_dir_tag;

    advfs_dbg("domain: domain_id=%d.%06d, max_vds=%u, "
              "set_dir_tag=%u.%u (from cell %u)",
              domain->domain_id.id_sec, domain->domain_id.id_usec,
              domain->max_vds,
              domain->bf_set_dir_tag.num,
              domain->bf_set_dir_tag.seq,
              cell_idx);

    /* Find BSR_DMN_MATTR (type 15) -- optional for V3 */
    adv_dmn_mattr_t *mattr = find_rec_in_page(&bmt_pg,
                                               ADVFS_BSR_DMN_MATTR,
                                               &cell_idx, &rhdr);
    if (mattr && ADV_REC_DATA_SZ(rhdr) >= sizeof(adv_dmn_mattr_t)) {
        domain->has_mattr = 1;
        memcpy(&domain->mattr, mattr, sizeof(domain->mattr));

        advfs_dbg("domain: mattr: vd_cnt=%u, set_dir_tag=%u.%u, "
                  "ftx_log_tag=%u.%u",
                  domain->mattr.vd_cnt,
                  domain->mattr.bf_set_dir_tag.num,
                  domain->mattr.bf_set_dir_tag.seq,
                  domain->mattr.ftx_log_tag.num,
                  domain->mattr.ftx_log_tag.seq);

        /* Prefer the mattr set_dir_tag if it has a valid tag num */
        if (domain->mattr.bf_set_dir_tag.num != 0) {
            domain->bf_set_dir_tag = domain->mattr.bf_set_dir_tag;
        }
    }

    /* Find BSR_VD_ATTR (type 3) -- optional but useful */
    adv_vd_attr_t *vd_attr = find_rec_in_page(&bmt_pg,
                                               ADVFS_BSR_VD_ATTR,
                                               &cell_idx, &rhdr);
    if (vd_attr && ADV_REC_DATA_SZ(rhdr) >= sizeof(adv_vd_attr_t)) {
        domain->has_vd_attr = 1;
        memcpy(&domain->vd_attr, vd_attr, sizeof(domain->vd_attr));

        advfs_dbg("domain: vd_attr: vd_index=%u, blk_cnt=%u, "
                  "cluster=%u, bmt_xtnt_pgs=%u",
                  domain->vd_attr.vd_index,
                  domain->vd_attr.vd_blk_cnt,
                  domain->vd_attr.stg_cluster,
                  domain->vd_attr.bmt_xtnt_pgs);
    }

    /*
     * Build the BMT extent map.
     *
     * The BMT is itself a bitfile. Its pages are NOT necessarily
     * contiguous on disk. This map is required for all subsequent
     * BMT page reads (via advfs_domain_read_bmt_page).
     *
     * V3: cell 0 on BMT page 0 holds the BMT's own extent map
     *     (BSR_XTNTS, type=1); chain mcells live on BMT pages
     *     covered by the extents already collected (self-bootstrap).
     *
     * V4: the page read above is RBMT page 0. Build the RBMT's own
     *     extent map first (cell 0, ADVFS_BFM_RBMT, self-bootstrap),
     *     because the BMT's chain mcells live in the RBMT. Then
     *     bootstrap the BMT extent map from cell 4 (ADVFS_BFM_BMT),
     *     resolving chain pages through the RBMT map.
     */
    if (domain->ods_version >= ADVFS_FIRST_RBMT_VERSION) {
        err = build_meta_extent_map(vol, &bmt_pg, ADVFS_BFM_RBMT, "RBMT",
                                    NULL, 0,
                                    domain->rbmt_map,
                                    &domain->rbmt_map_count);
        if (err) {
            return err;
        }

        err = build_meta_extent_map(vol, &bmt_pg, ADVFS_BFM_BMT, "BMT",
                                    domain->rbmt_map,
                                    domain->rbmt_map_count,
                                    domain->bmt_map,
                                    &domain->bmt_map_count);
        if (err) {
            return err;
        }
    } else {
        err = build_meta_extent_map(vol, &bmt_pg, ADVFS_BFM_BMT_V3, "BMT",
                                    NULL, 0,
                                    domain->bmt_map,
                                    &domain->bmt_map_count);
        if (err) {
            return err;
        }
    }

    return 0;
}

/* Look up the per-vd entry for a 1-based vd_index (0 = primary). */
static const advfs_domain_vd_t *domain_find_vd(const advfs_domain_t *domain,
                                               uint32_t vd_index)
{
    if (!domain || domain->vd_count < 1) {
        return NULL;
    }
    if (vd_index == 0) {
        return &domain->vds[0];
    }
    for (int i = 0; i < domain->vd_count; i++) {
        if (domain->vds[i].vd_index == vd_index) {
            return &domain->vds[i];
        }
    }
    return NULL;
}

/*
 * Attach a secondary volume to an already-opened domain: verify it
 * belongs to the same domain, discover its vd_index from BSR_VD_ATTR,
 * and build its private BMT extent map.
 */
static int attach_secondary_volume(advfs_domain_t *domain,
                                   advfs_volume_t *vol)
{
    if (domain->vd_count >= ADVFS_MAX_DOM_VDS) {
        advfs_err("domain: too many volumes (max %d)", ADVFS_MAX_DOM_VDS);
        return -E2BIG;
    }

    int version = advfs_volume_detect_version(vol);
    if (version < 0) {
        return version;
    }
    if (version != domain->ods_version) {
        advfs_err("domain: volume '%s' is ODS v%d, domain is v%d",
                  advfs_volume_path(vol), version, domain->ods_version);
        return -EINVAL;
    }

    adv_bmt_pg_t pg0;
    int err = advfs_bmt_read_page(vol, domain->bmt_page, &pg0);
    if (err) {
        advfs_err("domain: failed to read metadata page 0 of '%s'",
                  advfs_volume_path(vol));
        return err;
    }

    /* Same domain? BSR_DMN_ATTR carries the domain id on every volume. */
    uint32_t cell_idx = 0;
    adv_rec_hdr_t rhdr;
    adv_dmn_attr_t *dmn_attr = find_rec_in_page(&pg0, ADVFS_BSR_DMN_ATTR,
                                                &cell_idx, &rhdr);
    if (!dmn_attr || ADV_REC_DATA_SZ(rhdr) < sizeof(adv_dmn_attr_t)) {
        advfs_err("domain: no BSR_DMN_ATTR on volume '%s'",
                  advfs_volume_path(vol));
        return -EILSEQ;
    }
    if (dmn_attr->bf_domain_id.id_sec != domain->domain_id.id_sec ||
        dmn_attr->bf_domain_id.id_usec != domain->domain_id.id_usec) {
        advfs_err("domain: volume '%s' belongs to a different domain "
                  "(id %d.%06d, expected %d.%06d)",
                  advfs_volume_path(vol),
                  dmn_attr->bf_domain_id.id_sec,
                  dmn_attr->bf_domain_id.id_usec,
                  domain->domain_id.id_sec, domain->domain_id.id_usec);
        return -EINVAL;
    }

    /* Volume index from BSR_VD_ATTR. */
    uint32_t vd_index;
    adv_vd_attr_t *vd_attr = find_rec_in_page(&pg0, ADVFS_BSR_VD_ATTR,
                                              &cell_idx, &rhdr);
    if (vd_attr && ADV_REC_DATA_SZ(rhdr) >= sizeof(adv_vd_attr_t)) {
        vd_index = vd_attr->vd_index;
    } else {
        vd_index = (uint32_t)domain->vd_count + 1;
        advfs_warn("domain: no BSR_VD_ATTR on volume '%s'; "
                   "assuming vd_index %u from argument order",
                   advfs_volume_path(vol), vd_index);
    }
    if (vd_index == 0 || domain_find_vd(domain, vd_index)) {
        advfs_err("domain: volume '%s' has invalid or duplicate "
                  "vd_index %u", advfs_volume_path(vol), vd_index);
        return -EEXIST;
    }

    /* Build this volume's own BMT extent map (each volume has its
     * own BMT; on V4 also its own RBMT for chain resolution). */
    advfs_domain_vd_t *vd = &domain->vds[domain->vd_count];
    memset(vd, 0, sizeof(*vd));
    vd->vol = vol;
    vd->vd_index = vd_index;

    if (domain->ods_version >= ADVFS_FIRST_RBMT_VERSION) {
        adv_xtnt_t rbmt_map[ADVFS_MAX_BMT_EXTENTS];
        int rbmt_count = 0;
        err = build_meta_extent_map(vol, &pg0, ADVFS_BFM_RBMT, "RBMT",
                                    NULL, 0, rbmt_map, &rbmt_count);
        if (err) {
            return err;
        }
        err = build_meta_extent_map(vol, &pg0, ADVFS_BFM_BMT, "BMT",
                                    rbmt_map, rbmt_count,
                                    vd->bmt_map, &vd->bmt_map_count);
    } else {
        err = build_meta_extent_map(vol, &pg0, ADVFS_BFM_BMT_V3, "BMT",
                                    NULL, 0,
                                    vd->bmt_map, &vd->bmt_map_count);
    }
    if (err) {
        return err;
    }

    domain->vd_count++;
    advfs_dbg("domain: attached volume '%s' as vd %u (%d BMT extents)",
              advfs_volume_path(vol), vd_index, vd->bmt_map_count);
    return 0;
}

/* Discover domain metadata; single-volume convenience wrapper. */
int advfs_domain_open(advfs_volume_t *vol, advfs_domain_t *domain)
{
    return advfs_domain_open_multi(&vol, 1, domain);
}

/* Discover domain metadata across one or more volumes. */
int advfs_domain_open_multi(advfs_volume_t **vols, int nvols,
                            advfs_domain_t *domain)
{
    if (!vols || nvols < 1 || nvols > ADVFS_MAX_DOM_VDS || !domain) {
        return -EINVAL;
    }

    int err = domain_open_primary(vols[0], domain);
    if (err) {
        return err;
    }

    /* Register the primary volume as vds[0]. Its vd_index comes from
     * BSR_VD_ATTR when present (multi-volume domains), else 1. */
    uint32_t primary_vd = 1;
    if (domain->has_vd_attr && domain->vd_attr.vd_index != 0) {
        primary_vd = domain->vd_attr.vd_index;
    }
    domain->vds[0].vol = vols[0];
    domain->vds[0].vd_index = primary_vd;
    memcpy(domain->vds[0].bmt_map, domain->bmt_map,
           sizeof(domain->bmt_map));
    domain->vds[0].bmt_map_count = domain->bmt_map_count;
    domain->vd_count = 1;

    for (int i = 1; i < nvols; i++) {
        err = attach_secondary_volume(domain, vols[i]);
        if (err) {
            return err;
        }
    }

    if (domain->has_mattr && domain->mattr.vd_cnt > (uint32_t)nvols) {
        advfs_warn("domain: %u volume(s) attached but domain has %u; "
                   "files on missing volumes will be unreadable",
                   (uint32_t)nvols, domain->mattr.vd_cnt);
    }

    return 0;
}

/* Look up the volume registered under vd_index (0 = primary). */
advfs_volume_t *advfs_domain_vd_vol(const advfs_domain_t *domain,
                                    uint32_t vd_index)
{
    const advfs_domain_vd_t *vd = domain_find_vd(domain, vd_index);
    if (vd) {
        return vd->vol;
    }
    /* Legacy contexts without a vd table: 0 = the primary volume. */
    if (domain && vd_index == 0) {
        return domain->vol;
    }
    return NULL;
}

/* Look up the BMT extent map of the volume under vd_index. */
int advfs_domain_vd_map(const advfs_domain_t *domain, uint32_t vd_index,
                        const adv_xtnt_t **map_out, int *count_out)
{
    const advfs_domain_vd_t *vd = domain_find_vd(domain, vd_index);
    if (!vd) {
        /* Legacy contexts without a vd table: 0 = the primary. */
        if (domain && vd_index == 0) {
            if (map_out) {
                *map_out = domain->bmt_map;
            }
            if (count_out) {
                *count_out = domain->bmt_map_count;
            }
            return 0;
        }
        return -ENOENT;
    }
    if (map_out) {
        *map_out = vd->bmt_map;
    }
    if (count_out) {
        *count_out = vd->bmt_map_count;
    }
    return 0;
}

/* Release domain resources. */
void advfs_domain_close(advfs_domain_t *domain)
{
    if (!domain) {
        return;
    }
    /* Currently nothing to free -- domain does not own the volume.
     * Reserved for future use (e.g., cached BMT pages). */
    memset(domain, 0, sizeof(*domain));
}

/* Read a BMT page by resolving its page number through the extent map. */
int advfs_domain_read_bmt_page(advfs_domain_t *domain,
                               uint32_t bmt_page_num,
                               adv_bmt_pg_t *bmt_pg)
{
    if (!domain || !domain->vol || !bmt_pg) {
        return -EINVAL;
    }

    uint32_t vol_page;
    int err = advfs_bmt_resolve_page(domain->bmt_map, domain->bmt_map_count,
                                     bmt_page_num, &vol_page);
    if (err) {
        advfs_err("domain: BMT page %u not in extent map (%d entries)",
                  bmt_page_num, domain->bmt_map_count);
        return err;
    }

    advfs_dbg("domain: BMT page %u -> vol page %u",
              bmt_page_num, vol_page);

    return advfs_bmt_read_page(domain->vol, vol_page, bmt_pg);
}

/* Read a BMT page from the volume registered under vd_index. */
int advfs_domain_read_bmt_page_vd(advfs_domain_t *domain,
                                  uint32_t vd_index,
                                  uint32_t bmt_page_num,
                                  adv_bmt_pg_t *bmt_pg)
{
    if (!domain || !bmt_pg) {
        return -EINVAL;
    }

    const advfs_domain_vd_t *vd = domain_find_vd(domain, vd_index);
    if (!vd) {
        /* Legacy contexts without a vd table: 0 = the primary. */
        if (vd_index == 0) {
            return advfs_domain_read_bmt_page(domain, bmt_page_num,
                                              bmt_pg);
        }
        advfs_warn("domain: no volume attached for vd %u "
                   "(BMT page %u unreachable)", vd_index, bmt_page_num);
        return -ENOENT;
    }

    uint32_t vol_page;
    int err = advfs_bmt_resolve_page(vd->bmt_map, vd->bmt_map_count,
                                     bmt_page_num, &vol_page);
    if (err) {
        advfs_err("domain: vd %u BMT page %u not in extent map "
                  "(%d entries)", vd->vd_index, bmt_page_num,
                  vd->bmt_map_count);
        return err;
    }

    return advfs_bmt_read_page(vd->vol, vol_page, bmt_pg);
}

/*
 * Enumerate filesets by walking the bitfile set directory mcell chain.
 *
 * Strategy: The bitfile set directory's primary mcell is on BMT page 0
 * at the cell index for BFSDIR (cell 2 for V3). Each mcell in its chain
 * may contain BSR_BFS_ATTR records describing individual filesets.
 *
 * For a simple V3 domain, all fileset attributes are typically in the
 * BFSDIR mcell chain on BMT page 0. We walk the chain following
 * next_mcid/next_vd_index links.
 */
int advfs_domain_list_filesets(advfs_domain_t *domain,
                              advfs_fileset_cb cb, void *user_data)
{
    if (!domain || !domain->vol || !cb) {
        return -EINVAL;
    }

    /*
     * Determine the BFSDIR cell index based on ODS version.
     * For V3: cell 2 on BMT page 0.
     * For V4: cell 2 on RBMT page 0.
     */
    uint32_t bfsdir_cell;
    if (domain->ods_version >= ADVFS_FIRST_RBMT_VERSION) {
        bfsdir_cell = ADVFS_BFM_BFSDIR;
    } else {
        bfsdir_cell = ADVFS_BFM_BFSDIR_V3;
    }

    /*
     * Read BMT page 0.
     *
     * V3: the page-0 metadata page at domain->bmt_page IS BMT page 0.
     * V4: domain->bmt_page is the RBMT; fileset BSR_BFS_ATTR records
     *     live in the BMT proper (a separate bitfile), so BMT page 0
     *     must be resolved through the BMT extent map.
     */
    adv_bmt_pg_t bmt_pg;
    int err;
    if (domain->ods_version >= ADVFS_FIRST_RBMT_VERSION) {
        err = advfs_domain_read_bmt_page(domain, 0, &bmt_pg);
    } else {
        err = advfs_bmt_read_page(domain->vol, domain->bmt_page, &bmt_pg);
    }
    if (err) {
        advfs_err("domain: failed to read BMT page 0 for fileset scan");
        return err;
    }

    /*
     * Walk all mcells on BMT page 0 looking for BSR_BFS_ATTR records.
     * Fileset attributes can appear in ANY mcell that is part of the
     * bitfile-set directory chain. Rather than trying to follow the
     * chain precisely (which would need extent resolution for the
     * tag directory -- not yet implemented), we scan all occupied
     * mcells on page 0 for BSR_BFS_ATTR records.
     *
     * This works because domains with a small number of filesets
     * store all BSR_BFS_ATTR records in mcells on BMT page 0.
     */
    int found = 0;

    for (uint32_t i = 0; i < ADVFS_BSPG_CELLS; i++) {
        adv_mcell_t *mc = advfs_bmt_get_mcell(&bmt_pg, i);
        if (!mc) {
            continue;
        }

        /* Look for BSR_BFS_ATTR in this mcell */
        adv_rec_hdr_t rhdr;
        adv_bf_set_attr_t *bfs = advfs_bmt_find_rec(mc,
                                                     ADVFS_BSR_BFS_ATTR,
                                                     &rhdr);
        if (!bfs) {
            continue;
        }

        if (ADV_REC_DATA_SZ(rhdr) < sizeof(adv_bf_set_attr_t)) {
            advfs_dbg("domain: BSR_BFS_ATTR in cell %u too small "
                      "(%u < %zu), skipping",
                      i, ADV_REC_DATA_SZ(rhdr), sizeof(adv_bf_set_attr_t));
            continue;
        }

        advfs_fileset_info_t info;
        memset(&info, 0, sizeof(info));
        memcpy(info.name, bfs->set_name, ADVFS_BS_SET_NAME_SZ);
        info.name[ADVFS_BS_SET_NAME_SZ - 1] = '\0';
        info.dir_tag = mc->tag;
        info.set_id = bfs->bf_set_id;
        info.state = bfs->state;
        info.fs_dev = bfs->fs_dev;
        info.clone_id = bfs->clone_id;
        info.clone_cnt = bfs->clone_cnt;
        info.orig_set_tag = bfs->orig_set_tag;
        info.next_clone_set_tag = bfs->next_clone_set_tag;

        advfs_dbg("domain: fileset '%s' in cell %u, "
                  "tag=%u.%u, state=%u",
                  info.name, i, mc->tag.num, mc->tag.seq,
                  info.state);
        advfs_fileset_log_clone(&info);

        found++;
        int ret = cb(&info, user_data);
        if (ret != 0) {
            return 0;  /* callback requested stop */
        }
    }

    /*
     * If no filesets found on page 0, try following the BFSDIR mcell
     * chain onto subsequent pages. Walk up to 16 extra pages
     * to avoid infinite loops on corrupt metadata.
     *
     * On V4 the BFSDIR primary mcell lives on the RBMT page (not the
     * BMT page scanned above), and its chain mcells are RBMT cells,
     * so chain page numbers resolve through the RBMT extent map.
     */
    if (found == 0) {
        adv_mcell_t *bfsdir_mc = NULL;

        if (domain->ods_version >= ADVFS_FIRST_RBMT_VERSION) {
            if (advfs_bmt_read_page(domain->vol, domain->bmt_page,
                                    &bmt_pg) == 0) {
                bfsdir_mc = advfs_bmt_get_mcell(&bmt_pg, bfsdir_cell);
            }
        } else {
            bfsdir_mc = advfs_bmt_get_mcell(&bmt_pg, bfsdir_cell);
        }

        if (bfsdir_mc) {
            adv_mcell_id_t chain = bfsdir_mc->next_mcid;
            uint32_t hops = 0;

            while (chain.raw != 0 && hops < 16) {
                uint32_t chain_page = ADV_MCID_PAGE(chain);
                uint32_t chain_cell = ADV_MCID_CELL(chain);

                adv_bmt_pg_t chain_pg;
                if (domain->ods_version >= ADVFS_FIRST_RBMT_VERSION) {
                    uint32_t cvp;
                    err = advfs_bmt_resolve_page(domain->rbmt_map,
                                                 domain->rbmt_map_count,
                                                 chain_page, &cvp);
                    if (err == 0) {
                        err = advfs_bmt_read_page(domain->vol, cvp,
                                                  &chain_pg);
                    }
                } else {
                    err = advfs_domain_read_bmt_page(domain, chain_page,
                                                     &chain_pg);
                }
                if (err) {
                    advfs_dbg("domain: chain read failed at "
                              "metadata page %u", chain_page);
                    break;
                }

                if (chain_cell >= ADVFS_BSPG_CELLS) {
                    break;
                }

                adv_mcell_t *mc = advfs_bmt_get_mcell(&chain_pg,
                                                       chain_cell);
                if (!mc) {
                    break;
                }

                adv_rec_hdr_t rhdr;
                adv_bf_set_attr_t *bfs = advfs_bmt_find_rec(mc,
                    ADVFS_BSR_BFS_ATTR, &rhdr);
                if (bfs && ADV_REC_DATA_SZ(rhdr) >= sizeof(adv_bf_set_attr_t)) {
                    advfs_fileset_info_t info;
                    memset(&info, 0, sizeof(info));
                    memcpy(info.name, bfs->set_name,
                           ADVFS_BS_SET_NAME_SZ);
                    info.name[ADVFS_BS_SET_NAME_SZ - 1] = '\0';
                    info.dir_tag = mc->tag;
                    info.set_id = bfs->bf_set_id;
                    info.state = bfs->state;
                    info.fs_dev = bfs->fs_dev;
                    info.clone_id = bfs->clone_id;
                    info.clone_cnt = bfs->clone_cnt;
                    info.orig_set_tag = bfs->orig_set_tag;
                    info.next_clone_set_tag = bfs->next_clone_set_tag;
                    advfs_fileset_log_clone(&info);

                    found++;
                    int ret = cb(&info, user_data);
                    if (ret != 0) {
                        return 0;
                    }
                }

                /* Follow the chain */
                chain = mc->next_mcid;
                hops++;
            }
        }
    }

    if (found == 0) {
        advfs_dbg("domain: no filesets found");
    }

    return 0;
}
