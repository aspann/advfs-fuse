/*
 * domain.h -- AdvFS domain discovery for advfs-fuse
 *
 * Discovers domain metadata from the BMT/RBMT and enumerates
 * filesets within the domain.
 *
 * Copyright (C) 2026 Armas Spann
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef ADVFS_DOMAIN_H
#define ADVFS_DOMAIN_H

#include "ods.h"
#include "volume.h"

/* Maximum number of fileset records we can return in one enumeration. */
#define ADVFS_MAX_FILESETS  64

/* Maximum BMT extent descriptors cached in the domain context. */
#define ADVFS_MAX_BMT_EXTENTS  256

/* Maximum volumes (virtual disks) attachable to one domain context. */
#define ADVFS_MAX_DOM_VDS  8

/* Fileset info passed to the enumeration callback. */
typedef struct {
    char            name[ADVFS_BS_SET_NAME_SZ];
    adv_bf_tag_t    dir_tag;        /* fileset tag directory tag */
    adv_bf_set_id_t set_id;
    uint32_t        state;
    uint32_t        fs_dev;

    /* Clone (copy-on-write snapshot) info from BSR_BFS_ATTR.
     * clone_id != 0 means this fileset is a clone of orig_set_tag. */
    uint32_t        clone_id;           /* non-zero if this is a clone */
    uint32_t        clone_cnt;          /* number of clones of this set */
    adv_bf_tag_t    orig_set_tag;       /* original fileset (clones only) */
    adv_bf_tag_t    next_clone_set_tag; /* next clone in the clone chain */
} advfs_fileset_info_t;

/* Per-volume state within a domain -- one entry per attached vdisk.
 * Each volume of a multi-volume domain has its own BMT (and on V4 its
 * own RBMT); mcell references carry a vd_index selecting the volume. */
typedef struct {
    advfs_volume_t *vol;            /* underlying volume (not owned) */
    uint32_t        vd_index;       /* 1-based volume index in domain */
    adv_xtnt_t      bmt_map[ADVFS_MAX_BMT_EXTENTS];
    int             bmt_map_count;
} advfs_domain_vd_t;

/* Domain context -- populated by advfs_domain_open(). */
typedef struct {
    advfs_volume_t *vol;            /* primary volume (not owned) */
    int             ods_version;    /* 3 or 4 */
    uint32_t        bmt_page;       /* absolute volume page of the page-0
                                     * metadata page: BMT page 0 on V3,
                                     * RBMT page 0 on V4 */

    /* BMT extent map -- resolves BMT page numbers to disk locations.
     * V3: built from the BSR_XTNTS record in cell 0 of BMT page 0.
     * V4: built from the BSR_XTNTS record in cell 4 (ADVFS_BFM_BMT)
     *     of RBMT page 0, chains resolved through rbmt_map. */
    adv_xtnt_t      bmt_map[ADVFS_MAX_BMT_EXTENTS];
    int             bmt_map_count;

    /* RBMT extent map (V4 only) -- resolves RBMT page numbers to disk
     * locations. Built from the BSR_XTNTS record in cell 0
     * (ADVFS_BFM_RBMT) of RBMT page 0. Empty (count 0) on V3. */
    adv_xtnt_t      rbmt_map[ADVFS_MAX_BMT_EXTENTS];
    int             rbmt_map_count;

    /* From BSR_DMN_ATTR (type 4) */
    adv_id_t        domain_id;
    uint32_t        max_vds;
    adv_bf_tag_t    bf_set_dir_tag; /* root fileset directory tag */

    /* From BSR_DMN_MATTR (type 15) -- may be absent on some V3 disks */
    int             has_mattr;
    adv_dmn_mattr_t mattr;

    /* From BSR_VD_ATTR (type 3) */
    int             has_vd_attr;
    adv_vd_attr_t   vd_attr;

    /* Per-volume table (multi-volume domains).
     * vds[0] is the primary volume (same as vol/bmt_map above);
     * additional volumes are appended in attach order. Entries are
     * looked up by their 1-based vd_index, not by slot position. */
    int               vd_count;
    advfs_domain_vd_t vds[ADVFS_MAX_DOM_VDS];
} advfs_domain_t;

/* Fileset enumeration callback. Return 0 to continue, non-zero to stop. */
typedef int (*advfs_fileset_cb)(const advfs_fileset_info_t *info,
                                void *user_data);

/*
 * Discover domain metadata from BMT/RBMT page 0.
 * vol must be an already-opened volume.
 * domain must point to caller-allocated storage.
 * Returns 0 on success, -errno on failure.
 */
int advfs_domain_open(advfs_volume_t *vol, advfs_domain_t *domain);

/*
 * Multi-volume variant: attach nvols already-opened volumes to one
 * domain context. vols[0] must be the volume holding the bitfile set
 * directory (typically vd_index 1); additional volumes are verified
 * to belong to the same domain (matching BSR_DMN_ATTR domain id) and
 * are registered by the vd_index found in their BSR_VD_ATTR record.
 * Returns 0 on success, -errno on failure.
 */
int advfs_domain_open_multi(advfs_volume_t **vols, int nvols,
                            advfs_domain_t *domain);

/* Release any resources held by domain context (currently a no-op). */
void advfs_domain_close(advfs_domain_t *domain);

/*
 * Look up the volume registered under vd_index.
 * vd_index 0 is accepted as an alias for the primary volume (some ODS
 * fields use 0 to mean "same/default volume").
 * Returns NULL if no such volume is attached.
 */
advfs_volume_t *advfs_domain_vd_vol(const advfs_domain_t *domain,
                                    uint32_t vd_index);

/*
 * Look up the BMT extent map of the volume registered under vd_index
 * (0 = primary). Returns 0 and sets *map_out and *count_out on success,
 * -ENOENT if no such volume is attached.
 */
int advfs_domain_vd_map(const advfs_domain_t *domain, uint32_t vd_index,
                        const adv_xtnt_t **map_out, int *count_out);

/*
 * Read a BMT page by resolving its page number through the BMT's
 * own extent map. This correctly handles non-contiguous BMT layouts
 * where BMT page N is not at volume page (bmt_base + N).
 *
 * bmt_page_num: BMT-internal page number (0-based).
 * bmt_pg:       caller-provided buffer for the BMT page data.
 *
 * Returns 0 on success, -errno on failure.
 */
int advfs_domain_read_bmt_page(advfs_domain_t *domain,
                               uint32_t bmt_page_num,
                               adv_bmt_pg_t *bmt_pg);

/*
 * Like advfs_domain_read_bmt_page(), but reads the BMT of the volume
 * registered under vd_index (0 = primary), resolving the page number
 * through that volume's own BMT extent map.
 */
int advfs_domain_read_bmt_page_vd(advfs_domain_t *domain,
                                  uint32_t vd_index,
                                  uint32_t bmt_page_num,
                                  adv_bmt_pg_t *bmt_pg);

/*
 * Enumerate filesets within the domain by walking the bitfile set
 * directory mcell chain. Calls cb for each fileset found.
 * Returns 0 on success, -errno on failure.
 */
int advfs_domain_list_filesets(advfs_domain_t *domain,
                              advfs_fileset_cb cb, void *user_data);

#endif /* ADVFS_DOMAIN_H */
