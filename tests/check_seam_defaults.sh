#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Seam-fallback gate. KickOS resolves an optional arch/chip seam by ARCHIVE MEMBER
# EXTRACTION, not by __attribute__((weak)): the fallback body sits alone in a
# <symbol>_default.cc translation unit, and a backend that defines the symbol keeps that
# member out of the link. arch/CMakeLists.txt states the rule; this script enforces it.
#
# Usage:
#   check_seam_defaults.sh <nm> <readelf> <elf> <map> <allowlist> <archive>...
#
# Legs 1, 2 and 3 count STRONG definitions only. A COMDAT (vague-linkage) definition is
# emitted in EVERY translation unit that needs it out of line, so it can never be the
# undefined reference that extracts a member, and duplicates merge instead of colliding.
# Whether an inline function, template or implicit member lands out of line at all is an
# optimization-level and compiler-version artifact (-Os inlines kickos::arm::reg32 away,
# -O0 emits it), so counting one is a false positive. Leg 3 still catches extraction for
# the wrong symbol whatever its linkage. Leg 4 still requires every weak C++ symbol to BE
# COMDAT, so a deliberate weak attribute is not exempted anywhere.
#
# Four legs:
#   1. Each fallback member defines EXACTLY ONE strong global symbol, no other member of
#      the same archive defines that symbol, and no fallback sits in kickos_kernel. A
#      second strong symbol would drag the member in unconditionally and collide with
#      every backend; a kernel-resident fallback is extracted before the chip archive is
#      even scanned.
#   2. For a seam a backend DOES define, the link resolved it from that backend's member
#      and no fallback member for it entered the image. This is the leg that catches a
#      backend definition placed in an archive member nothing anchors: such a board links
#      clean and SILENTLY DECLINES at runtime.
#   3. For a seam NO backend defines, the link resolved it from the fallback member. Keeps
#      a no-backend board from resolving somewhere unintended, and keeps leg 2 honest by
#      proving the fallback path is really exercised on this board.
#   4. Zero weak symbols outside tests/weak_allowlist.txt. In an archive a C++ mangled
#      weak symbol is additionally required to be COMDAT (vague linkage, which the
#      language mandates); a deliberate weak attribute on a C++ function is not COMDAT and
#      still fails. Section groups are resolved away in the final ELF, so there the
#      mangled names are taken on trust and the archive leg is what covers our code.

set -u
. "$(dirname "$0")/lib/gate.sh"

# Locale-independent sort/grep collation; the map and the tool output are both parsed
# structurally, never by their translated headings (readelf's are, under any other
# locale, and the "File:" member marker the awks key on is one of them).
export LC_ALL=C

if [ "$#" -lt 6 ]; then
    echo "usage: $0 <nm> <readelf> <elf> <map> <allowlist> <archive>..." >&2
    exit 2
fi

NM="$1"; shift
READELF="$1"; shift
ELF="$1"; shift
MAP="$1"; shift
ALLOWLIST="$1"; shift
# Everything left is a definition source: a .a is scanned member by member and joins the
# link only when the map says so; a plain .o is on the link command line and is therefore
# always in the image (the app's own TUs, which may define an app-side seam).
#
# Split on ';' as well as on argument boundaries: add_test does NOT split a
# $<TARGET_OBJECTS:> expansion, so the app's whole object list arrives as ONE
# semicolon-joined argument (verified in the generated CTestTestfile.cmake).
ARCHIVES=""
OBJECTS=""
for _in in $(printf '%s\n' "$@" | tr ';' '\n'); do
    case "$_in" in
        *.a) ARCHIVES="$ARCHIVES $_in" ;;
        *)   OBJECTS="$OBJECTS $_in" ;;
    esac
done

for f in "$ELF" "$MAP" "$ALLOWLIST"; do
    if [ ! -r "$f" ]; then
        fail "cannot read $f"
    fi
done

scratch_dir
rc=0
# A seam violation is accumulated, so one run names every one of them; a BROKEN TOOL is
# not, and takes gate.sh's fail()/tool_out() hard exit instead.
bad() { echo "FAIL: $*" >&2; rc=1; }

# nm prints "<addr> <type> <symbol>", prefixed "<archive>:<member>:" with -A. A source
# of definitions with no GLOBAL definition at all is not one of ours, so requiring one
# is a positive control on nm itself.
NM_DEF_RE='^[0-9a-fA-F]+ [A-Z] '
NM_ARCHIVE_DEF_RE=':[0-9a-fA-F]+ [A-Z] '
# readelf -sW numbers every symbol-table row; a file it could not read has no row at all.
READELF_SYM_RE='^ *[0-9]+: '

