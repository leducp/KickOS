#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Refuses cache-as-SRAM pinning under the own-image AMP posture on the RP2350.
#
# usage: check_amp_no_xip_pin.sh <build-dir> <src-dir>
#
# The refusal is a property of the POSTURE and not of the part: a single-image kernel on this
# chip may pin freely, there being no second kernel to have its lines destroyed. The rule, its
# three independent reasons, the partial mitigation the datasheet offers and what would lift it
# are stated in docs/reference/boards.md under the rp2350 board.
#
# A gate rather than a Kconfig refusal because nothing in the tree pins a cache line, so there
# is no knob to refuse. What is enforceable is the textual property, and it is narrower than the
# posture's rule: NOTHING IN THIS TREE PINS. The maintenance window is the only way to issue a
# PIN (4.4.1.1, p.342), so no reference to it means no pinned line exists on either node, and
# the posture's rule follows because there is nothing pinned to destroy. This does not scan for
# destructive operations.
#
# What this gate cannot see: the bootrom's flash_flush_cache unpins every line (5.4.8.8, p.386)
# and is reached by a ROM table lookup on a two-character code, so no address pattern matches
# it. arch/arm/chip/rp2350/chip_rp2350.cc calls it once, in kickos_rp2350_xip_identity, on node
# 0 only, before anything is pinned and before core 1 is launched. That call site is read by a
# person, not by this gate.

# NOT APPLICABLE EXITS 77, WHICH IS CTEST'S SKIP. The registration carries SKIP_RETURN_CODE 77
# or the skip reads as a failure. Exit 0 here would report a green this gate did not earn on
# every rp2350 preset outside the own-image posture.

set -eu
here="$(dirname "$0")"
. "$here/../lib/gate.sh"

BUILD="${1:?usage: check_amp_no_xip_pin.sh <build-dir> <src-dir>}"
SRC="${2:?}"

CFG="$BUILD/generated/.config"
[ -f "$CFG" ] || fail "no resolved Kconfig at $CFG"

# Read the posture from the RESOLVED configuration, not from a preset name. KICKOS_AMP_OWN_IMAGE
# is an int symbol with exactly two defaults (1 or 0), so a resolved .config always carries
# exactly one line spelling one of those; a missing, duplicated or otherwise malformed line is
# not "posture off", it is a .config this gate cannot trust, and a chip that can lose a pinned
# line must not earn a skip on that account.
own_n="$(grep -c '^CONFIG_KICKOS_AMP_OWN_IMAGE=' "$CFG" || true)"
[ "$own_n" -eq 1 ] \
    || fail "$CFG carries $own_n CONFIG_KICKOS_AMP_OWN_IMAGE line(s), expected exactly 1; a
  resolved Kconfig always has one, and a missing or duplicated one must not read as this
  gate not applying"
own="$(sed -n 's/^CONFIG_KICKOS_AMP_OWN_IMAGE=\(.*\)$/\1/p' "$CFG")"
case "$own" in
    0|1)
        ;;
    *)
        fail "$CFG's CONFIG_KICKOS_AMP_OWN_IMAGE is '$own', neither of the two values this
  int symbol ever resolves to; a broken .config must not read as posture off"
        ;;
esac
if [ "$own" = "0" ]; then
    echo "SKIP: not the own-image AMP posture, so no peer kernel can lose a pinned line"
    exit 77
fi

# And the chip, for the same reason: a chip whose cache is partitionable, or which has none,
# is not this hazard, but a missing, duplicated or malformed chip string is not that either.
# COUNTED BEFORE IT IS SHAPED: the counting pattern requires only the "=", never the opening
# quote, or a well-formed line beside a malformed duplicate assignment of the same symbol
# counts as the ONE valid line and the duplicate is never seen at all.
chip_n="$(grep -c '^CONFIG_KICKOS_CHIP=' "$CFG" || true)"
[ "$chip_n" -eq 1 ] \
    || fail "$CFG carries $chip_n CONFIG_KICKOS_CHIP=... line(s), expected exactly 1"
chip="$(sed -n 's/^CONFIG_KICKOS_CHIP="\(.*\)"$/\1/p' "$CFG")"
[ -n "$chip" ] \
    || fail "$CFG's CONFIG_KICKOS_CHIP is empty or unterminated, which is not a chip this
  gate can rule applicable or not"
if [ "$chip" != "rp2350" ]; then
    echo "SKIP: chip is '$chip', and the shared XIP cache this refuses is the RP2350's"
    exit 77
fi

cd "$SRC" || fail "cannot enter $SRC"
[ -f CMakeLists.txt ] || fail "run against the repo root (see WORKING_DIRECTORY)"

