/*
 * test_dir.c -- verify directory entry parsing
 *
 * Usage: test_dir <vdisk-path>
 *
 * Opens an AdvFS vdisk, discovers the root fileset, resolves the
 * fileset's tag directory extents, and lists the root directory
 * entries using advfs_dir_read().
 *
 * Copyright (C) 2026 Armas Spann
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _XOPEN_SOURCE 500  /* pread() */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/volume.h"
#include "../src/domain.h"
#include "../src/bmt.h"
#include "../src/extents.h"
#include "../src/fileset.h"
#include "../src/dir.h"
#include "../src/util.h"

/* Storage for the first fileset found during enumeration. */
typedef struct {
    advfs_fileset_info_t info;
    int found;
} first_fileset_t;

/* Callback: capture the first fileset and stop. */
static int capture_first_fileset(const advfs_fileset_info_t *info,
                                  void *user_data)
{
    first_fileset_t *fs = (first_fileset_t *)user_data;
    fs->info = *info;
    fs->found = 1;
    return 1;  /* stop after first */
}

/* Callback: print each directory entry. */
static int print_dir_entry(const char *name, adv_bf_tag_t tag,
                            void *user_data)
{
    int *count = (int *)user_data;
    (*count)++;
    printf("  [%3d] %-30s  tag=%u.%u\n",
           *count, name, tag.num, tag.seq);
    return 0;
}

/*
 * Resolve the fileset tag directory extents from BFSDIR.
 *
 * Given the fileset's dir_tag (from fileset enumeration), looks it up
 * in the BFSDIR tag directory, follows the mcell chain to find
 * BSR_XTNTS, and reads the extent map.
 *
 * Returns 0 on success, -errno on failure.
 */
