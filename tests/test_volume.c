/*
 * test_volume.c -- verify AdvFS magic detection on a vdisk file
 *
 * Usage: test_volume <vdisk-path>
 *
 * Copyright (C) 2026 Armas Spann
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdio.h>
#include <stdlib.h>
#include "../src/volume.h"
#include "../src/util.h"

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <vdisk-path>\n", argv[0]);
        return 1;
    }

    advfs_volume_t *vol = NULL;
    int err = advfs_volume_open(argv[1], &vol);
    if (err) {
        fprintf(stderr, "Not an AdvFS volume (or open failed): %s\n", argv[1]);
        return 1;
    }

    printf("AdvFS detected: %s\n", advfs_volume_path(vol));
    printf("  Size: %lu bytes (%lu pages)\n",
           (unsigned long)advfs_volume_size(vol),
           (unsigned long)advfs_volume_page_count(vol));

    int version = advfs_volume_detect_version(vol);
    if (version > 0) {
        printf("  ODS version: %d", version);
        if (version == 3) {
            printf(" (Digital UNIX V4.x / Tru64 V5.x with -V3)");
        } else if (version == 4) {
            printf(" (Tru64 UNIX V5.x)");
        }
        printf("\n");
    } else {
        printf("  ODS version: detection failed (%d)\n", version);
    }

    advfs_volume_close(vol);
    return 0;
}
