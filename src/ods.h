/*
 * ods.h -- AdvFS on-disk structure definitions for Linux
 *
 * Derived from the GPL-licensed bs_ods.h and bs_public.h from the
 * Tru64 UNIX AdvFS kernel source (Digital Equipment Corporation,
 * Hewlett-Packard Development Company).
 *
 * Adapted for Linux/x86_64: OSF/1 types replaced with C11 stdint
 * types. Only definitions needed for READ-ONLY access are included.
 *
 * Reference: TheSledgeHammer/AdvFS on GitHub
 * Original:  kernel/msfs/msfs/bs_ods.h, bs_public.h
 *
 * Copyright (C) 2026 Armas Spann
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef ADVFS_ODS_H
#define ADVFS_ODS_H

#include <stdint.h>

/* ================================================================
 * Fundamental constants
 * ================================================================ */

#define ADVFS_PGSZ            8192      /* page size in bytes (8 KB) */
#define ADVFS_BLKSZ           512       /* disk block size in bytes */
#define ADVFS_BLKS_PER_PG     (ADVFS_PGSZ / ADVFS_BLKSZ)  /* 16 */
#define ADVFS_MAGIC           0x11081953u
#define ADVFS_MAGIC_OFFSET    1372      /* byte offset within fake superblock */
#define ADVFS_FAKE_SB_BLK     16        /* fake superblock starts at block 16 */
#define ADVFS_RESERVED_BLKS   32        /* first usable block for metadata */

/* Absolute byte offset of the magic number from volume start */
#define ADVFS_MAGIC_ABS_OFFSET \
    ((uint64_t)ADVFS_FAKE_SB_BLK * ADVFS_BLKSZ + ADVFS_MAGIC_OFFSET)

/* ================================================================
 * ODS version constants
 * ================================================================ */

#define ADVFS_ODS_V3                3   /* Digital UNIX V4.x */
#define ADVFS_ODS_V4                4   /* Tru64 UNIX V5.x */
#define ADVFS_FIRST_VALID_VERSION   3
#define ADVFS_LAST_VALID_VERSION    4

/* First version with the Reserved BMT (RBMT) */
#define ADVFS_FIRST_RBMT_VERSION    4

/* First version with extent info in primary mcell of non-reserved files */
#define ADVFS_FIRST_XTNT_IN_PRIM_MCELL_VERSION  4

/* First version with directory indexing */
#define ADVFS_FIRST_INDEXED_DIRS_VERSION        4

/* ================================================================
 * Reserved metadata file indices
 *
 * V4 has a separate RBMT; V3 uses the BMT for everything.
 * The index determines which cell in RBMT/BMT page 0 holds
 * each metadata file's primary mcell.
 * ================================================================ */

/* V4 indices */
#define ADVFS_BFM_RBMT        0   /* Reserved Bitfile Metadata Table */
#define ADVFS_BFM_SBM         1   /* Storage Bitmap */
#define ADVFS_BFM_BFSDIR      2   /* Bitfile Set Directory */
#define ADVFS_BFM_FTXLOG      3   /* Transaction Log */
#define ADVFS_BFM_BMT         4   /* Bitfile Metadata Table */
#define ADVFS_BFM_MISC        5   /* Fake superblock, boot blocks */
#define ADVFS_BFM_RBMT_EXT    6   /* RBMT extension mcell */
#define ADVFS_BFM_RSVD_CELLS  7   /* V4 reserved cell count */

/* V3 indices (BMT_V3 is at 0, no RBMT) */
#define ADVFS_BFM_BMT_V3      0
#define ADVFS_BFM_SBM_V3      1
#define ADVFS_BFM_BFSDIR_V3   2
#define ADVFS_BFM_FTXLOG_V3   3
#define ADVFS_BFM_BMT_EXT_V3  4
#define ADVFS_BFM_MISC_V3     5
#define ADVFS_BFM_RSVD_CELLS_V3  6

