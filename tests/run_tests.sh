#!/bin/sh
# tests/run_tests.sh -- build and run all advfs-fuse test programs
#
# Compiles each test binary against the project source files, then
# runs them against the real vdisk and validates the output.
#
# Copyright (C) 2026 Armas Spann
# SPDX-License-Identifier: GPL-2.0-only

set -e

# Locate the project root from this script's own directory.
# Works whether the script is invoked as "sh run_tests.sh" (from tests/)
# or as "sh tests/run_tests.sh" (from the project root).
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)

VDISK="$ROOT/resources/disks/v3/test_single.vdisk"
STRIPE_VDISK="$ROOT/resources/disks/v3/test_stripe_vol1.vdisk"
CLONE_VDISK="$ROOT/resources/disks/v3/test_clone.vdisk"
V4_VDISK="$ROOT/resources/disks/v4/test_single.vdisk"
V4_STRIPE_VDISK="$ROOT/resources/disks/v4/test_stripe_vol1.vdisk"
V4_CLONE_VDISK="$ROOT/resources/disks/v4/test_clone.vdisk"
TESTS_DIR="$ROOT/tests"

CFLAGS="-Wall -Wextra -Werror -std=c11 -O2 -D_FILE_OFFSET_BITS=64"

# Common source files used by all tests except test_volume.
COMMON_SRCS="$ROOT/src/volume.c $ROOT/src/bmt.c $ROOT/src/domain.c $ROOT/src/extents.c $ROOT/src/fileset.c $ROOT/src/dir.c $ROOT/src/util.c"

passed=0
failed=0
build_errors=0

# Temporary directory for build logs and captured test output.
OUTDIR=$(mktemp -d /tmp/advfs_tests.XXXXXX)
trap 'rm -rf "$OUTDIR"' EXIT

printf '=== advfs-fuse test suite ===\n'

# The synthetic tests run without a vdisk; vdisk-based tests are
# skipped (not failed) when the image is absent.
HAVE_VDISK=1
if [ ! -f "$VDISK" ]; then
    printf 'NOTE: vdisk not found: %s\n' "$VDISK"
    printf 'NOTE: vdisk-based tests will be skipped.\n'
    HAVE_VDISK=0
fi

# Stripe tests need volume 1 of the striped test domain.
HAVE_STRIPE_VDISK=1
if [ ! -f "$STRIPE_VDISK" ]; then
    printf 'NOTE: stripe vdisk not found: %s\n' "$STRIPE_VDISK"
    printf 'NOTE: stripe-domain tests will be skipped.\n'
    HAVE_STRIPE_VDISK=0
fi

# V4 (RBMT) tests need the ODS v4 single-volume image.
HAVE_V4_VDISK=1
if [ ! -f "$V4_VDISK" ]; then
    printf 'NOTE: v4 vdisk not found: %s\n' "$V4_VDISK"
    printf 'NOTE: v4 vdisk-based tests will be skipped.\n'
    HAVE_V4_VDISK=0
fi

# V4 stripe tests need volume 1 of the ODS v4 striped test domain.
HAVE_V4_STRIPE_VDISK=1
if [ ! -f "$V4_STRIPE_VDISK" ]; then
    printf 'NOTE: v4 stripe vdisk not found: %s\n' "$V4_STRIPE_VDISK"
    printf 'NOTE: v4 stripe-domain tests will be skipped.\n'
    HAVE_V4_STRIPE_VDISK=0
fi

# Clone tests need the ODS v3 clone image (primary + clone_snap filesets).
HAVE_CLONE_VDISK=1
if [ ! -f "$CLONE_VDISK" ]; then
    printf 'NOTE: clone vdisk not found: %s\n' "$CLONE_VDISK"
    printf 'NOTE: clone-fileset tests will be skipped.\n'
    HAVE_CLONE_VDISK=0
fi

