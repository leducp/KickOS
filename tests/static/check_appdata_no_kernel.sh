#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Security CI gate for the inverted .appdata scheme. The enforcing linker scripts capture the
# privileged archives into the KERNEL sections by `archive:member` colon selectors, and the
# app sections are pure CATCH-ALLS, so an archive nobody listed lands app-side.
# kernel/domain/domain.cc (arch_domain_static_regions) grants the app's windows to EVERY
# unprivileged thread in every domain, so a kernel object that lands in one is directly
# reachable by an unprivileged thread: a privilege-escalation primitive, not a layout wart.
#
# THE WINDOW BOUNDS AND THE PRIVILEGED SET ARE BOTH ARGUMENTS, because they differ by board
# shape and a gate carrying either would be wrong on half the fleet:
#   - MPU boards carve ONE writable window, __kickos_appdata_start/_end, and their scripts
#     select kernel/arch/chip/lib kernel-side, so the privileged set is those four.
#   - The split-image boards (armv8a, rv64imac) link the app LOW and the kernel HIGH and carve
#     TWO windows, __kickos_app_sram_start/_end (writable) and __kickos_app_rom_start/_end
#     (EL0-executable). Their scripts select kernel/arch/chip only, libkickos_lib.a being
#     app-side by design, so passing it would be a false positive waiting on lib's first
#     global.
# A window is REFUSED, never skipped, when its bounds are missing or empty: a board whose
# script states an absent window as start == end (arch/x86/x86_64/pe_image.ld does, every
# window, because `ld -m i386pep` builds no GOT and a weak-undefined reference would resolve
# to itself) fails here rather than reading clean.
#
# The linker scripts' own ASSERT(_ebss > _sbss) catches TOTAL selector failure only: a
# renamed selector drops ONE archive into an app window while the other archives keep the
# kernel .bss non-empty, and the link stays green.
#
# The LINK MAP is the instrument, naming the archive MEMBER behind every input section, so
# it covers file-static globals, anonymous-namespace globals and COMMON. nm serves the window
# bounds only: nm reports locals with lowercase types and names that repeat tree-wide, so an
# address lookup keyed on a local name is ambiguous.
#
# usage: check_appdata_no_kernel.sh <nm> <elf> <map> <start-sym>:<end-sym>... `--` <archive>...

set -eu
. "$(dirname "$0")/../lib/gate.sh"

if [ "$#" -lt 6 ]; then
    echo "usage: $0 <nm> <elf> <map> <start-sym>:<end-sym>... '--' <archive>..." >&2
    exit 2
fi

NM="$1"; shift
ELF="$1"; shift
MAP="$1"; shift

WINDOWS=""
while [ "$#" -gt 0 ]; do
    if [ "$1" = "--" ]; then shift; break; fi
    case "$1" in
        *:*) WINDOWS="$WINDOWS $1" ;;
        *)   echo "usage: window spec is <start-sym>:<end-sym>, got '$1'" >&2; exit 2 ;;
    esac
    shift
done

command -v "$NM" >/dev/null 2>&1 || fail "nm not found: $NM"
[ -r "$ELF" ] || fail "cannot read $ELF"
[ -r "$MAP" ] || fail "cannot read $MAP"
[ -n "$WINDOWS" ] || fail "no window given (the gate would examine nothing)"
[ "$#" -gt 0 ] || fail "no archives given (guard would pass vacuously)"

scratch_dir

