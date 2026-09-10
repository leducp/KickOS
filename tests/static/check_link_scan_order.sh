#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The archive scan order, read off the LINK MAP rather than off the CMake variable that
# builds the group. ld emits one `LOAD <file>` line per input in the order it processed
# them, so the map is the linker's own record of the order every seam resolution below
# depends on.
#
# WHY THE ORDER IS LOAD-BEARING. A seam is resolved by archive member extraction, and within
# a --start-group the archives are still scanned LEFT TO RIGHT: the first archive holding a
# definition is the one that gets extracted, and a definition in a later archive is then
# either dropped (its member anchored by nothing else) or a hard multiple-definition error
# (its member anchored by something else). So a seam that may live in kickos_chip_<chip>
# beside a fallback in kickos_arch_<arch> may NOT live the other way round, and moving one
# across that boundary changes which side wins. arch/CMakeLists.txt states the rule; this
# gate is what says the link still has the order the rule assumes.
#
# usage: check_link_scan_order.sh <map>

set -u
. "$(dirname "$0")/../lib/gate.sh"

# ld's `LOAD` line is not translated, but the heading a locale would move is not read here at
# all; LC_ALL is set for sort and grep collation.
export LC_ALL=C

[ "$#" -eq 1 ] || { echo "usage: $0 <map>" >&2; exit 2; }
MAP="$1"
[ -r "$MAP" ] || fail "cannot read $MAP"

scratch_dir

# The KickOS archives in LOAD order, one basename per line. `LOAD linker stubs` is a real ld
# line with no path in it, and the toolchain's own libc.a/libgcc.a LOAD lines sit in the same
# block: neither is a KickOS archive, so the ^libkickos_ filter is what keeps them out
# instead of a position that would shift when the group gains a member.
scan() { # <map> -> the KickOS archive basenames in LOAD order
    awk '/^LOAD /{
        n = split($2, p, "/")
        b = p[n]
        if (b ~ /^libkickos_.*\.a$/) { print b }
    }' "$1"
}

# The verdict over one such list. The three roles are matched on the archive NAMING RULE and
# not on a list of names, so a new chip or a new arch joins without an edit here.
verdict() { # <basename list file> -> BAD/OK records
    awk '
{
    i++
    seen[$0]++
    if ($0 == "libkickos_kernel.a")   { if (!kernel) { kernel = i }; kernel_n++ }
    if ($0 ~ /^libkickos_chip_/)      { if (!chip)   { chip = i };   chip_n++ }
    if ($0 ~ /^libkickos_arch_/)      { if (!arch)   { arch = i };   arch_n++ }
}
END {
    if (i == 0)        { print "BAD NO_ARCHIVES"; exit }
    if (!kernel)       { print "BAD NO_KERNEL" }
    if (!arch)         { print "BAD NO_ARCH" }
    if (kernel_n > 1)  { print "BAD KERNEL_TWICE" }
    if (chip_n > 1)    { print "BAD CHIP_TWICE" }
    if (arch_n > 1)    { print "BAD ARCH_TWICE" }
    if (kernel && arch && kernel > arch) { print "BAD KERNEL_AFTER_ARCH" }
    if (kernel && chip && kernel > chip) { print "BAD KERNEL_AFTER_CHIP" }
    if (chip && arch && chip > arch)     { print "BAD CHIP_AFTER_ARCH" }
    printf "ORDER %d %d %d %d\n", i, kernel + 0, chip + 0, arch + 0
}' "$1"
}

# --- self-test: one control per clause ----------------------------------------
# The LOAD block ld really emits, with the toolchain archives and the pathless line that
# would otherwise be read as an input name.
cat > "$TMP/ctl.map" <<'EOF'
Archive member included to satisfy reference by file (symbol)

kernel/libkickos_kernel.a(sched.cc.obj)
                              main.cc.obj (kickos::sched_yield())
Linker script and memory map

LOAD user/apps/x/CMakeFiles/x.dir/main.cc.obj
LOAD user/libkickos_user.a
LOAD kernel/libkickos_kernel.a
LOAD system/libkickos_default_init.a
LOAD arch/libkickos_chip_mps2.a
LOAD arch/libkickos_arch_armv7m.a
LOAD lib/libkickos_lib.a
LOAD /toolchain/lib/libc.a
LOAD /toolchain/lib/libgcc.a
LOAD linker stubs
EOF
scan "$TMP/ctl.map" > "$TMP/ctl.names"
cat > "$TMP/ctl.names.want" <<'EOF'
libkickos_user.a
libkickos_kernel.a
libkickos_default_init.a
libkickos_chip_mps2.a
libkickos_arch_armv7m.a
libkickos_lib.a
EOF
cmp -s "$TMP/ctl.names" "$TMP/ctl.names.want" \
    || fail "the LOAD scan read $(tr '\n' ' ' < "$TMP/ctl.names"), expected
      $(tr '\n' ' ' < "$TMP/ctl.names.want"); it is reading the wrong block, taking the
      toolchain archives, or tripping on the pathless `LOAD linker stubs` line"