# V4 clone tests need the ODS v4 clone image.
HAVE_V4_CLONE_VDISK=1
if [ ! -f "$V4_CLONE_VDISK" ]; then
    printf 'NOTE: v4 clone vdisk not found: %s\n' "$V4_CLONE_VDISK"
    printf 'NOTE: v4 clone-fileset tests will be skipped.\n'
    HAVE_V4_CLONE_VDISK=0
fi

# ---------------------------------------------------------------------------
# Build helper
# ---------------------------------------------------------------------------

# do_build: compile one test binary and print the result.
# Usage: do_build <label> <output-path> [compiler-flags-and-sources...]
# Returns 0 on success, 1 on failure (increments build_errors).
do_build() {
    label="$1"
    out="$2"
    shift 2
    printf '  [BUILD] %-15s ... ' "$label"
    # $CFLAGS is intentionally unquoted so flags are word-split.
    # shellcheck disable=SC2086
    if gcc $CFLAGS -o "$out" "$@" -lpthread \
            >"$OUTDIR/${label}.build.log" 2>&1; then
        printf 'ok\n'
        return 0
    else
        printf 'FAILED\n'
        cat "$OUTDIR/${label}.build.log"
        build_errors=$((build_errors + 1))
        return 1
    fi
}

# ---------------------------------------------------------------------------
# Run helpers
# ---------------------------------------------------------------------------

# pass_test: record a passed test and print the one-line summary.
pass_test() {
    label="$1"
    summary="$2"
    passed=$((passed + 1))
    printf '  [PASS] %s: %s\n' "$label" "$summary"
}

# fail_test: record a failed test and dump its captured output.
fail_test() {
    label="$1"
    reason="$2"
    outfile="$3"
    failed=$((failed + 1))
    printf '  [FAIL] %s: %s\n' "$label" "$reason"
    if [ -f "$outfile" ]; then
        printf '    --- output ---\n'
        sed 's/^/    /' "$outfile"
        printf '    --- end ---\n'
    fi
}

# ---------------------------------------------------------------------------
# Build phase
# ---------------------------------------------------------------------------

printf 'Building tests...\n'

# test_volume: volume + util only; no ADVFS_DEBUG needed.
do_build test_volume "$TESTS_DIR/test_volume" \
    "$TESTS_DIR/test_volume.c" \
    "$ROOT/src/volume.c" \
    "$ROOT/src/util.c" || true

# test_domain: all common sources with ADVFS_DEBUG.
# $COMMON_SRCS is intentionally unquoted for word splitting.
# shellcheck disable=SC2086
do_build test_domain "$TESTS_DIR/test_domain" \
    -DADVFS_DEBUG \
    "$TESTS_DIR/test_domain.c" $COMMON_SRCS || true

# test_fileset: all common sources with ADVFS_DEBUG.
# shellcheck disable=SC2086
do_build test_fileset "$TESTS_DIR/test_fileset" \
    -DADVFS_DEBUG \
    "$TESTS_DIR/test_fileset.c" $COMMON_SRCS || true

# test_dir: all common sources with ADVFS_DEBUG.
# shellcheck disable=SC2086
do_build test_dir "$TESTS_DIR/test_dir" \
    -DADVFS_DEBUG \
    "$TESTS_DIR/test_dir.c" $COMMON_SRCS || true

# test_extents: all common sources with ADVFS_DEBUG.
# shellcheck disable=SC2086
do_build test_extents "$TESTS_DIR/test_extents" \
    -DADVFS_DEBUG \
    "$TESTS_DIR/test_extents.c" $COMMON_SRCS || true

# test_edge_cases: all common sources with ADVFS_DEBUG.
# shellcheck disable=SC2086
do_build test_edge_cases "$TESTS_DIR/test_edge_cases" \
    -DADVFS_DEBUG \
    "$TESTS_DIR/test_edge_cases.c" $COMMON_SRCS || true