# The EREs the two scanners key on, defined once and handed in, so the self-test below and
# the real scan cannot disagree about what the rule is.
#
# The RX ABI prefixes every C identifier with an underscore, so rx72m.ld spells the window
# bounds ___kickos_appdata_start/_end with THREE. BOTH ENDS ARE ANCHORED, which is what keeps
# the lookup exact: a prefix match would also take __kickos_appdata_start_hi, and a substring
# match __kickos_appdata_load_end.
win_ere() { # <name> -> the ERE matching that name and its RX spelling, and nothing else
    printf '^_?%s$' "$1"
}
MAP_HEADING='^Linker script and memory map'
# EVERY BACKSLASH BELOW IS DOUBLED. awk processes escape sequences in a -v assignment, so a
# single `\.` reaches the program as a plain `.` and matches any byte: the leading dot of a
# section name stops being literal, and the anchored attributes test starts accepting
# .ARM_attributes.
#
# A non-allocated section carries a 0-BASED file offset, not a load address, so on a chip
# whose RAM base is 0 (rx72m) those offsets fall numerically inside the window and every
# .debug_* record reads as a leak. Only allocated bytes can be in the grant.
NONALLOC_ERE='^\\.(debug|comment|note|stab|line)'
# ANCHORED AT BOTH ENDS: an unanchored /attributes$/ also takes an allocated writable global
# whose -fdata-sections name ends that way (.bss.g_attributes).
ATTR_ERE='^\\.(ARM|riscv)\\.attributes$'
WRITABLE_ERE='^\\.(data|bss|sdata|sbss)'
# The gate's premise is a FALLTHROUGH: an archive matched no selector and the catch-all took
# it. A section the script places by an explicit named rule did not fall through, so it is
# exempt. .apptrap is the EL0 trap leaf (arch/*/switch.S), named because the archive holding
# it is kernel-side and `.text.*` would capture it there. ANCHORED AT BOTH ENDS: unanchored,
# a kernel `.text.apptrap_helper` falling app-side would be exempted with it.
PLACED_ERE='^\\.apptrap$'

win_sym() { # <symfile> <name-ere> -> the one hex address on stdout, non-zero if 0 or >1 found
    awk -v pat="$2" '$3 ~ pat { seen[$1] = 1 }
                     END { c = 0; for (a in seen) { c++; last = a }
                           if (c != 1) { exit 1 }
                           print last }' "$1"
}

# Field shape in the memory-map region, both forms ld emits:
#   " .data.SystemCoreClock            0x1fff0038  0x4 arch/libkickos_chip_mk64f.a(x.obj)"
#   " .data._ZN...longname\n                       0x1fff0000 0x20 kernel/libkickos_kernel.a(y.obj)"
# so the record is the last three fields and the section name is either the first field of
# the same line or the bare name line above it. Only the memory-map region is read: the
# "Discarded input sections" and "Archive member included" blocks name the same archives
# with addresses that are not placements.
map_scan() { # <map> <basenames> <win-start> <win-end> <heading> <nonalloc> <attr> <writable> <placed>
    awk -v names="$2" -v win_start="$3" -v win_end="$4" -v heading="$5" \
        -v nonalloc="$6" -v attrsec="$7" -v writ="$8" -v placed="$9" '
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
# Guarded on inmap so the heading clause can be turned OFF by a heading that matches every
# line; without the guard such a heading would consume every record instead.
!inmap && $0 ~ heading { inmap = 1; next }
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
    if (sec ~ nonalloc) { next }
    if (sec ~ attrsec) { next }
    total[base]++
    if (sec ~ writ || sec == "COMMON") { writable[base]++ }
    start = h2n($(NF - 2))
    size = h2n($(NF - 1))
    if (start < 0 || size < 0) { next }
    # size > 0 first: the overlap test is half-open, so a zero-length placement satisfies
    # `start + size > lo` on its own and reports as a leak of nothing.
    if (size > 0 && start < hi && start + size > lo) {
        if (sec ~ placed) {
            exempt++
        } else {
            printf "LEAK %s %s %s %s\n", member, sec, $(NF - 2), $(NF - 1)
            leaks++
        }
    }
    grand++
}
END {
    for (b in want) { printf "SEEN %s %d %d\n", b, total[b] + 0, writable[b] + 0 }
    printf "TOTAL %d %d %d\n", grand + 0, leaks + 0, exempt + 0
}' "$1"
}

# --- self-test: prove every clause of the rule, one control per clause ---------
# Each control below is a MINIMAL PAIR: the positive and the negative differ in one property
# only, and every expected count is exact.
CTL_LO=0x2000
CTL_HI=0x3000
CTL_NAMES=' libkickos_kernel.a libkickos_arch_ctl.a'
CTL_HEADING='Linker script and memory map'