/* ================================================================
 * BMT record type constants
 *
 * Each typed record in an mcell is identified by its type field
 * in the record header (adv_rec_hdr_t).
 * ================================================================ */

#define ADVFS_BSR_NIL               0   /* nil/unused -- terminates record list */
#define ADVFS_BSR_XTNTS             1   /* primary extent map header */
#define ADVFS_BSR_ATTR              2   /* bitfile attributes */
#define ADVFS_BSR_VD_ATTR           3   /* virtual disk attributes */
#define ADVFS_BSR_DMN_ATTR          4   /* domain attributes (permanent) */
#define ADVFS_BSR_XTRA_XTNTS       5   /* overflow extent descriptors */
#define ADVFS_BSR_SHADOW_XTNTS     6   /* shadow/stripe extent descriptors */
#define ADVFS_BSR_MCELL_FREE_LIST   7   /* mcell free list head */
#define ADVFS_BSR_BFS_ATTR          8   /* bitfile set (fileset) attributes */
#define ADVFS_BSR_VD_IO_PARAMS      9   /* I/O tuning parameters */
#define ADVFS_BSR_DEF_DEL_MCELL_LIST 14 /* deferred delete list */
#define ADVFS_BSR_DMN_MATTR        15   /* domain mutable attributes */
#define ADVFS_BSR_BF_INHERIT_ATTR  16   /* inheritable attributes */
#define ADVFS_BSR_BFS_QUOTA_ATTR   18   /* fileset quota attributes */
#define ADVFS_BSR_PROPLIST_HEAD    19   /* property list header */
#define ADVFS_BSR_PROPLIST_DATA    20   /* property list data */
#define ADVFS_BSR_DMN_TRANS_ATTR   21   /* domain transaction attributes */
#define ADVFS_BSR_DMN_SS_ATTR      22   /* vfast (SmartStor) attributes */
#define ADVFS_BSR_DMN_FREEZE_ATTR  23   /* domain freeze attributes */
#define ADVFS_BSR_MAX              23   /* highest record type */

/* Filesystem-level BMT record types (counted down from 255) */
#define ADVFS_BMTR_FS_STAT         255  /* filesystem stat (inode data) */
#define ADVFS_BMTR_FS_DATA         254  /* filesystem data */
#define ADVFS_BMTR_FS_UNDEL_DIR    252  /* undelete directory */
#define ADVFS_BMTR_FS_TIME         251  /* additional timestamps */
#define ADVFS_BMTR_FS_INDEX_FILE   250  /* directory index file */
#define ADVFS_BMTR_FS_DIR_INDEX    249  /* directory index pointer */

/* Extent descriptor terminators */
#define ADVFS_XTNT_TERM     ((uint32_t)-1)  /* end of extent list */
#define ADVFS_PERM_HOLE     ((uint32_t)-2)  /* permanent hole (V4 clones) */

/* Extent map type values (bsXtntMapTypeT) */
#define ADVFS_BSXMT_APPEND  0   /* normal append-only file */
#define ADVFS_BSXMT_STRIPE  2   /* striped file */

/* Bitfile state values (bfStatesT) */
#define ADVFS_BSRA_INVALID   0
#define ADVFS_BSRA_CREATING  1
#define ADVFS_BSRA_DELETING  2
#define ADVFS_BSRA_VALID     3

/* Tag directory constants */
#define ADVFS_BS_TD_IN_USE     0x8000  /* high bit of seq = slot in use */
#define ADVFS_BS_TD_DEAD_SLOT  0x7FFF  /* permanently unavailable */

/* Size constants */
#define ADVFS_BS_SET_NAME_SZ     32
#define ADVFS_BS_FS_CONTEXT_SZ    8
#define ADVFS_BS_CLIENT_AREA_SZ   4

