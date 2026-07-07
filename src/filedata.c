/*
 * filedata.c -- file data reading for advfs-fuse
 *
 * Data flow for advfs_filedata_read():
 *   1. Look up file_tag in the fileset's tag directory (BMT scan
 *      fallback when the tag dir is dead/uninitialized)
 *   2. Walk the primary mcell chain for BMTR_FS_STAT (size + frag)
 *   3. If a BMTR_FS_DATA record is in the chain, the contents are
 *      stored inline in the mcell (fast symlinks, small files) --
 *      copy them and stop
 *   4. Find the mcell containing BSR_XTNTS, read the extent map
 *   5. Read each extent-mapped data page; holes and unmapped pages
 *      read as zeros
 *   6. If a frag is attached, read the sub-page tail from the
 *      fileset's frag file (tag 1) and overlay it
 *   7. Truncate to the exact file size from FS_STAT
 *
 * Copyright (C) 2026 Armas Spann
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _XOPEN_SOURCE 500  /* pread() */

#include "filedata.h"
#include "bmt.h"
#include "fileset.h"
#include "util.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Maximum mcell chain hops when searching for a record. */
#define MAX_CHAIN_HOPS  16

/* Maximum BMT pages to scan in the brute-force fallback. */
#define MAX_BMT_SCAN_PAGES  1024

/* Compute a BMT scan range from one BMT extent map. */
static uint32_t bmt_scan_pages(const adv_xtnt_t *map, int count)
{
    uint32_t max_pages = 0;
    for (int i = 0; i < count; i++) {
        uint32_t cand;
        if (map[i].vd_blk == ADVFS_XTNT_TERM) {
            cand = map[i].bs_page;
        } else {
            cand = map[i].bs_page + ((i + 1 < count) ? 1 : 64);
        }
        if (cand > max_pages) {
            max_pages = cand;
        }
    }
    if (max_pages > MAX_BMT_SCAN_PAGES) {
        max_pages = MAX_BMT_SCAN_PAGES;
    }
    if (max_pages == 0) {
        max_pages = 16;
    }
    return max_pages;
}

/*
 * Find a tag's mcell by scanning BMT pages (brute-force fallback).
 *
 * Mirrors the fallback in dir.c: used when the fileset's tag
 * directory is dead/uninitialized. Scans the BMT of every attached
 * volume. Returns 0 with *vd_out, *bmt_pg_out and *cell_out set, or
 * -ENOENT if not found.
 */
static int find_tag_by_bmt_scan(advfs_domain_t *domain,
                                adv_bf_tag_t fs_tag,
                                uint32_t target_tag_num,
                                uint32_t *vd_out,
                                uint32_t *bmt_pg_out, uint32_t *cell_out)
{
    /* Legacy contexts without a vd table scan the primary only. */
    int nvds = (domain->vd_count > 0) ? domain->vd_count : 1;

    for (int v = 0; v < nvds; v++) {
        uint32_t vd;
        const adv_xtnt_t *map;
        int map_count;

        if (domain->vd_count > 0) {
            vd = domain->vds[v].vd_index;
            map = domain->vds[v].bmt_map;
            map_count = domain->vds[v].bmt_map_count;
        } else {
            vd = 0;
            map = domain->bmt_map;
            map_count = domain->bmt_map_count;
        }

        uint32_t max_pages = bmt_scan_pages(map, map_count);

        for (uint32_t pg = 0; pg < max_pages; pg++) {
            adv_bmt_pg_t bmt_pg;
            int err = advfs_domain_read_bmt_page_vd(domain, vd, pg, &bmt_pg);
            if (err) {
                continue;  /* skip unreadable pages */
            }

            for (uint32_t c = 0; c < ADVFS_BSPG_CELLS; c++) {
                adv_mcell_t *mc = advfs_bmt_get_mcell(&bmt_pg, c);
                if (!mc || mc->tag.num == 0) {
                    continue;
                }
                if (mc->tag.num == target_tag_num &&
                    mc->bf_set_tag.num == fs_tag.num) {
                    *vd_out = vd;
                    *bmt_pg_out = pg;
                    *cell_out = c;
                    return 0;
                }
            }
        }
    }

    return -ENOENT;
}

