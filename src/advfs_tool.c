/*
 * advfs_tool.c -- standalone CLI analysis tool for AdvFS vdisk images
 *
 * A read-only inspection tool built on the advfs-fuse core parser
 * libraries (volume, domain, fileset, dir, filedata). No FUSE
 * dependency -- usable on systems without libfuse, e.g. for forensic
 * analysis of Tru64 AdvFS disk images.
 *
 * Modes:
 *   advfs-tool --info <vdisk>            metadata summary
 *   advfs-tool --scan-deleted <vdisk>    undelete scanner
 *   advfs-tool --list <vdisk> [path]     directory listing (ls -la style)
 *   advfs-tool --list -r <vdisk> [path]  recursive file count
 *
 * Copyright (C) 2026 Armas Spann
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _XOPEN_SOURCE 500  /* localtime_r() */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "dir.h"
#include "domain.h"
#include "extents.h"
#include "filedata.h"
#include "fileset.h"
#include "ods.h"
#include "util.h"
#include "volume.h"

#define ADVFS_TOOL_VERSION  "1.0.3"

/* Maximum file name length in a directory entry (mirrors dir.c). */
#define TOOL_MAX_NAME  255

/* Maximum path length for reconstructed paths. */
#define TOOL_MAX_PATH  4096

/* Maximum directory recursion depth for the deleted-entry scanner. */
#define TOOL_MAX_DEPTH  64

/* Root directory tag within a fileset (fixed by AdvFS convention). */
#define TOOL_ROOT_TAG  2u

/* ----------------------------------------------------------------
 * st_mode file type bits (Tru64 uses the standard UNIX encoding).
 * Defined locally to avoid depending on host <sys/stat.h> values.
 * ---------------------------------------------------------------- */
#define TOOL_S_IFMT    0170000u
#define TOOL_S_IFIFO   0010000u
#define TOOL_S_IFCHR   0020000u
#define TOOL_S_IFDIR   0040000u
#define TOOL_S_IFBLK   0060000u
#define TOOL_S_IFREG   0100000u
#define TOOL_S_IFLNK   0120000u
#define TOOL_S_IFSOCK  0140000u

#define TOOL_ISDIR(m)  (((m) & TOOL_S_IFMT) == TOOL_S_IFDIR)

/* ================================================================
 * Small shared helpers
 * ================================================================ */

/* Fileset context: everything needed to read within one fileset. */
typedef struct {
    advfs_volume_t *vol;
    advfs_domain_t *domain;
    adv_bf_tag_t    fs_tag;
    char            fs_name[ADVFS_BS_SET_NAME_SZ];
    advfs_extent_t  tag_extents[ADVFS_MAX_EXTENTS];
    int             num_tag_extents;

    /* Clone/snapshot fallback: permanent-hole pages of a clone fileset
     * are read from the original fileset (orig_set_tag). */
    int             is_clone;
    adv_bf_tag_t    orig_set_tag;
} fs_ctx_t;

/* One collected directory entry. */
typedef struct {
    char         name[TOOL_MAX_NAME + 1];
    adv_bf_tag_t tag;
    int          deleted;
} dirent_rec_t;

/* Growable list of directory entries. */
typedef struct {
    dirent_rec_t *items;
    size_t        count;
    size_t        cap;
    int           oom;
} dirent_list_t;

/* Collected fileset info. */
typedef struct {
    advfs_fileset_info_t sets[ADVFS_MAX_FILESETS];
    int count;
} fileset_list_t;

static void dirent_list_free(dirent_list_t *list)
{
    free(list->items);
    list->items = NULL;
    list->count = list->cap = 0;
}

/* Append one entry; returns 0 on success, 1 (stop walk) on OOM. */
static int dirent_list_add(dirent_list_t *list, const char *name,
                           adv_bf_tag_t tag, int deleted)
{
    if (list->count == list->cap) {
        size_t new_cap = list->cap ? list->cap * 2 : 32;
        dirent_rec_t *p = realloc(list->items, new_cap * sizeof(*p));
        if (!p) {
            list->oom = 1;
            return 1;
        }
        list->items = p;
        list->cap = new_cap;
    }

    dirent_rec_t *e = &list->items[list->count++];
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->tag = tag;
    e->deleted = deleted;
    return 0;
}

static int collect_live_cb(const char *name, adv_bf_tag_t tag,
                           void *user_data)
{
    return dirent_list_add((dirent_list_t *)user_data, name, tag, 0);
}

static int collect_deleted_cb(const char *name, adv_bf_tag_t tag,
                              void *user_data)
{
    return dirent_list_add((dirent_list_t *)user_data, name, tag, 1);
}

/*
 * Read all entries of a directory into a list. Deleted entries are
 * included when include_deleted is non-zero.
 * Returns 0 on success (caller frees the list), -errno on failure.
 */
static int read_dir_entries(fs_ctx_t *fs, adv_bf_tag_t dir_tag,
                            int include_deleted, dirent_list_t *list)
{
    memset(list, 0, sizeof(*list));

    /* Route through the clone-aware directory reader: for a clone
     * fileset, directory pages that are permanent (copy-on-write) holes
     * -- or absent from the clone's stub extent map -- are read from the
     * same directory tag in the original fileset. For a non-clone
     * fileset this is exactly equivalent to advfs_dir_read_full(). */
    advfs_clone_ctx_t clone = {
        .is_clone = fs->is_clone,
        .orig_set_tag = fs->orig_set_tag,
    };

    int err = advfs_dir_read_full_clone(fs->vol, fs->domain, fs->tag_extents,
                                        fs->num_tag_extents, fs->fs_tag,
                                        dir_tag, collect_live_cb,
                                        include_deleted ? collect_deleted_cb
                                                        : NULL,
                                        &clone, list);
    if (err == 0 && list->oom) {
        err = -ENOMEM;
    }
    if (err) {
        dirent_list_free(list);
    }
    return err;
}

/*
 * Read the FS_STAT record for a tag, guarding against recycled tag
 * slots/mcells: the returned record's st_ino must match the tag
 * number we asked for. In strict mode (deleted entries), the tag
 * sequence must match too -- a bumped sequence means the slot was
 * reused by a newer file and the old metadata is gone.
 * Returns 0 on success, -ESTALE on mismatch, -errno on failure.
 */
