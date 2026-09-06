#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Refuses cache-as-SRAM pinning under the own-image AMP posture on the RP2350.
#
# usage: check_amp_no_xip_pin.sh <build-dir> <src-dir>
#
# The refusal is a property of the POSTURE and not of the part: a single-image kernel on this
# chip may pin freely, there being no second kernel to have its lines destroyed. Three reasons,
# each standing alone:
#
#   1. The hardware offers no unit to grant. The XIP cache is one logically single 16 KiB
#      structure (datasheet 4.4, p.340); its two 8 KiB banks split odd and even lines for
#      bandwidth and are not a per-core partition. The isolation principle grants the narrowest
#      unit the hardware permits, and here there is no half to grant a node.
#   2. A protocol would misrepresent the isolation. flash_flush_cache "unpins pinned cache
#      lines" (5.4.8.8, p.386) and a flush is global, so one node's flush destroys another
#      node's cache-as-SRAM with no error and no local symptom.
#   3. Nothing could enforce it. Every structural gate in this tree binds ONE image at a time,
#      so a pin-ownership rule would rest on every future driver on both nodes remembering it,
#      with no local symptom when violated.
#
# The datasheet does offer a partial mitigation: pinned lines exit only by an INVALIDATE (0x0 or
# 0x2), a CLEAN leaves them pinned (4.4.1.2 and Figure 17, p.343), invalidate-by-address touches
# only the lines it names, and 4.4.1.3 p.344 recommends pinning outside the QMI range entirely.
# It does not lift the refusal: flash_flush_cache ('F','C', 5.4.8.8 p.386) is whole-cache and
# unpins every line wherever it lives, and a disciplined pair is the unenforceable rule of
# reason 3. The mitigation makes a protocol thinkable, not checkable.
#
# What would lift the refusal: per-core cache partitioning, or a documented pin-ownership
# mechanism. This part has neither.
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

set -eu
here="$(dirname "$0")"
. "$here/../lib/gate.sh"

BUILD="${1:?usage: check_amp_no_xip_pin.sh <build-dir> <src-dir>}"
SRC="${2:?}"

CFG="$BUILD/generated/.config"
[ -f "$CFG" ] || fail "no resolved Kconfig at $CFG"

# Read the posture from the RESOLVED configuration, not from a preset name.
own="$(sed -n 's/^CONFIG_KICKOS_AMP_OWN_IMAGE=\(.*\)$/\1/p' "$CFG" | tail -1)"
if [ "${own:-0}" != "1" ]; then
    echo "SKIP: not the own-image AMP posture, so no peer kernel can lose a pinned line"
    exit 0
fi

# And the chip, for the same reason: a chip whose cache is partitionable, or which has none,
# is not this hazard.
chip="$(sed -n 's/^CONFIG_KICKOS_CHIP="\(.*\)"$/\1/p' "$CFG" | tail -1)"
if [ "${chip:-}" != "rp2350" ]; then
    echo "SKIP: chip is '${chip:-unset}', and the shared XIP cache this refuses is the RP2350's"
    exit 0
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
corpus_files() {
    git ls-files -z -- 'arch/arm/chip/rp2350/*' 'arch/arm/common/*' 'arch/arm/armv7m/*' \
                       'arch/common/*' 'arch/include/*' \
                       'kernel/*' 'user/*' 'system/*' 'lib/*' \
        | tr '\0' '\n' \
        | grep -E '\.(c|cc|h|S)$'
}

# Tracked files the worktree does not have, one per line. This loop is the right of a pipe and
# so a subshell: it PRINTS them and the caller refuses, a fail() here reaching only a status
# that an `if` or a `|| true` at the call site would absorb.
corpus_absent() {
    corpus_files \
        | while IFS= read -r f; do
              if [ ! -f "$f" ]; then
                  printf '%s\n' "$f"
              fi
          done
}

scan() { # <pattern> -> matching tracked source lines, one per line
    corpus_files \
        | while IFS= read -r f; do
              # "./$f" rather than a `--` separator: the pathspec cannot then be read as a
              # flag, and check_dash_punct.sh recognises a separator only where the options
              # visibly end, not after -E's own argument.
              LC_ALL=C grep -nH -E "$1" "./$f" || true
          done
}

PATTERN='0x18[0-9a-fA-F]{6}|XIP_MAINTENANCE'

# A planted control first: a reader that cannot see the shape it forbids cannot go red. The
# control is a file this gate writes and scans itself, so no tracked file carries the forbidden
# text.
scratch_dir
printf 'volatile unsigned *p = (unsigned *)0x18000000u; /* pin */\n' >"$TMP/planted.c"
if ! LC_ALL=C grep -qE "$PATTERN" "$TMP/planted.c"; then
    fail "PLANTED CONTROL WAS NOT MATCHED: this gate's pattern does not recognise a reference
  to the XIP maintenance window, so a green run of it witnesses nothing."
fi
echo "== planted control matched, so the pattern can go red =="

# Before the scan, and in this shell: the difference between 'no reference' and 'not looked
# at' is the whole gate, and a file git lists that grep cannot open reads as the first.
absent="$(corpus_absent)"
if [ -n "$absent" ]; then
    printf '%s\n' "$absent" >&2
    fail "the tracked file(s) above are listed by git and missing from the worktree, so this
  gate would report 'no reference' for source it never read"
fi

found="$(scan "$PATTERN")"
count=0
if [ -n "$found" ]; then
    count="$(printf '%s\n' "$found" | grep -c .)"
fi

if [ "$count" -ne 0 ]; then
    printf '%s\n' "$found" >&2
    fail "$count reference(s) to the RP2350 XIP cache maintenance window under the own-image
  AMP posture. That window is where a PIN is issued, and a pinned line on this part belongs to
  no node: the cache is one structure both kernels share, and the bootrom's whole-cache flush
  unpins every line from either side with no error and no local symptom. See this file's header
  for the three reasons and for what would lift the refusal."
fi

corpus="$(corpus_files | grep -c .)"
[ "$corpus" -gt 0 ] || fail "the corpus is empty, so this gate looked at nothing"
echo "== scanned $corpus tracked source file(s): the rp2350 chip layer, the arm and armv7m"
echo "   common layers, and every layer above them =="
echo "PASS: no source reaches the XIP cache maintenance window, which is where a PIN is issued,"
echo "      so no line is pinned on either node. NOT CHECKED HERE, and named in this file's"
echo "      header: the bootrom's whole-cache flush, which is reached by a ROM table lookup no"
echo "      address pattern matches"