# The map's OTHER blocks name the same archives. A scan reading them too would see
# kernel/libkickos_kernel.a above the LOAD block and read a different order.
[ "$(grep -c 'libkickos_kernel\.a' "$TMP/ctl.map")" -eq 2 ] \
    || fail "the control map no longer names an archive outside its LOAD block, so it cannot
      show that the scan reads the LOAD block alone"

ctl_bad() { # <lines> -> the BAD reasons, space separated
    printf '%s' "$1" | verdict /dev/stdin | awk '/^BAD /{ printf "%s ", $2 }'
}
GOOD='libkickos_user.a
libkickos_kernel.a
libkickos_chip_mps2.a
libkickos_arch_armv7m.a
libkickos_lib.a
'
[ -z "$(ctl_bad "$GOOD")" ] || fail "the correctly ordered control reported $(ctl_bad "$GOOD")"
printf '%s' "$GOOD" | verdict /dev/stdin | grep -qxF 'ORDER 5 2 3 4' \
    || fail "the correctly ordered control tallied $(printf '%s' "$GOOD" | verdict /dev/stdin | grep '^ORDER ')"

# Each positive control differs from GOOD in exactly one property.
ctl_is() { # <what> <lines> <expected reasons>
    _got="$(ctl_bad "$2")"
    [ "$_got" = "$3" ] || fail "the $1 control reported '$_got', expected '$3'"
}
ctl_is "arch before chip" 'libkickos_kernel.a
libkickos_arch_armv7m.a
libkickos_chip_mps2.a
' 'CHIP_AFTER_ARCH '
ctl_is "kernel after chip" 'libkickos_chip_mps2.a
libkickos_kernel.a
libkickos_arch_armv7m.a
' 'KERNEL_AFTER_CHIP '
ctl_is "kernel after arch" 'libkickos_chip_mps2.a
libkickos_arch_armv7m.a
libkickos_kernel.a
' 'KERNEL_AFTER_ARCH KERNEL_AFTER_CHIP '
ctl_is "no kernel archive" 'libkickos_chip_mps2.a
libkickos_arch_armv7m.a
' 'NO_KERNEL '
ctl_is "no arch archive" 'libkickos_kernel.a
libkickos_chip_mps2.a
' 'NO_ARCH '
ctl_is "kernel twice" 'libkickos_kernel.a
libkickos_kernel.a
libkickos_arch_armv7m.a
' 'KERNEL_TWICE '
ctl_is "arch twice" 'libkickos_kernel.a
libkickos_arch_armv7m.a
libkickos_arch_armv7m.a
' 'ARCH_TWICE '
ctl_is "no archive at all" '' 'NO_ARCHIVES '
# A chipless link is legitimate (the host sim carries no chip archive) and must not read as
# a missing role.
ctl_is "chipless link" 'libkickos_kernel.a
libkickos_arch_sim.a
' ''

# --- the map ------------------------------------------------------------------
scan "$MAP" > "$TMP/names"
require_nonempty "$TMP/names" "no KickOS archive appears in a LOAD line of $MAP; the map has
      no LOAD block, or the archive naming changed, and this gate would pass vacuously"
verdict "$TMP/names" > "$TMP/verdict"

if grep -q '^BAD ' "$TMP/verdict"; then
    echo "FAIL: the link did not scan the KickOS archives in the order every seam" >&2
    echo "      resolution assumes: kickos_kernel, then kickos_chip_<chip>, then" >&2
    echo "      kickos_arch_<arch>. Inside a --start-group the archives are still" >&2
    echo "      scanned left to right, so the first archive holding a definition wins" >&2
    echo "      and a seam that may sit chip-side beside an arch fallback may not sit" >&2
    echo "      arch-side beside a chip backend. The order is set by _kickos_group in" >&2
    echo "      the root CMakeLists.txt. LOAD order was:" >&2
    sed 's/^/        /' "$TMP/names" >&2
    awk '/^BAD /{ printf "      %s\n", $2 }' "$TMP/verdict" >&2
    exit 1
fi

awk '{ n = $2; k = $3; c = $4; a = $5
       if (c == 0) { chip = "no chip archive" } else { chip = "chip at " c }
       printf "PASS: %d KickOS archive(s) LOADed, kernel at %d, %s, arch at %d\n", n, k, chip, a
     }' "$TMP/verdict"
