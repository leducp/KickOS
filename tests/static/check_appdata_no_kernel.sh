#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Security CI gate for the inverted .appdata scheme. Under KICKOS_HAVE_MPU the enforcing
# linker scripts capture the four privileged archives (kernel/arch/chip/lib) into the
# KERNEL .data/.bss by `archive:member` colon selectors, and .appdata/.appbss are pure
# CATCH-ALLS. kernel/domain/domain.cc (arch_domain_static_regions) grants
# [__kickos_appdata_start, __kickos_appdata_end) R+W to EVERY unprivileged thread in every
# domain, so a kernel object that lands in that window is directly writable by an
# unprivileged thread: a privilege-escalation primitive, not a layout wart.
#
# The linker scripts' own ASSERT(_ebss > _sbss) catches TOTAL selector failure only. A
# renamed or typoed selector drops ONE archive into the app window while the other three
# keep the kernel .bss non-empty, and the link stays green.
#
# The LINK MAP is the instrument: it names the archive MEMBER behind every input section,
# and so covers file-static globals, anonymous-namespace globals and COMMON. nm serves the
# two window bounds only, where both symbols are global and unique; nm reports locals with
# lowercase types and names that repeat tree-wide, so an address lookup keyed on a local
# name is ambiguous.
#
# Scope: the static app window, in ONE map (selftest's, the only target linked with -Map),
# over the archives named on the command line, which are the caller's claim of the
# privileged set. Per-thread stack grants and the RAM pool arena are a separate mechanism
# with their own gates.
#
# usage: check_appdata_no_kernel.sh <nm> <elf> <map> <kernel.a> <arch.a> <chip.a> <lib.a>

set -eu
. "$(dirname "$0")/../lib/gate.sh"

# The awk below keys on ld's English "Linker script and memory map" heading.
export LC_ALL=C

if [ "$#" -lt 4 ]; then
    echo "usage: $0 <nm> <elf> <map> <archive>..." >&2
    exit 2
fi

NM="$1"; shift
ELF="$1"; shift
MAP="$1"; shift
# remaining args ($@) are the privileged KickOS-owned archives

command -v "$NM" >/dev/null 2>&1 || fail "nm not found: $NM"
[ -r "$ELF" ] || fail "cannot read $ELF"
[ -r "$MAP" ] || fail "cannot read $MAP"
[ "$#" -gt 0 ] || fail "no archives given (guard would pass vacuously)"

scratch_dir

# The landmark is a symbol SHAPE, not a name: the identifier prefix is per-target (below).
tool_out "$TMP/sym" '^[0-9a-fA-F]+[[:space:]]+[A-Za-z][[:space:]]' "$NM" "$ELF"

# The RX ABI prefixes every C identifier with an underscore, so rx72m.ld spells the window
# bounds ___kickos_appdata_start/_end with THREE. Both spellings, and nothing looser: a
# substring match would also take __kickos_appdata_load_end.
win_sym() { # <name> -> the one hex address on stdout, non-zero exit if 0 or >1 found
    awk -v n="$1" '$3 == n || $3 == "_" n { seen[$1] = 1 }
                   END { c = 0; for (a in seen) { c++; last = a }
                         if (c != 1) { exit 1 }
                         print last }' "$TMP/sym"
}

# REFUSE, not skip: registration is limited to boards whose linker script carves the window,
# so a missing bound means the registration guard drifted, and passing would report "no
# kernel object in the app window" about an image that has none to check.
WIN_START="$(win_sym __kickos_appdata_start)" \
    || fail "$ELF defines no __kickos_appdata_start: not an enforcing image, gate would be vacuous"
WIN_END="$(win_sym __kickos_appdata_end)" \
    || fail "$ELF defines no __kickos_appdata_end: not an enforcing image, gate would be vacuous"
WIN_START="0x$WIN_START"
WIN_END="0x$WIN_END"

# ld records the path it was given, so only the basename is stable between the link line
# and this argv.
NAMES=""
for A in "$@"; do
    [ -f "$A" ] || fail "archive not found: $A"
    NAMES="$NAMES $(basename "$A")"
done