/* ================================================================
 * On-disk structure definitions
 *
 * Layout notes:
 * - Alpha (Tru64) and x86_64 (Linux) are both LITTLE-ENDIAN.
 * - The original compiler used NATURAL ALIGNMENT (no pragma pack).
 * - All struct fields here are <= 4-byte aligned, so natural alignment
 *   on x86_64 matches the original Alpha layout exactly.
 * - Bitfield packing: we store bitfield containers as plain uint32_t
 *   and provide extraction macros, avoiding compiler-specific
 *   bitfield layout assumptions.
 * - _Static_assert checks verify each structure's compiled size
 *   matches the on-disk expectation.
 * ================================================================ */

/* ----------------------------------------------------------------
 * Bitfile mcell ID -- page + cell packed in one uint32_t.
 *
 * On-disk bit layout (little-endian uint32_t):
 *   bits [4:0]  = cell number (0-27)
 *   bits [31:5] = page number (0-134217727)
 * ---------------------------------------------------------------- */
typedef struct {
    uint32_t raw;
} adv_mcell_id_t;

#define ADV_MCID_CELL(id)   ((id).raw & 0x1Fu)
#define ADV_MCID_PAGE(id)   (((id).raw >> 5) & 0x07FFFFFFu)
#define ADV_MCID_MAKE(pg, cl) \
    ((adv_mcell_id_t){ .raw = (((pg) & 0x07FFFFFFu) << 5) | ((cl) & 0x1Fu) })
#define ADV_MCID_NIL        ((adv_mcell_id_t){ .raw = 0 })

_Static_assert(sizeof(adv_mcell_id_t) == 4, "adv_mcell_id_t size");

/* ----------------------------------------------------------------
 * Bitfile tag -- unique file identifier within a fileset.
 *
 * num: 1-based for regular files; negative for reserved metadata.
 * seq: monotonically increasing; high bit set when slot is in use.
 * ---------------------------------------------------------------- */
typedef struct {
    uint32_t num;
    uint32_t seq;
} adv_bf_tag_t;

_Static_assert(sizeof(adv_bf_tag_t) == 8, "adv_bf_tag_t size");

/* ----------------------------------------------------------------
 * Domain/volume ID -- timestamp-based unique identifier.
 *
 * Originally typedef'd as struct timeval (two 32-bit ints on Alpha).
 * Total size: 64 bits as documented in bs_public.h.
 * ---------------------------------------------------------------- */
typedef struct {
    int32_t id_sec;
    int32_t id_usec;
} adv_id_t;

_Static_assert(sizeof(adv_id_t) == 8, "adv_id_t size");

/* ----------------------------------------------------------------
 * Bitfile set (fileset) ID = domain ID + tag directory tag.
 * ---------------------------------------------------------------- */
typedef struct {
    adv_id_t     domain_id;
    adv_bf_tag_t dir_tag;
} adv_bf_set_id_t;

_Static_assert(sizeof(adv_bf_set_id_t) == 16, "adv_bf_set_id_t size");

/* ----------------------------------------------------------------
 * Fragment group list head (used in fileset attributes).
 * ---------------------------------------------------------------- */
typedef struct {
    uint32_t first_free_grp;
    uint32_t last_free_grp;
} adv_frag_grp_t;

_Static_assert(sizeof(adv_frag_grp_t) == 8, "adv_frag_grp_t size");

/* ----------------------------------------------------------------
 * BMT record header -- precedes each typed record in an mcell.
 *
 * Packed bitfield in a single uint32_t (4 bytes):
 *   bits [15:0]  = bCnt    (byte count INCLUDING this 4-byte header)
 *   bits [23:16] = type    (ADVFS_BSR_* constant)
 *   bits [31:24] = version
 *
 * bCnt INCLUDES the 4-byte header. Data starts at header + 4.
 * Next record at: round_up(bCnt, 4) from start of this header.
 * ---------------------------------------------------------------- */
typedef struct {
    uint32_t raw;
} adv_rec_hdr_t;