# test_stripe: all common sources with ADVFS_DEBUG.
# shellcheck disable=SC2086
do_build test_stripe "$TESTS_DIR/test_stripe" \
    -DADVFS_DEBUG \
    "$TESTS_DIR/test_stripe.c" $COMMON_SRCS || true

# test_filedata: all common sources + filedata.c with ADVFS_DEBUG.
# shellcheck disable=SC2086
do_build test_filedata "$TESTS_DIR/test_filedata" \
    -DADVFS_DEBUG \
    "$TESTS_DIR/test_filedata.c" $COMMON_SRCS \
    "$ROOT/src/filedata.c" || true

# test_clone: all common sources + filedata.c with ADVFS_DEBUG.
# shellcheck disable=SC2086
do_build test_clone "$TESTS_DIR/test_clone" \
    -DADVFS_DEBUG \
    "$TESTS_DIR/test_clone.c" $COMMON_SRCS \
    "$ROOT/src/filedata.c" || true

if [ "$build_errors" -gt 0 ]; then
    printf '\n%d build(s) failed -- aborting.\n' "$build_errors"
    exit 1
fi

# ---------------------------------------------------------------------------
# Run phase
# ---------------------------------------------------------------------------

printf '\nRunning tests...\n'

# ---------------------------------------------------------------------------
# Vdisk-based tests (skipped when the image is absent).
# The body below is intentionally not re-indented to keep the diff small.
# ---------------------------------------------------------------------------
if [ "$HAVE_VDISK" -eq 1 ]; then

# --- test_volume ---
# Expected output: "AdvFS detected: <path>" and "ODS version: 3 ..."
outfile="$OUTDIR/test_volume.out"
if "$TESTS_DIR/test_volume" "$VDISK" >"$outfile" 2>&1; then
    ok=1
    grep -q "AdvFS detected" "$outfile" || ok=0
    grep -q "ODS version: 3"  "$outfile" || ok=0
    if [ "$ok" -eq 1 ]; then
        pages=$(grep "pages)" "$outfile" \
            | sed 's/.*(\([0-9]*\) pages).*/\1/' | head -1)
        pass_test "test_volume" "AdvFS detected, ODS v3, ${pages} pages"
    else
        fail_test "test_volume" "output validation failed" "$outfile"
    fi
else
    fail_test "test_volume" "non-zero exit" "$outfile"
fi

# --- test_domain ---
# Expected output: "Domain ID: <sec>.<usec>" and "ODS version: <n>"
outfile="$OUTDIR/test_domain.out"
if "$TESTS_DIR/test_domain" "$VDISK" >"$outfile" 2>&1; then
    ok=1
    grep -q "Domain ID:"  "$outfile" || ok=0
    grep -q "ODS version" "$outfile" || ok=0
    if [ "$ok" -eq 1 ]; then
        did=$(grep "Domain ID:" "$outfile" | head -1 \
            | sed 's/.*Domain ID:[[:space:]]*//')
        ods=$(grep "ODS version" "$outfile" | head -1 \
            | sed 's/.*ODS version:[[:space:]]*\([0-9]*\).*/\1/')
        pass_test "test_domain" "Domain ${did}, ODS v${ods}"
    else
        fail_test "test_domain" "output validation failed" "$outfile"
    fi
else
    fail_test "test_domain" "non-zero exit" "$outfile"
fi

# --- test_fileset ---
# Expected output: fileset named "primary" (plus the clone
# "primary_snap") and "Total: N fileset(s)" with N > 0.
outfile="$OUTDIR/test_fileset.out"
if "$TESTS_DIR/test_fileset" "$VDISK" >"$outfile" 2>&1; then
    ok=1
    grep -q "Name:[[:space:]]*primary" "$outfile" || ok=0
    grep -q "^Total: [1-9][0-9]* fileset" "$outfile" || ok=0
    if [ "$ok" -eq 1 ]; then
        total=$(grep "^Total: [0-9]* fileset" "$outfile" | head -1 \
            | sed 's/Total: \([0-9]*\) fileset.*/\1/')
        name=$(grep "Name:" "$outfile" | head -1 \
            | sed 's/.*Name:[[:space:]]*//')
        pass_test "test_fileset" "${total} fileset(s) found (${name})"
    else
        fail_test "test_fileset" "output validation failed" "$outfile"
    fi