# Field shape in the memory-map region, both forms ld emits:
#   " .data.SystemCoreClock            0x1fff0038  0x4 arch/libkickos_chip_mk64f.a(x.obj)"
#   " .data._ZN...longname\n                       0x1fff0000 0x20 kernel/libkickos_kernel.a(y.obj)"
# so the record is the last three fields and the section name is either the first field of
# the same line or the bare name line above it. Only the memory-map region is read: the
# "Discarded input sections" and "Archive member included" blocks name the same archives
# with addresses that are not placements.
awk -v names="$NAMES" -v win_start="$WIN_START" -v win_end="$WIN_END" '
function h2n(s,   i, c, d, v) {
    sub(/^0[xX]/, "", s)
    v = 0
    for (i = 1; i <= length(s); i++) {
        c = tolower(substr(s, i, 1))
        d = index("0123456789abcdef", c) - 1
        if (d < 0) { return -1 }
        v = v * 16 + d
    }
    return v
}
BEGIN {
    n = split(names, a, " ")
    for (i = 1; i <= n; i++) { want[a[i]] = 1 }
    lo = h2n(win_start)
    hi = h2n(win_end)
    if (lo < 0 || hi < 0 || hi <= lo) {
        printf "BADWIN %s %s\n", win_start, win_end
        exit
    }
}
/^Linker script and memory map/ { inmap = 1; next }
!inmap { next }
NF == 1 && $1 ~ /^([.*]|COMMON$)/ { sec = $1; next }
NF >= 3 && $(NF - 2) ~ /^0x/ && $(NF - 1) ~ /^0x/ && $NF ~ /\.a\(/ {
    if (NF >= 4) { sec = $1 }
    member = $NF
    split(member, p, "(")
    path = p[1]
    k = split(path, q, "/")
    base = q[k]
    if (!(base in want)) { next }
    # A non-allocated section carries a 0-BASED file offset, not a load address, so on a
    # chip whose RAM base is 0 (rx72m) those offsets fall numerically inside the window and
    # every .debug_* record reads as a leak. Only allocated bytes can be in the grant.
    if (sec ~ /^\.(debug|comment|note|stab|line)/) { next }
    # NAMED, not a shape: an unanchored /attributes$/ also takes an allocated writable
    # global whose -fdata-sections name ends that way (.bss.g_attributes).
    if (sec == ".ARM.attributes" || sec == ".riscv.attributes") { next }
    total[base]++
    if (sec ~ /^\.(data|bss|sdata|sbss)/ || sec == "COMMON") { writable[base]++ }
    start = h2n($(NF - 2))
    size = h2n($(NF - 1))
    if (start < 0 || size < 0) { next }
    if (start < hi && start + size > lo) {
        printf "LEAK %s %s %s %s\n", member, sec, $(NF - 2), $(NF - 1)
        leaks++
    }
    grand++
}
END {
    for (b in want) { printf "SEEN %s %d %d\n", b, total[b] + 0, writable[b] + 0 }
    printf "TOTAL %d %d\n", grand + 0, leaks + 0
}' "$MAP" > "$TMP/verdict"

if grep -q '^BADWIN ' "$TMP/verdict"; then
    fail "unusable app window from $ELF: $(sed -n 's/^BADWIN //p' "$TMP/verdict")"
fi

grand="$(awk '/^TOTAL /{ print $2 }' "$TMP/verdict")"
leaks="$(awk '/^TOTAL /{ print $3 }' "$TMP/verdict")"
[ "$grand" -gt 0 ] || fail "no input section from any given archive appears in $MAP (map format or basename mismatch; guard would pass vacuously)"

# Every KickOS archive carries code, so an archive the map never mentions was not matched
# at all and its data was never examined.
missing="$(awk '/^SEEN / && $3 == 0 { print $2 }' "$TMP/verdict" | sort)"
if [ -n "$missing" ]; then
    fail "these archives contribute no input section to $MAP (basename mismatch? wrong map?): $(echo "$missing" | tr '\n' ' ')"
fi

if [ "$leaks" -gt 0 ]; then
    echo "FAIL: privileged KickOS archive data sits INSIDE the app-granted RW window" >&2
    echo "      [$WIN_START, $WIN_END) is R+W to every unprivileged thread in every domain" >&2
    echo "      (kernel/domain/domain.cc, arch_domain_static_regions): direct privilege escalation." >&2
    echo "      A closed-set archive:member selector in the chip linker script matched nothing," >&2
    echo "      so the archive fell through to the .appdata/.appbss catch-all. Leaked placements:" >&2
    awk '/^LEAK /{ printf "        %s %s at %s size %s\n", $2, $3, $4, $5 }' "$TMP/verdict" >&2
    exit 1
fi

echo "PASS: 0 of $grand placement(s) from $# privileged archive(s) inside [$WIN_START, $WIN_END)"
awk '/^SEEN /{ printf "      %s: %d placement(s), %d writable (.data/.bss/COMMON)\n", $2, $3, $4 }' \
    "$TMP/verdict" | sort
