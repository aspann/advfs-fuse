/*
 * volume.h -- raw vdisk I/O for advfs-fuse
 *
 * Opens a virtual disk image file, verifies the AdvFS magic number,
 * and provides page/block-level read access using pread() for
 * thread safety.
 *
 * Copyright (C) 2026 Armas Spann
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef ADVFS_VOLUME_H
#define ADVFS_VOLUME_H

#include <stdint.h>

/* Opaque volume context -- allocated by advfs_volume_open(). */
typedef struct advfs_volume advfs_volume_t;

/* Open a vdisk file and verify AdvFS magic. Returns 0 on success, -errno on failure. */
int advfs_volume_open(const char *path, advfs_volume_t **vol_out);

/*
 * Open a vdisk file at a byte offset into the file.
 * Used when the vdisk is a partition inside a larger disk image.
 * The offset is applied to all subsequent reads transparently.
 * size limits the usable bytes to the partition size (0 = to EOF);
 * the effective size is min(file_size - offset, size).
 */
int advfs_volume_open_at(const char *path, uint64_t offset, uint64_t size,
                         advfs_volume_t **vol_out);

/*
 * Read the BSD/Tru64 disklabel from a raw disk image and return the
 * byte offset and size of the given partition letter ('a'..'p').
 * Returns 0 on success, -errno on failure.
 */
int advfs_volume_read_disklabel(const char *path, char letter,
                                uint64_t *offset_out, uint64_t *size_out);

/* Close a volume and free its context. */
void advfs_volume_close(advfs_volume_t *vol);

/* Read one 8 KB page at the given page number. buf must be >= ADVFS_PGSZ bytes. */
int advfs_volume_read_page(advfs_volume_t *vol, uint32_t page_num, void *buf);

/* Read one 512-byte block at the given block number. buf must be >= ADVFS_BLKSZ bytes. */
int advfs_volume_read_block(advfs_volume_t *vol, uint32_t block_num, void *buf);

/* Detect the on-disk structure version (3 or 4). Returns version or -errno. */
int advfs_volume_detect_version(advfs_volume_t *vol);

/* Return the volume size in 8 KB pages. */
uint64_t advfs_volume_page_count(const advfs_volume_t *vol);

/* Return the volume size in bytes. */
uint64_t advfs_volume_size(const advfs_volume_t *vol);

/* Return the file path of the opened volume. */
const char *advfs_volume_path(const advfs_volume_t *vol);

#endif /* ADVFS_VOLUME_H */