else
    fail_test "test_fileset" "non-zero exit" "$outfile"
fi

# --- test_dir ---
# Expected output: "." and ".." entries, the known test files
# (random_1k.bin, random_4k.bin, subdir) and "Total: N entries"
# with N > 0.
# The directory listing format is "  [%3d] %-30s  tag=%u.%u", so the
# "." entry appears as "  [  1] . " and ".." as "  [  2] .. ".
outfile="$OUTDIR/test_dir.out"
if "$TESTS_DIR/test_dir" "$VDISK" >"$outfile" 2>&1; then
    ok=1
    grep -qF "] . "  "$outfile" || ok=0
    grep -qF "] .. " "$outfile" || ok=0
    grep -qF "random_1k.bin" "$outfile" || ok=0
    grep -qF "random_4k.bin" "$outfile" || ok=0
    grep -qF "subdir"        "$outfile" || ok=0
    entries=$(grep "^Total:" "$outfile" | tail -1 \
        | sed 's/Total: \([0-9]*\) entries.*/\1/')
    [ -n "$entries" ] && [ "$entries" -gt 0 ] || ok=0
    if [ "$ok" -eq 1 ]; then
        pass_test "test_dir" "${entries} entries in root directory"
    else
        fail_test "test_dir" "output validation failed" "$outfile"
    fi
else
    fail_test "test_dir" "non-zero exit" "$outfile"
fi

# --- test_extents ---
# Expected output: "[PASS]" lines only, no "[FAIL]".
outfile="$OUTDIR/test_extents.out"
if "$TESTS_DIR/test_extents" "$VDISK" >"$outfile" 2>&1; then
    ok=1
    grep -q  "\[PASS\]" "$outfile" || ok=0
    grep -q  "\[FAIL\]" "$outfile" && ok=0
    if [ "$ok" -eq 1 ]; then
        npass=$(grep -c "\[PASS\]" "$outfile")
        pass_test "test_extents" "${npass} extent edge-case checks passed"
    else
        fail_test "test_extents" "output validation failed" "$outfile"
    fi
else
    fail_test "test_extents" "non-zero exit" "$outfile"
fi

fi # HAVE_VDISK

# ---------------------------------------------------------------------------
# Stripe-domain tests (volume 1 of the two-volume striped domain).
# Skipped when the stripe image is absent.
# ---------------------------------------------------------------------------
if [ "$HAVE_STRIPE_VDISK" -eq 1 ]; then

# --- test_stripe ---
# Expected output: "[PASS]" lines only, no "[FAIL]", plus the
# "striped file detected" warning from advfs_extents_read() proving
# that BSXMT_STRIPE detection fired.
outfile="$OUTDIR/test_stripe.out"
if "$TESTS_DIR/test_stripe" "$STRIPE_VDISK" >"$outfile" 2>&1; then
    ok=1
    grep -q "\[PASS\]" "$outfile" || ok=0
    grep -q "\[FAIL\]" "$outfile" && ok=0
    grep -q "striped file detected" "$outfile" || ok=0
    if [ "$ok" -eq 1 ]; then
        npass=$(grep -c "\[PASS\]" "$outfile")
        pass_test "test_stripe" \
            "${npass} stripe-domain checks passed, stripe warning fired"
    else
        fail_test "test_stripe" "output validation failed" "$outfile"
    fi
else
    fail_test "test_stripe" "non-zero exit" "$outfile"
fi

fi # HAVE_STRIPE_VDISK