ctl_scan() { # <map> [heading] [nonalloc] [attr] [placed]
    _h="$MAP_HEADING"
    _n="$NONALLOC_ERE"
    _a="$ATTR_ERE"
    _p="$PLACED_ERE"
    if [ "$#" -ge 2 ]; then _h="$2"; fi
    if [ "$#" -ge 3 ]; then _n="$3"; fi
    if [ "$#" -ge 4 ]; then _a="$4"; fi
    if [ "$#" -ge 5 ]; then _p="$5"; fi
    map_scan "$1" "$CTL_NAMES" "$CTL_LO" "$CTL_HI" "$_h" "$_n" "$_a" "$WRITABLE_ERE" "$_p"
}
ctl_leaks() { # <map> [heading] [nonalloc] [attr] [placed] -> LEAK line count
    ctl_scan "$@" | grep -c '^LEAK ' || :
}

# One leak per shape and per boundary. The pre-heading record is the SAME shape as the
# same-line positive and differs only in sitting above the heading. The last two are the
# near misses the .apptrap exemption's anchors exist for.
cat > "$TMP/ctl.pos.map" <<'EOF'
Archive member included to satisfy reference by file (symbol)

 .data.pre_heading
                0x00002100       0x10 kernel/libkickos_kernel.a(discarded.cc.obj)
Linker script and memory map

 .data.same_line 0x00002000      0x10 kernel/libkickos_kernel.a(same.cc.obj)
 .bss._ZN6kickos7wrappedE
                0x00002200       0x20 kernel/libkickos_kernel.a(wrap.cc.obj)
COMMON
                0x00002300        0x8 kernel/libkickos_kernel.a(common.cc.obj)
 .bss.g_attributes
                0x00002400        0x4 kernel/libkickos_kernel.a(attr.cc.obj)
 .data.ends_past_lo 0x00001000  0x1001 kernel/libkickos_kernel.a(edge.cc.obj)
 .data.starts_below_hi
                0x00002fff        0x1 kernel/libkickos_kernel.a(edge.cc.obj)
 .apptrap.veneer 0x00002600       0x8 kernel/libkickos_kernel.a(trap.cc.obj)
 .text.apptrap  0x00002610        0x8 kernel/libkickos_kernel.a(trap.cc.obj)
EOF

# Every line here is the same-line shape, so the per-control loop below can read one at a
# time. Each differs from a positive above in exactly one property.
cat > "$TMP/ctl.neg.map" <<'EOF'
 .debug_info    0x00002100       0x10 kernel/libkickos_kernel.a(dbg.cc.obj)
 .comment       0x00002110        0x8 kernel/libkickos_kernel.a(dbg.cc.obj)
 .ARM.attributes 0x00002120       0x4 kernel/libkickos_kernel.a(attr.cc.obj)
 .riscv.attributes 0x00002130      0x4 kernel/libkickos_kernel.a(attr.cc.obj)
 .data.stranger 0x00002140       0x10 other/libkickos_other.a(x.cc.obj)
 .data.ends_at_lo 0x00001000    0x1000 kernel/libkickos_kernel.a(edge.cc.obj)
 .data.starts_at_hi 0x00003000     0x10 kernel/libkickos_kernel.a(edge.cc.obj)
 .data.badaddr  0xzzzz0000       0x10 kernel/libkickos_kernel.a(edge.cc.obj)
 .data.below_window 0x00000100     0x10 kernel/libkickos_kernel.a(edge.cc.obj)
 .data.empty_inside 0x00002500      0x0 kernel/libkickos_kernel.a(edge.cc.obj)
 .apptrap       0x00002700        0x8 kernel/libkickos_arch_ctl.a(switch.S.obj)
EOF

# ctl.neg.map carries no heading, so that the per-control loop below reads records only. A
# scan of it MUST be given one, or every control is silent because nothing was read at all.
printf '%s\n' "$CTL_HEADING" > "$TMP/ctl.neg.full.map"
cat "$TMP/ctl.neg.map" >> "$TMP/ctl.neg.full.map"