static int resolve_fileset_tag_extents(advfs_volume_t *vol,
                                        advfs_domain_t *domain,
                                        adv_bf_tag_t fs_dir_tag,
                                        advfs_extent_t *extents_out,
                                        int max_extents,
                                        int *count_out)
{
    /* Read BFSDIR extents from BMT page 0 */
    uint32_t bfsdir_cell = (domain->ods_version >= 4)
                            ? ADVFS_BFM_BFSDIR : ADVFS_BFM_BFSDIR_V3;

    advfs_extent_t bfsdir_extents[ADVFS_MAX_EXTENTS];
    int bfsdir_num_extents = 0;

    int err = advfs_extents_read(vol, domain->bmt_map, domain->bmt_map_count,
                                  domain->bmt_page, bfsdir_cell,
                                  bfsdir_extents, ADVFS_MAX_EXTENTS,
                                  &bfsdir_num_extents);
    if (err) {
        fprintf(stderr, "Failed to read BFSDIR extents (err=%d)\n", err);
        return err;
    }

    printf("  BFSDIR: %d extent(s)\n", bfsdir_num_extents);

    /* Look up fileset dir_tag in BFSDIR tag directory */
    uint32_t tag_page = fs_dir_tag.num / ADVFS_TAGS_PER_PG;
    uint32_t tag_idx = fs_dir_tag.num % ADVFS_TAGS_PER_PG;

    adv_tag_dir_pg_t bfsdir_tag_pg;
    err = advfs_extents_read_data(vol, bfsdir_extents, bfsdir_num_extents,
                                   tag_page, &bfsdir_tag_pg);
    if (err) {
        fprintf(stderr, "Failed to read BFSDIR tag page %u (err=%d)\n",
                tag_page, err);
        return err;
    }

    adv_tag_map_t *tm = &bfsdir_tag_pg.maps[tag_idx];
    if (!ADV_TMAP_IN_USE(*tm)) {
        fprintf(stderr, "Fileset tag %u not in use in BFSDIR\n",
                fs_dir_tag.num);
        return -ENOENT;
    }

    uint32_t bmt_pg_num = ADV_MCID_PAGE(tm->u.s3.mcid);
    uint32_t mcell_cell = ADV_MCID_CELL(tm->u.s3.mcid);

    printf("  Fileset mcell: BMT p%u.c%u\n", bmt_pg_num, mcell_cell);

    /* Resolve BMT page to volume page */
    uint32_t vol_page;
    err = advfs_bmt_resolve_page(domain->bmt_map, domain->bmt_map_count,
                                  bmt_pg_num, &vol_page);
    if (err) {
        fprintf(stderr, "Failed to resolve BMT page %u (err=%d)\n",
                bmt_pg_num, err);
        return err;
    }

    /* Try to read extents from this mcell */
    err = advfs_extents_read(vol, domain->bmt_map, domain->bmt_map_count,
                             vol_page, mcell_cell,
                             extents_out, max_extents, count_out);
    if (err == 0) {
        return 0;
    }

    if (err != -ENODATA) {
        fprintf(stderr, "Failed to read fileset extents (err=%d)\n", err);
        return err;
    }

    /*
     * V3: BSR_XTNTS not in primary mcell. Follow the chain.
     */
    adv_bmt_pg_t bmt_pg;
    err = advfs_bmt_read_page(vol, vol_page, &bmt_pg);
    if (err) {
        return err;
    }

    adv_mcell_t *mc = advfs_bmt_get_mcell(&bmt_pg, mcell_cell);
    if (!mc) {
        return -EINVAL;
    }

    adv_mcell_id_t next = mc->next_mcid;
    int hops = 0;

    while (next.raw != 0 && hops < 16) {
        uint32_t np = ADV_MCID_PAGE(next);
        uint32_t nc = ADV_MCID_CELL(next);
        uint32_t nvp;

        err = advfs_bmt_resolve_page(domain->bmt_map,
                                      domain->bmt_map_count, np, &nvp);
        if (err) {
            return err;
        }

        err = advfs_extents_read(vol, domain->bmt_map,
                                 domain->bmt_map_count,
                                 nvp, nc, extents_out, max_extents,
                                 count_out);
        if (err == 0) {
            printf("  BSR_XTNTS found at chain hop %d (BMT p%u.c%u)\n",
                   hops + 1, np, nc);
            return 0;
        }

        if (err != -ENODATA) {
            return err;
        }

        /* Follow the chain further */
        adv_bmt_pg_t next_pg;
        err = advfs_bmt_read_page(vol, nvp, &next_pg);
        if (err) {
            return err;
        }

        adv_mcell_t *nmc = advfs_bmt_get_mcell(&next_pg, nc);
        if (!nmc) {
            return -EINVAL;
        }

        next = nmc->next_mcid;
        hops++;
    }

    fprintf(stderr, "Failed to find BSR_XTNTS in fileset mcell chain\n");
    return -ENODATA;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <vdisk-path>\n", argv[0]);
        return 1;
    }

    /* Step 1: Open the volume */
    advfs_volume_t *vol = NULL;
    int err = advfs_volume_open(argv[1], &vol);
    if (err) {
        fprintf(stderr, "Failed to open volume: %s (err=%d)\n",
                argv[1], err);
        return 1;
    }

    printf("Volume: %s\n", advfs_volume_path(vol));
    printf("  Size: %lu bytes (%lu pages)\n",
           (unsigned long)advfs_volume_size(vol),
           (unsigned long)advfs_volume_page_count(vol));

    /* Step 2: Open the domain */
    advfs_domain_t domain;
    err = advfs_domain_open(vol, &domain);
    if (err) {
        fprintf(stderr, "Failed to open domain (err=%d)\n", err);
        advfs_volume_close(vol);
        return 1;
    }

    printf("\nDomain:\n");
    printf("  ODS version: %d\n", domain.ods_version);
    printf("  Domain ID:   %d.%06d\n",
           domain.domain_id.id_sec, domain.domain_id.id_usec);

    /* Step 3: Find the first fileset */
    first_fileset_t first_fs;
    first_fs.found = 0;

    err = advfs_fileset_list(vol, &domain, capture_first_fileset, &first_fs);
    if (err) {
        fprintf(stderr, "Failed to list filesets (err=%d)\n", err);
        advfs_domain_close(&domain);
        advfs_volume_close(vol);
        return 1;
    }

    if (!first_fs.found) {
        fprintf(stderr, "No filesets found\n");
        advfs_domain_close(&domain);
        advfs_volume_close(vol);
        return 1;
    }

    printf("\nFileset: %s (tag=%u.%u)\n",
           first_fs.info.name,
           first_fs.info.dir_tag.num,
           first_fs.info.dir_tag.seq);

    /* Step 4: Resolve the fileset's tag directory extents */
    advfs_extent_t fs_tag_extents[ADVFS_MAX_EXTENTS];
    int fs_tag_num_extents = 0;

    err = resolve_fileset_tag_extents(vol, &domain, first_fs.info.dir_tag,
                                       fs_tag_extents, ADVFS_MAX_EXTENTS,
                                       &fs_tag_num_extents);
    if (err) {
        fprintf(stderr, "Failed to resolve fileset tag dir extents "
                "(err=%d)\n", err);
        advfs_domain_close(&domain);
        advfs_volume_close(vol);
        return 1;
    }

    printf("  Fileset tag dir: %d extent(s)\n", fs_tag_num_extents);
    for (int i = 0; i < fs_tag_num_extents; i++) {
        if (fs_tag_extents[i].vd_blk == ADVFS_XTNT_TERM) {
            printf("    [%d] TERM\n", i);
        } else {
            printf("    [%d] bs_page=%u -> vd_blk=%u (vol_page=%u)\n",
                   i, fs_tag_extents[i].bs_page,
                   fs_tag_extents[i].vd_blk,
                   fs_tag_extents[i].vd_blk / ADVFS_BLKS_PER_PG);
        }
    }

    /* Step 5: Read the root directory (tag 2) */
    adv_bf_tag_t root_tag = { .num = 2, .seq = 0 };

    printf("\n--- Root directory (tag %u) ---\n", root_tag.num);
    int entry_count = 0;
    err = advfs_dir_read(vol, &domain, fs_tag_extents, fs_tag_num_extents,
                          first_fs.info.dir_tag, root_tag,
                          print_dir_entry, &entry_count);
    if (err) {
        fprintf(stderr, "Root directory tag 2 failed (err=%d), "
                "trying tag 3...\n", err);

        root_tag.num = 3;
        entry_count = 0;
        printf("\n--- Root directory (tag %u) ---\n", root_tag.num);
        err = advfs_dir_read(vol, &domain, fs_tag_extents,
                              fs_tag_num_extents,
                              first_fs.info.dir_tag, root_tag,
                              print_dir_entry, &entry_count);
        if (err) {
            fprintf(stderr, "Root directory tag 3 also failed "
                    "(err=%d)\n", err);
        }
    }

    if (entry_count > 0) {
        printf("\nTotal: %d entries\n", entry_count);
    } else {
        printf("  (no entries found)\n");
    }

    advfs_domain_close(&domain);
    advfs_volume_close(vol);
    return 0;
}
