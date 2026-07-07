/*
 * fuse_ops.c -- FUSE operation callbacks for advfs-fuse
 *
 * Read-only operations only: getattr, readdir, read, readlink, open,
 * opendir, statfs. All state lives in the advfs_fuse_ctx_t passed to
 * fuse_main() as private_data; operations fetch it through
 * fuse_get_context().
 *
 * Path resolution walks directory entries from the fileset root
 * (tag 2), verifying that intermediate components are directories.
 * File contents are served through advfs_filedata_read(), which
 * materializes the whole file; a one-entry cache in the context
 * makes sequential reads of the same file cheap.
 *
 * FUSE 2/3 compatibility: signature differences (getattr's extra
 * fuse_file_info, readdir's flags, the filler's flags argument) are
 * bridged with FUSE_USE_VERSION guards.
 *
 * Copyright (C) 2026 Armas Spann
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _XOPEN_SOURCE 700

#include "fuse_ops.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include "dir.h"
#include "filedata.h"
#include "util.h"

/* ----------------------------------------------------------------
 * Context and cache helpers
 * ---------------------------------------------------------------- */

/* Fetch the mount context stashed in FUSE's private_data. */
static advfs_fuse_ctx_t *get_ctx(void)
{
    return (advfs_fuse_ctx_t *)fuse_get_context()->private_data;
}

/* Initialize the context's cache fields (call once before mounting). */
void advfs_fuse_ctx_init_cache(advfs_fuse_ctx_t *ctx)
{
    pthread_mutex_init(&ctx->cache_lock, NULL);
    ctx->cache_tag_num = 0;
    ctx->cache_buf = NULL;
    ctx->cache_size = 0;
}

/* Free the context's cached file buffer (call at teardown). */
void advfs_fuse_ctx_drop_cache(advfs_fuse_ctx_t *ctx)
{
    pthread_mutex_lock(&ctx->cache_lock);
    free(ctx->cache_buf);
    ctx->cache_buf = NULL;
    ctx->cache_tag_num = 0;
    ctx->cache_size = 0;
    pthread_mutex_unlock(&ctx->cache_lock);
    pthread_mutex_destroy(&ctx->cache_lock);
}

/* ----------------------------------------------------------------
 * Path resolution
 * ---------------------------------------------------------------- */

/* Directory-entry lookup state for resolve_component(). */
typedef struct {
    const char  *name;   /* component we are searching for */
    adv_bf_tag_t tag;    /* entry's tag when found */
    int          found;
} lookup_state_t;

/* advfs_dir_read callback: stop when the wanted name is found. */
static int lookup_cb(const char *name, adv_bf_tag_t tag, void *user_data)
{
    lookup_state_t *ls = (lookup_state_t *)user_data;

    if (strcmp(name, ls->name) == 0) {
        ls->tag = tag;
        ls->found = 1;
        return 1;  /* stop iteration */
    }
    return 0;
}

/* Look up one path component in the directory dir_tag. */
static int resolve_component(advfs_fuse_ctx_t *ctx, adv_bf_tag_t dir_tag,
                             const char *name, adv_bf_tag_t *tag_out)
{
    lookup_state_t ls = { .name = name, .found = 0 };

    int err = advfs_dir_read(ctx->vol, ctx->domain, ctx->fs_tag_extents,
                             ctx->fs_tag_num_extents, ctx->fs_tag,
                             dir_tag, lookup_cb, &ls);
    if (err) {
        return err;
    }
    if (!ls.found) {
        return -ENOENT;
    }

    *tag_out = ls.tag;
    return 0;
}

/* Read a file's FS_STAT record by tag (thin wrapper). */
static int stat_by_tag(advfs_fuse_ctx_t *ctx, adv_bf_tag_t tag,
                       adv_fs_stat_t *st_out)
{
    return advfs_filedata_stat(ctx->vol, ctx->domain, ctx->fs_tag_extents,
                               ctx->fs_tag_num_extents, ctx->fs_tag,
                               tag, st_out);
}

