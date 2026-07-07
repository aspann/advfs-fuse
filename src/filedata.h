/*
 * filedata.h -- file data reading for advfs-fuse
 *
 * Reads the data contents of a regular file identified by its tag:
 * resolves the tag to its primary mcell, reads the BMTR_FS_STAT
 * record for the file size and frag info, walks the extent map for
 * full data pages, and reads the sub-page tail (or the whole small
 * file) from the fileset's frag file when one is attached.
 *
 * Copyright (C) 2026 Armas Spann
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef ADVFS_FILEDATA_H
#define ADVFS_FILEDATA_H

#include <stdint.h>

#include "domain.h"
#include "extents.h"
#include "ods.h"
#include "volume.h"

/* ----------------------------------------------------------------
 * BMTR_FS_STAT (type 255) record payload -- the "inode" data.
 *
 * On-disk layout verified against real Tru64 V3/V4 disks: 88 bytes,
 * little-endian, st_size at byte offset 24.
 *
 * Frag fields: files whose size is not a multiple of 8 KB store the
 * sub-page tail (or, for files under 8 KB, the entire contents) in
 * the fileset's frag file (tag 1). frag_type is the frag size in
 * 1 KB units (1-7; 0 = no frag). frag_slot is the frag's location
 * within the frag file in 1 KB units. frag_page_offset is the file
 * page the frag supplies (0 for whole-file frags).
 * ---------------------------------------------------------------- */
typedef struct {
    adv_bf_tag_t st_ino;            /* file tag */
    uint32_t     st_mode;           /* file type + permission bits */
    uint32_t     st_dev;
    uint32_t     st_uid;
    uint32_t     st_gid;
    uint64_t     st_size;           /* file size in bytes */
    int32_t      st_atime_sec;
    int32_t      st_uatime;
    int32_t      st_mtime_sec;
    int32_t      st_umtime;
    int32_t      st_ctime_sec;
    int32_t      st_uctime;
    uint32_t     st_flags;
    adv_bf_tag_t dir_tag;           /* parent directory tag */
    uint32_t     frag_slot;         /* fragId.frag: 1 KB slot in frag file */
    uint32_t     frag_type;         /* fragId.type: size in KB, 0 = none */
    uint32_t     st_nlink;
    uint32_t     frag_page_offset;  /* file page supplied by the frag */
    uint32_t     st_unused;
} adv_fs_stat_t;

_Static_assert(sizeof(adv_fs_stat_t) == 88, "adv_fs_stat_t size");

/* Tag of the fileset's frag file (fixed by AdvFS convention). */
#define ADVFS_FRAG_FILE_TAG   1u

/* Frag slot size in bytes: frag_slot and frag_type are 1 KB units. */
#define ADVFS_FRAG_SLOT_SZ    1024u

/* Sanity cap on file size to defend against corrupt FS_STAT records. */
#define ADVFS_FILEDATA_MAX_SIZE  (1ull << 30)  /* 1 GB */

/* ----------------------------------------------------------------
 * Clone / snapshot fallback context.
 *
 * A clone fileset is a copy-on-write snapshot: at creation it shares
 * all data with its original. Pages that were later modified in the
 * ORIGINAL are copied into the clone; pages that were never touched
 * remain permanent holes (ADVFS_PERM_HOLE) in the clone and must be
 * read from the original fileset instead of being zero-filled.
 *
 * Supplying this context to advfs_filedata_read_clone() enables that
 * fallback. A NULL context, or is_clone == 0, reproduces the legacy
 * behavior exactly (permanent holes read as zeros).
 * ---------------------------------------------------------------- */
typedef struct {
    int          is_clone;      /* 0 = not a clone; no fallback performed */
    adv_bf_tag_t orig_set_tag;  /* BFSDIR tag of the original fileset */
} advfs_clone_ctx_t;

/*
 * Read the BMTR_FS_STAT record for a file.
 *
 * Looks up file_tag in the fileset's tag directory (with BMT scan
 * fallback for dead tag dirs, as advfs_dir_read does), then walks
 * the primary mcell chain for the FS_STAT record.
 *
 * vol:                 volume context.
 * domain:              domain context.
 * fs_tag_extents:      extent map of the fileset's tag directory.
 * fs_tag_num_extents:  number of entries in fs_tag_extents.
 * fs_tag:              fileset's tag in BFSDIR.
 * file_tag:            tag of the file.
 * stat_out:            receives the decoded FS_STAT record.
 *
 * Returns 0 on success, -errno on failure.
 */
int advfs_filedata_stat(advfs_volume_t *vol, advfs_domain_t *domain,
                        const advfs_extent_t *fs_tag_extents,
                        int fs_tag_num_extents,
                        adv_bf_tag_t fs_tag, adv_bf_tag_t file_tag,
                        adv_fs_stat_t *stat_out);

/*
 * Read a file's entire data contents into a malloc'd buffer.
 *
 * Resolves file_tag to its mcell, reads FS_STAT for the size and
 * frag info, reads all extent-mapped pages (sparse holes and pages
 * beyond the extent map read as zeros), then overlays the frag tail
 * from the fileset's frag file when frag_type != 0. The buffer is
 * truncated to the exact file size.
 *
 * On success, *buf_out points to a malloc'd buffer of *size_out
 * bytes (at least 1 byte is allocated for empty files) and the
 * caller must free() it.
 *
 * Parameters as for advfs_filedata_stat().
 * Returns 0 on success, -errno on failure.
 */
int advfs_filedata_read(advfs_volume_t *vol, advfs_domain_t *domain,
                        const advfs_extent_t *fs_tag_extents,
                        int fs_tag_num_extents,
                        adv_bf_tag_t fs_tag, adv_bf_tag_t file_tag,
                        uint8_t **buf_out, uint64_t *size_out);

/*
 * Clone-aware variant of advfs_filedata_read().
 *
 * Identical to advfs_filedata_read(), except that when `clone` is
 * non-NULL and clone->is_clone is set, permanent-hole (copy-on-write)
 * data pages are read from the original fileset (clone->orig_set_tag)
 * rather than zero-filled. The same file tag is looked up in the
 * original fileset and its extent map supplies the missing pages.
 *
 * Only one level of fallback is followed: if the original fileset also
 * marks the page as a permanent hole (a clone of a clone), the page is
 * zero-filled and a warning is logged.
 *
 * Passing clone == NULL is exactly equivalent to advfs_filedata_read().
 * Returns 0 on success, -errno on failure.
 */
int advfs_filedata_read_clone(advfs_volume_t *vol, advfs_domain_t *domain,
                              const advfs_extent_t *fs_tag_extents,
                              int fs_tag_num_extents,
                              adv_bf_tag_t fs_tag, adv_bf_tag_t file_tag,
                              const advfs_clone_ctx_t *clone,
                              uint8_t **buf_out, uint64_t *size_out);

#endif /* ADVFS_FILEDATA_H */
