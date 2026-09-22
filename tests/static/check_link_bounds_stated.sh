#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Eleven bounds are read out of the image's own layout through KICKOS_LINK_BOUND
# (include/kickos/klink.h): __kickos_code_{start,end}, __kickos_appdata_{start,end},
# _kickos_heap_{start,limit}, __kickos_app_{rom,sram}_{start,end} and
# __kickos_app_load_delta. The reference is STRONG, so a chip script that states none
# fails the link naming the symbol; a chip that carves no such window states it EMPTY
# instead (start == end), which is what arch/common/sections.ld.h's three *_NONE macros
# do. See docs/reference/invariants.md, link-bounds-stated-or-the-link-fails.
#
# THIS GATE ASSERTS OVER THE PREPROCESSED SCRIPT, NOT THE SOURCE ONE. Every chip .ld is
# run through cpp before the link (arch/CMakeLists.txt, the kickos_ldscript_<chip>
# target), and the assignment for a bound routinely sits behind #if KICKOS_HAVE_MPU with
# an #else on the *_NONE macro. A grep over the source file cannot tell "assigned in the
# branch that survived" from "assigned only in the branch that didn't", so it answers the
# wrong question; only the file cpp already resolved does. Give this gate the GENERATED
# script (CMAKE_CURRENT_BINARY_DIR/<chip>.ld under the arch build directory, the same
# file the link itself reads with -T), never a copy made for the occasion.
#
# RX SPELLS ITS SYMBOLS DIFFERENTLY. arch/rx/chip/rx72m/rx72m.ld defines
# KICKOS_LD_C_SYM(name) as _##name before including the shared header, so every bound
# there carries one extra leading underscore (___kickos_app_rom_start for the C name
# __kickos_app_rom_start). This gate accepts either spelling for every target: the C
# name, or that same name with one leading underscore prepended. It does not need to be
# told which target is RX, and accepting both on every target costs nothing because no
# other backend's psABI adds a leading underscore to a C identifier that survives to the
# linker symbol table unprefixed.
#
# arch/x86/x86_64/pe_image.ld is a stated exception, not an oversight: it deliberately
# omits the five app-split bounds (__kickos_app_rom_{start,end},
# __kickos_app_sram_{start,end}, __kickos_app_load_delta). x86_64 compiles no address
# space today (KICKOS_HAVE_ASPACE=0), so kernel/mem/aspace.cc's whole body, declarations
# included, is preprocessed out and nothing in that image references the five; the day
# x86_64 gains an address space and a reference reaches the link, the missing symbol is
# what turns that on, exactly the property this gate exists to keep. Pass --no-app-split
# for that one script; every other target is required to state all eleven.
#
# Usage:
#   check_link_bounds_stated.sh <preprocessed .ld file> [--no-app-split]
#   check_link_bounds_stated.sh --controls
#
# --controls proves the scanner on planted text, with no build tree needed: a plain
# assignment counts, the RX spelling counts, a start==end empty window counts (the
# distinction the whole change buys), a comparison inside an ASSERT does not, and a
# longer identifier that merely contains the name as a substring does not.

set -u
. "$(dirname "$0")/../lib/gate.sh"

# The eleven C names KICKOS_LINK_BOUND covers, grouped as sections.ld.h groups them.
BOUNDS_CODE="__kickos_code_start __kickos_code_end"
BOUNDS_APPDATA="__kickos_appdata_start __kickos_appdata_end"
BOUNDS_HEAP="_kickos_heap_start _kickos_heap_limit"
BOUNDS_SPLIT="__kickos_app_rom_start __kickos_app_rom_end __kickos_app_sram_start __kickos_app_sram_end __kickos_app_load_delta"

# assigned_counts <file> <name>... -> "<name> <count>" per line on stdout.
#
# An IDENTIFIER immediately followed by optional blanks then a single '=' (not '==') is
# an ld assignment. Scanned by repeatedly matching the next identifier and slicing past
# it, never with \b: awk has no word-boundary escape, and a scan built on one matches
# nothing and says so calmly. cpp -E -P strips every comment before this ever sees the
# file, so a name inside one cannot forge a hit.
assigned_counts() {
    _ac_file="$1"
    shift
    awk -v names="$*" '
        BEGIN {
            n = split(names, want, " ")
            for (i = 1; i <= n; i++) { iswant[want[i]] = 1; cnt[want[i]] = 0 }
        }
        {
            line = $0
            while (match(line, /[A-Za-z_][A-Za-z0-9_]*/)) {
                ident = substr(line, RSTART, RLENGTH)
                rest = substr(line, RSTART + RLENGTH)
                asg = 0
                if (rest ~ /^[ \t]*=[^=]/) { asg = 1 }
                else if (rest ~ /^[ \t]*=$/) { asg = 1 }
                if (asg) {
                    if (ident in iswant) { cnt[ident]++ }
                    if (substr(ident, 1, 1) == "_") {
                        plain = substr(ident, 2)
                        if (plain in iswant) { cnt[plain]++ }
                    }
                }
                line = substr(line, RSTART + RLENGTH)
            }
        }
        END {
            for (i = 1; i <= n; i++) { printf "%s %d\n", want[i], cnt[want[i]] }
        }' "$_ac_file"
}

# missing_bounds <file> <name>... -> the names assigned_counts found at zero, one per line.
missing_bounds() {
    _mb_file="$1"
    shift
    assigned_counts "$_mb_file" "$@" | awk '$2 == 0 { print $1 }'
}