/* Resolve a FUSE path to the file's AdvFS tag. */
int advfs_fuse_resolve_path(advfs_fuse_ctx_t *ctx, const char *path,
                            adv_bf_tag_t *tag_out)
{
    if (!ctx || !path || !tag_out) {
        return -EINVAL;
    }
    if (strlen(path) >= ADVFS_FUSE_PATH_MAX) {
        return -ENAMETOOLONG;
    }

    adv_bf_tag_t cur = { .num = ADVFS_ROOT_TAG_NUM, .seq = 0 };

    /* Walk components on a private copy (strtok-style, no strdup). */
    char buf[ADVFS_FUSE_PATH_MAX];
    snprintf(buf, sizeof(buf), "%s", path);

    char *p = buf;
    while (*p == '/') {
        p++;
    }

    while (*p != '\0') {
        char *comp = p;
        char *slash = strchr(p, '/');
        if (slash) {
            *slash = '\0';
            p = slash + 1;
            while (*p == '/') {
                p++;
            }
        } else {
            p = comp + strlen(comp);
        }

        adv_bf_tag_t next;
        int err = resolve_component(ctx, cur, comp, &next);
        if (err) {
            return err;
        }

        /*
         * If more components follow, this one must be a directory --
         * otherwise advfs_dir_read would parse file data as entries.
         */
        if (*p != '\0') {
            adv_fs_stat_t st;
            err = stat_by_tag(ctx, next, &st);
            if (err) {
                return err;
            }
            if (!S_ISDIR(st.st_mode)) {
                return -ENOTDIR;
            }
        }

        cur = next;
    }

    *tag_out = cur;
    return 0;
}

/* ----------------------------------------------------------------
 * getattr
 * ---------------------------------------------------------------- */

/* Translate an AdvFS FS_STAT record into a struct stat. */
static void fill_stat(const adv_fs_stat_t *fst, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_ino = fst->st_ino.num;
    st->st_mode = fst->st_mode;
    st->st_nlink = fst->st_nlink ? fst->st_nlink : 1;
    st->st_uid = fst->st_uid;
    st->st_gid = fst->st_gid;
    st->st_size = (off_t)fst->st_size;
    st->st_blksize = ADVFS_PGSZ;
    st->st_blocks = (blkcnt_t)((fst->st_size + 511) / 512);
    st->st_atim.tv_sec = fst->st_atime_sec;
    st->st_atim.tv_nsec = (long)fst->st_uatime * 1000;
    st->st_mtim.tv_sec = fst->st_mtime_sec;
    st->st_mtim.tv_nsec = (long)fst->st_umtime * 1000;
    st->st_ctim.tv_sec = fst->st_ctime_sec;
    st->st_ctim.tv_nsec = (long)fst->st_uctime * 1000;
}

/* advfs_dir_read callback for probe_is_dir(): stop at first entry. */
static int probe_cb(const char *name, adv_bf_tag_t tag, void *user_data)
{
    (void)name;
    (void)tag;
    *(int *)user_data = 1;
    return 1;
}

/* Heuristic fallback: does the tag parse as a directory? */
static int probe_is_dir(advfs_fuse_ctx_t *ctx, adv_bf_tag_t tag)
{
    int has_entry = 0;
    int err = advfs_dir_read(ctx->vol, ctx->domain, ctx->fs_tag_extents,
                             ctx->fs_tag_num_extents, ctx->fs_tag,
                             tag, probe_cb, &has_entry);
    return err == 0 && has_entry;
}

#if FUSE_USE_VERSION >= 30
static int advfs_op_getattr(const char *path, struct stat *st,
                            struct fuse_file_info *fi)
#else
static int advfs_op_getattr(const char *path, struct stat *st)
#endif
{
#if FUSE_USE_VERSION >= 30
    (void)fi;
#endif
    advfs_fuse_ctx_t *ctx = get_ctx();

    adv_bf_tag_t tag;
    int err = advfs_fuse_resolve_path(ctx, path, &tag);
    if (err) {
        return err;
    }

    adv_fs_stat_t fst;
    err = stat_by_tag(ctx, tag, &fst);
    if (err == 0) {
        fill_stat(&fst, st);
        return 0;
    }

    /*
     * No FS_STAT record. The path resolved, so the object exists --
     * synthesize minimal attributes (metadata files such as the frag
     * file lack FS_STAT; the root of very old filesets may too).
     */
    advfs_dbg("fuse: no FS_STAT for tag %u, synthesizing attrs (err=%d)",
              tag.num, err);
    memset(st, 0, sizeof(*st));
    st->st_ino = tag.num;
    st->st_blksize = ADVFS_PGSZ;
    if (tag.num == ADVFS_ROOT_TAG_NUM || probe_is_dir(ctx, tag)) {
        st->st_mode = S_IFDIR | 0555;
        st->st_nlink = 2;
    } else {
        st->st_mode = S_IFREG | 0444;
        st->st_nlink = 1;
    }
    return 0;
}