#define ADV_REC_BCNT(r)     ((r).raw & 0xFFFFu)
#define ADV_REC_TYPE(r)     (((r).raw >> 16) & 0xFFu)
#define ADV_REC_VERSION(r)  (((r).raw >> 24) & 0xFFu)

/* Is this a nil (terminator) record? */
#define ADV_REC_IS_NIL(r)   (ADV_REC_TYPE(r) == ADVFS_BSR_NIL)

/* Byte offset from start of header to start of next record */
#define ADV_REC_NEXT_OFF(bcnt)  (((bcnt) + 3u) & ~3u)

/* Size of the record header */
#define ADVFS_REC_HDR_SZ    4

/* Data portion size (bCnt minus header) */
#define ADV_REC_DATA_SZ(r)  (ADV_REC_BCNT(r) > ADVFS_REC_HDR_SZ ? ADV_REC_BCNT(r) - ADVFS_REC_HDR_SZ : 0)

_Static_assert(sizeof(adv_rec_hdr_t) == 4, "adv_rec_hdr_t size");

/* ----------------------------------------------------------------
 * Mcell -- metadata cell.
 *
 * The fundamental unit of metadata storage. Each mcell is assigned
 * to one bitfile (identified by tag + bf_set_tag) and contains a
 * sequence of typed records in the records[] area.
 *
 * Mcells can be chained via next_mcid/next_vd_index.
 *
 * sizeof(adv_mcell_t) = 292 bytes exactly.
 * ---------------------------------------------------------------- */

/* Size of the record data area within one mcell */
#define ADVFS_BSC_R_SZ \
    (292 - sizeof(adv_mcell_id_t) - 2 * sizeof(adv_bf_tag_t) \
     - 2 * sizeof(uint16_t))
/* = 292 - 4 - 16 - 4 = 268 */

/* Usable space after accounting for two record headers (one for the
 * first record, one for the nil terminator).
 * Record header is 4 bytes (adv_rec_hdr_t). */
#define ADVFS_USABLE_MCELL_SPACE \
    (ADVFS_BSC_R_SZ - 2 * ADVFS_REC_HDR_SZ)
/* = 268 - 8 = 260 */

typedef struct {
    adv_mcell_id_t next_mcid;       /* link to next mcell */
    uint16_t       next_vd_index;   /* vd index of next mcell */
    uint16_t       link_segment;    /* segment in chain (0-based) */
    adv_bf_tag_t   tag;             /* tag this mcell serves */
    adv_bf_tag_t   bf_set_tag;      /* fileset this mcell belongs to */
    char           records[ADVFS_BSC_R_SZ]; /* typed record data */
} adv_mcell_t;

_Static_assert(sizeof(adv_mcell_t) == 292, "adv_mcell_t size");

/* ----------------------------------------------------------------
 * BMT/RBMT page -- one 8 KB page of metadata cells.
 *
 * Page header fields:
 *   pg_id_ver packs two values in one uint32_t:
 *     bits [26:0]  = pageId      (page number within this BMT/RBMT)
 *     bits [31:27] = megaVersion (ODS version: 3 or 4)
 *
 * Each page holds ADVFS_BSPG_CELLS mcells.
 * Total size: sizeof(adv_bmt_pg_t) == ADVFS_PGSZ == 8192.
 * ---------------------------------------------------------------- */

#define ADVFS_BSPG_CELLS \
    ((ADVFS_PGSZ - (3 * sizeof(uint32_t) + sizeof(adv_mcell_id_t))) \
     / sizeof(adv_mcell_t))
/* = (8192 - 16) / 292 = 28 */

/* Last mcell on RBMT page is reserved for RBMT itself */
#define ADVFS_RBMT_RSVD_CELL  (ADVFS_BSPG_CELLS - 1)  /* = 27 */