# The RX psABI prefixes every C identifier with an underscore, so the allowlist carries
# both spellings and nothing here needs to know which target it is looking at.
grep -vE '^[[:space:]]*(#|$)' "$ALLOWLIST" | awk '{print $1}' | sort -u > "$TMP/allow"
allowed() { grep -qxF "$1" "$TMP/allow"; }

# --- inventory: every defined symbol of every archive, member by member -------
# `nm -A` on an archive prints "<path>:<member>:<addr> <type> <symbol>". A local symbol
# carries a lowercase type; only the uppercase ones can take part in cross-member
# resolution. N (debug) and U (undefined) are neither.
: > "$TMP/defs"
for a in $ARCHIVES; do
    if [ ! -r "$a" ]; then
        bad "cannot read archive $a"
        continue
    fi
    tool_out "$TMP/tool" "$NM_ARCHIVE_DEF_RE" "$NM" -A --defined-only "$a"
    awk -v A="$a" '
        {
            split($1, p, ":")
            if (p[2] == "") { next }
            t = $(NF - 1)
            if (t !~ /^[A-Z]$/ || t == "N" || t == "U") { next }
            print A "\t" p[2] "\t" t "\t" $NF
        }' "$TMP/tool" >> "$TMP/defs"
done
# An object that cannot be read is the same failure the archive arm above reports, and
# skipping it classifies every seam the app itself defines (kickos_app_authority) as
# having NO backend: the fallback then reads as correct and the app's narrower authority
# mask is silently widened to the fallback's.
for o in $OBJECTS; do
    if [ ! -r "$o" ]; then
        bad "cannot read object $o"
        continue
    fi
    tool_out "$TMP/tool" "$NM_DEF_RE" "$NM" --defined-only "$o"
    awk -v O="$(basename "$o")" '
        {
            t = $(NF - 1)
            if (t !~ /^[A-Z]$/ || t == "N" || t == "U") { next }
            print "-\t" O "\t" t "\t" $NF
        }' "$TMP/tool" >> "$TMP/defs"
    basename "$o" >> "$TMP/cmdline_members"
done
require_nonempty "$TMP/cmdline_members" \
    "no app object was inventoried; a seam the app itself defines would read as backend-less"