/*
 * Resolve a file tag to its primary mcell location (BMT page + cell).
 *
 * Looks up the tag in the fileset's tag directory; falls back to a
 * BMT scan when the tag dir page is unreadable or the slot is dead.
 * Returns 0 on success, -errno on failure.
 */
static int resolve_tag_mcell(advfs_volume_t *vol, advfs_domain_t *domain,
                             const advfs_extent_t *fs_tag_extents,
                             int fs_tag_num_extents,
                             adv_bf_tag_t fs_tag, adv_bf_tag_t file_tag,
                             uint32_t *vd_out,
                             uint32_t *bmt_pg_out, uint32_t *cell_out)
{
    (void)vol;
    uint32_t tag_page = file_tag.num / ADVFS_TAGS_PER_PG;
    uint32_t tag_idx = file_tag.num % ADVFS_TAGS_PER_PG;

    adv_tag_dir_pg_t tag_pg;
    int err = advfs_extents_read_data_dom(domain, fs_tag_extents,
                                          fs_tag_num_extents, tag_page,
                                          &tag_pg);
    if (err == 0) {
        adv_tag_map_t *tm = &tag_pg.maps[tag_idx];
        if (ADV_TMAP_IN_USE(*tm) && tm->u.s3.seq_no != 0xFFFF) {
            *vd_out = tm->u.s3.vd_index;
            *bmt_pg_out = ADV_MCID_PAGE(tm->u.s3.mcid);
            *cell_out = ADV_MCID_CELL(tm->u.s3.mcid);
            advfs_dbg("filedata: tag %u -> vd=%u, mcid=p%u.c%u",
                      file_tag.num, *vd_out, *bmt_pg_out, *cell_out);
            return 0;
        }
        advfs_dbg("filedata: tag %u not in use in tag dir (seq=%u), "
                  "trying BMT scan fallback",
                  file_tag.num, tm->u.s3.seq_no);
    } else {
        advfs_dbg("filedata: failed to read tag dir page %u for tag %u "
                  "(err=%d), trying BMT scan fallback",
                  tag_page, file_tag.num, err);
    }

    err = find_tag_by_bmt_scan(domain, fs_tag, file_tag.num,
                               vd_out, bmt_pg_out, cell_out);
    if (err) {
        advfs_err("filedata: tag %u not found via tag dir or BMT scan",
                  file_tag.num);
        return -ENOENT;
    }

    return 0;
}

/*
 * Walk a primary mcell chain looking for a record of rec_type.
 *
 * On success, copies min(data size, buf_sz) record bytes into buf;
 * if data_sz_out is non-NULL it receives the number of bytes copied.
 * Returns 0 on success, -ENODATA if the record is not in the chain,
 * -errno on read failure.
 */
static int find_rec_in_chain(advfs_volume_t *vol, advfs_domain_t *domain,
                             uint32_t vd, uint32_t bmt_pg_num, uint32_t cell,
                             uint8_t rec_type, void *buf, size_t buf_sz,
                             size_t *data_sz_out)
{
    (void)vol;
    int hops = 0;

    for (;;) {
        adv_bmt_pg_t bmt_pg;
        int err = advfs_domain_read_bmt_page_vd(domain, vd, bmt_pg_num,
                                                &bmt_pg);
        if (err) {
            advfs_err("filedata: cannot read vd %u BMT page %u",
                      vd, bmt_pg_num);
            return err;
        }

        adv_mcell_t *mc = advfs_bmt_get_mcell(&bmt_pg, cell);
        if (!mc) {
            advfs_err("filedata: invalid cell %u on vd %u BMT page %u",
                      cell, vd, bmt_pg_num);
            return -EINVAL;
        }

        adv_rec_hdr_t hdr;
        void *rec = advfs_bmt_find_rec(mc, rec_type, &hdr);
        if (rec) {
            size_t data_sz = ADV_REC_DATA_SZ(hdr);
            if (data_sz > buf_sz) {
                data_sz = buf_sz;
            }
            memcpy(buf, rec, data_sz);
            if (data_sz_out) {
                *data_sz_out = data_sz;
            }
            return 0;
        }

        adv_mcell_id_t next = mc->next_mcid;
        if (next.raw == 0 || hops >= MAX_CHAIN_HOPS) {
            return -ENODATA;
        }

        /* The chain may cross volumes via next_vd_index. */
        if (mc->next_vd_index != 0) {
            vd = mc->next_vd_index;
        }
        bmt_pg_num = ADV_MCID_PAGE(next);
        cell = ADV_MCID_CELL(next);
        hops++;
    }
}