# ---------------------------------------------------------------------------
# File data reading + hash verification (V3).
# Reads file contents through the parser, computes POSIX cksum, compares
# against known-good values from Tru64 (CMD_ENV=xpg4).
# ---------------------------------------------------------------------------
if [ "$HAVE_VDISK" -eq 1 ]; then

outfile="$OUTDIR/test_filedata.out"
if "$TESTS_DIR/test_filedata" "$VDISK" >"$outfile" 2>&1; then
    ok=1
    grep -q "\[PASS\]" "$outfile" || ok=0
    grep -q "\[FAIL\]" "$outfile" && ok=0
    if [ "$ok" -eq 1 ]; then
        npass=$(grep -c "\[PASS\]" "$outfile")
        pass_test "test_filedata" "${npass} file data checks passed (V3)"
    else
        fail_test "test_filedata" "output validation failed" "$outfile"
    fi
else
    fail_test "test_filedata" "non-zero exit" "$outfile"
fi

fi # HAVE_VDISK

# ---------------------------------------------------------------------------
# Clone (copy-on-write snapshot) file data verification (V3).
# Selects the clone_snap fileset and reads files whose pages are
# PERM_HOLEs served from the original fileset via the clone fallback.
# ---------------------------------------------------------------------------
if [ "$HAVE_CLONE_VDISK" -eq 1 ]; then

outfile="$OUTDIR/test_clone.out"
if "$TESTS_DIR/test_clone" "$CLONE_VDISK" >"$outfile" 2>&1; then
    ok=1
    grep -q "\[PASS\]" "$outfile" || ok=0
    grep -q "\[FAIL\]" "$outfile" && ok=0
    if [ "$ok" -eq 1 ]; then
        npass=$(grep -c "\[PASS\]" "$outfile")
        pass_test "test_clone" "${npass} clone file data checks passed (V3)"
    else
        fail_test "test_clone" "output validation failed" "$outfile"
    fi
else
    fail_test "test_clone" "non-zero exit" "$outfile"
fi

fi # HAVE_CLONE_VDISK

# ---------------------------------------------------------------------------
# ODS v4 (RBMT) vdisk-based tests. Same binaries, run against the v4
# images. Skipped when the v4 image is absent.
# ---------------------------------------------------------------------------
if [ "$HAVE_V4_VDISK" -eq 1 ]; then

# --- test_volume (v4) ---
# Expected output: "AdvFS detected: <path>" and "ODS version: 4 ..."
outfile="$OUTDIR/test_volume_v4.out"
if "$TESTS_DIR/test_volume" "$V4_VDISK" >"$outfile" 2>&1; then
    ok=1
    grep -q "AdvFS detected" "$outfile" || ok=0
    grep -q "ODS version: 4"  "$outfile" || ok=0
    if [ "$ok" -eq 1 ]; then
        pages=$(grep "pages)" "$outfile" \
            | sed 's/.*(\([0-9]*\) pages).*/\1/' | head -1)
        pass_test "test_volume_v4" "AdvFS detected, ODS v4, ${pages} pages"
    else
        fail_test "test_volume_v4" "output validation failed" "$outfile"
    fi
else
    fail_test "test_volume_v4" "non-zero exit" "$outfile"
fi

# --- test_domain (v4) ---
# Expected output: "Domain ID: <sec>.<usec>" and "ODS version: 4"
outfile="$OUTDIR/test_domain_v4.out"
if "$TESTS_DIR/test_domain" "$V4_VDISK" >"$outfile" 2>&1; then
    ok=1
    grep -q "Domain ID:" "$outfile" || ok=0
    grep -q "ODS version:[[:space:]]*4" "$outfile" || ok=0
    if [ "$ok" -eq 1 ]; then
        did=$(grep "Domain ID:" "$outfile" | head -1 \
            | sed 's/.*Domain ID:[[:space:]]*//')
        pass_test "test_domain_v4" "Domain ${did}, ODS v4"
    else
        fail_test "test_domain_v4" "output validation failed" "$outfile"
    fi