POS="$(ctl_leaks "$TMP/ctl.pos.map")"
[ "$POS" -eq 8 ] || fail "the map scan found $POS of 8 planted leaks; it would miss a real one"

# The SECTION NAME of each leak, not just the count. A scan that stopped reading the bare
# name line above a wrapped placement still reports the leak, carrying the PREVIOUS record's
# section name, so the count alone cannot see that clause break.
ctl_scan "$TMP/ctl.pos.map" | awk '/^LEAK /{ print $3 }' | sort > "$TMP/ctl.secs"
cat > "$TMP/ctl.secs.want" <<'EOF'
.apptrap.veneer
.bss._ZN6kickos7wrappedE
.bss.g_attributes
.data.ends_past_lo
.data.same_line
.data.starts_below_hi
.text.apptrap
COMMON
EOF
sort "$TMP/ctl.secs.want" -o "$TMP/ctl.secs.want"
if ! cmp -s "$TMP/ctl.secs" "$TMP/ctl.secs.want"; then
    fail "the planted leaks resolved section names $(tr '\n' ' ' < "$TMP/ctl.secs"), expected
      $(tr '\n' ' ' < "$TMP/ctl.secs.want"); a wrapped placement is being read against the
      section name of the record above it"
fi

if [ "$(ctl_leaks "$TMP/ctl.neg.full.map" | tr -d ' ')" != 0 ]; then
    ctl_scan "$TMP/ctl.neg.full.map" | sed 's/^/      /' >&2
    fail "the map scan reported a non-allocated section, an attributes section, an unlisted
      archive, an explicitly placed section or a placement outside the window; the gate would
      cry wolf and be switched off"
fi

# Each control is re-read on its own, so a control silent for the WRONG reason is visible. A
# whole-file zero cannot tell "every clause works" from "one clause swallowed the file".
i=0
while IFS= read -r line; do
    i=$((i + 1))
    printf '%s\n%s\n' "$CTL_HEADING" "$line" > "$TMP/ctl.one.map"
    n="$(ctl_leaks "$TMP/ctl.one.map" | tr -d ' ')"
    [ "$n" -eq 0 ] || fail "negative control $i reports a leak: $line"
done < "$TMP/ctl.neg.map"
[ "$i" -eq 11 ] || fail "$i negative control(s) ran, expected 11"

# The exempted placement must be COUNTED as exempt and not merely absent from the leaks: a
# clause that dropped the record before the tally would read the same way here.
printf '%s\n' "$CTL_HEADING" > "$TMP/ctl.trap.map"
grep '^ \.apptrap ' "$TMP/ctl.neg.map" >> "$TMP/ctl.trap.map"
ctl_scan "$TMP/ctl.trap.map" | grep -qxF 'TOTAL 1 0 1' \
    || fail "the explicitly placed control did not tally as one exempt placement: $(ctl_scan "$TMP/ctl.trap.map" | grep '^TOTAL ')"

# Turn each clause OFF and the count over the control corpus must MOVE by an EXACT amount:
# a control kept quiet by the wrong clause then shows up as the wrong number.
NEVER='KICKOS_THIS_ERE_MATCHES_NOTHING'
mutate() { # <what> <expect> <map> [heading] [nonalloc] [attr] [placed]
    _what="$1"; _want="$2"; shift 2
    _got="$(ctl_leaks "$@" | tr -d ' ')"
    [ "$_got" -eq "$_want" ] || fail "with the $_what clause disabled the map scan reported
      $_got leak(s) of $1, expected $_want; the controls for it are not near misses and prove nothing"
}
# A heading matching every line puts the scan inside the map region from line 1, so the
# pre-heading record joins the eight.
mutate "heading"  9 "$TMP/ctl.pos.map" '^'
mutate "nonalloc" 2 "$TMP/ctl.neg.full.map" "$MAP_HEADING" "$NEVER"
mutate "attr"     2 "$TMP/ctl.neg.full.map" "$MAP_HEADING" "$NONALLOC_ERE" "$NEVER"
mutate "placed"   1 "$TMP/ctl.neg.full.map" "$MAP_HEADING" "$NONALLOC_ERE" "$ATTR_ERE" "$NEVER"
# With the attributes test UNANCHORED the planted .bss.g_attributes leak is exempted, so the
# count drops instead of rising.
mutate "attr-anchor" 7 "$TMP/ctl.pos.map" "$MAP_HEADING" "$NONALLOC_ERE" 'attributes$'
# With the placed test UNANCHORED both near misses are exempted with it.
mutate "placed-anchor" 6 "$TMP/ctl.pos.map" "$MAP_HEADING" "$NONALLOC_ERE" "$ATTR_ERE" 'apptrap'

