/*
 * fileset.h -- fileset enumeration for advfs-fuse
 *
 * Enumerates filesets within a domain by reading the Bitfile Set
 * Directory (BFSDIR) via extents and parsing its tag directory pages.
 *
 * Copyright (C) 2026 Armas Spann
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef ADVFS_FILESET_H
#define ADVFS_FILESET_H

#include "domain.h"
#include "extents.h"
#include "ods.h"
#include "volume.h"

/*
 * Enumerate filesets by reading the BFSDIR tag directory.
 *
 * This is the proper way to find filesets: read BFSDIR's extent map
 * from cell ADVFS_BFM_BFSDIR_V3 (=2), then read each data page as
 * a tag directory page (adv_tag_dir_pg_t). For each in-use tag,
 * follow the tag map to the mcell and look for BSR_BFS_ATTR (type=8).
 *
 * Calls cb for each fileset found. Returns 0 on success, -errno on
 * failure.
 */
int advfs_fileset_list(advfs_volume_t *vol, advfs_domain_t *domain,
                       advfs_fileset_cb cb, void *user_data);

/*
 * Log the clone status of a fileset.
 *
 * Reports at debug level for both clones and originals. A clone
 * (clone_id != 0) is served correctly by the read path: pages that
 * were never copy-on-write'd are permanent holes whose data is read
 * back from the original fileset (advfs_filedata_read_clone()).
 */
void advfs_fileset_log_clone(const advfs_fileset_info_t *info);

/*
 * Resolve the extent map of a fileset's tag directory.
 *
 * Reads BFSDIR's extent map (cell ADVFS_BFM_BFSDIR on V4,
 * ADVFS_BFM_BFSDIR_V3 on V3), looks up the fileset's dir_tag in the
 * BFSDIR tag directory, then follows the fileset's primary mcell
 * chain to its BSR_XTNTS record (on V3 the record may live in a
 * chained mcell rather than the primary).
 *
 * The resulting extent map is the fs_tag_extents argument required
 * by advfs_dir_read(), advfs_filedata_stat() and
 * advfs_filedata_read(). It does not change while the domain is
 * mounted, so callers may resolve it once and cache it.
 *
 * vol:         volume context.
 * domain:      domain context.
 * fs_dir_tag:  the fileset's tag directory tag (from
 *              advfs_fileset_info_t.dir_tag).
 * extents_out: caller-provided array of at least max_extents entries.
 * max_extents: capacity of extents_out.
 * count_out:   on success, number of valid entries written.
 *
 * Returns 0 on success, -errno on failure.
 */
int advfs_fileset_tag_extents(advfs_volume_t *vol, advfs_domain_t *domain,
                              adv_bf_tag_t fs_dir_tag,
                              advfs_extent_t *extents_out, int max_extents,
                              int *count_out);

#endif /* ADVFS_FILESET_H */
