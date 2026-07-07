/*
 * fuse_ops.h -- FUSE operation callbacks for advfs-fuse
 *
 * Read-only FUSE layer: resolves FUSE paths to AdvFS tags by walking
 * directories from the fileset root (tag 2), then serves getattr,
 * readdir, read and readlink through the filedata/dir APIs.
 *
 * Supports both libfuse 2 (FUSE_USE_VERSION 26) and libfuse 3
 * (FUSE_USE_VERSION 30, build with -DADVFS_FUSE3).
 *
 * Copyright (C) 2026 Armas Spann
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef ADVFS_FUSE_OPS_H
#define ADVFS_FUSE_OPS_H

#ifndef FUSE_USE_VERSION
#ifdef ADVFS_FUSE3
#define FUSE_USE_VERSION 30
#else
#define FUSE_USE_VERSION 26
#endif
#endif

#include <fuse.h>
#include <pthread.h>
#include <stdint.h>

#include "domain.h"
#include "extents.h"
#include "ods.h"
#include "volume.h"

/* Root directory tag within a fileset (fixed by AdvFS convention). */
#define ADVFS_ROOT_TAG_NUM  2u

/* Maximum accepted FUSE path length. */
#define ADVFS_FUSE_PATH_MAX  4096

/*
 * Mounted-fileset context. Filled by main() before fuse_main() and
 * handed to FUSE as private_data; every operation retrieves it via
 * fuse_get_context()->private_data (no global mutable state).
 *
 * The fileset tag directory extents are resolved once at mount time
 * (advfs_fileset_tag_extents) and never change afterwards.
 */
typedef struct {
    advfs_volume_t *vol;                             /* not owned */
    advfs_domain_t *domain;                          /* not owned */
    advfs_extent_t  fs_tag_extents[ADVFS_MAX_EXTENTS];
    int             fs_tag_num_extents;
    adv_bf_tag_t    fs_tag;                          /* fileset dir tag */
    char            fs_name[ADVFS_BS_SET_NAME_SZ];   /* fileset name */

    /*
     * Clone/snapshot fallback: when the mounted fileset is a clone,
     * permanent-hole (copy-on-write) pages are read from the original
     * fileset. is_clone == 0 (the common case) disables the fallback
     * and the read path behaves exactly as before.
     */
    int             is_clone;
    adv_bf_tag_t    orig_set_tag;                    /* original fileset tag */

    /*
     * One-entry file-data cache for the read path. advfs_filedata_read
     * always materializes the whole file, so sequential FUSE reads of
     * the same file reuse the buffer instead of re-reading the disk.
     * Guarded by cache_lock (FUSE may run multithreaded).
     */
    pthread_mutex_t cache_lock;
    uint32_t        cache_tag_num;   /* 0 = empty */
    uint8_t        *cache_buf;       /* malloc'd by advfs_filedata_read */
    uint64_t        cache_size;
} advfs_fuse_ctx_t;

/* Initialize the context's cache fields (call once before mounting). */
void advfs_fuse_ctx_init_cache(advfs_fuse_ctx_t *ctx);

/* Free the context's cached file buffer (call at teardown). */
void advfs_fuse_ctx_drop_cache(advfs_fuse_ctx_t *ctx);

/*
 * Resolve a FUSE path ("/subdir/file") to the file's AdvFS tag by
 * walking directory entries from the root (tag 2). Intermediate
 * components are verified to be directories.
 *
 * Returns 0 on success with *tag_out set; -ENOENT if a component
 * does not exist, -ENOTDIR if an intermediate component is not a
 * directory, -ENAMETOOLONG for oversized paths, -errno on I/O error.
 */
int advfs_fuse_resolve_path(advfs_fuse_ctx_t *ctx, const char *path,
                            adv_bf_tag_t *tag_out);

/* Return the read-only FUSE operations table. */
const struct fuse_operations *advfs_fuse_get_ops(void);

#endif /* ADVFS_FUSE_OPS_H */