static int stat_tag_checked(fs_ctx_t *fs, adv_bf_tag_t tag, int strict,
                            adv_fs_stat_t *st)
{
    int err = advfs_filedata_stat(fs->vol, fs->domain, fs->tag_extents,
                                  fs->num_tag_extents, fs->fs_tag, tag, st);
    if (err) {
        return err;
    }
    if (st->st_ino.num != 0 && st->st_ino.num != tag.num) {
        return -ESTALE;
    }
    if (strict && st->st_ino.seq != 0 && tag.seq != 0 &&
        st->st_ino.seq != tag.seq) {
        return -ESTALE;
    }
    return 0;
}

/* Fileset enumeration callback: collect into a fixed array. */
static int collect_fileset_cb(const advfs_fileset_info_t *info,
                              void *user_data)
{
    fileset_list_t *l = (fileset_list_t *)user_data;
    if (l->count < ADVFS_MAX_FILESETS) {
        l->sets[l->count++] = *info;
    }
    return 0;
}

/* Resolve a fileset's tag directory extents into a fs_ctx_t. */
static int fs_ctx_open(fs_ctx_t *fs, advfs_volume_t *vol,
                       advfs_domain_t *domain,
                       const advfs_fileset_info_t *info)
{
    memset(fs, 0, sizeof(*fs));
    fs->vol = vol;
    fs->domain = domain;
    fs->fs_tag = info->dir_tag;
    snprintf(fs->fs_name, sizeof(fs->fs_name), "%s", info->name);
    fs->is_clone = (info->clone_id != 0);
    fs->orig_set_tag = info->orig_set_tag;

    return advfs_fileset_tag_extents(vol, domain, info->dir_tag,
                                     fs->tag_extents, ADVFS_MAX_EXTENTS,
                                     &fs->num_tag_extents);
}

/* Format a byte count as a human-readable string. */
static void fmt_human_size(uint64_t bytes, char *buf, size_t sz)
{
    static const char *units[] = { "B", "KB", "MB", "GB", "TB" };
    double v = (double)bytes;
    int u = 0;

    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        u++;
    }
    if (u == 0) {
        snprintf(buf, sz, "%llu %s", (unsigned long long)bytes, units[0]);
    } else {
        snprintf(buf, sz, "%.1f %s", v, units[u]);
    }
}

/* Format a UNIX timestamp; "unknown" for zero/unconvertible values. */
static void fmt_time(int32_t secs, char *buf, size_t sz)
{
    struct tm tmv;
    time_t t = (time_t)secs;

    if (secs == 0 || !localtime_r(&t, &tmv)) {
        snprintf(buf, sz, "unknown");
        return;
    }
    strftime(buf, sz, "%Y-%m-%d %H:%M:%S", &tmv);
}

/* File type name from st_mode. */
static const char *mode_type_str(uint32_t mode)
{
    switch (mode & TOOL_S_IFMT) {
    case TOOL_S_IFDIR:  return "dir";
    case TOOL_S_IFREG:  return "regular";
    case TOOL_S_IFLNK:  return "symlink";
    case TOOL_S_IFCHR:  return "chardev";
    case TOOL_S_IFBLK:  return "blockdev";
    case TOOL_S_IFIFO:  return "fifo";
    case TOOL_S_IFSOCK: return "socket";
    default:            return "unknown";
    }
}

/* Build an ls-style permission string ("drwxr-xr-x") from st_mode. */
static void mode_to_perms(uint32_t mode, char out[11])
{
    char type;

    switch (mode & TOOL_S_IFMT) {
    case TOOL_S_IFDIR:  type = 'd'; break;
    case TOOL_S_IFREG:  type = '-'; break;
    case TOOL_S_IFLNK:  type = 'l'; break;
    case TOOL_S_IFCHR:  type = 'c'; break;
    case TOOL_S_IFBLK:  type = 'b'; break;
    case TOOL_S_IFIFO:  type = 'p'; break;
    case TOOL_S_IFSOCK: type = 's'; break;
    default:            type = '?'; break;
    }

    out[0] = type;
    out[1] = (mode & 0400) ? 'r' : '-';
    out[2] = (mode & 0200) ? 'w' : '-';
    out[3] = (mode & 0100) ? ((mode & 04000) ? 's' : 'x')
                           : ((mode & 04000) ? 'S' : '-');
    out[4] = (mode & 0040) ? 'r' : '-';
    out[5] = (mode & 0020) ? 'w' : '-';
    out[6] = (mode & 0010) ? ((mode & 02000) ? 's' : 'x')
                           : ((mode & 02000) ? 'S' : '-');
    out[7] = (mode & 0004) ? 'r' : '-';
    out[8] = (mode & 0002) ? 'w' : '-';
    out[9] = (mode & 0001) ? ((mode & 01000) ? 't' : 'x')
                           : ((mode & 01000) ? 'T' : '-');
    out[10] = '\0';
}

/* Return 1 if path exists and is a regular file, 0 otherwise. */
static int is_regular_file(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0 && S_ISREG(st.st_mode));
}

/*
 * Open one or more volumes and discover the domain.
 * vols[] must have capacity for ADVFS_MAX_DOM_VDS entries; an
 * nvdisks outside 1..ADVFS_MAX_DOM_VDS is rejected here so every
 * caller is guarded at once.  offset/size apply to the first (primary)
 * volume only, for partition support (size 0 = to EOF).  On failure,
 * any volumes that were opened are closed and set to NULL.
 * Returns 0 on success, -errno on failure.
 */
static int open_multi_domain(char **vdisk_paths, int nvdisks,
                             uint64_t offset, uint64_t size,
                             advfs_volume_t **vols, advfs_domain_t *domain)
{
    if (nvdisks < 1) {
        fprintf(stderr, "advfs-tool: no vdisk paths given\n");
        return -EINVAL;
    }
    if (nvdisks > ADVFS_MAX_DOM_VDS) {
        fprintf(stderr, "advfs-tool: too many vdisk paths (%d, max %d)\n",
                nvdisks, ADVFS_MAX_DOM_VDS);
        return -EINVAL;
    }

    for (int i = 0; i < nvdisks; i++) {
        vols[i] = NULL;
    }

    for (int i = 0; i < nvdisks; i++) {
        uint64_t off = (i == 0) ? offset : 0;
        uint64_t sz = (i == 0) ? size : 0;
        int err = advfs_volume_open_at(vdisk_paths[i], off, sz, &vols[i]);
        if (err) {
            fprintf(stderr, "advfs-tool: cannot open '%s': %s\n",
                    vdisk_paths[i], strerror(-err));
            for (int j = 0; j < i; j++) {
                advfs_volume_close(vols[j]);
                vols[j] = NULL;
            }
            return err;
        }
    }

    int err = advfs_domain_open_multi(vols, nvdisks, domain);
    if (err) {
        fprintf(stderr, "advfs-tool: cannot read AdvFS domain from "
                "'%s': %s\n", vdisk_paths[0], strerror(-err));
        for (int i = 0; i < nvdisks; i++) {
            advfs_volume_close(vols[i]);
            vols[i] = NULL;
        }
        return err;
    }

    return 0;
}