else
    fail_test "test_domain_v4" "non-zero exit" "$outfile"
fi

# --- test_fileset (v4) ---
# Expected output: fileset named "primary" (plus the clone
# "primary_snap") and "Total: N fileset(s)" with N > 0.
outfile="$OUTDIR/test_fileset_v4.out"
if "$TESTS_DIR/test_fileset" "$V4_VDISK" >"$outfile" 2>&1; then
    ok=1
    grep -q "Name:[[:space:]]*primary" "$outfile" || ok=0
    grep -q "^Total: [1-9][0-9]* fileset" "$outfile" || ok=0
    if [ "$ok" -eq 1 ]; then
        total=$(grep "^Total: [0-9]* fileset" "$outfile" | head -1 \
            | sed 's/Total: \([0-9]*\) fileset.*/\1/')
        name=$(grep "Name:" "$outfile" | head -1 \
            | sed 's/.*Name:[[:space:]]*//')
        pass_test "test_fileset_v4" "${total} fileset(s) found (${name})"
    else
        fail_test "test_fileset_v4" "output validation failed" "$outfile"
    fi
else
    fail_test "test_fileset_v4" "non-zero exit" "$outfile"
fi

# --- test_dir (v4) ---
# Expected output: "." and ".." entries, the known test files
# (random_1k.bin, random_4k.bin, subdir) and "Total: N entries"
# with N > 0.
outfile="$OUTDIR/test_dir_v4.out"
if "$TESTS_DIR/test_dir" "$V4_VDISK" >"$outfile" 2>&1; then
    ok=1
    grep -qF "] . "  "$outfile" || ok=0
    grep -qF "] .. " "$outfile" || ok=0
    grep -qF "random_1k.bin" "$outfile" || ok=0
    grep -qF "random_4k.bin" "$outfile" || ok=0
    grep -qF "subdir"        "$outfile" || ok=0
    entries=$(grep "^Total:" "$outfile" | tail -1 \
        | sed 's/Total: \([0-9]*\) entries.*/\1/')
    [ -n "$entries" ] && [ "$entries" -gt 0 ] || ok=0
    if [ "$ok" -eq 1 ]; then
        pass_test "test_dir_v4" "${entries} entries in root directory"
    else
        fail_test "test_dir_v4" "output validation failed" "$outfile"
    fi
else
    fail_test "test_dir_v4" "non-zero exit" "$outfile"
fi

# --- test_extents (v4) ---
# Expected output: "[PASS]" lines only, no "[FAIL]".
outfile="$OUTDIR/test_extents_v4.out"
if "$TESTS_DIR/test_extents" "$V4_VDISK" >"$outfile" 2>&1; then
    ok=1
    grep -q  "\[PASS\]" "$outfile" || ok=0
    grep -q  "\[FAIL\]" "$outfile" && ok=0
    if [ "$ok" -eq 1 ]; then
        npass=$(grep -c "\[PASS\]" "$outfile")
        pass_test "test_extents_v4" "${npass} extent edge-case checks passed"
    else
        fail_test "test_extents_v4" "output validation failed" "$outfile"
    fi
else
    fail_test "test_extents_v4" "non-zero exit" "$outfile"
fi

fi # HAVE_V4_VDISK

# ---------------------------------------------------------------------------
# ODS v4 stripe-domain tests (volume 1 of the v4 striped domain).
# Skipped when the v4 stripe image is absent.
# ---------------------------------------------------------------------------
if [ "$HAVE_V4_STRIPE_VDISK" -eq 1 ]; then