# --- inventory: COMDAT sections and the symbols they define -------------------
# A C++ vague-linkage definition lives in a section carrying readelf's G (SHF_GROUP) flag.
# Built once here; legs 1, 2, 3 subtract these symbols and leg 4 tests membership.
# In readelf -SW a section header line is "[<nr>] <name> <type> <addr> <off> <size> <es>
# <flg> <lk> <inf> <al>", so the flags are the field before the last three. A section with
# NO flags prints an empty column and shifts those fields left onto the hex entry size,
# which cannot contain a G.
comdat_sections() { # <file> <archive-key> <member-name-when-not-an-archive>
    tool_out "$TMP/tool" '^ *\[ *[0-9]+\]' "$READELF" -SW "$1"
    awk -v A="$2" -v M0="$3" '
        BEGIN { m = M0 }
        /^File:/ {
            m = $2
            sub(/^.*\(/, "", m); sub(/\)$/, "", m)
            next
        }
        /^ *\[ *[0-9]+\]/ {
            line = $0
            sub(/^ *\[ */, "", line)
            if ($(NF - 3) ~ /G/) { print A "\t" m "\t" (line + 0) }
        }' "$TMP/tool"
}

# Only a GLOBAL or WEAK definition with a real section index can participate in
# cross-member resolution, which is the same set the nm inventory above keeps.
comdat_symbols() { # <file> <archive-key> <member-name-when-not-an-archive>
    tool_out "$TMP/syms" "$READELF_SYM_RE" "$READELF" -sW "$1"
    awk -v A="$2" -v M0="$3" -v SEC="$TMP/comdat_sec" '
        BEGIN {
            m = M0
            while ((getline line < SEC) > 0) {
                split(line, f, "\t")
                if (f[1] == A) { g[f[2] SUBSEP (f[3] + 0)] = 1 }
            }
        }
        /^File:/ {
            m = $2
            sub(/^.*\(/, "", m); sub(/\)$/, "", m)
            next
        }
        ($5 == "GLOBAL" || $5 == "WEAK") && $7 ~ /^[0-9]+$/ {
            if ((m SUBSEP ($7 + 0)) in g) { print A "\t" m "\t" $8 }
        }' "$TMP/syms"
}

: > "$TMP/comdat_sec"
for a in $ARCHIVES; do
    comdat_sections "$a" "$a" "" >> "$TMP/comdat_sec"
done
for o in $OBJECTS; do
    comdat_sections "$o" "-" "$(basename "$o")" >> "$TMP/comdat_sec"
done

: > "$TMP/comdat_syms"
for a in $ARCHIVES; do
    comdat_symbols "$a" "$a" "" >> "$TMP/comdat_syms"
done
for o in $OBJECTS; do
    comdat_symbols "$o" "-" "$(basename "$o")" >> "$TMP/comdat_syms"
done
sort -u "$TMP/comdat_syms" -o "$TMP/comdat_syms"

# defs carries "<archive> <member> <nm-type> <symbol>"; the COMDAT key is columns 1, 2, 4.
awk -F'\t' -v C="$TMP/comdat_syms" '
    BEGIN { while ((getline line < C) > 0) { g[line] = 1 } }
    !((($1 "\t" $2 "\t" $4)) in g)' "$TMP/defs" > "$TMP/strong_defs"

awk -F'\t' '$2 ~ /_default\.cc\.o(bj)?$/' "$TMP/strong_defs" > "$TMP/fb_defs"
awk -F'\t' '$2 !~ /_default\.cc\.o(bj)?$/' "$TMP/strong_defs" > "$TMP/be_defs"

# --- leg 1: one strong symbol per fallback member, unique in its archive ------
awk -F'\t' '{ print $1 "\t" $2 }' "$TMP/fb_defs" | sort -u > "$TMP/fb_members"
if [ ! -s "$TMP/fb_members" ]; then
    bad "no <symbol>_default.cc member found in any archive; the gate would be vacuous"
fi
while IFS=$'\t' read -r arch member; do
    # kickos_kernel is scanned BEFORE kickos_chip in the rescan group, so a fallback
    # placed there is extracted in the pass that first makes the symbol undefined and then
    # collides with the chip's definition. It belongs in the arch library.
    case "$arch" in
        *libkickos_kernel.a)
            bad "leg 1: $member is in kickos_kernel; a fallback must sit in an archive the link scans AFTER the chip archive"
            ;;
    esac
    syms=$(awk -F'\t' -v a="$arch" -v m="$member" '$1 == a && $2 == m { print $4 }' "$TMP/fb_defs" | sort -u)
    n=$(printf '%s\n' "$syms" | grep -c .)
    if [ "$n" -ne 1 ]; then
        bad "leg 1: $member defines $n global symbols, expected exactly 1: $(echo $syms)"
        continue
    fi
    # Same archive, same symbol, another member: resolution would fall to member order.
    twin=$(awk -F'\t' -v a="$arch" -v m="$member" -v s="$syms" \
        '$1 == a && $2 != m && $4 == s { print $2 }' "$TMP/strong_defs" | sort -u)
    if [ -n "$twin" ]; then
        bad "leg 1: $syms is defined by both $member and $(echo $twin) inside $arch"
    fi
done < "$TMP/fb_members"

# --- map: which archive members entered the link, and which symbol pulled each -
# The map opens with the archive-member inclusion list:
#
#   arch/libkickos_arch_armv7m.a(arch_pinmux_set_default.cc.obj)
#                                 kernel/libkickos_kernel.a(syscall.cc.obj) (arch_pinmux_set)
#
# Selected structurally, never by the block's heading: only an inclusion entry puts an
# `<archive>(<member>)` at column 0. The headings are translated by the host linker's
# locale, and the per-function section names the rest of the map would offer do not exist
# in the hosted (sim) build, which compiles without -ffunction-sections.
awk '
    /^[^ \t].*\.a\(.*\)$/ {
        m = $1
        sub(/^.*\(/, "", m); sub(/\)$/, "", m)
        pend = m
        next
    }
    pend != "" {
        why = ""
        if ($NF ~ /^\(.*\)$/) { why = substr($NF, 2, length($NF) - 2) }
        print pend "\t" why
        pend = ""
    }' "$MAP" | sort -u > "$TMP/included"

