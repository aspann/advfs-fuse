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
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Volume context -- holds the open fd and cached metadata. */
struct advfs_volume {
    int      fd;            /* file descriptor (O_RDONLY) */
    uint64_t part_offset;   /* byte offset into file (partition support) */
    uint64_t size_bytes;    /* usable size, clamped to the partition size */
    uint64_t page_count;    /* size_bytes / ADVFS_PGSZ */
    char     path[4096];    /* file path for diagnostics */
};

/* Read exactly 'len' bytes at 'offset' from the volume. Returns 0 or -errno.
 * The volume's partition offset is added transparently. */
static int vol_pread(advfs_volume_t *vol, void *buf, size_t len, uint64_t offset)
{
    /* Overflow-safe bounds check: "offset + len" could wrap for an
     * extreme offset, so compare against size_bytes - len instead. */
    if (len > vol->size_bytes || offset > vol->size_bytes - len) {
        return -EINVAL;
    }

    uint64_t abs_offset = vol->part_offset + offset;
    size_t total = 0;
    while (total < len) {
        ssize_t n = pread(vol->fd, (char *)buf + total,
                          len - total, (off_t)(abs_offset + total));
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

/* Format a byte count with an adaptive unit: MB below 1 GB, else GB. */
static void fmt_size_adaptive(uint64_t bytes, char *buf, size_t sz)
{
    if (bytes < 1024ULL * 1024ULL * 1024ULL) {
        snprintf(buf, sz, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else {
        snprintf(buf, sz, "%.1f GB",
                 (double)bytes / (1024.0 * 1024.0 * 1024.0));
    }
}

/* ================================================================
 * BSD disklabel support (Alpha / Tru64)
 *
 * On Alpha, the disklabel sits at byte 64 from the start of the
 * disk.  We only need the partition table to extract byte offsets.
 * ================================================================ */

#define BSD_DISKMAGIC       0x82564557u
#define BSD_LABEL_OFFSET    64    /* Alpha: byte 64 from disk start */
#define BSD_D_MAGIC_OFS     0     /* d_magic within label */
#define BSD_D_MAGIC2_OFS    132   /* d_magic2 within label */
#define BSD_D_SECSIZE_OFS   40    /* d_secsize within label */
#define BSD_D_CKSUM_OFS     136   /* d_checksum within label */
#define BSD_D_NPARTS_OFS    138   /* d_npartitions within label */
#define BSD_D_PARTS_OFS     148   /* partition table within label */
#define BSD_PART_ENTRY_SZ   16    /* bytes per partition entry */
#define BSD_MAXPARTITIONS   16

/* Read the BSD disklabel and return byte offset + size for a partition. */
int advfs_volume_read_disklabel(const char *path, char letter,
                                uint64_t *offset_out, uint64_t *size_out)
{
    if (!path || !offset_out || !size_out) {
        return -EINVAL;
    }

    int idx = letter - 'a';
    if (idx < 0 || idx >= BSD_MAXPARTITIONS) {
        advfs_err("disklabel: invalid partition letter '%c'", letter);
        return -EINVAL;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        int err = errno;
        advfs_err("disklabel: open %s: %s", path, strerror(err));
        return -err;
    }

    /* Read the label area (enough for header + 16 partition entries) */
    uint8_t label[BSD_D_PARTS_OFS + BSD_MAXPARTITIONS * BSD_PART_ENTRY_SZ];
    ssize_t n = pread(fd, label, sizeof(label), BSD_LABEL_OFFSET);
    if (n < 0) {
        int err = errno;
        advfs_err("disklabel: read %s: %s", path, strerror(err));
        close(fd);
        return -err;
    }
    if (n < (ssize_t)sizeof(label)) {
        advfs_err("disklabel: short read from %s", path);
        close(fd);
        return -EIO;
    }

    /* Verify both magic numbers */
    uint32_t m1, m2;
    memcpy(&m1, label + BSD_D_MAGIC_OFS, 4);
    memcpy(&m2, label + BSD_D_MAGIC2_OFS, 4);
    m1 = advfs_le32(m1);
    m2 = advfs_le32(m2);

    if (m1 != BSD_DISKMAGIC || m2 != BSD_DISKMAGIC) {
        advfs_err("disklabel: bad magic in %s (0x%08x / 0x%08x, "
                  "expected 0x%08x)", path, m1, m2, BSD_DISKMAGIC);
        close(fd);
        return -ENODATA;
    }

    /* Read sector size */
    uint32_t secsize;
    memcpy(&secsize, label + BSD_D_SECSIZE_OFS, 4);
    secsize = advfs_le32(secsize);
    if (secsize == 0) {
        secsize = 512;  /* safe default */
    }

    /* Check partition count */
    uint16_t nparts;
    memcpy(&nparts, label + BSD_D_NPARTS_OFS, 2);
    nparts = advfs_le16(nparts);

    /* Verify d_checksum: the XOR of all 16-bit words from the label
     * start through the last of d_npartitions entries (d_checksum
     * included) must be zero.  Warn only -- some imaging tools write
     * labels with a stale checksum. */
    size_t cksum_len = BSD_D_PARTS_OFS +
                       (size_t)nparts * BSD_PART_ENTRY_SZ;
    if (cksum_len > sizeof(label)) {
        cksum_len = sizeof(label);
    }
    uint16_t cksum = 0;
    for (size_t k = 0; k + 2 <= cksum_len; k += 2) {
        uint16_t word;
        memcpy(&word, label + k, 2);
        cksum ^= advfs_le16(word);
    }
    if (cksum != 0) {
        advfs_warn("disklabel: checksum mismatch in %s (xor residue "
                   "0x%04x), continuing anyway", path, cksum);
    }

    if (idx >= nparts) {
        advfs_err("disklabel: partition '%c' (idx %d) >= npartitions %u",
                  letter, idx, nparts);
        close(fd);
        return -ENOENT;
    }

    /* Parse the partition entry: [p_size(4), p_offset(4), ...] */
    const uint8_t *pent = label + BSD_D_PARTS_OFS + idx * BSD_PART_ENTRY_SZ;
    uint32_t p_size, p_offset;
    memcpy(&p_size,   pent + 0, 4);
    memcpy(&p_offset, pent + 4, 4);
    p_size   = advfs_le32(p_size);
    p_offset = advfs_le32(p_offset);

    if (p_size == 0) {
        advfs_err("disklabel: partition '%c' has zero size", letter);
        close(fd);
        return -ENOENT;
    }

    *offset_out = (uint64_t)p_offset * secsize;
    *size_out   = (uint64_t)p_size * secsize;

    char human[32];
    fmt_size_adaptive(*size_out, human, sizeof(human));
    advfs_info("disklabel: partition '%c' at sector %u (offset 0x%" PRIx64
               "), %u sectors (%s)",
               letter, p_offset, *offset_out, p_size, human);

    close(fd);
    return 0;
}

/* Open a vdisk file at a byte offset (size 0 = to EOF) and verify magic. */
int advfs_volume_open_at(const char *path, uint64_t offset, uint64_t size,
                         advfs_volume_t **vol_out)
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
    if (offset >= file_size) {
        advfs_err("%s: offset 0x%" PRIx64 " beyond file size 0x%" PRIx64,
                  path, offset, file_size);
        close(fd);
        return -EINVAL;
    }

    /* Usable bytes: to EOF, clamped to the partition size when given. */
    uint64_t usable = file_size - offset;
    if (size > 0 && size < usable) {
        usable = size;
    }

    /* The magic is at an absolute offset within the partition */
    if (usable < ADVFS_MAGIC_ABS_OFFSET + sizeof(uint32_t)) {
        advfs_err("%s: partition too small to contain AdvFS magic", path);
        close(fd);
        return -ENODATA;
    }

    /* Read and verify the magic number at partition_offset + magic_offset */
    uint32_t magic = 0;
    ssize_t n = pread(fd, &magic, sizeof(magic),
                      (off_t)(offset + ADVFS_MAGIC_ABS_OFFSET));
    if (n != (ssize_t)sizeof(magic)) {
        int err = (n < 0) ? errno : 0;
        advfs_err("%s: failed to read magic at offset 0x%" PRIx64 ": %s",
                  path, offset + ADVFS_MAGIC_ABS_OFFSET,
                  err ? strerror(err) : "short read");
        close(fd);
        return err ? -err : -EIO;
    }

    magic = advfs_le32(magic);
    if (magic != ADVFS_MAGIC) {
        advfs_err("%s: bad magic 0x%08x at offset 0x%" PRIx64
                  " (expected 0x%08x)", path, magic, offset, ADVFS_MAGIC);
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
    vol->part_offset = offset;
    vol->size_bytes = usable;
    vol->page_count = usable / ADVFS_PGSZ;
    strncpy(vol->path, path, sizeof(vol->path) - 1);

    if (offset > 0) {
        char human[32];
        fmt_size_adaptive(usable, human, sizeof(human));
        advfs_info("volume '%s' opened at offset 0x%" PRIx64
                   " (%s usable)", path, offset, human);
    }

    *vol_out = vol;
    return 0;
}

/* Open a vdisk file and verify AdvFS magic (offset 0, size to EOF). */
int advfs_volume_open(const char *path, advfs_volume_t **vol_out)
{
    return advfs_volume_open_at(path, 0, 0, vol_out);
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
