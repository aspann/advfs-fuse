/*
 * test_edge_cases.c -- synthetic edge-case tests (no vdisk needed)
 *
 * Usage: test_edge_cases
 *
 * Crafts malformed directory blocks and synthetic extent maps in
 * memory and verifies the parsers handle them gracefully:
 *   1. Directory entry with entry_size == 0 (no infinite loop)
 *   2. Directory entry crossing the 512-byte block boundary
 *   3. Directory entry with name_count larger than entry_size
 *   4. Sparse hole (PERM_HOLE) pages read as zeros
 *   5. Reads past the extent map's TERM boundary return -ENOENT
 *
 * To reach the static walk_dir_block() in dir.c, this file includes
 * dir.c directly. The external symbol advfs_dir_read is renamed via
 * macro so the test still links against the regular dir.c object.
 *
 * Each test prints "[PASS] name: description" or
 * "[FAIL] name: reason". Exit code 0 if all pass, 1 otherwise.
 *
 * Copyright (C) 2026 Armas Spann
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _XOPEN_SOURCE 500  /* pread(), mkstemp() */
#define ADVFS_TESTING

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Include dir.c to gain access to the static walk_dir_block().
 * Rename its external function so it does not collide with the
 * separately compiled dir.c object linked into this binary.
 */
#define advfs_dir_read advfs_dir_read_included_for_test
#define advfs_dir_read_full advfs_dir_read_full_included_for_test
#define advfs_dir_read_clone advfs_dir_read_clone_included_for_test
#define advfs_dir_read_full_clone advfs_dir_read_full_clone_included_for_test
#define advfs_dir_has_undelete advfs_dir_has_undelete_included_for_test
#include "../src/dir.c"
#undef advfs_dir_has_undelete
#undef advfs_dir_read_full_clone
#undef advfs_dir_read_clone
#undef advfs_dir_read_full
#undef advfs_dir_read

/* Watchdog: abort the whole test run if a parser loops forever. */
#define TEST_TIMEOUT_SECONDS  10

/* Test result counters. */
static int tests_passed;
static int tests_failed;

/* Record a passed test. */
static void test_pass(const char *name, const char *fmt, ...)
{
    va_list ap;
    printf("[PASS] %s: ", name);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    tests_passed++;
}

/* Record a failed test. */
static void test_fail(const char *name, const char *fmt, ...)
{
    va_list ap;
    printf("[FAIL] %s: ", name);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    tests_failed++;
}

/* Names collected by the directory walk callback. */
#define MAX_COLLECTED  16

typedef struct {
    char names[MAX_COLLECTED][MAX_DIR_NAME + 1];
    adv_bf_tag_t tags[MAX_COLLECTED];
    int count;
} collected_t;

/* Callback: record each delivered directory entry. */
static int collect_entry(const char *name, adv_bf_tag_t tag,
                         void *user_data)
{
    collected_t *c = (collected_t *)user_data;
    if (c->count < MAX_COLLECTED) {
        strncpy(c->names[c->count], name, MAX_DIR_NAME);
        c->names[c->count][MAX_DIR_NAME] = '\0';
        c->tags[c->count] = tag;
    }
    c->count++;
    return 0;
}

/*
 * Write one directory entry into a raw block at the given offset.
 * Lays out: tag_num(4), entry_size(2), name_count(2), name (padded),
 * trailing adv_bf_tag_t in the last 8 bytes of the entry.
 * The caller is responsible for entry_size consistency -- malformed
 * values are the point of these tests.
 */
static void put_entry(uint8_t *block, uint32_t off, uint32_t tag_num,
                      uint16_t entry_size, uint16_t name_count,
                      const char *name, uint32_t tag_seq)
{
    memcpy(block + off, &tag_num, sizeof(tag_num));
    memcpy(block + off + 4, &entry_size, sizeof(entry_size));
    memcpy(block + off + 6, &name_count, sizeof(name_count));

    size_t name_len = strlen(name);
    if (off + ADVFS_DIR_ENTRY_HDR_SZ + name_len < ADVFS_BLKSZ) {
        memcpy(block + off + ADVFS_DIR_ENTRY_HDR_SZ, name, name_len + 1);
    }

    if (entry_size >= ADVFS_DIR_ENTRY_HDR_SZ + sizeof(adv_bf_tag_t) &&
        off + entry_size <= ADVFS_BLKSZ) {
        adv_bf_tag_t tag = { .num = tag_num, .seq = tag_seq };
        memcpy(block + off + entry_size - sizeof(adv_bf_tag_t),
               &tag, sizeof(tag));
    }
}