# --- controls: prove the scanner before it is trusted --------------------------------
run_controls() {
    scratch_dir

    cat > "$TMP/ctl.ld" <<'EOF'
__kickos_code_start = 0;
__kickos_code_end = 0;
___kickos_appdata_start = ORIGIN(RAM);
___kickos_appdata_end = ORIGIN(RAM) + 0x1000;
_kickos_heap_start = .;
_kickos_heap_limit = .;
__kickos_app_rom_start_hi = 1;
ASSERT(__kickos_app_rom_end == __kickos_app_rom_start, "never assigned, only compared")
__kickos_app_sram_start = ADDR(.appdata);
__kickos_app_sram_end = __kickos_app_sram_start;
__kickos_app_load_delta = 0;
EOF

    # Plain assignment, both start and end of an empty window (start == end): must count.
    n="$(assigned_counts "$TMP/ctl.ld" __kickos_code_start | awk '{print $2}')"
    [ "$n" -eq 1 ] || bad "plain assignment __kickos_code_start counted $n, expected 1"
    n="$(assigned_counts "$TMP/ctl.ld" __kickos_code_end | awk '{print $2}')"
    [ "$n" -eq 1 ] || bad "plain assignment __kickos_code_end counted $n, expected 1"

    # RX psABI spelling (one leading underscore prepended to the C name): must count for
    # the PLAIN name, which is the name every caller asks about.
    n="$(assigned_counts "$TMP/ctl.ld" __kickos_appdata_start | awk '{print $2}')"
    [ "$n" -eq 1 ] || bad "RX-spelled ___kickos_appdata_start did not satisfy __kickos_appdata_start (counted $n)"
    n="$(assigned_counts "$TMP/ctl.ld" __kickos_appdata_end | awk '{print $2}')"
    [ "$n" -eq 1 ] || bad "RX-spelled ___kickos_appdata_end did not satisfy __kickos_appdata_end (counted $n)"

    # A comparison inside ASSERT(a == b, ...) must NOT count as an assignment of either
    # side: __kickos_app_rom_end appears only there.
    n="$(assigned_counts "$TMP/ctl.ld" __kickos_app_rom_end | awk '{print $2}')"
    [ "$n" -eq 0 ] || bad "a bare comparison (ASSERT a == b) was counted as an assignment of __kickos_app_rom_end ($n)"

    # A longer identifier that merely contains the wanted name as a prefix must not
    # satisfy it: __kickos_app_rom_start_hi is assigned, __kickos_app_rom_start is not.
    n="$(assigned_counts "$TMP/ctl.ld" __kickos_app_rom_start | awk '{print $2}')"
    [ "$n" -eq 0 ] || bad "a longer identifier (_hi suffix) satisfied the shorter name it merely starts with ($n)"

    # An explicitly empty window, start == end both assigned to the same location: this is
    # the distinction the whole change buys, and it must read as fully stated, not absent.
    n="$(missing_bounds "$TMP/ctl.ld" __kickos_app_sram_start __kickos_app_sram_end | wc -l | tr -d ' ')"
    [ "$n" -eq 0 ] || bad "an empty window (start == end, both assigned) was reported missing"

    # load_delta: plain assignment, single symbol.
    n="$(assigned_counts "$TMP/ctl.ld" __kickos_app_load_delta | awk '{print $2}')"
    [ "$n" -eq 1 ] || bad "plain assignment __kickos_app_load_delta counted $n, expected 1"

    # The full driver, on a script missing one symbol entirely: must name exactly that one.
    cat > "$TMP/ctl.missing.ld" <<'EOF'
__kickos_code_start = 0;
__kickos_code_end = 0;
EOF
    missing="$(missing_bounds "$TMP/ctl.missing.ld" $BOUNDS_CODE $BOUNDS_APPDATA)"
    want="__kickos_appdata_start
__kickos_appdata_end"
    if [ "$missing" != "$want" ]; then
        bad "a script defining neither appdata bound reported [$missing], expected [$want]"
    fi

    if [ "$rc" -ne 0 ]; then
        exit 1
    fi
    echo "PASS: scanner controls (RX spelling, empty window, ASSERT comparison, prefix near miss, missing-symbol naming)"
}

rc=0
usage="usage: $0 <preprocessed .ld file> [--no-app-split] | $0 --controls"

if [ "${1:-}" = "--controls" ]; then
    run_controls
    exit $?
fi

LD="${1:?$usage}"
shift
NO_SPLIT=0
if [ "${1:-}" = "--no-app-split" ]; then
    NO_SPLIT=1
    shift
fi
[ -r "$LD" ] || fail "cannot read $LD"
[ -s "$LD" ] || fail "$LD is empty; cpp failed silently or was never run"

WANT="$BOUNDS_CODE $BOUNDS_APPDATA $BOUNDS_HEAP"
if [ "$NO_SPLIT" -eq 0 ]; then
    WANT="$WANT $BOUNDS_SPLIT"
fi

MISSING="$(missing_bounds "$LD" $WANT)"
if [ -n "$MISSING" ]; then
    echo "FAIL: $LD does not assign the following bound(s) (checked both the plain C name" >&2
    echo "      and the RX psABI spelling, one leading underscore prepended):" >&2
    printf '%s\n' "$MISSING" | while IFS= read -r m; do
        echo "        $m" >&2
    done
    exit 1
fi

COUNT="$(printf '%s\n' $WANT | wc -l | tr -d ' ')"
echo "PASS: $LD states all $COUNT bound(s)"