# The tallies the verdict is read off, and the two refusals that keep an unusable window or
# an unmatched archive from reading clean.
CTL_V="$(ctl_scan "$TMP/ctl.pos.map")"
printf '%s\n' "$CTL_V" | grep -qxF 'SEEN libkickos_kernel.a 8 6' \
    || fail "the placement tally miscounted the control corpus: $(printf '%s\n' "$CTL_V" | grep '^SEEN ')"
printf '%s\n' "$CTL_V" | grep -qxF 'SEEN libkickos_arch_ctl.a 0 0' \
    || fail "an archive with no placement did not report a zero tally, so a basename mismatch would read clean"
printf '%s\n' "$CTL_V" | grep -qxF 'TOTAL 8 8 0' \
    || fail "the grand tally miscounted the control corpus: $(printf '%s\n' "$CTL_V" | grep '^TOTAL ')"
map_scan "$TMP/ctl.pos.map" "$CTL_NAMES" 0x3000 0x2000 "$MAP_HEADING" "$NONALLOC_ERE" \
    "$ATTR_ERE" "$WRITABLE_ERE" "$PLACED_ERE" | grep -q '^BADWIN ' \
    || fail "an inverted window was not refused, so every placement would read as outside it"
map_scan "$TMP/ctl.pos.map" "$CTL_NAMES" 0x2000 0x2000 "$MAP_HEADING" "$NONALLOC_ERE" \
    "$ATTR_ERE" "$WRITABLE_ERE" "$PLACED_ERE" | grep -q '^BADWIN ' \
    || fail "an EMPTY window (start == end, how a script states a window it does not carve)
      was not refused, so a board with no window at all would read clean"
map_scan "$TMP/ctl.pos.map" "$CTL_NAMES" 0xnothex 0x3000 "$MAP_HEADING" "$NONALLOC_ERE" \
    "$ATTR_ERE" "$WRITABLE_ERE" "$PLACED_ERE" | grep -q '^BADWIN ' \
    || fail "an unparsable window bound was not refused"

# The window lookup, on a symbol table holding both near misses the anchors exist for.
cat > "$TMP/ctl.sym" <<'EOF'
1fff45a0 D __kickos_appdata_start
1fff4600 D __kickos_appdata_start_hi
200145a0 D __kickos_appdata_end
20014600 D __kickos_appdata_load_end
2001a000 D ___kickos_appdata_rxbound
2001b000 D __kickos_appdata_twice
2001c000 D __kickos_appdata_twice
EOF
ctl_win() { # <name> -> the address, or nothing on refusal
    win_sym "$TMP/ctl.sym" "$(win_ere "$1")" || :
}
[ "$(ctl_win __kickos_appdata_start)" = 1fff45a0 ] \
    || fail "the window lookup did not resolve the exact start symbol"
[ "$(ctl_win __kickos_appdata_end)" = 200145a0 ] \
    || fail "the window lookup did not resolve the exact end symbol"
[ "$(ctl_win __kickos_appdata_rxbound)" = 2001a000 ] \
    || fail "the window lookup does not accept the RX spelling, so the gate is vacuous on that board"
[ -z "$(ctl_win __kickos_appdata_absent)" ] \
    || fail "the window lookup resolved a symbol the table does not define"
[ -z "$(ctl_win __kickos_appdata_twice)" ] \
    || fail "the window lookup accepted a symbol at two addresses, so the window would be one of them by table order"
