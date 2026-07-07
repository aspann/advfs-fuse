/*
 * main.c -- entry point for advfs-fuse
 *
 * Usage: advfs-fuse <vdisk-path> [vdisk2 ...] <mountpoint> [FUSE options]
 *
 * Opens one or more vdisks (multi-volume domain support), discovers the
 * domain, picks the first fileset, resolves its tag directory extents
 * once, then hands control to fuse_main().  The filesystem is forced
 * read-only ("-o ro") and single-threaded ("-s") -- correctness over
 * throughput for an archival read-only driver.
 *
 * The last non-option argument is interpreted as the FUSE mountpoint;
 * every non-option argument before it is a vdisk path.  FUSE options
 * start with '-'; the value of a '-o' option is skipped when scanning
 * for the mountpoint.
 *
 * Signals: fuse_main() installs SIGINT/SIGTERM handlers that exit
 * the event loop and unmount cleanly; teardown below then closes
 * the domain and volumes.
 *
 * Copyright (C) 2026 Armas Spann
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "fuse_ops.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fileset.h"
#include "util.h"

/* Extra argv slots we may append: "-s", "-o", "ro", NULL. */
#define EXTRA_FUSE_ARGS 4

/*
 * Fileset capture state for the enumeration callback.
 *
 * target_name: when non-NULL, stop at the first fileset whose name
 *              matches (clones are allowed).
 *              When NULL, use the first non-clone fileset (legacy
 *              behaviour: prefer originals, fall back to the first
 *              clone encountered as a last resort).
 */
typedef struct {
    advfs_fileset_info_t info;
    int                  found;
    const char          *target_name;
} first_fileset_t;

/* Fileset enumeration callback: keep the first (or named) fileset. */
static int capture_first_fileset(const advfs_fileset_info_t *info,
                                 void *user_data)
{
    first_fileset_t *fs = (first_fileset_t *)user_data;

    if (fs->target_name != NULL) {
        /* Named-fileset mode: stop only when the name matches. */
        if (strcmp(info->name, fs->target_name) == 0) {
            fs->info  = *info;
            fs->found = 1;
            return 1;  /* stop enumeration */
        }
        return 0;  /* continue */
    }

    /* Default mode: prefer the first original (non-clone) fileset. */
    if (info->clone_id != 0 && !fs->found) {
        /* Remember a clone only as a last resort. */
        if (fs->info.name[0] == '\0') {
            fs->info = *info;
        }
        return 0;
    }

    fs->info  = *info;
    fs->found = 1;
    return 1;  /* stop at the first original fileset */
}

/* Print usage to stderr. */
static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [--fileset <name>] <vdisk> [vdisk2 ...] "
            "<mountpoint> [FUSE options]\n"
            "\n"
            "Mount a Tru64 AdvFS vdisk image read-only via FUSE.\n"
            "Multiple vdisk paths may be given for multi-volume domains.\n"
            "The last non-option argument is the FUSE mountpoint.\n"
            "By default the first non-clone fileset is mounted.\n"
            "\n"
            "Options:\n"
            "  --fileset <name>   Mount the named fileset (clones allowed).\n"
            "\n"
            "Common FUSE options:\n"
            "  -f    stay in the foreground\n"
            "  -d    FUSE debug output (implies -f)\n",
            prog);
}