/* Close all non-NULL volumes in vols[0..nvols-1]. */
static void close_volumes(advfs_volume_t **vols, int nvols)
{
    for (int i = 0; i < nvols; i++) {
        if (vols[i]) {
            advfs_volume_close(vols[i]);
            vols[i] = NULL;
        }
    }
}

/* ================================================================
 * Mode 1: --info
 * ================================================================ */

/* Root directory entry counters for --info. */
typedef struct {
    uint32_t live;
    uint32_t deleted;
} entry_counts_t;

static int count_live_cb(const char *name, adv_bf_tag_t tag, void *user_data)
{
    (void)name;
    (void)tag;
    ((entry_counts_t *)user_data)->live++;
    return 0;
}

static int count_deleted_cb(const char *name, adv_bf_tag_t tag,
                            void *user_data)
{
    (void)name;
    (void)tag;
    ((entry_counts_t *)user_data)->deleted++;
    return 0;
}

/* --info: print domain metadata and fileset summary. */
static int cmd_info(char **vdisks, int nvdisks, uint64_t offset,
                    uint64_t size)
{
    advfs_volume_t *vols[ADVFS_MAX_DOM_VDS];
    advfs_domain_t domain;

    int err = open_multi_domain(vdisks, nvdisks, offset, size, vols,
                                &domain);
    if (err) {
        return 1;
    }

    advfs_volume_t *vol = vols[0];  /* primary volume */

    uint64_t pages = advfs_volume_page_count(vol);
    uint64_t bytes = advfs_volume_size(vol);
    char human[32];
    fmt_human_size(bytes, human, sizeof(human));

    /* Count mapped extents (excluding TERM/PERM_HOLE boundaries). */
    int bmt_extents = 0;
    for (int i = 0; i < domain.bmt_map_count; i++) {
        if (domain.bmt_map[i].vd_blk != ADVFS_XTNT_TERM &&
            domain.bmt_map[i].vd_blk != ADVFS_PERM_HOLE) {
            bmt_extents++;
        }
    }

    printf("AdvFS vdisk: %s\n", vdisks[0]);
    for (int i = 1; i < nvdisks; i++) {
        printf("             %s\n", vdisks[i]);
    }
    if (nvdisks > 1) {
        printf("  Volumes:      %d\n", nvdisks);
    }
    printf("  Magic:        0x%08x (valid)\n", ADVFS_MAGIC);
    printf("  ODS version:  %d (%s)\n", domain.ods_version,
           domain.ods_version >= ADVFS_ODS_V4 ? "Tru64 UNIX V5.x"
                                              : "Digital UNIX V4.x");
    printf("  Domain ID:    %d.%06d\n",
           domain.domain_id.id_sec, domain.domain_id.id_usec);
    printf("  Volume size:  %llu pages, %llu bytes (%s)\n",
           (unsigned long long)pages, (unsigned long long)bytes, human);
    printf("  BMT:          %s page 0 at volume page %u, "
           "%d mapped extent(s) (%d map entries)\n",
           domain.ods_version >= ADVFS_FIRST_RBMT_VERSION ? "RBMT" : "BMT",
           domain.bmt_page, bmt_extents, domain.bmt_map_count);

    /* Enumerate filesets. */
    fileset_list_t filesets;
    memset(&filesets, 0, sizeof(filesets));

    err = advfs_fileset_list(vol, &domain, collect_fileset_cb, &filesets);
    if (err) {
        fprintf(stderr, "advfs-tool: fileset enumeration failed: %s\n",
                strerror(-err));
        advfs_domain_close(&domain);
        close_volumes(vols, nvdisks);
        return 1;
    }

    printf("\nFilesets (%d):\n", filesets.count);

    for (int i = 0; i < filesets.count; i++) {
        const advfs_fileset_info_t *info = &filesets.sets[i];

        printf("  [%d] '%s'  tag=%u.%u", i + 1, info->name,
               info->dir_tag.num, info->dir_tag.seq);
        if (info->clone_id != 0) {
            printf("  (clone of tag %u)", info->orig_set_tag.num);
        } else if (info->clone_cnt != 0) {
            printf("  (original, %u clone(s))", info->clone_cnt);
        } else {
            printf("  (original)");
        }
        printf("\n");

        /* Clone filesets share (copy-on-write) the original's data;
         * reading their content is not supported by the core parser. */
        if (info->clone_id != 0) {
            printf("      root dir:  skipped (clone fallback not "
                   "implemented)\n");
            continue;
        }

        /* Root directory entry counts. */
        fs_ctx_t fs;
        err = fs_ctx_open(&fs, vol, &domain, info);
        if (err) {
            printf("      root dir:  unavailable (%s)\n", strerror(-err));
            continue;
        }

        entry_counts_t counts = { 0, 0 };
        adv_bf_tag_t root_tag = { .num = TOOL_ROOT_TAG, .seq = 0 };

        err = advfs_dir_read_full(vol, &domain, fs.tag_extents,
                                  fs.num_tag_extents, fs.fs_tag, root_tag,
                                  count_live_cb, count_deleted_cb, &counts);
        if (err) {
            printf("      root dir:  unreadable (%s)\n", strerror(-err));
        } else {
            printf("      root dir:  %u entr%s, %u deleted entr%s\n",
                   counts.live, counts.live == 1 ? "y" : "ies",
                   counts.deleted, counts.deleted == 1 ? "y" : "ies");
        }
    }

    if (filesets.count == 0) {
        printf("  (none found)\n");
    }

    advfs_domain_close(&domain);
    close_volumes(vols, nvdisks);
    return 0;
}

/* ================================================================
 * Mode 2: --scan-deleted
 * ================================================================ */