/* ----------------------------------------------------------------
 * readdir
 * ---------------------------------------------------------------- */

/* State for the readdir fill callback. */
typedef struct {
    void           *buf;
    fuse_fill_dir_t filler;
    int             full;        /* filler reported a full buffer */
    int             saw_dot;
    int             saw_dotdot;
} readdir_state_t;

/* Hand one entry to FUSE's filler (FUSE 2/3 signatures differ). */
static int fill_one(readdir_state_t *rs, const char *name)
{
#if FUSE_USE_VERSION >= 30
    return rs->filler(rs->buf, name, NULL, 0, (enum fuse_fill_dir_flags)0);
#else
    return rs->filler(rs->buf, name, NULL, 0);
#endif
}

/* advfs_dir_read callback: forward each entry to the FUSE filler. */
static int readdir_cb(const char *name, adv_bf_tag_t tag, void *user_data)
{
    readdir_state_t *rs = (readdir_state_t *)user_data;
    (void)tag;

    if (strcmp(name, ".") == 0) {
        rs->saw_dot = 1;
    } else if (strcmp(name, "..") == 0) {
        rs->saw_dotdot = 1;
    }

    if (fill_one(rs, name) != 0) {
        rs->full = 1;
        return 1;  /* buffer full -- stop */
    }
    return 0;
}

#if FUSE_USE_VERSION >= 30
static int advfs_op_readdir(const char *path, void *buf,
                            fuse_fill_dir_t filler, off_t offset,
                            struct fuse_file_info *fi,
                            enum fuse_readdir_flags flags)
#else
static int advfs_op_readdir(const char *path, void *buf,
                            fuse_fill_dir_t filler, off_t offset,
                            struct fuse_file_info *fi)
#endif
{
    (void)offset;
    (void)fi;
#if FUSE_USE_VERSION >= 30
    (void)flags;
#endif
    advfs_fuse_ctx_t *ctx = get_ctx();

    adv_bf_tag_t tag;
    int err = advfs_fuse_resolve_path(ctx, path, &tag);
    if (err) {
        return err;
    }

    adv_fs_stat_t fst;
    err = stat_by_tag(ctx, tag, &fst);
    if (err == 0 && !S_ISDIR(fst.st_mode)) {
        return -ENOTDIR;
    }

    readdir_state_t rs = { .buf = buf, .filler = filler };

    err = advfs_dir_read(ctx->vol, ctx->domain, ctx->fs_tag_extents,
                         ctx->fs_tag_num_extents, ctx->fs_tag,
                         tag, readdir_cb, &rs);
    if (err) {
        return err;
    }

    /* AdvFS directories carry "." and ".." as real entries; supply
     * them ourselves if this one (unexpectedly) did not. */
    if (!rs.full && !rs.saw_dot) {
        fill_one(&rs, ".");
    }
    if (!rs.full && !rs.saw_dotdot) {
        fill_one(&rs, "..");
    }

    return 0;
}

/* ----------------------------------------------------------------
 * open / read / readlink
 * ---------------------------------------------------------------- */

static int advfs_op_open(const char *path, struct fuse_file_info *fi)
{
    advfs_fuse_ctx_t *ctx = get_ctx();

    if ((fi->flags & O_ACCMODE) != O_RDONLY) {
        return -EROFS;
    }

    adv_bf_tag_t tag;
    int err = advfs_fuse_resolve_path(ctx, path, &tag);
    if (err) {
        return err;
    }

    adv_fs_stat_t fst;
    err = stat_by_tag(ctx, tag, &fst);
    if (err == 0 && S_ISDIR(fst.st_mode)) {
        return -EISDIR;
    }

    /* Stash the tag so read() can skip path resolution. */
    fi->fh = tag.num;
    return 0;
}

/*
 * Ensure the file identified by tag is in the context cache.
 * Called with cache_lock held. Returns 0 on success, -errno on failure.
 */