/* Test 1: entry_size == 0 must terminate the walk, not loop forever. */
static void test_entry_size_zero(void)
{
    const char *name = "dir_entry_size_zero";

    uint8_t block[ADVFS_BLKSZ];
    memset(block, 0, sizeof(block));
    put_entry(block, 0, 1, 0, 1, "a", 0x80000001u);

    collected_t c;
    memset(&c, 0, sizeof(c));

    uint32_t deleted = 0;
    int ret = walk_dir_block(block, collect_entry, NULL, &c, &deleted);

    if (ret != 0) {
        test_fail(name, "walk returned %d, expected 0", ret);
        return;
    }
    if (c.count != 0) {
        test_fail(name, "walk delivered %d entries from a zero-size "
                  "entry, expected 0", c.count);
        return;
    }

    test_pass(name, "walk terminates on entry_size == 0 without "
              "delivering entries");
}

/* Test 2: an entry crossing the 512-byte boundary must stop the walk. */
static void test_entry_crosses_boundary(void)
{
    const char *name = "dir_entry_crosses_boundary";

    uint8_t block[ADVFS_BLKSZ];
    memset(block, 0, sizeof(block));

    /* Valid first entry: "a", 20 bytes. */
    put_entry(block, 0, 5, 20, 1, "a", 0x80000005u);

    /* Second entry claims 508 bytes: 20 + 508 = 528 > 512. */
    put_entry(block, 20, 6, 508, 1, "b", 0x80000006u);

    collected_t c;
    memset(&c, 0, sizeof(c));

    uint32_t deleted = 0;
    int ret = walk_dir_block(block, collect_entry, NULL, &c, &deleted);

    if (ret != 0) {
        test_fail(name, "walk returned %d, expected 0", ret);
        return;
    }
    if (c.count != 1 || strcmp(c.names[0], "a") != 0) {
        test_fail(name, "expected exactly entry \"a\", got %d entries",
                  c.count);
        return;
    }

    test_pass(name, "walk stops cleanly at boundary-crossing entry, "
              "valid entry before it still delivered");
}

/* Test 3: name_count larger than entry_size must be skipped gracefully. */
static void test_name_count_too_large(void)
{
    const char *name = "dir_name_count_too_large";

    uint8_t block[ADVFS_BLKSZ];
    memset(block, 0, sizeof(block));

    /* First entry: name_count 200 cannot fit in entry_size 24. */
    put_entry(block, 0, 7, 24, 200, "x", 0x80000007u);

    /* Second entry: valid "b". */
    put_entry(block, 24, 8, 20, 1, "b", 0x80000008u);

    collected_t c;
    memset(&c, 0, sizeof(c));

    uint32_t deleted = 0;
    int ret = walk_dir_block(block, collect_entry, NULL, &c, &deleted);

    if (ret != 0) {
        test_fail(name, "walk returned %d, expected 0", ret);
        return;
    }
    if (c.count != 1 || strcmp(c.names[0], "b") != 0) {
        test_fail(name, "expected only entry \"b\", got %d entries",
                  c.count);
        return;
    }

    test_pass(name, "oversized name_count skipped, following valid "
              "entry still delivered");
}

/*
 * Create a temporary 4-page volume image with a valid AdvFS magic.
 * Page 2 is filled with 0xAB, page 3 with 0xCD. Returns 0 and sets
 * *vol_out on success, -1 on failure.
 */
static int make_test_volume(advfs_volume_t **vol_out, char *path_out,
                            size_t path_sz)
{
    char path[] = "/tmp/advfs_edge_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        return -1;
    }

    uint8_t page[ADVFS_PGSZ];

    /* Pages 0 and 1: zeros (magic patched below). */
    memset(page, 0, sizeof(page));
    for (int i = 0; i < 2; i++) {
        if (write(fd, page, sizeof(page)) != (ssize_t)sizeof(page)) {
            close(fd);
            unlink(path);
            return -1;
        }
    }

    /* Page 2: 0xAB pattern. */
    memset(page, 0xAB, sizeof(page));
    if (write(fd, page, sizeof(page)) != (ssize_t)sizeof(page)) {
        close(fd);
        unlink(path);
        return -1;
    }

    /* Page 3: 0xCD pattern. */
    memset(page, 0xCD, sizeof(page));
    if (write(fd, page, sizeof(page)) != (ssize_t)sizeof(page)) {
        close(fd);
        unlink(path);
        return -1;
    }

    /* Patch in the AdvFS magic (offset 9564 is inside unused page 1). */
    uint32_t magic = ADVFS_MAGIC;
    if (pwrite(fd, &magic, sizeof(magic),
               (off_t)ADVFS_MAGIC_ABS_OFFSET) != (ssize_t)sizeof(magic)) {
        close(fd);
        unlink(path);
        return -1;
    }
    close(fd);

    int err = advfs_volume_open(path, vol_out);
    if (err) {
        unlink(path);
        return -1;
    }

    snprintf(path_out, path_sz, "%s", path);
    return 0;
}