int main(int argc, char *argv[])
{
    if (argc >= 2 && (strcmp(argv[1], "-h") == 0 ||
                      strcmp(argv[1], "--help") == 0)) {
        usage(argv[0]);
        return 0;
    }
    if (argc < 3) {
        usage(argv[0]);
        return 2;
    }

    /*
     * Pre-pass: extract --fileset <name> and build a filtered argv
     * that omits those two entries.  The rest of main() uses the
     * filtered view so that the clone_snap argument is never mistaken
     * for a vdisk path or a FUSE mountpoint.
     */
    const char *fileset_name = NULL;
    char **filt_argv = calloc((size_t)argc, sizeof(char *));
    if (!filt_argv) {
        advfs_err("main: out of memory");
        return 1;
    }
    int filt_argc = 0;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--fileset") == 0 && i + 1 < argc) {
            fileset_name = argv[i + 1];
            i++;  /* skip the fileset name too */
            continue;
        }
        filt_argv[filt_argc++] = argv[i];
    }

    /*
     * Find the mountpoint: the last non-option argument in filt_argv.
     * FUSE options start with '-'.  The value of a '-o' option is the
     * following argument (which does not start with '-'); we must not
     * mistake it for the mountpoint.
     * Scan backward so we find the mountpoint without a full getopt pass.
     */
    int mountpoint_idx = -1;
    int i = filt_argc - 1;
    while (i >= 1) {
        if (filt_argv[i][0] == '-') {
            /* FUSE flag, skip. */
            i--;
        } else if (i > 1 && strcmp(filt_argv[i - 1], "-o") == 0) {
            /* Value of a '-o' option, skip both. */
            i -= 2;
        } else {
            mountpoint_idx = i;
            break;
        }
    }

    if (mountpoint_idx < 0) {
        usage(argv[0]);
        free(filt_argv);
        return 2;
    }

    /* Everything between filt_argv[1] and filt_argv[mountpoint_idx-1]
     * (inclusive) that does not start with '-' is a vdisk path. */
    const char *vdisk_paths[ADVFS_MAX_DOM_VDS];
    int nvdisks = 0;
    for (int j = 1; j < mountpoint_idx; j++) {
        if (filt_argv[j][0] == '-') {
            continue;  /* unexpected option before mountpoint -- ignore */
        }
        if (nvdisks >= ADVFS_MAX_DOM_VDS) {
            advfs_err("main: too many vdisk paths (max %d)",
                      ADVFS_MAX_DOM_VDS);
            free(filt_argv);
            return 2;
        }
        vdisk_paths[nvdisks++] = filt_argv[j];
    }

    if (nvdisks == 0) {
        usage(argv[0]);
        free(filt_argv);
        return 2;
    }

    const char *mountpoint = filt_argv[mountpoint_idx];

    /* Step 1: open all volumes and discover the domain. */
    advfs_volume_t *vols[ADVFS_MAX_DOM_VDS];
    for (int j = 0; j < nvdisks; j++) {
        vols[j] = NULL;
    }

    for (int j = 0; j < nvdisks; j++) {
        int err = advfs_volume_open(vdisk_paths[j], &vols[j]);
        if (err) {
            advfs_err("main: cannot open volume '%s' (err=%d)",
                      vdisk_paths[j], err);
            for (int k = 0; k < j; k++) {
                advfs_volume_close(vols[k]);
            }
            free(filt_argv);
            return 1;
        }
    }

    advfs_domain_t domain;
    int err = advfs_domain_open_multi(vols, nvdisks, &domain);
    if (err) {
        advfs_err("main: cannot open domain (err=%d)", err);
        for (int j = 0; j < nvdisks; j++) {
            advfs_volume_close(vols[j]);
        }
        free(filt_argv);
        return 1;
    }

    advfs_volume_t *vol = vols[0];  /* primary volume */

    /* Step 2: pick the target fileset. */
    first_fileset_t first;
    memset(&first, 0, sizeof(first));
    first.target_name = fileset_name;  /* NULL = first non-clone */

    err = advfs_fileset_list(vol, &domain, capture_first_fileset, &first);
    if (err || first.info.name[0] == '\0') {
        if (fileset_name != NULL) {
            advfs_err("main: fileset '%s' not found", fileset_name);
        } else {
            advfs_err("main: no filesets found (err=%d)", err);
        }
        advfs_domain_close(&domain);
        for (int j = 0; j < nvdisks; j++) {
            advfs_volume_close(vols[j]);
        }
        free(filt_argv);
        return 1;
    }
    advfs_fileset_log_clone(&first.info);

    /* Step 3: resolve the fileset tag directory extents (fixed for
     * the lifetime of the mount, resolved exactly once). */
    advfs_fuse_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.vol = vol;
    ctx.domain = &domain;
    ctx.fs_tag = first.info.dir_tag;
    snprintf(ctx.fs_name, sizeof(ctx.fs_name), "%s", first.info.name);

    /* Record clone/snapshot info so the read path can fall back to the
     * original fileset for copy-on-write (permanent-hole) pages. Only a
     * clone fileset (clone_id != 0) enables the fallback. */
    ctx.is_clone = (first.info.clone_id != 0);
    ctx.orig_set_tag = first.info.orig_set_tag;
    if (ctx.is_clone) {
        fprintf(stderr, "advfs-fuse: fileset '%s' is a clone; permanent-hole "
                "pages will read from original fileset tag %u\n",
                first.info.name, first.info.orig_set_tag.num);
    }

    err = advfs_fileset_tag_extents(vol, &domain, first.info.dir_tag,
                                    ctx.fs_tag_extents, ADVFS_MAX_EXTENTS,
                                    &ctx.fs_tag_num_extents);
    if (err) {
        advfs_err("main: cannot resolve fileset tag dir extents "
                  "(err=%d)", err);
        advfs_domain_close(&domain);
        for (int j = 0; j < nvdisks; j++) {
            advfs_volume_close(vols[j]);
        }
        free(filt_argv);
        return 1;
    }

    advfs_fuse_ctx_init_cache(&ctx);

    fprintf(stderr, "advfs-fuse: mounting fileset '%s' (ODS v%d) from "
            "%s on %s (read-only)\n",
            ctx.fs_name, domain.ods_version, vdisk_paths[0], mountpoint);

    /*
     * Step 4: build the FUSE argument vector.  Drop all vdisk paths;
     * keep the program name, the mountpoint and any user options;
     * force single-threaded, read-only operation.
     */
    int fuse_argc = 0;
    char **fuse_argv = calloc((size_t)filt_argc + EXTRA_FUSE_ARGS,
                              sizeof(char *));
    if (!fuse_argv) {
        advfs_err("main: out of memory");
        advfs_fuse_ctx_drop_cache(&ctx);
        advfs_domain_close(&domain);
        for (int j = 0; j < nvdisks; j++) {
            advfs_volume_close(vols[j]);
        }
        free(filt_argv);
        return 1;
    }

    fuse_argv[fuse_argc++] = filt_argv[0];
    /* Pass the mountpoint and everything after it (FUSE options). */
    for (int j = mountpoint_idx; j < filt_argc; j++) {
        fuse_argv[fuse_argc++] = filt_argv[j];
    }
    fuse_argv[fuse_argc++] = (char *)"-s";
    fuse_argv[fuse_argc++] = (char *)"-o";
    fuse_argv[fuse_argc++] = (char *)"ro";

    /* Step 5: run the FUSE event loop until unmount or signal. */
    int ret = fuse_main(fuse_argc, fuse_argv, advfs_fuse_get_ops(), &ctx);

    /* Step 6: teardown. */
    free(fuse_argv);
    free(filt_argv);
    advfs_fuse_ctx_drop_cache(&ctx);
    advfs_domain_close(&domain);
    for (int j = 0; j < nvdisks; j++) {
        advfs_volume_close(vols[j]);
    }

    return ret;
}