/*
 * Find the mcell containing BSR_XTNTS starting from a primary mcell,
 * then read the extent map. Mirrors find_xtnts_mcell() in dir.c:
 * on V3 disks BSR_XTNTS may live in a chained mcell rather than the
 * primary.
 *
 * Returns 0 on success, -ENODATA if no BSR_XTNTS exists anywhere in
 * the chain (legitimate for files stored entirely in a frag),
 * -errno on other failure.
 */
static int read_file_extents(advfs_volume_t *vol, advfs_domain_t *domain,
                             uint32_t vd, uint32_t bmt_pg_num, uint32_t cell,
                             advfs_extent_t *extents_out, int max_extents,
                             int *count_out)
{
    (void)vol;
    uint32_t cur_vd = vd;
    uint32_t cur_pg = bmt_pg_num;
    uint32_t cur_cell = cell;
    int hops = 0;

    /*
     * Locate the mcell holding BSR_XTNTS. On V3 disks it may live in a
     * chained mcell (possibly on another volume) rather than the
     * primary; follow the chain until we find it, then hand that mcell
     * to the domain-aware extent reader (which itself walks the
     * BSR_XTRA_XTNTS/BSR_SHADOW_XTNTS chain and tags each extent with
     * the vd_index of the volume holding its blocks).
     */
    for (;;) {
        adv_bmt_pg_t bmt_pg;
        int err = advfs_domain_read_bmt_page_vd(domain, cur_vd, cur_pg,
                                                &bmt_pg);
        if (err) {
            return err;
        }

        adv_mcell_t *mc = advfs_bmt_get_mcell(&bmt_pg, cur_cell);
        if (!mc) {
            return -EINVAL;
        }

        if (advfs_bmt_find_rec(mc, ADVFS_BSR_XTNTS, NULL)) {
            return advfs_extents_read_dom(domain, cur_vd, cur_pg, cur_cell,
                                          extents_out, max_extents,
                                          count_out);
        }

        adv_mcell_id_t next = mc->next_mcid;
        if (next.raw == 0 || hops >= MAX_CHAIN_HOPS) {
            return -ENODATA;
        }

        if (mc->next_vd_index != 0) {
            cur_vd = mc->next_vd_index;
        }
        cur_pg = ADV_MCID_PAGE(next);
        cur_cell = ADV_MCID_CELL(next);
        hops++;
    }
}

/* ----------------------------------------------------------------
 * Clone / snapshot fallback.
 *
 * When reading a clone fileset, data pages that were never
 * copy-on-write'd are permanent holes (ADVFS_PERM_HOLE). The real data
 * lives in the ORIGINAL fileset: the same file tag, resolved through
 * the original fileset's tag directory and read from its extent map.
 *
 * A clone_fallback_t caches the original file's extent map so it is
 * resolved at most once per file read, no matter how many permanent
 * holes the clone contains. Resolution is lazy (first permanent hole)
 * and its outcome is memoized in `resolved`.
 * ---------------------------------------------------------------- */
typedef struct {
    advfs_volume_t *vol;
    advfs_domain_t *domain;
    adv_bf_tag_t    orig_fs_tag;     /* original fileset's BFSDIR tag */
    adv_bf_tag_t    file_tag;        /* the file being read */
    int             resolved;        /* 0=untried, 1=ready, -1=unavailable */
    advfs_extent_t  orig_extents[ADVFS_MAX_EXTENTS];
    int             orig_num_extents;
} clone_fallback_t;

/*
 * Lazily resolve the original file's extent map. Returns 0 when the
 * fallback is ready to serve pages, -1 when the original file cannot be
 * resolved (the caller then leaves the page zero-filled). The outcome
 * is memoized so a file with many holes resolves the original only once.
 */