typedef struct {
    fs_ctx_t *fs;
    uint32_t  total_deleted;
    uint32_t  recoverable;
} scan_stats_t;

/*
 * Print one deleted-entry table row. Metadata comes from FS_STAT via
 * the tag; "unknown" when the mcell was recycled or is gone.
 */
static void print_deleted_row(scan_stats_t *st, const char *path,
                              adv_bf_tag_t tag)
{
    adv_fs_stat_t fstat;
    int err = stat_tag_checked(st->fs, tag, 1 /* strict */, &fstat);

    st->total_deleted++;

    if (err == 0) {
        st->recoverable++;

        char size_buf[24];
        char time_buf[32];
        snprintf(size_buf, sizeof(size_buf), "%llu",
                 (unsigned long long)fstat.st_size);
        fmt_time(fstat.st_ctime_sec, time_buf, sizeof(time_buf));

        printf("  %-40s %-10u %-9s %-12s %s\n",
               path, tag.num, mode_type_str(fstat.st_mode),
               size_buf, time_buf);
    } else {
        printf("  %-40s %-10u %-9s %-12s %s\n",
               path, tag.num, "unknown", "unknown", "unknown");
    }
}

/*
 * Recursively scan a directory for deleted entries.
 * path is the directory's path without a trailing slash ("" = root).
 */
static void scan_dir_deleted(scan_stats_t *st, adv_bf_tag_t dir_tag,
                             const char *path, int depth)
{
    if (depth > TOOL_MAX_DEPTH) {
        fprintf(stderr, "advfs-tool: max depth exceeded at '%s/', "
                "not descending further\n", path);
        return;
    }

    dirent_list_t list;
    int err = read_dir_entries(st->fs, dir_tag, 1, &list);
    if (err) {
        fprintf(stderr, "advfs-tool: cannot read directory '%s/': %s\n",
                path, strerror(-err));
        return;
    }

    /* Report deleted entries in this directory. */
    for (size_t i = 0; i < list.count; i++) {
        const dirent_rec_t *e = &list.items[i];
        if (!e->deleted) {
            continue;
        }

        char full[TOOL_MAX_PATH];
        int n = snprintf(full, sizeof(full), "%s/%s", path, e->name);
        if (n < 0 || (size_t)n >= sizeof(full)) {
            continue;  /* path too long -- skip */
        }
        print_deleted_row(st, full, e->tag);
    }

    /* Recurse into live subdirectories. */
    for (size_t i = 0; i < list.count; i++) {
        const dirent_rec_t *e = &list.items[i];
        if (e->deleted ||
            strcmp(e->name, ".") == 0 || strcmp(e->name, "..") == 0) {
            continue;
        }

        adv_fs_stat_t fstat;
        if (stat_tag_checked(st->fs, e->tag, 0, &fstat) != 0) {
            continue;
        }
        if (!TOOL_ISDIR(fstat.st_mode)) {
            continue;
        }

        char full[TOOL_MAX_PATH];
        int n = snprintf(full, sizeof(full), "%s/%s", path, e->name);
        if (n < 0 || (size_t)n >= sizeof(full)) {
            continue;
        }
        scan_dir_deleted(st, e->tag, full, depth + 1);
    }

    dirent_list_free(&list);
}

