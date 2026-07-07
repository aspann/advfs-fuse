/*
 * test_domain.c -- verify domain discovery and fileset enumeration
 *
 * Usage: test_domain <vdisk-path>
 *
 * Opens an AdvFS vdisk, reads domain metadata, and lists all filesets.
 *
 * Copyright (C) 2026 Armas Spann
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _XOPEN_SOURCE 500  /* pread() */

#include <stdio.h>
#include <stdlib.h>
#include "../src/volume.h"
#include "../src/domain.h"
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
    printf("  ODS version:    %d", domain.ods_version);
    if (domain.ods_version == 3) {
        printf(" (Digital UNIX V4.x / Tru64 V5.x with -V3)");
    } else if (domain.ods_version == 4) {
        printf(" (Tru64 UNIX V5.x)");
    }
    printf("\n");
    printf("  Domain ID:      %d.%06d\n",
           domain.domain_id.id_sec, domain.domain_id.id_usec);
    printf("  Max VDs:        %u\n", domain.max_vds);
    printf("  Set dir tag:    %u.%u\n",
           domain.bf_set_dir_tag.num, domain.bf_set_dir_tag.seq);

    if (domain.has_mattr) {
        printf("  VD count:       %u\n", domain.mattr.vd_cnt);
        printf("  FTX log tag:    %u.%u\n",
               domain.mattr.ftx_log_tag.num,
               domain.mattr.ftx_log_tag.seq);
        printf("  FTX log pages:  %u\n", domain.mattr.ftx_log_pgs);
    }

    if (domain.has_vd_attr) {
        printf("  VD index:       %u\n", domain.vd_attr.vd_index);
        printf("  VD blocks:      %u (%u MB)\n",
               domain.vd_attr.vd_blk_cnt,
               domain.vd_attr.vd_blk_cnt / 2048);
        printf("  Cluster size:   %u blocks\n",
               domain.vd_attr.stg_cluster);
        printf("  BMT extent pgs: %u\n", domain.vd_attr.bmt_xtnt_pgs);
    }

    /* Enumerate filesets */
    printf("\nFilesets:\n");
    int count = 0;
    err = advfs_domain_list_filesets(&domain, print_fileset, &count);
    if (err) {
        fprintf(stderr, "Failed to list filesets (err=%d)\n", err);
    } else if (count == 0) {
        printf("  (none found)\n");
    } else {
        printf("\nTotal: %d fileset(s)\n", count);
    }

    advfs_domain_close(&domain);
    advfs_volume_close(vol);
    return 0;
}