/*
 * Synthetic extent map used by tests 4 and 5:
 *   page 0 -> disk block 32 (vol page 2, 0xAB)
 *   page 1 -> PERM_HOLE (sparse, reads as zeros)
 *   page 2 -> disk block 48 (vol page 3, 0xCD)
 *   page 3 -> TERM (end of bitfile, page count = 3)
 */
static const advfs_extent_t test_extents[] = {
    { .bs_page = 0, .vd_blk = 32 },
    { .bs_page = 1, .vd_blk = ADVFS_PERM_HOLE },
    { .bs_page = 2, .vd_blk = 48 },
    { .bs_page = 3, .vd_blk = ADVFS_XTNT_TERM },
};

#define TEST_EXTENT_COUNT  ((int)(sizeof(test_extents) / sizeof(test_extents[0])))

/* Verify that every byte of buf equals value. Returns 1 if uniform. */
static int buf_is_uniform(const uint8_t *buf, size_t len, uint8_t value)
{
    for (size_t i = 0; i < len; i++) {
        if (buf[i] != value) {
            return 0;
        }
    }
    return 1;
}

/* Test 4: PERM_HOLE pages must zero-fill; mapped pages read real data. */
static void test_perm_hole_zero_fill(advfs_volume_t *vol)
{
    const char *name = "extent_perm_hole_zero_fill";

    uint8_t buf[ADVFS_PGSZ];

    /* Mapped page 0 -> vol page 2 -> 0xAB. */
    memset(buf, 0x55, sizeof(buf));
    int err = advfs_extents_read_data(vol, test_extents,
                                      TEST_EXTENT_COUNT, 0, buf);
    if (err || !buf_is_uniform(buf, sizeof(buf), 0xAB)) {
        test_fail(name, "mapped page 0 read failed (err=%d) or wrong "
                  "data", err);
        return;
    }

    /* Hole page 1 -> zeros. */
    memset(buf, 0x55, sizeof(buf));
    err = advfs_extents_read_data(vol, test_extents,
                                  TEST_EXTENT_COUNT, 1, buf);
    if (err) {
        test_fail(name, "hole page 1 returned err=%d, expected 0", err);
        return;
    }
    if (!buf_is_uniform(buf, sizeof(buf), 0x00)) {
        test_fail(name, "hole page 1 was not zero-filled");
        return;
    }

    /* Mapped page 2 after the hole -> vol page 3 -> 0xCD. */
    memset(buf, 0x55, sizeof(buf));
    err = advfs_extents_read_data(vol, test_extents,
                                  TEST_EXTENT_COUNT, 2, buf);
    if (err || !buf_is_uniform(buf, sizeof(buf), 0xCD)) {
        test_fail(name, "mapped page 2 after hole read failed (err=%d) "
                  "or wrong data", err);
        return;
    }

    test_pass(name, "hole page zero-filled, mapped pages before and "
              "after the hole read correctly");
}

/* Test 5: reads past the TERM boundary must return -ENOENT. */
static void test_synthetic_past_eof(advfs_volume_t *vol)
{
    const char *name = "extent_past_eof_enoent";

    uint8_t buf[ADVFS_PGSZ];

    uint32_t count = advfs_extents_page_count(test_extents,
                                              TEST_EXTENT_COUNT);
    if (count != 3) {
        test_fail(name, "page count is %u, expected 3", count);
        return;
    }

    int err = advfs_extents_read_data(vol, test_extents,
                                      TEST_EXTENT_COUNT, 3, buf);
    if (err != -ENOENT) {
        test_fail(name, "page 3 (TERM boundary) returned %d, expected "
                  "-ENOENT (%d)", err, -ENOENT);
        return;
    }

    err = advfs_extents_read_data(vol, test_extents,
                                  TEST_EXTENT_COUNT, 1000, buf);
    if (err != -ENOENT) {
        test_fail(name, "page 1000 returned %d, expected -ENOENT (%d)",
                  err, -ENOENT);
        return;
    }

    test_pass(name, "page count is 3, pages 3 and 1000 both return "
              "-ENOENT");
}

int main(void)
{
    /* Kill the process if any parser loops forever. */
    alarm(TEST_TIMEOUT_SECONDS);

    /* Directory parser edge cases (pure in-memory). */
    test_entry_size_zero();
    test_entry_crosses_boundary();
    test_name_count_too_large();

    /* Extent edge cases (need a tiny synthetic volume file). */
    advfs_volume_t *vol = NULL;
    char vol_path[64];

    if (make_test_volume(&vol, vol_path, sizeof(vol_path)) != 0) {
        test_fail("extent_perm_hole_zero_fill",
                  "cannot create synthetic volume");
        test_fail("extent_past_eof_enoent",
                  "cannot create synthetic volume");
    } else {
        test_perm_hole_zero_fill(vol);
        test_synthetic_past_eof(vol);
        advfs_volume_close(vol);
        unlink(vol_path);
    }

    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