/* --scan-deleted: scan filesets recursively for deleted entries. */
static int cmd_scan_deleted(char **vdisks, int nvdisks, uint64_t offset,
                            uint64_t size, const char *fileset_name)
{
    advfs_volume_t *vols[ADVFS_MAX_DOM_VDS];
    advfs_domain_t domain;

    int err = open_multi_domain(vdisks, nvdisks, offset, size, vols,
                                &domain);
    if (err) {
        return 1;
    }

    advfs_volume_t *vol = vols[0];  /* primary volume */

    fileset_list_t filesets;
    memset(&filesets, 0, sizeof(filesets));

    err = advfs_fileset_list(vol, &domain, collect_fileset_cb, &filesets);
    if (err) {
        fprintf(stderr, "advfs-tool: fileset enumeration failed: %s\n",
                strerror(-err));
        advfs_domain_close(&domain);
        close_volumes(vols, nvdisks);
        return 1;
    }

    if (filesets.count == 0) {
        fprintf(stderr, "advfs-tool: no filesets found on '%s'\n", vdisks[0]);
        advfs_domain_close(&domain);
        close_volumes(vols, nvdisks);
        return 1;
    }

    /* When --fileset was given, verify it exists before scanning. */
    if (fileset_name != NULL) {
        int found = 0;
        for (int i = 0; i < filesets.count; i++) {
            if (strcmp(filesets.sets[i].name, fileset_name) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            fprintf(stderr, "advfs-tool: fileset '%s' not found\n",
                    fileset_name);
            advfs_domain_close(&domain);
            close_volumes(vols, nvdisks);
            return 1;
        }
    }

    uint32_t total = 0;
    uint32_t recoverable = 0;

    for (int i = 0; i < filesets.count; i++) {
        const advfs_fileset_info_t *info = &filesets.sets[i];

        if (fileset_name != NULL) {
            /* --fileset: only scan the named fileset (clones allowed). */
            if (strcmp(info->name, fileset_name) != 0) {
                continue;
            }
        } else {
            /* Default: skip clone filesets. */
            if (info->clone_id != 0) {
                printf("Fileset '%s': skipped (use --fileset to scan "
                       "clone filesets)\n\n", info->name);
                continue;
            }
        }

        fs_ctx_t fs;
        err = fs_ctx_open(&fs, vol, &domain, info);
        if (err) {
            fprintf(stderr, "advfs-tool: skipping fileset '%s': cannot "
                    "resolve tag directory (%s)\n",
                    info->name, strerror(-err));
            continue;
        }

        printf("Deleted entries in fileset '%s'%s:\n", info->name,
               fs.is_clone ? " (clone)" : "");
        printf("  %-40s %-10s %-9s %-12s %s\n",
               "PATH", "TAG", "TYPE", "SIZE", "DELETED AT (ctime)");
        printf("  %-40s %-10s %-9s %-12s %s\n",
               "----", "---", "----", "----", "------------------");

        scan_stats_t st;
        memset(&st, 0, sizeof(st));
        st.fs = &fs;

        adv_bf_tag_t root_tag = { .num = TOOL_ROOT_TAG, .seq = 0 };
        scan_dir_deleted(&st, root_tag, "", 0);

        if (st.total_deleted == 0) {
            printf("  (none)\n");
        }
        printf("\n");

        total += st.total_deleted;
        recoverable += st.recoverable;
    }

    printf("Summary: %u deleted entr%s found, %u with recoverable "
           "metadata.\n",
           total, total == 1 ? "y" : "ies", recoverable);

    advfs_domain_close(&domain);
    close_volumes(vols, nvdisks);
    return 0;
}

/* ================================================================
 * Mode 3: --list
 * ================================================================ */

/* Longest symlink target --list will display. */
#define TOOL_MAX_LINK_TARGET 1024

/*
 * Read a symlink's target into buf (NUL-terminated). Best effort:
 * returns 0 on success, -errno when unreadable (caller prints the
 * entry without a target).
 */
static int read_link_target(fs_ctx_t *fs, adv_bf_tag_t tag,
                            char *buf, size_t sz)
{
    uint8_t *data = NULL;
    uint64_t data_sz = 0;

    advfs_clone_ctx_t clone = {
        .is_clone = fs->is_clone,
        .orig_set_tag = fs->orig_set_tag,
    };

    int err = advfs_filedata_read_clone(fs->vol, fs->domain, fs->tag_extents,
                                        fs->num_tag_extents, fs->fs_tag, tag,
                                        &clone, &data, &data_sz);
    if (err) {
        return err;
    }

    if (data_sz >= sz) {
        data_sz = sz - 1;
    }
    memcpy(buf, data, data_sz);
    buf[data_sz] = '\0';
    free(data);
    return 0;
}

/* Print one ls -la style line for an entry. */
static void print_ls_entry(fs_ctx_t *fs, const char *name,
                           adv_bf_tag_t tag, int deleted)
{
    adv_fs_stat_t fstat;
    int err = stat_tag_checked(fs, tag, deleted /* strict for deleted */,
                               &fstat);

    if (err == 0) {
        char perms[11];
        char time_buf[32];
        mode_to_perms(fstat.st_mode, perms);
        fmt_time(fstat.st_mtime_sec, time_buf, sizeof(time_buf));

        printf("%s %4u %5u %5u %10llu %s %s",
               perms, fstat.st_nlink, fstat.st_uid, fstat.st_gid,
               (unsigned long long)fstat.st_size, time_buf, name);

        /* ls -la style " -> target" for symlinks (live only). */
        if ((fstat.st_mode & TOOL_S_IFMT) == TOOL_S_IFLNK && !deleted) {
            char target[TOOL_MAX_LINK_TARGET];
            if (read_link_target(fs, tag, target, sizeof(target)) == 0) {
                printf(" -> %s", target);
            }
        }

        printf("%s\n", deleted ? " [DELETED]" : "");
    } else {
        printf("%s %4s %5s %5s %10s %-19s %s%s\n",
               "??????????", "?", "?", "?", "?", "unknown", name,
               deleted ? " [DELETED]" : "");
    }
}

/*
 * Look up one live entry by name within a directory.
 * Returns 0 with *tag_out set, -ENOENT if absent, -errno on failure.
 */
static int lookup_entry(fs_ctx_t *fs, adv_bf_tag_t dir_tag,
                        const char *name, adv_bf_tag_t *tag_out)
{
    dirent_list_t list;
    int err = read_dir_entries(fs, dir_tag, 0, &list);
    if (err) {
        return err;
    }

    err = -ENOENT;
    for (size_t i = 0; i < list.count; i++) {
        if (strcmp(list.items[i].name, name) == 0) {
            *tag_out = list.items[i].tag;
            err = 0;
            break;
        }
    }

    dirent_list_free(&list);
    return err;
}

/* --list: list one directory (or a single file) ls -la style. */
static int cmd_list(char **vdisks, int nvdisks, uint64_t offset,
                    uint64_t size, const char *path,
                    const char *fileset_name)
{
    advfs_volume_t *vols[ADVFS_MAX_DOM_VDS];
    advfs_domain_t domain;

    int err = open_multi_domain(vdisks, nvdisks, offset, size, vols,
                                &domain);
    if (err) {
        return 1;
    }

    advfs_volume_t *vol = vols[0];  /* primary volume */

    fileset_list_t filesets;
    memset(&filesets, 0, sizeof(filesets));

    err = advfs_fileset_list(vol, &domain, collect_fileset_cb, &filesets);
    if (err || filesets.count == 0) {
        fprintf(stderr, "advfs-tool: no filesets found on '%s'\n", vdisks[0]);
        advfs_domain_close(&domain);
        close_volumes(vols, nvdisks);
        return 1;
    }

    const advfs_fileset_info_t *chosen = NULL;

    if (fileset_name != NULL) {
        /* --fileset: select by name, allowing clones. */
        for (int i = 0; i < filesets.count; i++) {
            if (strcmp(filesets.sets[i].name, fileset_name) == 0) {
                chosen = &filesets.sets[i];
                break;
            }
        }
        if (!chosen) {
            fprintf(stderr, "advfs-tool: fileset '%s' not found\n",
                    fileset_name);
            advfs_domain_close(&domain);
            close_volumes(vols, nvdisks);
            return 1;
        }
    } else {
        /* Default: pick the first non-clone fileset. */
        chosen = &filesets.sets[0];
        for (int i = 0; i < filesets.count; i++) {
            if (filesets.sets[i].clone_id == 0) {
                chosen = &filesets.sets[i];
                break;
            }
        }
    }

    fs_ctx_t fs;
    err = fs_ctx_open(&fs, vol, &domain, chosen);
    if (err) {
        fprintf(stderr, "advfs-tool: cannot resolve tag directory of "
                "fileset '%s': %s\n", chosen->name, strerror(-err));
        advfs_domain_close(&domain);
        close_volumes(vols, nvdisks);
        return 1;
    }

    /*
     * Resolve the path, component by component, starting at the root
     * directory (tag 2). Only live entries participate in resolution.
     */
    adv_bf_tag_t cur_tag = { .num = TOOL_ROOT_TAG, .seq = 0 };
    int is_dir = 1;
    char last_name[TOOL_MAX_NAME + 1] = "/";

    char path_buf[TOOL_MAX_PATH];
    int n = snprintf(path_buf, sizeof(path_buf), "%s", path);
    if (n < 0 || (size_t)n >= sizeof(path_buf)) {
        fprintf(stderr, "advfs-tool: path too long\n");
        advfs_domain_close(&domain);
        close_volumes(vols, nvdisks);
        return 1;
    }

    char *save = NULL;
    for (char *comp = strtok_r(path_buf, "/", &save); comp;
         comp = strtok_r(NULL, "/", &save)) {
        if (strcmp(comp, ".") == 0) {
            continue;
        }
        if (!is_dir) {
            fprintf(stderr, "advfs-tool: '%s' is not a directory\n",
                    last_name);
            advfs_domain_close(&domain);
            close_volumes(vols, nvdisks);
            return 1;
        }

        adv_bf_tag_t next_tag;
        err = lookup_entry(&fs, cur_tag, comp, &next_tag);
        if (err) {
            fprintf(stderr, "advfs-tool: '%s' not found under '%s': %s\n",
                    comp, last_name, strerror(-err));
            advfs_domain_close(&domain);
            close_volumes(vols, nvdisks);
            return 1;
        }

        adv_fs_stat_t fstat;
        err = stat_tag_checked(&fs, next_tag, 0, &fstat);
        is_dir = (err == 0) ? TOOL_ISDIR(fstat.st_mode) : 0;

        cur_tag = next_tag;
        snprintf(last_name, sizeof(last_name), "%s", comp);
    }

    if (!is_dir) {
        /* Path resolves to a non-directory: print its single entry. */
        printf("Fileset '%s', file '%s':\n", fs.fs_name, path);
        print_ls_entry(&fs, last_name, cur_tag, 0);
        advfs_domain_close(&domain);
        close_volumes(vols, nvdisks);
        return 0;
    }

    dirent_list_t list;
    err = read_dir_entries(&fs, cur_tag, 1, &list);
    if (err) {
        fprintf(stderr, "advfs-tool: cannot read directory '%s': %s\n",
                path, strerror(-err));
        advfs_domain_close(&domain);
        close_volumes(vols, nvdisks);
        return 1;
    }

    size_t deleted_count = 0;
    for (size_t i = 0; i < list.count; i++) {
        deleted_count += (size_t)(list.items[i].deleted != 0);
    }

    printf("Fileset '%s', directory '%s' (%zu entr%s, %zu deleted):\n",
           fs.fs_name, path, list.count - deleted_count,
           (list.count - deleted_count) == 1 ? "y" : "ies",
           deleted_count);

    for (size_t i = 0; i < list.count; i++) {
        const dirent_rec_t *e = &list.items[i];
        print_ls_entry(&fs, e->name, e->tag, e->deleted);
    }

    dirent_list_free(&list);
    advfs_domain_close(&domain);
    close_volumes(vols, nvdisks);
    return 0;
}

/* ================================================================
 * Recursive file count (-r / --recursive with --list)
 * ================================================================ */

typedef struct {
    uint64_t files;
    uint64_t dirs;
    uint64_t symlinks;
    uint64_t other;
    uint64_t errors;
    uint64_t total_size;
} rcount_t;

/* Growable set of visited directory tag numbers (cycle guard). */
typedef struct {
    uint32_t *tags;
    size_t    count;
    size_t    cap;
} visited_set_t;

/* Release the visited set's storage. */
static void visited_free(visited_set_t *set)
{
    free(set->tags);
    set->tags = NULL;
    set->count = set->cap = 0;
}

/* Return 1 when tag_num is already in the set, 0 otherwise. */
static int visited_contains(const visited_set_t *set, uint32_t tag_num)
{
    for (size_t i = 0; i < set->count; i++) {
        if (set->tags[i] == tag_num) {
            return 1;
        }
    }
    return 0;
}

/* Add tag_num to the set; returns 0 on success, -ENOMEM on OOM. */
static int visited_add(visited_set_t *set, uint32_t tag_num)
{
    if (set->count == set->cap) {
        size_t new_cap = set->cap ? set->cap * 2 : 64;
        uint32_t *p = realloc(set->tags, new_cap * sizeof(*p));
        if (!p) {
            return -ENOMEM;
        }
        set->tags = p;
        set->cap = new_cap;
    }
    set->tags[set->count++] = tag_num;
    return 0;
}

/* Recursively count files, dirs, symlinks in a directory tree. */
static void count_recursive(fs_ctx_t *fs, adv_bf_tag_t dir_tag,
                            const char *path, int depth, rcount_t *cnt,
                            visited_set_t *visited)
{
    if (depth > TOOL_MAX_DEPTH) {
        fprintf(stderr, "advfs-tool: max depth exceeded at '%s/'\n", path);
        cnt->errors++;
        return;
    }

    /* Cycle guard: a directory tag revisited via a corrupt or looped
     * directory structure is counted as an error, not descended. */
    if (visited_contains(visited, dir_tag.num)) {
        fprintf(stderr, "advfs-tool: directory cycle detected at '%s/' "
                "(tag %u already visited)\n", path, dir_tag.num);
        cnt->errors++;
        return;
    }
    if (visited_add(visited, dir_tag.num) != 0) {
        fprintf(stderr, "advfs-tool: out of memory tracking visited "
                "directories at '%s/'\n", path);
        cnt->errors++;
        return;
    }

    dirent_list_t list;
    int err = read_dir_entries(fs, dir_tag, 0, &list);
    if (err) {
        cnt->errors++;
        return;
    }

    for (size_t i = 0; i < list.count; i++) {
        const dirent_rec_t *e = &list.items[i];
        if (strcmp(e->name, ".") == 0 || strcmp(e->name, "..") == 0)
            continue;

        adv_fs_stat_t fstat;
        err = stat_tag_checked(fs, e->tag, 0, &fstat);
        if (err) {
            cnt->errors++;
            continue;
        }

        uint32_t ftype = fstat.st_mode & TOOL_S_IFMT;

        if (ftype == TOOL_S_IFDIR) {
            cnt->dirs++;
            char sub[TOOL_MAX_PATH];
            int n = snprintf(sub, sizeof(sub), "%s/%s", path, e->name);
            if (n >= 0 && (size_t)n < sizeof(sub)) {
                count_recursive(fs, e->tag, sub, depth + 1, cnt, visited);
            } else {
                fprintf(stderr, "advfs-tool: path too long under '%s/', "
                        "subtree '%s' skipped\n", path, e->name);
                cnt->errors++;
            }
        } else if (ftype == TOOL_S_IFREG) {
            cnt->files++;
            cnt->total_size += fstat.st_size;
        } else if (ftype == TOOL_S_IFLNK) {
            cnt->symlinks++;
        } else {
            cnt->other++;
        }
    }

    dirent_list_free(&list);
}

/* --list -r: open domain, resolve path, count all entries recursively. */
static int cmd_list_recursive(char **vdisks, int nvdisks, uint64_t offset,
                              uint64_t size, const char *path,
                              const char *fileset_name)
{
    advfs_volume_t *vols[ADVFS_MAX_DOM_VDS];
    advfs_domain_t domain;

    int err = open_multi_domain(vdisks, nvdisks, offset, size, vols,
                                &domain);
    if (err) {
        return 1;
    }

    advfs_volume_t *vol = vols[0];

    fileset_list_t filesets;
    memset(&filesets, 0, sizeof(filesets));

    err = advfs_fileset_list(vol, &domain, collect_fileset_cb, &filesets);
    if (err || filesets.count == 0) {
        fprintf(stderr, "advfs-tool: no filesets found on '%s'\n", vdisks[0]);
        advfs_domain_close(&domain);
        close_volumes(vols, nvdisks);
        return 1;
    }

    const advfs_fileset_info_t *chosen = NULL;

    if (fileset_name != NULL) {
        for (int i = 0; i < filesets.count; i++) {
            if (strcmp(filesets.sets[i].name, fileset_name) == 0) {
                chosen = &filesets.sets[i];
                break;
            }
        }
        if (!chosen) {
            fprintf(stderr, "advfs-tool: fileset '%s' not found\n",
                    fileset_name);
            advfs_domain_close(&domain);
            close_volumes(vols, nvdisks);
            return 1;
        }
    } else {
        chosen = &filesets.sets[0];
        for (int i = 0; i < filesets.count; i++) {
            if (filesets.sets[i].clone_id == 0) {
                chosen = &filesets.sets[i];
                break;
            }
        }
    }

    fs_ctx_t fs;
    err = fs_ctx_open(&fs, vol, &domain, chosen);
    if (err) {
        fprintf(stderr, "advfs-tool: cannot resolve tag directory of "
                "fileset '%s': %s\n", chosen->name, strerror(-err));
        advfs_domain_close(&domain);
        close_volumes(vols, nvdisks);
        return 1;
    }

    /* Resolve path to directory tag */
    adv_bf_tag_t cur_tag = { .num = TOOL_ROOT_TAG, .seq = 0 };
    int is_dir = 1;

    char path_buf[TOOL_MAX_PATH];
    int n = snprintf(path_buf, sizeof(path_buf), "%s", path);
    if (n < 0 || (size_t)n >= sizeof(path_buf)) {
        fprintf(stderr, "advfs-tool: path too long\n");
        advfs_domain_close(&domain);
        close_volumes(vols, nvdisks);
        return 1;
    }

    char *save = NULL;
    for (char *comp = strtok_r(path_buf, "/", &save); comp;
         comp = strtok_r(NULL, "/", &save)) {
        if (strcmp(comp, ".") == 0)
            continue;
        if (!is_dir) {
            fprintf(stderr, "advfs-tool: not a directory in path\n");
            advfs_domain_close(&domain);
            close_volumes(vols, nvdisks);
            return 1;
        }

        adv_bf_tag_t next_tag;
        err = lookup_entry(&fs, cur_tag, comp, &next_tag);
        if (err) {
            fprintf(stderr, "advfs-tool: '%s' not found: %s\n",
                    comp, strerror(-err));
            advfs_domain_close(&domain);
            close_volumes(vols, nvdisks);
            return 1;
        }

        adv_fs_stat_t fstat;
        err = stat_tag_checked(&fs, next_tag, 0, &fstat);
        is_dir = (err == 0) ? TOOL_ISDIR(fstat.st_mode) : 0;
        cur_tag = next_tag;
    }

    if (!is_dir) {
        fprintf(stderr, "advfs-tool: '%s' is not a directory\n", path);
        advfs_domain_close(&domain);
        close_volumes(vols, nvdisks);
        return 1;
    }

    rcount_t cnt;
    memset(&cnt, 0, sizeof(cnt));

    visited_set_t visited;
    memset(&visited, 0, sizeof(visited));

    fprintf(stderr, "advfs-tool: counting '%s' in fileset '%s'...\n",
            path, fs.fs_name);

    count_recursive(&fs, cur_tag, path, 0, &cnt, &visited);
    visited_free(&visited);

    char size_buf[32];
    fmt_human_size(cnt.total_size, size_buf, sizeof(size_buf));

    printf("Fileset '%s', path '%s' (recursive):\n", fs.fs_name, path);
    printf("  Files:      %llu (%s)\n", (unsigned long long)cnt.files,
           size_buf);
    printf("  Dirs:       %llu\n", (unsigned long long)cnt.dirs);
    printf("  Symlinks:   %llu\n", (unsigned long long)cnt.symlinks);
    printf("  Other:      %llu\n", (unsigned long long)cnt.other);
    printf("  Errors:     %llu\n", (unsigned long long)cnt.errors);
    printf("  Total:      %llu\n", (unsigned long long)(cnt.files +
           cnt.dirs + cnt.symlinks + cnt.other));

    advfs_domain_close(&domain);
    close_volumes(vols, nvdisks);
    return 0;
}

/* ================================================================
 * CLI entry point
 * ================================================================ */

static void usage(FILE *out, const char *prog)
{
    fprintf(out,
        "Usage: %s <mode> [--fileset <name>] <vdisk> [vdisk2 ...] [args]\n"
        "\n"
        "Read-only analysis tool for Tru64 AdvFS vdisk images.\n"
        "Multi-volume domains are supported: pass all member vdisks.\n"
        "\n"
        "Modes:\n"
        "  --info <vdisk> [vdisk2 ...]              Show domain metadata:\n"
        "                                           ODS version, domain ID,\n"
        "                                           volume size, BMT layout\n"
        "                                           and all filesets.\n"
        "  --scan-deleted <vdisk> [vdisk2 ...]      Scan all non-clone\n"
        "                                           filesets recursively for\n"
        "                                           deleted entries.\n"
        "  --list <vdisk> [vdisk2 ...] [path]       List a directory\n"
        "                                           (default: /) in the\n"
        "                                           first non-clone fileset,\n"
        "                                           ls -la style.\n"
        "                                           Deleted entries are\n"
        "                                           marked [DELETED].\n"
        "\n"
        "Options:\n"
        "  --fileset <name>          Select a specific fileset by name\n"
        "                            for --list and --scan-deleted.\n"
        "                            Clone filesets are allowed.\n"
        "  --partition <letter>      Read the BSD disklabel and use the\n"
        "                            given partition's offset and size\n"
        "                            (e.g. 'g' for usr).\n"
        "  --offset <bytes>          Manual byte offset into the first\n"
        "                            vdisk file (excludes --partition).\n"
        "  -r, --recursive           Recursive file count (with --list).\n"
        "  -h, --help                Show this help.\n"
        "  -V, --version             Show version.\n",
        prog);
}

int main(int argc, char *argv[])
{
    const char *prog = argv[0];

    if (argc < 2) {
        usage(stderr, prog);
        return 2;
    }

    const char *mode = argv[1];

    if (strcmp(mode, "-h") == 0 || strcmp(mode, "--help") == 0) {
        usage(stdout, prog);
        return 0;
    }
    if (strcmp(mode, "-V") == 0 || strcmp(mode, "--version") == 0) {
        printf("advfs-tool %s\n", ADVFS_TOOL_VERSION);
        return 0;
    }

    /*
     * Extract --fileset <name> from the remaining arguments and build a
     * filtered argument list without those two entries.  The filtered list
     * is what every mode-specific handler receives as its vdisk (and path)
     * arguments, so --fileset can appear before the vdisk path(s) without
     * confusing the is_regular_file() scan used by --list.
     */
    const char *fileset_name = NULL;
    const char *partition_letter = NULL;
    uint64_t volume_offset = 0;
    uint64_t volume_size = 0;
    int offset_given = 0;
    int recursive = 0;
    /* 256 slots: far more than any realistic argc value. */
    char *filtered[256];
    int n_filtered = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0 ||
            strcmp(argv[i], "--recursive") == 0) {
            recursive = 1;
            continue;
        }
        if (strcmp(argv[i], "--fileset") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "advfs-tool: --fileset requires a name\n");
                return 2;
            }
            fileset_name = argv[i + 1];
            i++;
            continue;
        }
        if (strcmp(argv[i], "--partition") == 0) {
            if (i + 1 >= argc || strlen(argv[i + 1]) != 1 ||
                argv[i + 1][0] < 'a' || argv[i + 1][0] > 'p') {
                fprintf(stderr, "advfs-tool: --partition requires a "
                        "letter a..p\n");
                return 2;
            }
            /* Disklabel read is deferred until the vdisk path is known. */
            partition_letter = argv[i + 1];
            i++;
            continue;
        }
        if (strcmp(argv[i], "--offset") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "advfs-tool: --offset requires a value\n");
                return 2;
            }
            /* Reject negative values explicitly: strtoull() would
             * silently wrap "-1" to 0xFFFFFFFFFFFFFFFF. Skip leading
             * whitespace to catch " -1" too. */
            const char *p = argv[i + 1];
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '-') {
                fprintf(stderr, "advfs-tool: --offset must not be "
                        "negative: '%s'\n", argv[i + 1]);
                return 2;
            }
            errno = 0;
            char *endp = NULL;
            volume_offset = strtoull(argv[i + 1], &endp, 0);
            if (errno == ERANGE || endp == argv[i + 1] || *endp != '\0') {
                fprintf(stderr, "advfs-tool: invalid --offset value "
                        "'%s'\n", argv[i + 1]);
                return 2;
            }
            offset_given = 1;
            i++;
            continue;
        }
        if (n_filtered < 256) {
            filtered[n_filtered++] = argv[i];
        }
    }

    if (partition_letter != NULL && offset_given) {
        fprintf(stderr, "advfs-tool: --partition and --offset are "
                "mutually exclusive\n");
        return 2;
    }

    /* Resolve --partition to a byte offset and size via the BSD
     * disklabel of the first vdisk path. */
    if (partition_letter != NULL) {
        if (n_filtered < 1) {
            fprintf(stderr, "advfs-tool: --partition requires a vdisk "
                    "argument\n");
            return 2;
        }
        int perr = advfs_volume_read_disklabel(filtered[0],
                                               partition_letter[0],
                                               &volume_offset,
                                               &volume_size);
        if (perr) {
            fprintf(stderr, "advfs-tool: cannot read partition '%c' "
                    "from disklabel: %s\n", partition_letter[0],
                    strerror(-perr));
            return 1;
        }
    }

    if (strcmp(mode, "--info") == 0) {
        if (n_filtered < 1) {
            fprintf(stderr, "advfs-tool: --info requires at least one "
                    "vdisk argument\n");
            return 2;
        }
        return cmd_info(filtered, n_filtered, volume_offset, volume_size);
    }

    if (strcmp(mode, "--scan-deleted") == 0) {
        if (n_filtered < 1) {
            fprintf(stderr, "advfs-tool: --scan-deleted requires at least "
                    "one vdisk argument\n");
            return 2;
        }
        return cmd_scan_deleted(filtered, n_filtered, volume_offset,
                                volume_size, fileset_name);
    }

    if (strcmp(mode, "--list") == 0) {
        if (n_filtered < 1) {
            fprintf(stderr, "advfs-tool: --list requires at least one "
                    "vdisk argument\n");
            return 2;
        }
        /*
         * Collect vdisk arguments (regular files) from the left of the
         * filtered list; the first argument that is NOT a regular file is
         * the optional AdvFS path.  This lets the caller pass multiple
         * vdisks followed by an optional virtual path:
         *   --list vol1.vdisk vol2.vdisk /usr/local
         * (--fileset has already been stripped from filtered[])
         */
        int nvdisks = 0;
        const char *list_path = "/";
        for (int i = 0; i < n_filtered; i++) {
            if (is_regular_file(filtered[i])) {
                nvdisks++;
            } else {
                list_path = filtered[i];
                break;
            }
        }
        if (nvdisks == 0) {
            fprintf(stderr, "advfs-tool: --list: first argument must be a "
                    "vdisk file\n");
            return 2;
        }
        if (recursive) {
            return cmd_list_recursive(filtered, nvdisks, volume_offset,
                                      volume_size, list_path, fileset_name);
        }
        return cmd_list(filtered, nvdisks, volume_offset, volume_size,
                        list_path, fileset_name);
    }

    fprintf(stderr, "advfs-tool: unknown mode '%s'\n", mode);
    usage(stderr, prog);
    return 2;
}