static int clone_fallback_resolve(clone_fallback_t *fb)
{
    if (fb->resolved != 0) {
        return fb->resolved == 1 ? 0 : -1;
    }

    /* Original fileset's tag directory extent map. */
    advfs_extent_t orig_fs_extents[ADVFS_MAX_EXTENTS];
    int orig_fs_num = 0;
    int err = advfs_fileset_tag_extents(fb->vol, fb->domain, fb->orig_fs_tag,
                                        orig_fs_extents, ADVFS_MAX_EXTENTS,
                                        &orig_fs_num);
    if (err) {
        advfs_warn("filedata: clone fallback: cannot read original "
                   "fileset (tag %u) tag dir (err=%d), zero-filling",
                   fb->orig_fs_tag.num, err);
        fb->resolved = -1;
        return -1;
    }

    /* Same file tag, resolved in the original fileset. */
    uint32_t vd, bmt_pg_num, cell;
    err = resolve_tag_mcell(fb->vol, fb->domain, orig_fs_extents,
                            orig_fs_num, fb->orig_fs_tag, fb->file_tag,
                            &vd, &bmt_pg_num, &cell);
    if (err) {
        advfs_warn("filedata: clone fallback: tag %u not found in "
                   "original fileset (tag %u), zero-filling",
                   fb->file_tag.num, fb->orig_fs_tag.num);
        fb->resolved = -1;
        return -1;
    }

    err = read_file_extents(fb->vol, fb->domain, vd, bmt_pg_num, cell,
                            fb->orig_extents, ADVFS_MAX_EXTENTS,
                            &fb->orig_num_extents);
    if (err) {
        /* -ENODATA: the original file has no extents either (frag-only);
         * nothing to fall back to. Treat like any other miss. */
        advfs_warn("filedata: clone fallback: no extents for tag %u in "
                   "original fileset (tag %u, err=%d), zero-filling",
                   fb->file_tag.num, fb->orig_fs_tag.num, err);
        fb->resolved = -1;
        return -1;
    }

    advfs_dbg("filedata: clone fallback ready for tag %u -> original "
              "fileset tag %u (%d extent(s))",
              fb->file_tag.num, fb->orig_fs_tag.num, fb->orig_num_extents);
    fb->resolved = 1;
    return 0;
}

/*
 * Fill page `pg` from the original fileset. On any failure the buffer
 * is left zero-filled (best effort) and 0 is returned, so a clone read
 * degrades to the legacy behavior rather than aborting. A hard I/O
 * error is propagated.
 */
static int clone_fallback_read_page(clone_fallback_t *fb, uint32_t pg,
                                    uint8_t *page_buf)
{
    if (clone_fallback_resolve(fb) != 0) {
        memset(page_buf, 0, ADVFS_PGSZ);
        return 0;
    }

    int orig_perm_hole = 0;
    int err = advfs_extents_read_data_dom_ex(fb->domain, fb->orig_extents,
                                             fb->orig_num_extents, pg,
                                             page_buf, &orig_perm_hole);
    if (err == -ENOENT) {
        /* Page beyond the original's extent map: read as zeros. */
        memset(page_buf, 0, ADVFS_PGSZ);
        return 0;
    }
    if (err) {
        return err;
    }

    if (orig_perm_hole) {
        /*
         * The original ALSO has a permanent hole here: this is a clone
         * of a clone. We only follow one level, so zero-fill and warn.
         */
        advfs_warn("filedata: clone fallback: tag %u page %u is a "
                   "permanent hole in the original too (nested clone) -- "
                   "one-level fallback only, zero-filling",
                   fb->file_tag.num, pg);
        memset(page_buf, 0, ADVFS_PGSZ);
    }

    return 0;
}

/*
 * Copy `len` bytes starting at byte offset `byte_off` of the bitfile
 * described by `extents` into dst, reading page by page. Unmapped
 * pages read as zeros (dst is assumed pre-zeroed by the caller for
 * that case to be cheap -- we still zero explicitly for safety).
 *
 * `fb` (optional): clone fallback context. When non-NULL, permanent
 * holes (copy-on-write pages absent from a clone) are filled from the
 * original fileset instead of read as zeros. NULL preserves the legacy
 * behavior for non-clone files and for the frag path.
 */
