/*
 * debug_symlink.c -- one-off debug tool: dump all mcell records of a
 * file identified by path, to locate where symlink targets live.
 *
 * Usage: debug_symlink <vdisk> <tag-num>
 *
 * Copyright (C) 2026 Armas Spann
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _XOPEN_SOURCE 500

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/bmt.h"
#include "../src/domain.h"
#include "../src/extents.h"
#include "../src/filedata.h"
#include "../src/fileset.h"
#include "../src/util.h"
#include "../src/volume.h"

typedef struct {
    advfs_fileset_info_t info;
    int found;
} first_fs_t;

static int first_cb(const advfs_fileset_info_t *info, void *ud)
{
    first_fs_t *f = (first_fs_t *)ud;
    if (info->clone_id != 0) {
        return 0;
    }
    f->info = *info;
    f->found = 1;
    return 1;
}

static void hexdump(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i += 16) {
        printf("      %04zx: ", i);
        for (size_t j = 0; j < 16; j++) {
            if (i + j < n) {
                printf("%02x ", p[i + j]);
            } else {
                printf("   ");
            }
        }
        printf(" |");
        for (size_t j = 0; j < 16 && i + j < n; j++) {
            uint8_t c = p[i + j];
            putchar(isprint(c) ? c : '.');
        }
        printf("|\n");
    }
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <vdisk> <tag-num>\n", argv[0]);
        return 2;
    }

    uint32_t tag_num = (uint32_t)strtoul(argv[2], NULL, 0);

    advfs_volume_t *vol = NULL;
    if (advfs_volume_open(argv[1], &vol) != 0) {
        return 1;
    }

    advfs_domain_t domain;
    if (advfs_domain_open(vol, &domain) != 0) {
        return 1;
    }

    first_fs_t first = { .found = 0 };
    memset(&first.info, 0, sizeof(first.info));
    advfs_fileset_list(vol, &domain, first_cb, &first);
    if (!first.found) {
        fprintf(stderr, "no fileset\n");
        return 1;
    }
    printf("fileset: %s (tag %u)\n", first.info.name,
           first.info.dir_tag.num);

    advfs_extent_t fs_ext[ADVFS_MAX_EXTENTS];
    int fs_next = 0;
    if (advfs_fileset_tag_extents(vol, &domain, first.info.dir_tag,
                                  fs_ext, ADVFS_MAX_EXTENTS,
                                  &fs_next) != 0) {
        fprintf(stderr, "no tag extents\n");
        return 1;
    }

    /* Look up the tag in the tag directory. */
    uint32_t tag_page = tag_num / ADVFS_TAGS_PER_PG;
    uint32_t tag_idx = tag_num % ADVFS_TAGS_PER_PG;
    adv_tag_dir_pg_t tag_pg;
    if (advfs_extents_read_data(vol, fs_ext, fs_next, tag_page,
                                &tag_pg) != 0) {
        fprintf(stderr, "cannot read tag dir page %u\n", tag_page);
        return 1;
    }
    adv_tag_map_t *tm = &tag_pg.maps[tag_idx];
    if (!ADV_TMAP_IN_USE(*tm)) {
        fprintf(stderr, "tag %u not in use\n", tag_num);
        return 1;
    }

    uint32_t bmt_pg_num = ADV_MCID_PAGE(tm->u.s3.mcid);
    uint32_t cell = ADV_MCID_CELL(tm->u.s3.mcid);
    printf("tag %u -> vd=%u BMT page %u cell %u\n",
           tag_num, tm->u.s3.vd_index, bmt_pg_num, cell);

    /* Walk the mcell chain, dumping every record. */
    int hops = 0;
    while (hops < 16) {
        adv_bmt_pg_t bmt_pg;
        if (advfs_domain_read_bmt_page(&domain, bmt_pg_num, &bmt_pg) != 0) {
            fprintf(stderr, "cannot read BMT page %u\n", bmt_pg_num);
            return 1;
        }
        adv_mcell_t *mc = advfs_bmt_get_mcell(&bmt_pg, cell);
        if (!mc) {
            break;
        }

        printf("mcell hop %d: BMT page %u cell %u, tag=%u.%u, "
               "set=%u, next=p%u.c%u vd%u seg%u\n",
               hops, bmt_pg_num, cell, mc->tag.num, mc->tag.seq,
               mc->bf_set_tag.num,
               ADV_MCID_PAGE(mc->next_mcid), ADV_MCID_CELL(mc->next_mcid),
               mc->next_vd_index, mc->link_segment);

        char *base = mc->records;
        char *end = base + ADVFS_BSC_R_SZ;
        char *ptr = base;
        while (ptr + ADVFS_REC_HDR_SZ <= end) {
            adv_rec_hdr_t hdr;
            memcpy(&hdr, ptr, sizeof(hdr));
            if (ADV_REC_IS_NIL(hdr)) {
                break;
            }
            uint16_t bcnt = ADV_REC_BCNT(hdr);
            uint8_t type = ADV_REC_TYPE(hdr);
            if (bcnt < ADVFS_REC_HDR_SZ || ptr + bcnt > end) {
                printf("    corrupt record (bcnt=%u)\n", bcnt);
                break;
            }
            printf("    record type=%u ver=%u bcnt=%u (data %u bytes):\n",
                   type, ADV_REC_VERSION(hdr), bcnt,
                   bcnt - ADVFS_REC_HDR_SZ);
            hexdump((uint8_t *)ptr + ADVFS_REC_HDR_SZ,
                    bcnt - ADVFS_REC_HDR_SZ);
            ptr += ADV_REC_NEXT_OFF(bcnt);
        }

        if (mc->next_mcid.raw == 0) {
            break;
        }
        bmt_pg_num = ADV_MCID_PAGE(mc->next_mcid);
        cell = ADV_MCID_CELL(mc->next_mcid);
        hops++;
    }

    advfs_domain_close(&domain);
    advfs_volume_close(vol);
    return 0;
}
