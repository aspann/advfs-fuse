# advfs-fuse

Read-only FUSE driver and forensics toolkit for AdvFS filesystems
from DEC Alpha / Tru64 UNIX.

Mounts raw AdvFS vdisk images as local filesystems on modern Linux.
No kernel module -- runs entirely in userspace via FUSE.
Includes a standalone CLI tool for image analysis and deleted-file
scanning that works without FUSE.

No prior Linux FUSE driver or kernel module for AdvFS existed
despite the on-disk format source being available under GPL v2
since 2008.

## Supported Formats

- **ODS v3** -- Digital UNIX V4.x (BMT-based, BSR_SHADOW_XTNTS)
- **ODS v4** -- Tru64 UNIX V5.x (RBMT-based, BSR_XTRA_XTNTS)

Both single-volume and multi-volume (striped) domains are supported;
striped files are de-interleaved (pass all member vdisks). Only when
member volumes are missing does reading fall back to sequential
column order with a warning.

## Building

```sh
make            # builds advfs-tool always, advfs-fuse when libfuse available
make tool       # builds only advfs-tool (no FUSE dependency)
make fuse       # builds only advfs-fuse (requires libfuse-dev)
make test       # runs the test suite
make install    # installs to /usr/local/bin (override with PREFIX= DESTDIR=)
```

Requirements:

- C11 compiler (GCC or Clang)
- `libfuse3-dev` or `libfuse-dev` for the FUSE binary (auto-detected)
- `advfs-tool` has no external dependencies

Gentoo:

```sh
emake DESTDIR="${D}" PREFIX="${EPREFIX}/usr" install
```

## Usage

### FUSE mount

```sh
advfs-fuse <vdisk> <mountpoint>
advfs-fuse vol1.vdisk vol2.vdisk <mountpoint>   # multi-volume
advfs-fuse --fileset clone_snap <vdisk> <mountpoint>  # mount a clone
```

Mounts the first non-clone fileset as a read-only filesystem.
Multiple vdisk paths for striped/multi-volume domains.
`--fileset` selects a specific fileset (e.g. a clone snapshot).
Standard FUSE options (`-d`, `-f`, `-s`) are passed through.

### CLI tool

```sh
advfs-tool --info <vdisk> [vdisk2 ...]                  # metadata
advfs-tool --list <vdisk> [path]                        # ls -la
advfs-tool --list --fileset clone_snap <vdisk> [path]   # list clone
advfs-tool --scan-deleted <vdisk>                       # find deleted
```

`--info` shows ODS version, domain ID, volume geometry, BMT/RBMT
layout, and fileset inventory with entry counts.

`--scan-deleted` walks all filesets recursively, recovers deleted
entry names from the directory blocks (AdvFS zeroes only the header
tag on deletion; the name and trailing tag survive), and probes
BMTR_FS_STAT for file size, type, and deletion timestamp.

## Architecture

| Module       | Purpose                                          |
|--------------|--------------------------------------------------|
| volume.c     | Raw vdisk I/O, magic detection, ODS version      |
| bmt.c        | BMT/RBMT page reading, mcell record walking       |
| domain.c     | Domain discovery, BMT/RBMT extent map bootstrap   |
| extents.c    | Extent map resolution (primary + shadow + extra)  |
| fileset.c    | Fileset enumeration, tag-dir extent resolution    |
| dir.c        | Directory parsing (V3/V4), deleted entry support  |
| filedata.c   | File data reading (frags, extents, sparse holes)  |
| fuse_ops.c   | FUSE callbacks (getattr, readdir, read, readlink) |
| main.c       | FUSE CLI, volume/domain init, fuse_main           |
| advfs_tool.c | Standalone CLI (--info, --list, --scan-deleted)   |
| ods.h        | On-disk structure defs (from GPL bs_ods.h)        |
| util.c       | Logging, page/block math, BMT page resolution     |

## Implementation Notes

- AdvFS magic: `0x11081953` at block 16, byte offset 1372
- BMT is not contiguous -- uses its own extent map (cell 0)
- ODS v3: no RBMT, BMT at volume page 2, cells: BMT=0 SBM=1
  BFSDIR=2 FTXLOG=3
- ODS v4: separate RBMT at page 0, cells: RBMT=0 SBM=1 BFSDIR=2
  FTXLOG=3 BMT=4 MISC=5 RBMT_EXT=6
- V3 extent chains: BSR_SHADOW_XTNTS (type 6)
- V4 extent chains: BSR_XTRA_XTNTS (type 5)
- Record header: 4-byte bitfield (bCnt:16, type:8, version:8),
  bCnt includes the header
- Sub-page files stored in frag file (fileset tag 1), 1-7 KB slots
- Tru64 `cksum` uses a BSD algorithm by default; set `CMD_ENV=xpg4`
  for POSIX 1003.2 CRC output

## Test Suite

```sh
make test
```

- Volume detection, domain discovery, fileset enumeration,
  directory parsing, extent resolution, stripe detection
  (V3 + V4)
- File data reading with POSIX cksum verification against
  Tru64 reference values (V3 + V4)
- Clone fallback verification (PERM_HOLE page recovery)
- Synthetic edge-case handling

Test images created on Tru64 UNIX V5.1B running under an
emulated AlphaServer ES40 (AXPbox).

## Known Limitations

- **Clone chains**: clone-of-clone (nested snapshots) limited to
  one level of fallback; deeper chains zero-fill with a warning.
- **Indexed directories**: V4 B-tree indexed directories are not
  supported; linear directory scanning is used for all versions.
- **Write support**: read-only by design; no write, mkdir, or
  unlink operations.

## References

- [AdvFS GPL Source (TheSledgeHammer/AdvFS)][advfs-gh]
- [AdvFS Gen2 Source (HP, 2008)][advfs-sf] -- GPL v2
- Hitchhiker's Guide to AdvFS v2.2 (internal DEC/HP document)
- `bs_ods.h` -- on-disk structure defs (GPL, adapted in `src/ods.h`)

[advfs-gh]: https://github.com/TheSledgeHammer/AdvFS
[advfs-sf]: https://sourceforge.net/projects/advfs/

## License

GPL-2.0-only

Derived from Hewlett-Packard's GPL v2 AdvFS source release (2008).