typedef struct {
    adv_mcell_id_t next_free_mcid;   /* next free mcell on page */
    uint32_t       next_free_pg;     /* next page in mcell free list */
    uint32_t       free_mcell_cnt;   /* free mcells on this page */
    uint32_t       pg_id_ver;        /* pageId:27 | megaVersion:5 */
    adv_mcell_t    mcells[ADVFS_BSPG_CELLS];
} adv_bmt_pg_t;

#define ADV_BMT_PG_ID(pg)       ((pg).pg_id_ver & 0x07FFFFFFu)
#define ADV_BMT_PG_VERSION(pg)  (((pg).pg_id_ver >> 27) & 0x1Fu)

_Static_assert(sizeof(adv_bmt_pg_t) == ADVFS_PGSZ, "adv_bmt_pg_t size");

/* ----------------------------------------------------------------
 * Extent descriptor -- maps bitfile page to disk block.
 * ---------------------------------------------------------------- */
typedef struct {
    uint32_t bs_page;   /* bitfile page number */
    uint32_t vd_blk;    /* disk block (XTNT_TERM = end, PERM_HOLE = hole) */
} adv_xtnt_t;

_Static_assert(sizeof(adv_xtnt_t) == 8, "adv_xtnt_t size");

/* ----------------------------------------------------------------
 * Delete link -- doubly linked deferred-delete list entries.
 * ---------------------------------------------------------------- */
typedef struct {
    adv_mcell_id_t next;
    adv_mcell_id_t prev;
} adv_del_link_t;

_Static_assert(sizeof(adv_del_link_t) == 8, "adv_del_link_t size");

/* ----------------------------------------------------------------
 * Delete restart -- tracks progress of large bitfile deletion.
 *
 * 2 bytes of padding between vd_index and xtnt_index match the
 * natural alignment the Alpha compiler inserted.
 * ---------------------------------------------------------------- */
typedef struct {
    adv_mcell_id_t mcid;
    uint16_t       vd_index;
    uint16_t       _pad;
    uint32_t       xtnt_index;
    uint32_t       offset;
    uint32_t       blocks;
} adv_del_rst_t;

_Static_assert(sizeof(adv_del_rst_t) == 20, "adv_del_rst_t size");

/* ----------------------------------------------------------------
 * Primary extent info -- first 2 extents, stored in extent header.
 * ---------------------------------------------------------------- */
#define ADVFS_BMT_XTNTS  2

typedef struct {
    uint16_t    mcell_cnt;
    uint16_t    x_cnt;
    adv_xtnt_t  xtnts[ADVFS_BMT_XTNTS];
} adv_prim_xtnt_t;

_Static_assert(sizeof(adv_prim_xtnt_t) == 20, "adv_prim_xtnt_t size");

/* ----------------------------------------------------------------
 * Extent header record (BSR_XTNTS = 1).
 *
 * The primary extent record for a bitfile. Contains links to
 * overflow mcells (via chain_vd_index/chain_mcid) and the first
 * two extent descriptors.
 * ---------------------------------------------------------------- */
typedef struct {
    uint32_t        type;            /* ADVFS_BSXMT_APPEND or _STRIPE */
    uint32_t        chain_vd_index;  /* vd of next extent mcell */
    adv_mcell_id_t  chain_mcid;      /* mcell of next extent record */
    uint32_t        rsvd1;
    adv_mcell_id_t  rsvd2;
    uint32_t        blks_per_page;   /* blocks per bitfile page */
    uint32_t        segment_size;    /* stripe segment min pages */
    adv_del_link_t  del_link;        /* deferred delete list */
    adv_del_rst_t   del_rst;         /* delete progress tracker */
    adv_prim_xtnt_t first_xtnt;      /* first extent descriptors */
} adv_xtnt_rec_t;

_Static_assert(sizeof(adv_xtnt_rec_t) == 76, "adv_xtnt_rec_t size");

