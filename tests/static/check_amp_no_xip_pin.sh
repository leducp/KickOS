#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Refuses cache-as-SRAM pinning on the RP2350, which the own-image AMP posture forbids. Run from
# the repo root. A tree gate: it reads the sources and no build, so a single-image kernel's pin,
# which the posture alone would allow, is refused too. The rule, its three independent reasons,
# the partial mitigation the datasheet offers and what would lift it are stated in
# docs/reference/boards.md under the rp2350 board.
#
# A gate rather than a Kconfig refusal because nothing in the tree pins a cache line, so there
# is no knob to refuse. What is enforceable is the textual property, and it is narrower than the
# posture's rule: NOTHING IN THIS TREE PINS. The maintenance window is the only way to issue a
# PIN (4.4.1.1, p.342), so no reference to it means no pinned line exists on either node. This
# does not scan for destructive operations.
#
# What this gate cannot see: the bootrom's flash_flush_cache unpins every line (5.4.8.8, p.386)
# and is reached by a ROM table lookup on a two-character code, so no address pattern matches
# it. arch/arm/chip/rp2350/chip_rp2350.cc calls it once, in kickos_rp2350_xip_identity, on node
# 0 only, before anything is pinned and before core 1 is launched. That call site is read by a
# person, not by this gate.

set -eu
. "$(dirname "$0")/../lib/gate.sh"

require_repo_root
scratch_dir

# 0x18000000 is XIP_MAINTENANCE_BASE (Table 10, section 2.2.2), matched on the BASE alone so an
# address computed into that window in any spelling is caught.
PATTERN='0x18[0-9a-fA-F]{6}|XIP_MAINTENANCE'

printf 'volatile unsigned *p = (unsigned *)0x18000000u;\n' > "$TMP/planted.c"
grep -qE "$PATTERN" "$TMP/planted.c" || fail "the pattern does not match a planted reference to
      the XIP maintenance window, so a green run witnesses nothing"

# What an RP2350 image compiles: the RP2040 names an unrelated XIP_SSI_BASE at the same
# 0x1800_0000, so a tree-wide scan would report another chip's register as this chip's hazard.
corpus "$TMP/tracked" "file under the layers an RP2350 image compiles" \
    'arch/arm/chip/rp2350/*' 'arch/arm/common/*' 'arch/arm/armv7m/*' 'arch/common/*' \
    'arch/include/*' 'kernel/*' 'user/*' 'system/*' 'lib/*'
grep -E '\.(c|cc|h|S)$' "$TMP/tracked" > "$TMP/corpus" || :
corpus_floor "$TMP/corpus" 190 "source file(s)"

# shellcheck disable=SC2046
set -- $(cat "$TMP/corpus")
rc=0
grep -nHE "$PATTERN" "$@" > "$TMP/found" || rc=$?
[ "$rc" -le 1 ] || fail "grep exited $rc scanning the corpus, so its verdict is UNKNOWN"
if [ -s "$TMP/found" ]; then
    cat "$TMP/found" >&2
    fail "$(wc -l < "$TMP/found" | tr -d ' ') reference(s) to the RP2350 XIP cache maintenance
      window under the own-image AMP posture. A pinned line on this part belongs to no node: the
      cache is one structure both kernels share, and the bootrom's whole-cache flush unpins every
      line from either side with no error and no local symptom."
fi
echo "PASS: none of $N source file(s) an RP2350 image compiles reaches the XIP cache maintenance
      window, so no line is pinned on either node (the bootrom flush is not checked here)"