# --- legs 2 and 3, per seam symbol -------------------------------------------
awk -F'\t' '{ print $4 }' "$TMP/fb_defs" | sort -u > "$TMP/seams"
checked_fallback=0
while read -r sym; do
    [ -n "$sym" ] || continue
    fbmember=$(awk -F'\t' -v s="$sym" '$4 == s { print $2 }' "$TMP/fb_defs" | sort -u)
    backends=$(awk -F'\t' -v s="$sym" '$4 == s { print $2 }' "$TMP/be_defs" | sort -u)
    nbackends=$(printf '%s\n' "$backends" | grep -c .)
    if [ "$nbackends" -gt 1 ]; then
        bad "leg 2: $sym has $nbackends backend definitions ($(echo $backends)); which one this link resolves to is member order"
        continue
    fi
    in_link=$(awk -F'\t' -v m="$fbmember" '$1 == m { print "yes" }' "$TMP/included" | head -1)
    if [ "$nbackends" -eq 1 ]; then
        backend_linked=""
        if grep -qxF "$backends" "$TMP/cmdline_members"; then
            backend_linked=yes
        elif awk -F'\t' -v m="$backends" '$1 == m { print "yes" }' "$TMP/included" | grep -q yes; then
            backend_linked=yes
        fi
        # A backend owns this seam, so its fallback member must not be in the image at
        # all. If it is, the backend's own member was not anchored, the fallback answered
        # the reference first, and the board SILENTLY DECLINES at runtime.
        if [ -n "$in_link" ]; then
            bad "leg 2: $fbmember entered the link although $backends defines $sym; that backend member is not anchored, so this board silently declines at runtime"
            continue
        fi
        if [ -z "$backend_linked" ]; then
            bad "leg 2: $backends defines $sym but never entered the link, and its fallback did not either"
        fi
        # The map must not mention the fallback member anywhere, not even as a discarded
        # section: an unextracted member cannot appear at all.
        if grep -qF "($fbmember)" "$MAP"; then
            bad "leg 2: $fbmember appears in the link map although $backends owns $sym"
        fi
        continue
    fi
    # No backend at all: the fallback must be what answered the reference. A link that
    # succeeded with the fallback ABSENT proves nothing in the image referenced the seam
    # (an unresolved arch_* symbol is a link error), so that case has nothing to assert.
    if [ -z "$in_link" ]; then
        continue
    fi
    why=$(awk -F'\t' -v m="$fbmember" '$1 == m { print $2 }' "$TMP/included" | head -1)
    # The RX psABI underscore is in the symbol table but not in what ld prints here.
    if [ "$why" != "$sym" ] && [ "_$why" != "$sym" ]; then
        bad "leg 3: $fbmember entered the link to satisfy '$why', not $sym"
        continue
    fi
    checked_fallback=$((checked_fallback + 1))
done < "$TMP/seams"
if [ "$checked_fallback" -eq 0 ]; then
    bad "leg 3: this board resolved no seam from its fallback, so the fallback path is untested here"
fi

# --- leg 4: no weak symbol outside the allowlist ------------------------------
comdat_ok() { # <archive> <member> <section index>
    grep -qxF "$1	$2	$(($3 + 0))" "$TMP/comdat_sec"
}

for a in $ARCHIVES; do
    tool_out "$TMP/syms" "$READELF_SYM_RE" "$READELF" -sW "$a"
    awk -v A="$a" '
        /^File:/ {
            m = $2
            sub(/^.*\(/, "", m); sub(/\)$/, "", m)
            next
        }
        $5 == "WEAK" { print A "\t" m "\t" $7 "\t" $8 }' "$TMP/syms" > "$TMP/weak_arch"
    while IFS=$'\t' read -r arch member ndx sym; do
        [ -n "$sym" ] || continue
        if allowed "$sym"; then
            continue
        fi
        case "$sym" in
            _Z*|__Z*)
                if [ "$ndx" = "UND" ]; then
                    bad "leg 4: weak undefined C++ reference $sym in $member ($arch)"
                elif ! comdat_ok "$arch" "$member" "$ndx"; then
                    bad "leg 4: weak $sym in $member ($arch) is NOT COMDAT, so it is a weak attribute and not C++ vague linkage"
                fi
                continue
                ;;
        esac
        bad "leg 4: weak symbol $sym in $member ($arch) is not on the allowlist"
    done < "$TMP/weak_arch"
done

tool_out "$TMP/tool" "$NM_DEF_RE" "$NM" --defined-only "$ELF"
awk '$(NF-1) ~ /^[wWvV]$/ { print $NF }' "$TMP/tool" > "$TMP/weak_elf"
# A fully linked image may legitimately carry no undefined symbol at all, so this one
# gets no landmark beyond nm having succeeded.
tool_out "$TMP/tool" '' "$NM" --undefined-only "$ELF"
awk '$(NF-1) == "w" { print $NF }' "$TMP/tool" >> "$TMP/weak_elf"
sort -u "$TMP/weak_elf" -o "$TMP/weak_elf"
while read -r sym; do
    [ -n "$sym" ] || continue
    allowed "$sym" && continue
    case "$sym" in
        _Z*|__Z*|DW.ref.*) continue ;;
    esac
    bad "leg 4: weak symbol $sym in the image is not on the allowlist"
done < "$TMP/weak_elf"

if [ "$rc" -eq 0 ]; then
    nfb=$(wc -l < "$TMP/fb_members" | tr -d ' ')
    nseam=$(wc -l < "$TMP/seams" | tr -d ' ')
    echo "seam_defaults: OK ($nfb fallback members, $nseam seams, $checked_fallback resolved from a fallback)"
fi
exit "$rc"