/* ----------------------------------------------------------------
 * Extra extent record (BSR_XTRA_XTNTS = 5).
 *
 * Overflow extent descriptors when the primary record runs out
 * of space. Holds 32 extent descriptors per mcell.
 * ---------------------------------------------------------------- */
#define ADVFS_BMT_XTRA_XTNTS \
    ((ADVFS_BSC_R_SZ - 2 * ADVFS_REC_HDR_SZ \
      - 2 * sizeof(uint16_t)) / sizeof(adv_xtnt_t))
/* = (268 - 8 - 4) / 8 = 32 */

typedef struct {
    uint16_t    blks_per_page;
    uint16_t    x_cnt;
    adv_xtnt_t  xtnts[ADVFS_BMT_XTRA_XTNTS];
} adv_xtra_xtnt_rec_t;

_Static_assert(sizeof(adv_xtra_xtnt_rec_t) == 260,
               "adv_xtra_xtnt_rec_t size");

/* ----------------------------------------------------------------
 * Shadow extent record (BSR_SHADOW_XTNTS = 6).
 *
 * Used for striped files. Holds 31 extent descriptors.
 * ---------------------------------------------------------------- */
#define ADVFS_BMT_SHADOW_XTNTS \
    ((ADVFS_BSC_R_SZ - 2 * ADVFS_REC_HDR_SZ \
      - sizeof(uint16_t) - 3 * sizeof(uint16_t)) / sizeof(adv_xtnt_t))
/* = (268 - 8 - 2 - 6) / 8 = 31 */

typedef struct {
    uint16_t    alloc_vd_index;   /* preferred allocation volume */
    uint16_t    mcell_cnt;
    uint16_t    blks_per_page;
    uint16_t    x_cnt;
    adv_xtnt_t  xtnts[ADVFS_BMT_SHADOW_XTNTS];
} adv_shadow_xtnt_rec_t;

_Static_assert(sizeof(adv_shadow_xtnt_rec_t) == 256,
               "adv_shadow_xtnt_rec_t size");

/* ----------------------------------------------------------------
 * Domain attributes record (BSR_DMN_ATTR = 4).
 *
 * Permanent (immutable after creation) domain attributes.
 * ---------------------------------------------------------------- */
typedef struct {
    adv_id_t     bf_domain_id;    /* unique domain identifier */
    uint32_t     max_vds;         /* maximum virtual disk count */
    adv_bf_tag_t bf_set_dir_tag;  /* root fileset tag directory */
} adv_dmn_attr_t;

_Static_assert(sizeof(adv_dmn_attr_t) == 20, "adv_dmn_attr_t size");

/* ----------------------------------------------------------------
 * Domain mutable attributes record (BSR_DMN_MATTR = 15).
 *
 * Can be updated at runtime. The disk with the highest seq_num
 * holds the current copy.
 * ---------------------------------------------------------------- */
typedef struct {
    uint32_t     seq_num;
    adv_bf_tag_t del_pending_bf_set;
    uint32_t     uid;
    uint32_t     gid;
    uint32_t     mode;
    uint16_t     vd_cnt;
    uint16_t     recovery_failed;
    adv_bf_tag_t bf_set_dir_tag;   /* root fileset tag directory */
    adv_bf_tag_t ftx_log_tag;      /* transaction log tag */
    uint32_t     ftx_log_pgs;      /* pages in transaction log */
} adv_dmn_mattr_t;

_Static_assert(sizeof(adv_dmn_mattr_t) == 48, "adv_dmn_mattr_t size");

/* ----------------------------------------------------------------
 * Virtual disk attributes record (BSR_VD_ATTR = 3).
 *
 * Per-volume metadata: size, cluster size, BMT extent settings.
 * ---------------------------------------------------------------- */