# --- test_stripe (v4) ---
# Expected output: "[PASS]" lines only, no "[FAIL]", plus the
# "striped file detected" warning from the synthetic fixture test.
# The second argument tells the binary to expect ODS v4.
outfile="$OUTDIR/test_stripe_v4.out"
if "$TESTS_DIR/test_stripe" "$V4_STRIPE_VDISK" 4 >"$outfile" 2>&1; then
    ok=1
    grep -q "\[PASS\]" "$outfile" || ok=0
    grep -q "\[FAIL\]" "$outfile" && ok=0
    grep -q "striped file detected" "$outfile" || ok=0
    if [ "$ok" -eq 1 ]; then
        npass=$(grep -c "\[PASS\]" "$outfile")
        pass_test "test_stripe_v4" \
            "${npass} stripe-domain checks passed, stripe warning fired"
    else
        fail_test "test_stripe_v4" "output validation failed" "$outfile"
    fi
else
    fail_test "test_stripe_v4" "non-zero exit" "$outfile"
fi

fi # HAVE_V4_STRIPE_VDISK

# ---------------------------------------------------------------------------
# File data reading + hash verification (V4).
# ---------------------------------------------------------------------------
if [ "$HAVE_V4_VDISK" -eq 1 ]; then

outfile="$OUTDIR/test_filedata_v4.out"
if "$TESTS_DIR/test_filedata" "$V4_VDISK" >"$outfile" 2>&1; then
    ok=1
    grep -q "\[PASS\]" "$outfile" || ok=0
    grep -q "\[FAIL\]" "$outfile" && ok=0
    if [ "$ok" -eq 1 ]; then
        npass=$(grep -c "\[PASS\]" "$outfile")
        pass_test "test_filedata_v4" "${npass} file data checks passed (V4)"
    else
        fail_test "test_filedata_v4" "output validation failed" "$outfile"
    fi
else
    fail_test "test_filedata_v4" "non-zero exit" "$outfile"
fi

fi # HAVE_V4_VDISK

# ---------------------------------------------------------------------------
# Clone (copy-on-write snapshot) file data verification (V4).
# ---------------------------------------------------------------------------
if [ "$HAVE_V4_CLONE_VDISK" -eq 1 ]; then

outfile="$OUTDIR/test_clone_v4.out"
if "$TESTS_DIR/test_clone" "$V4_CLONE_VDISK" >"$outfile" 2>&1; then
    ok=1
    grep -q "\[PASS\]" "$outfile" || ok=0
    grep -q "\[FAIL\]" "$outfile" && ok=0
    if [ "$ok" -eq 1 ]; then
        npass=$(grep -c "\[PASS\]" "$outfile")
        pass_test "test_clone_v4" "${npass} clone file data checks passed (V4)"
    else
        fail_test "test_clone_v4" "output validation failed" "$outfile"
    fi
else
    fail_test "test_clone_v4" "non-zero exit" "$outfile"
fi

fi # HAVE_V4_CLONE_VDISK

# ---------------------------------------------------------------------------
# Synthetic tests (no vdisk needed) -- always run.
# ---------------------------------------------------------------------------

# --- test_edge_cases ---
# Expected output: "[PASS]" lines only, no "[FAIL]".
outfile="$OUTDIR/test_edge_cases.out"
if "$TESTS_DIR/test_edge_cases" >"$outfile" 2>&1; then
    ok=1
    grep -q  "\[PASS\]" "$outfile" || ok=0
    grep -q  "\[FAIL\]" "$outfile" && ok=0
    if [ "$ok" -eq 1 ]; then
        npass=$(grep -c "\[PASS\]" "$outfile")
        pass_test "test_edge_cases" "${npass} synthetic edge-case checks passed"
    else
        fail_test "test_edge_cases" "output validation failed" "$outfile"
    fi
else
    fail_test "test_edge_cases" "non-zero exit" "$outfile"
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

total_tests=$((passed + failed))
printf '\n=== %d/%d tests passed ===\n' "$passed" "$total_tests"

if [ "$failed" -gt 0 ]; then
    exit 1
fi
exit 0
