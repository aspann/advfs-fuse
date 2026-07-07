/*
 * test_fileset.c -- verify extent resolution and fileset enumeration
 *
 * Usage: test_fileset <vdisk-path>
 *
 * Opens an AdvFS vdisk, reads domain metadata, resolves BFSDIR
 * extents, and enumerates filesets via the tag directory.
 *
 * Copyright (C) 2026 Armas Spann
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _XOPEN_SOURCE 500  /* pread() */

#include <stdio.h>
#include <stdlib.h>
#include "../src/volume.h"
#include "../src/domain.h"
#include "../src/bmt.h"
#include "../src/extents.h"
#include "../src/fileset.h"
#include "../src/util.h"

/* Callback for fileset enumeration. */
static int print_fileset(const advfs_fileset_info_t *info, void *user_data)
{
    int *count = (int *)user_data;
    (*count)++;

    printf("  Fileset %d:\n", *count);
    printf("    Name:     %s\n", info->name);
    printf("    Tag:      %u.%u\n", info->dir_tag.num, info->dir_tag.seq);
    printf("    State:    %u\n", info->state);
    printf("    fs_dev:   %u\n", info->fs_dev);
    printf("    Domain:   %d.%06d\n",
           info->set_id.domain_id.id_sec,
           info->set_id.domain_id.id_usec);
    printf("    Dir tag:  %u.%u\n",
           info->set_id.dir_tag.num,
           info->set_id.dir_tag.seq);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <vdisk-path>\n", argv[0]);
        return 1;
    }

    /* Open the volume */
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

    /* Discover domain metadata */
    advfs_domain_t domain;
    err = advfs_domain_open(vol, &domain);
    if (err) {
        fprintf(stderr, "Failed to open domain (err=%d)\n", err);
        advfs_volume_close(vol);
        return 1;
    }

    printf("\nDomain info:\n");
    printf("  ODS version:    %d\n", domain.ods_version);
    printf("  Domain ID:      %d.%06d\n",
           domain.domain_id.id_sec, domain.domain_id.id_usec);
    printf("  Set dir tag:    %u.%u\n",
           domain.bf_set_dir_tag.num, domain.bf_set_dir_tag.seq);

    /* --- Extent resolution test --- */
    printf("\n--- BFSDIR Extent Map ---\n");

    uint32_t bfsdir_cell;
    if (domain.ods_version >= 4) {
        bfsdir_cell = ADVFS_BFM_BFSDIR;
    } else {
        bfsdir_cell = ADVFS_BFM_BFSDIR_V3;
    }

    advfs_extent_t extents[ADVFS_MAX_EXTENTS];
    int num_extents = 0;

    err = advfs_extents_read(vol, domain.bmt_map, domain.bmt_map_count,
                             domain.bmt_page, bfsdir_cell, extents,
                             ADVFS_MAX_EXTENTS, &num_extents);
    if (err) {
        fprintf(stderr, "Failed to read BFSDIR extents (err=%d)\n", err);
    } else {
        printf("  BFSDIR extents (%d):\n", num_extents);
        for (int i = 0; i < num_extents; i++) {
            if (extents[i].vd_blk == ADVFS_XTNT_TERM) {
                printf("    [%d] TERM\n", i);
            } else {
                printf("    [%d] bs_page=%u -> vd_blk=%u "
                       "(vol_page=%u)\n",
                       i, extents[i].bs_page, extents[i].vd_blk,
                       extents[i].vd_blk / ADVFS_BLKS_PER_PG);
            }
        }
    }

    /* --- Fileset enumeration via extents + tag directory --- */
    printf("\n--- Filesets (via tag directory) ---\n");
    int count = 0;
    err = advfs_fileset_list(vol, &domain, print_fileset, &count);
    if (err) {
        fprintf(stderr, "Failed to list filesets (err=%d)\n", err);
    } else if (count == 0) {
        printf("  (none found)\n");
    } else {
        printf("\nTotal: %d fileset(s)\n", count);
    }

    /* --- Also show the old domain-based scan for comparison --- */
    printf("\n--- Filesets (via BMT page 0 scan, for comparison) ---\n");
    int old_count = 0;
    err = advfs_domain_list_filesets(&domain, print_fileset, &old_count);
    if (err) {
        fprintf(stderr, "Old scan failed (err=%d)\n", err);
    } else if (old_count == 0) {
        printf("  (none found)\n");
    } else {
        printf("\nTotal (old): %d fileset(s)\n", old_count);
    }

    advfs_domain_close(&domain);
    advfs_volume_close(vol);
    return 0;
}
