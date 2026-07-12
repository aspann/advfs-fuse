# Architecture

## Overview

Read-only FUSE driver and CLI forensics tool that presents AdvFS
volumes (raw vdisk files) as mountable Linux filesystems. No kernel
module -- runs entirely in userspace via FUSE. The CLI tool
(`advfs-tool`) works without FUSE for image analysis and deleted-file
scanning.

## AdvFS On-Disk Format (Summary)

Source: `bs_ods.h` (GPL), Hitchhiker's Guide v2.2

### Constants

- Block: 512 bytes
- Page: 16 blocks = 8192 bytes (8 KB)
- AdvFS magic: `0x11081953` at byte offset `0x55c` in the fake superblock
- Domain version 3 = Digital UNIX V4.x (BMT-based)
- Domain version 4 = Tru64 UNIX V5.x (RBMT-based)

Both versions are fully supported.

### Key Structures

```text
Volume
  +-- Fake Superblock (block 16, contains magic at +1372)
  +-- RBMT (Reserved BMT, V4 only, page 0)
  |     +-- cell 0: RBMT self (extent chain)
  |     +-- cell 1: SBM
  |     +-- cell 2: BFSDIR (root tag directory)
  |     +-- cell 3: Transaction log
  |     +-- cell 4: BMT
  |     +-- cell 5: Misc (fake superblock)
  |     +-- cell 6: RBMT extension (VD/DMN attrs)
  +-- BMT (Bitfile Metadata Table, page 2)
  |     +-- V3: cells 0-5 hold reserved metadata (no RBMT)
  |     +-- Array of 8 KB pages, 28 mcells per page
  |     +-- mcells contain typed records:
  |           +-- BSR_XTNTS (primary extent map)
  |           +-- BSR_SHADOW_XTNTS (V3 overflow extents)
  |           +-- BSR_XTRA_XTNTS (V4 overflow extents)
  |           +-- BSR_DMN_ATTR (domain attributes)
  |           +-- BSR_VD_ATTR (volume attributes)
  |           +-- BMTR_FS_STAT (type 255: inode data)
  +-- SBM (Storage Bitmap)
  |     +-- 8 KB pages, 1 bit per page, tracks free/allocated
  +-- Tag Directory
  |     +-- Maps tag numbers -> BMT cell locations
  |     +-- Root tag dir found via domain mutable attrs
  +-- Data Pages
  |     +-- Actual file content, addressed by extent maps
  +-- Frag File (fileset tag 1)
        +-- Sub-page file tails stored in 1 KB slots
        +-- frag_type: 1-7 KB; frag_slot: index in frag file
```

### Partition Offset (BSD Disklabel)

When an AdvFS domain resides in a partition of a raw disk image
(not a standalone vdisk), `advfs_volume_read_disklabel()` reads
the Alpha/BSD disklabel at byte 64 to locate the partition's
sector offset and size. `advfs_volume_open_at()` applies the byte
offset transparently to all subsequent I/O and clamps the usable
volume size to the partition size. The CLI exposes this as
`--partition <letter>` (auto-detect) or `--offset <bytes>`
(manual); the two options are mutually exclusive.

### Read Path

V3 (ODS version 3):

1. Open vdisk, verify magic
2. Read BMT page 0 (volume page 2), find BSR_DMN_ATTR
3. From domain attrs, get BFSDIR tag -> fileset list
4. Walk fileset tag directory to find the target file's tag
5. Look up tag in BMT -> mcell chain
6. Walk chain, find BSR_XTNTS -> BSR_SHADOW_XTNTS for extents
7. Read data pages from extent map
8. If file has a frag tail (size not page-aligned): read from
   frag file (tag 1) at the frag slot offset

V4 (ODS version 4):

1. Open vdisk, verify magic
2. Read RBMT page 0, build RBMT extent map from cell 0
3. Build BMT extent map from RBMT cell 4 (resolved through RBMT)
4. Find BSR_DMN_ATTR in RBMT cell 6
5. Same fileset/tag/file path as V3, but extent chains use
   BSR_XTRA_XTNTS instead of BSR_SHADOW_XTNTS

### Directory Format (V3 + V4)

AdvFS directories are stored as regular files. Each 8 KB data page
contains 16 x 512-byte directory blocks. Entries are variable-length
and do not cross block boundaries. Format is identical for V3 and V4.

On-disk entry layout (8-byte header):

```c
uint32_t tag_num       -- file's tag number (0 = deleted)
uint16_t entry_size    -- total size incl. name + padding + trailing tag
uint16_t name_count    -- name length without null terminator
char     name[]        -- null-terminated, padded to 4-byte boundary
adv_bf_tag_t tag       -- full tag (num + seq) at end of entry
```

