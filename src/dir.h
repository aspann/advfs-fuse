/*
 * dir.h -- AdvFS directory entry parsing for advfs-fuse
 *
 * Reads directory data pages through the extent map and walks
 * variable-length directory entries within 512-byte blocks.
 *
 * Copyright (C) 2026 Armas Spann
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef ADVFS_DIR_H
#define ADVFS_DIR_H

#include "ods.h"
#include "domain.h"
#include "extents.h"
#include "volume.h"
#include "filedata.h"  /* advfs_clone_ctx_t */

/* Callback for each directory entry. Return 0 to continue, non-zero to stop. */
typedef int (*advfs_dir_entry_cb)(const char *name, adv_bf_tag_t tag,
                                   void *user_data);

/*
 * Read all entries in a directory identified by its tag.
 *
 * Looks up dir_tag in the fileset's tag directory (via fs_tag_extents),
 * resolves the directory's extent map, and walks each data page to
 * parse directory entries. Calls cb for each valid entry.
 *
 * If the fileset's tag directory has dead/uninitialized entries, falls
 * back to scanning BMT pages to locate the directory's mcell using
 * fs_tag as the expected bf_set_tag.
 *
 * vol:                 volume context.
 * domain:              domain context (for BMT resolution).
 * fs_tag_extents:      extent map of the fileset's tag directory.
 * fs_tag_num_extents:  number of entries in fs_tag_extents.
 * fs_tag:              fileset's tag in BFSDIR (used as bf_set_tag for
 *                      BMT scan fallback).
 * dir_tag:             tag of the directory to read.
 * cb:                  callback for each entry.
 * user_data:           opaque pointer passed to cb.
 *
 * Returns 0 on success, -errno on failure.
 */
int advfs_dir_read(advfs_volume_t *vol, advfs_domain_t *domain,
                   const advfs_extent_t *fs_tag_extents, int fs_tag_num_extents,
                   adv_bf_tag_t fs_tag, adv_bf_tag_t dir_tag,
                   advfs_dir_entry_cb cb, void *user_data);

/*
 * Read all entries in a directory, optionally reporting deleted ones.
 *
 * Same walk as advfs_dir_read(), with a second, optional callback for
 * deleted directory entries. When a file is deleted, AdvFS zeroes the
 * entry header's tag_num but preserves the entry size, the name and
 * the trailing full tag -- so name and tag remain recoverable until
 * the entry space is reused.
 *
 * deleted_cb receives the preserved name and trailing tag of each
 * deleted entry that still carries plausible data (non-empty name,
 * non-zero trailing tag). Never-used gap entries (also tag_num == 0,
 * but without a preserved name/tag) are not reported. The associated
 * mcell may have been recycled; callers wanting metadata must probe
 * with advfs_filedata_stat() and treat failure as "unrecoverable".
 *
 * Either callback may be NULL to skip that class of entry. Both
 * receive user_data; returning non-zero from either stops the walk.
 *
 * Returns 0 on success, -errno on failure.
 */
int advfs_dir_read_full(advfs_volume_t *vol, advfs_domain_t *domain,
                        const advfs_extent_t *fs_tag_extents,
                        int fs_tag_num_extents,
                        adv_bf_tag_t fs_tag, adv_bf_tag_t dir_tag,
                        advfs_dir_entry_cb cb, advfs_dir_entry_cb deleted_cb,
                        void *user_data);

/*
 * Clone-aware variant of advfs_dir_read_full().
 *
 * Identical to advfs_dir_read_full(), except that when `clone` is
 * non-NULL and clone->is_clone is set, the directory is read as a
 * copy-on-write snapshot: the original fileset (clone->orig_set_tag)
 * supplies the directory's true page count and any page that is a
 * permanent hole -- or absent from the clone's stub extent map -- is
 * read from the same directory tag in the original fileset.
 *
 * A clone fileset shares its directory structure with the original at
 * snapshot time; unmodified directory pages remain permanent holes in
 * the clone (V4) or are described by a degenerate stub extent map that
 * points at reserved block 0 (V3). Without this fallback such pages
 * read as zeros and the directory appears empty or corrupt.
 *
 * Passing clone == NULL (or is_clone == 0) is exactly equivalent to
 * advfs_dir_read_full(). Returns 0 on success, -errno on failure.
 */
int advfs_dir_read_full_clone(advfs_volume_t *vol, advfs_domain_t *domain,
                              const advfs_extent_t *fs_tag_extents,
                              int fs_tag_num_extents,
                              adv_bf_tag_t fs_tag, adv_bf_tag_t dir_tag,
                              advfs_dir_entry_cb cb,
                              advfs_dir_entry_cb deleted_cb,
                              const advfs_clone_ctx_t *clone,
                              void *user_data);

/*
 * Clone-aware variant of advfs_dir_read() (deleted entries skipped).
 * Equivalent to advfs_dir_read() when clone == NULL / is_clone == 0.
 */
int advfs_dir_read_clone(advfs_volume_t *vol, advfs_domain_t *domain,
                         const advfs_extent_t *fs_tag_extents,
                         int fs_tag_num_extents,
                         adv_bf_tag_t fs_tag, adv_bf_tag_t dir_tag,
                         advfs_dir_entry_cb cb,
                         const advfs_clone_ctx_t *clone, void *user_data);

/*
 * Check whether a directory's mcell chain carries a BMTR_FS_UNDEL_DIR
 * (type=252) record, i.e. an undelete (trashcan) directory is attached.
 *
 * Walks the primary mcell at (bmt_pg_num, cell) and its chain looking
 * for the record. Restoring files through the undelete directory is
 * not implemented; this only detects and reports it.
 *
 * vol:         volume context.
 * domain:      domain context (for BMT page resolution).
 * vd:          vd_index of the volume holding the primary mcell
 *              (0 = primary volume).
 * bmt_pg_num:  BMT-internal page number of the primary mcell.
 * cell:        cell index within that BMT page.
 *
 * Returns 1 if found (logs a warning), 0 if not found, -errno on
 * failure to read the primary mcell.
 */
int advfs_dir_has_undelete(advfs_volume_t *vol, advfs_domain_t *domain,
                           uint32_t vd, uint32_t bmt_pg_num, uint32_t cell);

#endif /* ADVFS_DIR_H */