static int cache_file_locked(advfs_fuse_ctx_t *ctx, adv_bf_tag_t tag)
{
    if (ctx->cache_tag_num == tag.num && ctx->cache_buf != NULL) {
        return 0;
    }

    uint8_t *data = NULL;
    uint64_t size = 0;

    advfs_clone_ctx_t clone = {
        .is_clone = ctx->is_clone,
        .orig_set_tag = ctx->orig_set_tag,
    };

    int err = advfs_filedata_read_clone(ctx->vol, ctx->domain,
                                        ctx->fs_tag_extents,
                                        ctx->fs_tag_num_extents, ctx->fs_tag,
                                        tag, &clone, &data, &size);
    if (err) {
        return err;
    }

    free(ctx->cache_buf);
    ctx->cache_buf = data;
    ctx->cache_size = size;
    ctx->cache_tag_num = tag.num;
    return 0;
}

static int advfs_op_read(const char *path, char *buf, size_t size,
                         off_t offset, struct fuse_file_info *fi)
{
    advfs_fuse_ctx_t *ctx = get_ctx();

    adv_bf_tag_t tag;
    if (fi && fi->fh != 0) {
        tag.num = (uint32_t)fi->fh;
        tag.seq = 0;
    } else {
        int err = advfs_fuse_resolve_path(ctx, path, &tag);
        if (err) {
            return err;
        }
    }

    if (offset < 0) {
        return -EINVAL;
    }

    pthread_mutex_lock(&ctx->cache_lock);

    int err = cache_file_locked(ctx, tag);
    if (err) {
        pthread_mutex_unlock(&ctx->cache_lock);
        return err;
    }

    size_t n = 0;
    if ((uint64_t)offset < ctx->cache_size) {
        uint64_t avail = ctx->cache_size - (uint64_t)offset;
        n = (size < avail) ? size : (size_t)avail;
        memcpy(buf, ctx->cache_buf + offset, n);
    }

    pthread_mutex_unlock(&ctx->cache_lock);
    return (int)n;
}

static int advfs_op_readlink(const char *path, char *buf, size_t size)
{
    advfs_fuse_ctx_t *ctx = get_ctx();

    if (size == 0) {
        return -EINVAL;
    }

    adv_bf_tag_t tag;
    int err = advfs_fuse_resolve_path(ctx, path, &tag);
    if (err) {
        return err;
    }

    adv_fs_stat_t fst;
    err = stat_by_tag(ctx, tag, &fst);
    if (err) {
        return err;
    }
    if (!S_ISLNK(fst.st_mode)) {
        return -EINVAL;
    }

    /* Symlink target = the file's data contents. */
    uint8_t *data = NULL;
    uint64_t dsize = 0;
    err = advfs_filedata_read(ctx->vol, ctx->domain, ctx->fs_tag_extents,
                              ctx->fs_tag_num_extents, ctx->fs_tag,
                              tag, &data, &dsize);
    if (err) {
        return err;
    }

    size_t n = (dsize < size - 1) ? (size_t)dsize : size - 1;
    memcpy(buf, data, n);
    buf[n] = '\0';
    free(data);
    return 0;
}

/* ----------------------------------------------------------------
 * statfs
 * ---------------------------------------------------------------- */

static int advfs_op_statfs(const char *path, struct statvfs *sv)
{
    (void)path;
    advfs_fuse_ctx_t *ctx = get_ctx();

    memset(sv, 0, sizeof(*sv));
    sv->f_bsize = ADVFS_PGSZ;
    sv->f_frsize = ADVFS_PGSZ;
    sv->f_blocks = advfs_volume_page_count(ctx->vol);
    sv->f_bfree = 0;
    sv->f_bavail = 0;
    sv->f_files = 0;
    sv->f_ffree = 0;
    sv->f_namemax = 255;
    sv->f_flag = ST_RDONLY;
    return 0;
}

/* ----------------------------------------------------------------
 * Operations table
 * ---------------------------------------------------------------- */

static const struct fuse_operations advfs_ops = {
    .getattr  = advfs_op_getattr,
    .readlink = advfs_op_readlink,
    .open     = advfs_op_open,
    .read     = advfs_op_read,
    .statfs   = advfs_op_statfs,
    .readdir  = advfs_op_readdir,
};

/* Return the read-only FUSE operations table. */
const struct fuse_operations *advfs_fuse_get_ops(void)
{
    return &advfs_ops;
}