Minimum entry size: 20 bytes (8 header + 4 padded name + 8 trailing tag).

Deleted entries: AdvFS zeroes only `tag_num` in the header. The
`entry_size`, `name`, and trailing `tag` survive -- exploited by
`--scan-deleted` for recovery.

### Extent Map Format

AdvFS extent descriptor arrays are boundary lists, not ranges.
Entry i covers pages [bs_page[i], bs_page[i+1]). Special values:

- ADVFS_XTNT_TERM (0xFFFFFFFF): end-of-map boundary
- ADVFS_PERM_HOLE (0xFFFFFFFE): clone copy-on-write hole

V3: BSR_SHADOW_XTNTS (type=6) for extent chains. Primary BSR_XTNTS
may be stubs (x_cnt=65535) with real extents in the shadow chain.

V4: BSR_XTRA_XTNTS (type=5) for extent chains. Primary BSR_XTNTS
carry real extents.

### Frag Files

Files smaller than 8 KB (or with a sub-page tail) store data in
the fileset's frag file (tag 1). BMTR_FS_STAT contains:

- `frag_slot`: 1 KB slot index within the frag file
- `frag_type`: frag size in KB (1-7; 0 = no frag)
- `frag_page_offset`: which file page the frag supplies

Files entirely in a frag have no extent map (BSR_XTNTS returns
ENODATA). Files with both extents and a frag have extent-mapped
full pages followed by a frag tail.

### Snapshots, Striping, Undelete

- **Snapshots/Clones**: BSR_BFS_ATTR contains clone_id, clone_cnt,
  orig_set_tag, next_clone_set_tag. PERM_HOLE extents indicate pages
  that haven't been copy-on-write'd. The clone fallback reads these
  pages from the original fileset via advfs_filedata_read_clone().
  Both file data and directory pages support clone fallback.
  Limited to one level of clone chain (clone-of-clone zero-fills).
- **Striping**: BSR_XTNTS type field = BSXMT_STRIPE (2) or
  BSXMT_APPEND (0). Multi-volume domains dispatch reads to the
  correct volume via vd_index. Extent maps carry per-extent
  volume ownership.
- **Undelete**: BMTR_FS_UNDEL_DIR (type=252) in directory mcell chains
  references a trashcan directory for deleted files. advfs_dir_read_full()
  exposes deleted entries via a separate callback.
- **Symlink inline data**: Short symlinks store their target in a
  BMTR_FS_DATA (type 254) mcell record, not in extents or frags.
  advfs_filedata_read() checks for inline data before extent resolution.

## Module Interaction

```text
                   +----------+     +-------------+
                   |  main.c  |     | advfs_tool.c|
                   |  (FUSE)  |     |   (CLI)     |
                   +----+-----+     +------+------+
                        |                  |
                   +----v-----+            |
                   | fuse_ops |            |
                   +----+-----+            |
                        |                  |
              +---------+-------+----------+
              |                 |
         +----v-----+      +----v-----+
         | filedata |      |   dir    |
         +----+-----+      +----+-----+
              |                 |
         +----v-----+      +----v-----+
         | fileset  |      |   bmt    |
         +----+-----+      +----+-----+
              |                 |
         +----v-----+      +----v-----+
         | extents  |      |  domain  |
         +----+-----+      +----+-----+
              |                 |
              +--------+--------+
                       |
                  +----v-----+
                  |  volume  |  raw page I/O
                  +----------+

Cross-cutting: ods.h (on-disk structures, used everywhere)
               util.c (logging, page math, BMT resolution)
```

## Error Handling

- All functions return error codes (0 = success, negative = errno).
- FUSE callbacks translate to appropriate errno values.
- Corrupt metadata -> EILSEQ or EIO, logged to stderr.
- Recycled mcells detected by comparing FS_STAT st_ino against
  the expected tag (scan-deleted uses this to mark "unknown" metadata).
- No assert() in production code -- handle gracefully.

## Thread Safety

- FUSE runs single-threaded by default (`-s` forced in main.c).
- Volume I/O is thread-safe (pread, no shared file position).
- Domain/fileset metadata is read-only after init.
- File data cache in fuse_ops.c is mutex-guarded.

## Tru64 cksum Compatibility

Tru64 V5.1B `cksum` uses a BSD CRC algorithm by default, NOT
POSIX 1003.2. Set `CMD_ENV=xpg4` before running `cksum` on
Tru64 to get output compatible with GNU coreutils / modern Linux.
