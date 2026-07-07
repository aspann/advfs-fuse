/*
 * volume.c -- raw vdisk I/O for advfs-fuse
 *
 * Provides page-aligned and block-aligned reads on a virtual disk
 * image file. Verifies the AdvFS magic number on open and can
 * detect the on-disk structure version (3 or 4).
 *
 * All reads use pread() so the file position is never modified,
 * making concurrent reads from multiple threads safe.
 *
 * Copyright (C) 2026 Armas Spann
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _XOPEN_SOURCE 500  /* pread() */

#include "volume.h"
#include "ods.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Volume context -- holds the open fd and cached metadata. */
struct advfs_volume {
    int      fd;            /* file descriptor (O_RDONLY) */
    uint64_t size_bytes;    /* total file size */
    uint64_t page_count;    /* size_bytes / ADVFS_PGSZ */
    char     path[4096];    /* file path for diagnostics */
};

/* Read exactly 'len' bytes at 'offset' from the volume. Returns 0 or -errno. */
static int vol_pread(advfs_volume_t *vol, void *buf, size_t len, uint64_t offset)
{
    if (offset + len > vol->size_bytes) {
        return -EINVAL;
    }

    size_t total = 0;
    while (total < len) {
        ssize_t n = pread(vol->fd, (char *)buf + total,
                          len - total, (off_t)(offset + total));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -errno;
        }
        if (n == 0) {
            return -EIO;  /* unexpected EOF */
        }
        total += (size_t)n;
    }
    return 0;
}

/* Open a vdisk file and verify AdvFS magic. */
int advfs_volume_open(const char *path, advfs_volume_t **vol_out)
{
    if (!path || !vol_out) {
        return -EINVAL;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        advfs_err("open %s: %s", path, strerror(errno));
        return -errno;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        int err = errno;
        advfs_err("fstat %s: %s", path, strerror(err));
        close(fd);
        return -err;
    }

    if (!S_ISREG(st.st_mode) && !S_ISBLK(st.st_mode)) {
        advfs_err("%s: not a regular file or block device", path);
        close(fd);
        return -EINVAL;
    }

    uint64_t file_size = (uint64_t)st.st_size;

    /* The magic is at an absolute offset; the volume must be large enough */
    if (file_size < ADVFS_MAGIC_ABS_OFFSET + sizeof(uint32_t)) {
        advfs_err("%s: too small to contain AdvFS magic", path);
        close(fd);
        return -ENODATA;
    }

    /* Read and verify the magic number */
    uint32_t magic = 0;
    ssize_t n = pread(fd, &magic, sizeof(magic), (off_t)ADVFS_MAGIC_ABS_OFFSET);
    if (n != (ssize_t)sizeof(magic)) {
        advfs_err("%s: failed to read magic: %s", path,
                  n < 0 ? strerror(errno) : "short read");
        close(fd);
        return n < 0 ? -errno : -EIO;
    }

    magic = advfs_le32(magic);
    if (magic != ADVFS_MAGIC) {
        advfs_err("%s: bad magic 0x%08x (expected 0x%08x)",
                  path, magic, ADVFS_MAGIC);
        close(fd);
        return -ENODATA;
    }

    /* Allocate and fill volume context */
    advfs_volume_t *vol = calloc(1, sizeof(*vol));
    if (!vol) {
        close(fd);
        return -ENOMEM;
    }

    vol->fd = fd;
    vol->size_bytes = file_size;
    vol->page_count = file_size / ADVFS_PGSZ;
    strncpy(vol->path, path, sizeof(vol->path) - 1);

    *vol_out = vol;
    return 0;
}

/* Close a volume and free resources. */
void advfs_volume_close(advfs_volume_t *vol)
{
    if (!vol) {
        return;
    }
    if (vol->fd >= 0) {
        close(vol->fd);
    }
    free(vol);
}

/* Read one 8 KB page. */
int advfs_volume_read_page(advfs_volume_t *vol, uint32_t page_num, void *buf)
{
    if (!vol || !buf) {
        return -EINVAL;
    }

    uint64_t offset = advfs_page_to_offset(page_num);
    return vol_pread(vol, buf, ADVFS_PGSZ, offset);
}

/* Read one 512-byte block. */
int advfs_volume_read_block(advfs_volume_t *vol, uint32_t block_num, void *buf)
{
    if (!vol || !buf) {
        return -EINVAL;
    }

    uint64_t offset = advfs_block_to_offset(block_num);
    return vol_pread(vol, buf, ADVFS_BLKSZ, offset);
}

/* Detect ODS version by reading RBMT/BMT page 0 header. */
int advfs_volume_detect_version(advfs_volume_t *vol)
{
    if (!vol) {
        return -EINVAL;
    }

    /*
     * The first RBMT (V4) or BMT (V3) page is at block
     * ADVFS_RESERVED_BLKS (= 32), which is page 2 of the volume.
     * We only need the 16-byte page header.
     */
    uint32_t page_num = ADVFS_RESERVED_BLKS / ADVFS_BLKS_PER_PG;
    uint64_t offset = advfs_page_to_offset(page_num);

    /* Read just the page header (first 16 bytes) */
    struct {
        uint32_t next_free_mcid;
        uint32_t next_free_pg;
        uint32_t free_mcell_cnt;
        uint32_t pg_id_ver;
    } hdr;

    int err = vol_pread(vol, &hdr, sizeof(hdr), offset);
    if (err) {
        advfs_err("failed to read RBMT/BMT page header: %s",
                  strerror(-err));
        return err;
    }

    uint32_t version = (advfs_le32(hdr.pg_id_ver) >> 27) & 0x1F;

    if (version < ADVFS_FIRST_VALID_VERSION ||
        version > ADVFS_LAST_VALID_VERSION) {
        advfs_err("unsupported ODS version %u", version);
        return -ENOTSUP;
    }

    return (int)version;
}

/* Return the volume size in pages. */
uint64_t advfs_volume_page_count(const advfs_volume_t *vol)
{
    return vol ? vol->page_count : 0;
}

/* Return the volume size in bytes. */
uint64_t advfs_volume_size(const advfs_volume_t *vol)
{
    return vol ? vol->size_bytes : 0;
}

/* Return the file path. */
const char *advfs_volume_path(const advfs_volume_t *vol)
{
    return vol ? vol->path : "";
}