# The maintenance window and the pin op, both from the datasheet: 0x18000000 is
# XIP_MAINTENANCE_BASE (Table 10, section 2.2.2) and a maintenance address's low bits carry the
# operation, 0x7 being PIN (section 4.4.1.1, p.342). Matched on the BASE alone, so an address
# computed into that window in any spelling is caught.
#
# The corpus is what an RP2350 image compiles: the RP2040 names an unrelated peripheral
# XIP_SSI_BASE at the same 0x1800_0000, so a tree-wide scan would report another chip's register
# as this chip's hazard. One file list, read by the scan and by the count below, so the corpus
# reported is the corpus looked at.
# Builds $CORPUS_FILE: tracked files under the layers an RP2350 image compiles from, filtered
# to source extensions. A plain statement, called directly and never through a pipe or $(...):
# on either side of those, dash runs this in a SUBSHELL, and a fail() there would exit only the
# subshell while the caller reads an empty result as a clean corpus.
CORPUS_FILE=""
build_corpus() {
    CORPUS_FILE="$TMP/corpus_files"
    # set +e around the bare command, not `if git ...; then` or `git ... || true`: an `if`
    # here would put `git` after a non-command-position word and check_dash_punct.sh would no
    # longer recognise its `--` as a pathspec separator, and `!` or `||` both collapse $? to
    # something other than git's own exit code before this can read it.
    set +e
    git ls-files -z -- 'arch/arm/chip/rp2350/*' 'arch/arm/common/*' 'arch/arm/armv7m/*' \
                       'arch/common/*' 'arch/include/*' \
                       'kernel/*' 'user/*' 'system/*' 'lib/*' \
        > "$TMP/corpus.z" 2>"$TMP/corpus.z.err"
    _rc=$?
    set -e
    if [ "$_rc" -ne 0 ]; then
        sed -n '1,3p' "$TMP/corpus.z.err" >&2
        fail "git ls-files exited $_rc while listing the source corpus"
    fi
    # rc 1 from grep is a legitimate empty filter result, caught by the corpus floor below;
    # anything past 1 is grep itself failing and must not be read as an empty corpus.
    # NOT `if ! pipeline; then _rc=$?`: the `!` reduces $? to 0 or 1 for the if's own
    # verdict, so the branch below it would read grep's exit as 0 no matter what it was.
    if tr '\0' '\n' < "$TMP/corpus.z" | grep -E '\.(c|cc|h|S)$' > "$CORPUS_FILE"; then
        :
    else
        _rc=$?
        [ "$_rc" -le 1 ] || fail "grep exited $_rc filtering the source corpus by extension"
    fi
}

# Tracked files the worktree does not have, appended one per line to $TMP/absent. Reads
# $CORPUS_FILE by redirection, not by piping from a function, for the same subshell reason as
# build_corpus above.
corpus_absent() {
    : > "$TMP/absent"
    while IFS= read -r f; do
        if [ ! -f "$f" ]; then
            printf '%s\n' "$f" >> "$TMP/absent"
        fi
    done < "$CORPUS_FILE"
}

# <pattern>; appends matching tracked source lines to $TMP/found. A plain statement, called
# directly: a grep crash on one file must reach fail() in THIS shell, not a subshell a `|| true`
# or a $(...) at the call site would absorb.
scan() {
    : > "$TMP/found"
    while IFS= read -r f; do
        # "./$f" rather than a `--` separator: the pathspec cannot then be read as a
        # flag, and check_dash_punct.sh recognises a separator only where the options
        # visibly end, not after -E's own argument.
        # NOT `if ! grep ...; then _rc=$?`: the `!` reduces $? to 0 or 1 for the if's own
        # verdict, so the branch below it would read grep's exit as 0 no matter what it was.
        if LC_ALL=C grep -nH -E "$1" "./$f" >> "$TMP/found"; then
            :
        else
            _rc=$?
            [ "$_rc" -le 1 ] \
                || fail "grep exited $_rc scanning '$f' for the XIP maintenance window pattern"
        fi
    done < "$CORPUS_FILE"
}

PATTERN='0x18[0-9a-fA-F]{6}|XIP_MAINTENANCE'

# A planted control first: a reader that cannot see the shape it forbids cannot go red. The
# control is a file this gate writes and scans itself, so no tracked file carries the forbidden
# text.
scratch_dir
build_corpus
printf 'volatile unsigned *p = (unsigned *)0x18000000u; /* pin */\n' >"$TMP/planted.c"
if ! LC_ALL=C grep -qE "$PATTERN" "$TMP/planted.c"; then
    fail "PLANTED CONTROL WAS NOT MATCHED: this gate's pattern does not recognise a reference
  to the XIP maintenance window, so a green run of it witnesses nothing."
fi
echo "== planted control matched, so the pattern can go red =="

# Before the scan, and in this shell: the difference between 'no reference' and 'not looked
# at' is the whole gate, and a file git lists that grep cannot open reads as the first.
corpus_absent
if [ -s "$TMP/absent" ]; then
    cat "$TMP/absent" >&2
    fail "the tracked file(s) above are listed by git and missing from the worktree, so this
  gate would report 'no reference' for source it never read"
fi

scan "$PATTERN"
count=0
if [ -s "$TMP/found" ]; then
    count="$(wc -l < "$TMP/found" | tr -d ' ')"
fi

if [ "$count" -ne 0 ]; then
    cat "$TMP/found" >&2
    fail "$count reference(s) to the RP2350 XIP cache maintenance window under the own-image
  AMP posture. That window is where a PIN is issued, and a pinned line on this part belongs to
  no node: the cache is one structure both kernels share, and the bootrom's whole-cache flush
  unpins every line from either side with no error and no local symptom. See this file's header
  for the three reasons and for what would lift the refusal."
fi

corpus="$(wc -l < "$CORPUS_FILE" | tr -d ' ')"
[ "$corpus" -gt 0 ] || fail "the corpus is empty, so this gate looked at nothing"
echo "== scanned $corpus tracked source file(s): the rp2350 chip layer, the arm and armv7m"
echo "   common layers, and every layer above them =="
echo "PASS: no source reaches the XIP cache maintenance window, which is where a PIN is issued,"
echo "      so no line is pinned on either node. NOT CHECKED HERE, and named in this file's"
echo "      header: the bootrom's whole-cache flush, which is reached by a ROM table lookup no"
echo "      address pattern matches"