static int copy_bitfile_bytes(advfs_domain_t *domain,
                              const advfs_extent_t *extents, int num_extents,
                              uint64_t byte_off, uint8_t *dst, uint64_t len,
                              clone_fallback_t *fb)
{
    uint8_t page_buf[ADVFS_PGSZ];

    while (len > 0) {
        uint32_t pg = (uint32_t)(byte_off / ADVFS_PGSZ);
        uint32_t in_pg = (uint32_t)(byte_off % ADVFS_PGSZ);
        uint64_t chunk = ADVFS_PGSZ - in_pg;
        if (chunk > len) {
            chunk = len;
        }

        int err;
        if (fb) {
            /*
             * Clone read: detect permanent (copy-on-write) holes so we
             * can overlay the original fileset's data for them.
             */
            int perm_hole = 0;
            err = advfs_extents_read_data_dom_ex(domain, extents, num_extents,
                                                 pg, page_buf, &perm_hole);
            if (err == -ENOENT) {
                memset(page_buf, 0, ADVFS_PGSZ);
            } else if (err) {
                return err;
            } else if (perm_hole) {
                err = clone_fallback_read_page(fb, pg, page_buf);
                if (err) {
                    return err;
                }
            }
        } else {
            /*
             * Non-clone read: original behavior. Permanent holes are
             * zero-filled with a warning inside the extents layer.
             */
            err = advfs_extents_read_data_dom(domain, extents, num_extents,
                                              pg, page_buf);
            if (err == -ENOENT) {
                /* Page beyond the extent map: sparse -- reads as zeros. */
                memset(page_buf, 0, ADVFS_PGSZ);
            } else if (err) {
                return err;
            }
        }

        memcpy(dst, page_buf + in_pg, chunk);
        dst += chunk;
        byte_off += chunk;
        len -= chunk;
    }

    return 0;
}

/*
 * Read the frag tail of a file from the fileset's frag file (tag 1).
 *
 * The frag file is addressed in 1 KB slots: the frag's data starts
 * at byte offset frag_slot * 1024 and is frag_len bytes (at most
 * frag_type KB). The data is copied to buf + frag_page_offset * 8 KB.
 */
static int read_frag_tail(advfs_volume_t *vol, advfs_domain_t *domain,
                          const advfs_extent_t *fs_tag_extents,
                          int fs_tag_num_extents, adv_bf_tag_t fs_tag,
                          const adv_fs_stat_t *st,
                          uint8_t *buf, uint64_t size)
{
    uint64_t frag_off = (uint64_t)st->frag_page_offset * ADVFS_PGSZ;
    if (frag_off >= size) {
        advfs_err("filedata: frag page offset %u beyond file size %llu",
                  st->frag_page_offset, (unsigned long long)size);
        return -EINVAL;
    }

    uint64_t frag_len = size - frag_off;
    if (frag_len > (uint64_t)st->frag_type * ADVFS_FRAG_SLOT_SZ) {
        advfs_err("filedata: frag tail %llu bytes exceeds %uK frag",
                  (unsigned long long)frag_len, st->frag_type);
        return -EINVAL;
    }

    /* Resolve the frag file's mcell and extent map. */
    adv_bf_tag_t frag_tag = { .num = ADVFS_FRAG_FILE_TAG, .seq = 0 };
    uint32_t frag_vd, bmt_pg_num, cell;

    int err = resolve_tag_mcell(vol, domain, fs_tag_extents,
                                fs_tag_num_extents, fs_tag, frag_tag,
                                &frag_vd, &bmt_pg_num, &cell);
    if (err) {
        advfs_err("filedata: cannot resolve frag file (tag %u)",
                  ADVFS_FRAG_FILE_TAG);
        return err;
    }

    advfs_extent_t frag_extents[ADVFS_MAX_EXTENTS];
    int num_frag_extents = 0;

    err = read_file_extents(vol, domain, frag_vd, bmt_pg_num, cell,
                            frag_extents, ADVFS_MAX_EXTENTS,
                            &num_frag_extents);
    if (err) {
        advfs_err("filedata: cannot read frag file extents (err=%d)", err);
        return err;
    }

    uint64_t slot_off = (uint64_t)st->frag_slot * ADVFS_FRAG_SLOT_SZ;

    advfs_dbg("filedata: frag slot %u type %uK -> frag file offset %llu, "
              "%llu bytes at file offset %llu",
              st->frag_slot, st->frag_type,
              (unsigned long long)slot_off,
              (unsigned long long)frag_len,
              (unsigned long long)frag_off);

    return copy_bitfile_bytes(domain, frag_extents, num_frag_extents,
                              slot_off, buf + frag_off, frag_len, NULL);
}