typedef struct {
    adv_id_t  vd_mnt_id;        /* last mount ID */
    uint32_t  state;             /* 0=virgin, 1=mounted, 2=dismounted */
    uint16_t  vd_index;          /* this volume's index in the domain */
    uint16_t  _pad;              /* originally jays_new_field */
    uint32_t  vd_blk_cnt;        /* total blocks on this volume */
    uint32_t  stg_cluster;       /* blocks per SBM bit */
    uint32_t  max_pg_sz;         /* largest page size on this vd */
    uint32_t  bmt_xtnt_pgs;      /* pages per BMT extent */
    uint32_t  service_class;     /* service class provided */
} adv_vd_attr_t;

_Static_assert(sizeof(adv_vd_attr_t) == 36, "adv_vd_attr_t size");

/* ----------------------------------------------------------------
 * Bitfile client attributes -- user/filesystem changeable.
 * ---------------------------------------------------------------- */
typedef struct {
    uint32_t     data_safety;
    uint32_t     req_services;
    uint32_t     opt_services;
    int32_t      extend_size;
    int32_t      client_area[ADVFS_BS_CLIENT_AREA_SZ];
    int32_t      rsvd1;
    int32_t      rsvd2;
    adv_bf_tag_t acl;
    int32_t      rsvd_sec1;
    int32_t      rsvd_sec2;
    int32_t      rsvd_sec3;
} adv_bf_cl_attr_t;

_Static_assert(sizeof(adv_bf_cl_attr_t) == 60, "adv_bf_cl_attr_t size");

/* ----------------------------------------------------------------
 * Bitfile attributes record (BSR_ATTR = 2).
 *
 * Core metadata for a bitfile: state, page size, clone info.
 * ---------------------------------------------------------------- */
typedef struct {
    uint32_t          state;
    uint32_t          bf_pg_sz;
    uint32_t          transition_id;
    uint32_t          clone_id;
    uint32_t          clone_cnt;
    uint32_t          max_clone_pgs;
    int16_t           delete_with_clone;
    int16_t           out_of_sync_clone;
    adv_bf_cl_attr_t  cl;
} adv_bf_attr_t;

_Static_assert(sizeof(adv_bf_attr_t) == 88, "adv_bf_attr_t size");

/* ----------------------------------------------------------------
 * Bitfile set (fileset) attributes record (BSR_BFS_ATTR = 8).
 *
 * Describes one fileset within a domain. Each fileset is an
 * independent namespace with its own tag directory.
 * ---------------------------------------------------------------- */
#define ADVFS_BFS_FRAG_MAX  8

typedef struct {
    adv_bf_set_id_t bf_set_id;
    adv_bf_tag_t    frag_bf_tag;
    adv_bf_tag_t    next_clone_set_tag;
    adv_bf_tag_t    orig_set_tag;
    adv_bf_tag_t    nxt_del_pending;
    uint16_t        state;
    uint16_t        flags;
    uint32_t        clone_id;
    uint32_t        clone_cnt;
    uint32_t        num_clones;
    uint32_t        fs_dev;
    uint32_t        free_frag_grps;
    uint32_t        old_quota_status;
    uint32_t        uid;
    uint32_t        gid;
    uint32_t        mode;
    char            set_name[ADVFS_BS_SET_NAME_SZ];
    uint32_t        fs_context[ADVFS_BS_FS_CONTEXT_SZ];
    adv_frag_grp_t  frag_grps[ADVFS_BFS_FRAG_MAX];
} adv_bf_set_attr_t;

_Static_assert(sizeof(adv_bf_set_attr_t) == 216, "adv_bf_set_attr_t size");

/* ----------------------------------------------------------------
 * Tag map entry -- maps tag number to primary mcell location.
 *
 * Three uses via union:
 * s1: First entry on page 0 -- free list head + uninit page ptr
 * s2: Free list entry       -- seq + next free map index
 * s3: In-use entry          -- seq + vd_index + mcell ID
 * ---------------------------------------------------------------- */