# Unanchored, the start lookup also takes __kickos_appdata_start_hi and so resolves two
# addresses, which is what proves the anchors are load-bearing and not decoration.
win_sym "$TMP/ctl.sym" '^_?__kickos_appdata_start' >/dev/null 2>&1 \
    && fail "an unanchored window lookup still resolved one address; the near-miss symbol proves nothing"

# --- the image and the map ----------------------------------------------------
# A symbol SHAPE and not a name: the identifier prefix is per-target (below).
tool_out "$TMP/sym" '^[0-9a-fA-F]+[[:space:]]+[A-Za-z][[:space:]]' "$NM" "$ELF"

# ld records the path it was given, so only the basename is stable between the link line
# and this argv.
NAMES=""
for A in "$@"; do
    [ -f "$A" ] || fail "archive not found: $A"
    NAMES="$NAMES $(basename "$A")"
done

FOUND=0
LEAKED=0
REPORT=""
for W in $WINDOWS; do
    START_SYM="${W%%:*}"
    END_SYM="${W##*:}"
    # REFUSE, not skip: registration is limited to boards whose linker script carves these
    # windows, so a missing bound means the registration guard drifted.
    WIN_START="$(win_sym "$TMP/sym" "$(win_ere "$START_SYM")")" \
        || fail "$ELF defines no $START_SYM: not an enforcing image, gate would be vacuous"
    WIN_END="$(win_sym "$TMP/sym" "$(win_ere "$END_SYM")")" \
        || fail "$ELF defines no $END_SYM: not an enforcing image, gate would be vacuous"
    WIN_START="0x$WIN_START"
    WIN_END="0x$WIN_END"

    map_scan "$MAP" "$NAMES" "$WIN_START" "$WIN_END" "$MAP_HEADING" "$NONALLOC_ERE" \
        "$ATTR_ERE" "$WRITABLE_ERE" "$PLACED_ERE" > "$TMP/verdict"

    if grep -q '^BADWIN ' "$TMP/verdict"; then
        fail "unusable window $START_SYM..$END_SYM from $ELF: $(sed -n 's/^BADWIN //p' "$TMP/verdict")"
    fi

    grand="$(awk '/^TOTAL /{ print $2 }' "$TMP/verdict")"
    leaks="$(awk '/^TOTAL /{ print $3 }' "$TMP/verdict")"
    exempt="$(awk '/^TOTAL /{ print $4 }' "$TMP/verdict")"
    [ "$grand" -gt 0 ] || fail "no input section from any given archive appears in $MAP (map format or basename mismatch; guard would pass vacuously)"

    # Every KickOS archive carries code, so an archive the map never mentions was not matched
    # at all and its data was never examined.
    missing="$(awk '/^SEEN / && $3 == 0 { print $2 }' "$TMP/verdict" | sort)"
    if [ -n "$missing" ]; then
        fail "these archives contribute no input section to $MAP (basename mismatch? wrong map?): $(echo "$missing" | tr '\n' ' ')"
    fi

    if [ "$leaks" -gt 0 ]; then
        echo "FAIL: privileged KickOS archive content sits INSIDE an app-granted window" >&2
        echo "      [$WIN_START, $WIN_END) ($START_SYM..$END_SYM) is granted to every" >&2
        echo "      unprivileged thread in every domain (kernel/domain/domain.cc," >&2
        echo "      arch_domain_static_regions): direct privilege escalation." >&2
        echo "      A closed-set archive:member selector in the linker script matched nothing," >&2
        echo "      so the archive fell through to an app catch-all. Leaked placements:" >&2
        awk '/^LEAK /{ printf "        %s %s at %s size %s\n", $2, $3, $4, $5 }' "$TMP/verdict" >&2
        LEAKED=$((LEAKED + leaks))
    fi

    REPORT="$REPORT
      $START_SYM..$END_SYM [$WIN_START, $WIN_END): 0 of $grand placement(s), $exempt explicitly placed"
    FOUND=$((FOUND + 1))
done

[ "$LEAKED" -eq 0 ] || exit 1

echo "PASS: $FOUND window(s) clean of $# privileged archive(s)$REPORT"