/* Read the BMTR_FS_STAT record for a file. */
int advfs_filedata_stat(advfs_volume_t *vol, advfs_domain_t *domain,
                        const advfs_extent_t *fs_tag_extents,
                        int fs_tag_num_extents,
                        adv_bf_tag_t fs_tag, adv_bf_tag_t file_tag,
                        adv_fs_stat_t *stat_out)
{
    if (!vol || !domain || !fs_tag_extents || !stat_out) {
        return -EINVAL;
    }
    if (fs_tag_num_extents <= 0 || file_tag.num == 0) {
        return -EINVAL;
    }

    uint32_t vd, bmt_pg_num, cell;
    int err = resolve_tag_mcell(vol, domain, fs_tag_extents,
                                fs_tag_num_extents, fs_tag, file_tag,
                                &vd, &bmt_pg_num, &cell);
    if (err) {
        return err;
    }

    memset(stat_out, 0, sizeof(*stat_out));
    err = find_rec_in_chain(vol, domain, vd, bmt_pg_num, cell,
                            ADVFS_BMTR_FS_STAT, stat_out,
                            sizeof(*stat_out), NULL);
    if (err) {
        advfs_err("filedata: no FS_STAT record for tag %u (err=%d)",
                  file_tag.num, err);
        return err;
    }

    return 0;
}

/* Read a file's entire data contents into a malloc'd buffer.
 *
 * Shared implementation for the clone-unaware and clone-aware entry
 * points. When `clone` is non-NULL and marks a clone fileset, the
 * extent-mapped data pages that are permanent (copy-on-write) holes are
 * filled from the original fileset instead of being zero-filled. */