typedef struct {
    union {
        struct {
            uint32_t free_list;
            uint32_t uninit_pg;
        } s1;
        struct {
            uint16_t       seq_no;
            uint16_t       _pad;
            uint32_t       next_map;
        } s2;
        struct {
            uint16_t       seq_no;
            uint16_t       vd_index;
            adv_mcell_id_t mcid;
        } s3;
    } u;
} adv_tag_map_t;

_Static_assert(sizeof(adv_tag_map_t) == 8, "adv_tag_map_t size");

/* Is this tag map entry in use? */
#define ADV_TMAP_IN_USE(tm)  ((tm).u.s3.seq_no & ADVFS_BS_TD_IN_USE)

/* ----------------------------------------------------------------
 * Tag directory page header.
 * ---------------------------------------------------------------- */
typedef struct {
    uint32_t curr_page;
    uint32_t next_free_page;
    uint16_t next_free_map;
    uint16_t num_alloc_tmaps;
    uint16_t num_dead_tmaps;
    uint16_t _pad;
} adv_tag_dir_hdr_t;

_Static_assert(sizeof(adv_tag_dir_hdr_t) == 16, "adv_tag_dir_hdr_t size");

/* Number of tag map entries per tag directory page */
#define ADVFS_TAGS_PER_PG \
    ((ADVFS_PGSZ - sizeof(adv_tag_dir_hdr_t)) / sizeof(adv_tag_map_t))
/* = (8192 - 16) / 8 = 1022 */

/* ----------------------------------------------------------------
 * Tag directory page -- header + array of tag map entries.
 * ---------------------------------------------------------------- */
typedef struct {
    adv_tag_dir_hdr_t hdr;
    adv_tag_map_t     maps[ADVFS_TAGS_PER_PG];
} adv_tag_dir_pg_t;

_Static_assert(sizeof(adv_tag_dir_pg_t) <= ADVFS_PGSZ,
               "adv_tag_dir_pg_t must fit in one page");

/* ----------------------------------------------------------------
 * Directory entry -- on-disk format in directory data pages (V3).
 *
 * Entries are variable-length and do not cross DIRBLKSIZ (512-byte)
 * block boundaries. The name is null-terminated and the entry is
 * padded to a 4-byte boundary.
 *
 * V3 layout (8-byte header):
 *   uint32_t tag_num     -- file's tag number (NOT full bf_tag)
 *   uint16_t entry_size  -- total size incl. name + padding + trailing tag
 *   uint16_t name_count  -- name length without null terminator
 *   char name[]          -- null-terminated, padded to 4-byte boundary
 *   adv_bf_tag_t tag     -- full tag (num + seq) at end of entry
 *
 * Minimum valid entry: 8 (hdr) + 4 (1 char padded) + 8 (tag) = 20.
 * Deleted entries (tag_num == 0) may be smaller.
 * ---------------------------------------------------------------- */
typedef struct {
    uint32_t     tag_num;       /* file's tag number (NOT full bf_tag) */
    uint16_t     entry_size;    /* total size including name + padding + trailing tag */
    uint16_t     name_count;    /* name length without null terminator */
    /* Followed by:
     *   char name[name_count+1] padded to 4-byte boundary
     *   adv_bf_tag_t tag (8 bytes: num + seq, at end of entry)
     */
} adv_dir_entry_t;

_Static_assert(sizeof(adv_dir_entry_t) == 8, "adv_dir_entry_t size");

#define ADVFS_DIR_ENTRY_HDR_SZ  8   /* size of fixed header */
#define ADVFS_DIR_ENTRY_NAME(e) ((const char *)((e) + 1))

/* ----------------------------------------------------------------
 * Mcell free list head record (BSR_MCELL_FREE_LIST = 7).
 * ---------------------------------------------------------------- */
typedef struct {
    uint32_t head_pg;
} adv_mcell_free_list_t;

_Static_assert(sizeof(adv_mcell_free_list_t) == 4,
               "adv_mcell_free_list_t size");

#endif /* ADVFS_ODS_H */