static int filedata_read_impl(advfs_volume_t *vol, advfs_domain_t *domain,
                              const advfs_extent_t *fs_tag_extents,
                              int fs_tag_num_extents,
                              adv_bf_tag_t fs_tag, adv_bf_tag_t file_tag,
                              const advfs_clone_ctx_t *clone,
                              uint8_t **buf_out, uint64_t *size_out)
{
    if (!buf_out || !size_out) {
        return -EINVAL;
    }
    *buf_out = NULL;
    *size_out = 0;

    /* Step 1+2: primary mcell and FS_STAT (size, frag info). */
    adv_fs_stat_t st;
    int err = advfs_filedata_stat(vol, domain, fs_tag_extents,
                                  fs_tag_num_extents, fs_tag, file_tag,
                                  &st);
    if (err) {
        return err;
    }

    if (st.st_size > ADVFS_FILEDATA_MAX_SIZE) {
        advfs_err("filedata: tag %u size %llu exceeds sanity cap",
                  file_tag.num, (unsigned long long)st.st_size);
        return -EFBIG;
    }

    uint64_t size = st.st_size;

    advfs_dbg("filedata: tag %u size=%llu frag(slot=%u type=%u pgoff=%u)",
              file_tag.num, (unsigned long long)size,
              st.frag_slot, st.frag_type, st.frag_page_offset);

    /* Zero-filled buffer: holes and unmapped tails read as zeros. */
    uint8_t *buf = calloc(1, size ? size : 1);
    if (!buf) {
        return -ENOMEM;
    }

    if (size == 0) {
        *buf_out = buf;
        return 0;
    }

    uint32_t vd, bmt_pg_num, cell;
    err = resolve_tag_mcell(vol, domain, fs_tag_extents,
                            fs_tag_num_extents, fs_tag, file_tag,
                            &vd, &bmt_pg_num, &cell);
    if (err) {
        free(buf);
        return err;
    }

    /*
     * Step 3: inline data. Fast symlinks (and small files) store
     * their contents directly in a BMTR_FS_DATA record in the mcell
     * chain -- no extents, no frag. Checked before the extent map:
     * such files carry an empty BSR_XTNTS (x_cnt == 0), which would
     * otherwise leave the buffer zero-filled.
     */
    if (st.frag_type == 0) {
        uint8_t inline_buf[ADVFS_BSC_R_SZ];
        size_t inline_sz = 0;

        err = find_rec_in_chain(vol, domain, vd, bmt_pg_num, cell,
                                ADVFS_BMTR_FS_DATA, inline_buf,
                                sizeof(inline_buf), &inline_sz);
        if (err == 0) {
            if (inline_sz > size) {
                inline_sz = size;
            }
            memcpy(buf, inline_buf, inline_sz);
            if (inline_sz < size) {
                advfs_warn("filedata: tag %u inline data %zu bytes "
                           "shorter than file size %llu; tail reads "
                           "as zeros",
                           file_tag.num, inline_sz,
                           (unsigned long long)size);
            }
            advfs_dbg("filedata: tag %u read %zu inline bytes "
                      "(BMTR_FS_DATA)", file_tag.num, inline_sz);
            *buf_out = buf;
            *size_out = size;
            return 0;
        } else if (err != -ENODATA) {
            free(buf);
            return err;
        }
    }

    /*
     * Step 4: extent map. -ENODATA (no BSR_XTNTS in the chain) is
     * legitimate for files stored entirely in a frag; any mapped
     * portion is then absent and the buffer stays zeroed.
     */
    advfs_extent_t extents[ADVFS_MAX_EXTENTS];
    int num_extents = 0;

    err = read_file_extents(vol, domain, vd, bmt_pg_num, cell,
                            extents, ADVFS_MAX_EXTENTS, &num_extents);
    if (err && err != -ENODATA) {
        free(buf);
        return err;
    }

    /*
     * Step 5: copy the extent-mapped portion. The extent map covers
     * whole 8 KB pages; the final page may overshoot the file size,
     * and the sub-page tail may instead live in a frag. Copy only up
     * to the file size -- the frag overlay below supplies the tail.
     */
    if (err == 0 && num_extents > 0) {
        uint64_t mapped_len =
            (uint64_t)advfs_extents_page_count(extents, num_extents) *
            ADVFS_PGSZ;
        if (mapped_len > size) {
            mapped_len = size;  /* Step f: truncate the overshoot */
        }

        /*
         * Clone fallback: only set up when this fileset is a clone with
         * a valid original. The fallback context resolves the original
         * file lazily (on the first permanent hole) and caches its
         * extent map, so files with no holes pay nothing. NULL fb means
         * permanent holes read as zeros (legacy behavior).
         */
        clone_fallback_t fb;
        clone_fallback_t *fbp = NULL;
        if (clone && clone->is_clone && clone->orig_set_tag.num != 0) {
            memset(&fb, 0, sizeof(fb));
            fb.vol = vol;
            fb.domain = domain;
            fb.orig_fs_tag = clone->orig_set_tag;
            fb.file_tag = file_tag;
            fb.resolved = 0;
            fbp = &fb;
        }

        err = copy_bitfile_bytes(domain, extents, num_extents, 0,
                                 buf, mapped_len, fbp);
        if (err) {
            free(buf);
            return err;
        }
    }

    /* Step 6: overlay the frag tail (frag_type 0 = no frag). */
    if (st.frag_type != 0) {
        err = read_frag_tail(vol, domain, fs_tag_extents,
                             fs_tag_num_extents, fs_tag, &st, buf, size);
        if (err) {
            free(buf);
            return err;
        }
    }

    *buf_out = buf;
    *size_out = size;
    return 0;
}

/* Read a file's entire data contents into a malloc'd buffer. */
int advfs_filedata_read(advfs_volume_t *vol, advfs_domain_t *domain,
                        const advfs_extent_t *fs_tag_extents,
                        int fs_tag_num_extents,
                        adv_bf_tag_t fs_tag, adv_bf_tag_t file_tag,
                        uint8_t **buf_out, uint64_t *size_out)
{
    return filedata_read_impl(vol, domain, fs_tag_extents, fs_tag_num_extents,
                              fs_tag, file_tag, NULL, buf_out, size_out);
}

/* Clone-aware variant: permanent holes fall back to the original set. */
int advfs_filedata_read_clone(advfs_volume_t *vol, advfs_domain_t *domain,
                              const advfs_extent_t *fs_tag_extents,
                              int fs_tag_num_extents,
                              adv_bf_tag_t fs_tag, adv_bf_tag_t file_tag,
                              const advfs_clone_ctx_t *clone,
                              uint8_t **buf_out, uint64_t *size_out)
{
    return filedata_read_impl(vol, domain, fs_tag_extents, fs_tag_num_extents,
                              fs_tag, file_tag, clone, buf_out, size_out);
}
