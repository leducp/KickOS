#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Seam-fallback gate. KickOS resolves an optional arch/chip seam by ARCHIVE MEMBER
# EXTRACTION, not by __attribute__((weak)): the fallback body sits alone in a
# <symbol>_default.cc translation unit, and a backend that defines the symbol keeps that
# member out of the link. arch/CMakeLists.txt states the rule.
#
# Legs 1, 2 and 3 count STRONG definitions only. A COMDAT (vague-linkage) definition is
# emitted in EVERY translation unit that needs it out of line, so it can never be the
# undefined reference that extracts a member, and duplicates merge instead of colliding.
# Whether an inline function, template or implicit member lands out of line at all is an
# optimization-level and compiler-version artifact (-Os inlines kickos::arm::reg32 away,
# -O0 emits it), so counting one is a false positive. Leg 3 still catches extraction for
# the wrong symbol whatever its linkage, and leg 4 still requires every weak C++ symbol to
# BE COMDAT, so a deliberate weak attribute is not exempted anywhere.
#
# Four legs:
#   1. Each fallback member defines EXACTLY ONE strong global symbol, since a second drags
#      the member in unconditionally and collides with every backend; no other member of
#      the same archive defines that symbol; and no fallback sits in kickos_kernel.
#   2. For a seam a backend DOES define, the link resolved it from that backend's member
#      and no fallback member for it entered the image. This is the leg that catches a
#      backend definition placed in an archive member nothing anchors: such a board links
#      clean and SILENTLY DECLINES at runtime.
#   3. For a seam NO backend defines, the link resolved it from the fallback member. Keeps
#      a no-backend board from resolving somewhere unintended, and keeps leg 2 honest by
#      proving the fallback path is really exercised on this board.
#   4. Zero weak symbols outside tests/static/weak_allowlist.txt, and in an archive a C++
#      mangled weak symbol must additionally be COMDAT, so a deliberate weak attribute on a
#      C++ function still fails. Section groups are resolved away in the final ELF, so there
#      the mangled names are taken on trust and the archive leg is what covers our code.

set -u
# Every path arrives as an argument and is re-split unquoted below; a glob character in a
# build path must not expand against the cwd and inventory a different file.
set -f
. "$(dirname "$0")/../lib/gate.sh"

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
# semicolon-joined argument.
#
# A path that does not exist is not the same thing as a source with no definitions, and
# downstream the two are the same empty result, so it is a hard failure here.
ARCHIVES=""
OBJECTS=""
_oldifs=$IFS
IFS=';'
for _arg in "$@"; do
    for _in in $_arg; do
        [ -n "$_in" ] || continue
        [ -e "$_in" ] || fail "definition source does not exist: $_in"
        # ARCHIVES/OBJECTS below are space-joined lists that every inventory loop re-splits,
        # so a path with whitespace is refused here instead of being read as two paths.
        case "$_in" in
            *[[:space:]]*) fail "definition source path contains whitespace: $_in" ;;
        esac
        case "$_in" in
            *.a) ARCHIVES="$ARCHIVES $_in" ;;
            *)   OBJECTS="$OBJECTS $_in" ;;
        esac
    done
done
IFS=$_oldifs

for f in "$ELF" "$MAP" "$ALLOWLIST"; do
    if [ ! -r "$f" ]; then
        fail "cannot read $f"
    fi
done

scratch_dir
rc=0

# Every leg prints one finding per line and the caller turns each into a `bad`. NOT a
# pipeline: `bad` sets rc, and a pipeline would set it in a subshell and lose it.
collect() { # <findings file>
    while IFS= read -r _finding; do
        [ -n "$_finding" ] || continue
        bad "$_finding"
    done < "$1"
}

# `grep -c .` exits 1 on zero matches, so counting that way makes the count depend on this
# script never using `set -e` and dies before the diagnostic it feeds.
nlines() { # <newline-joined list>
    if [ -z "$1" ]; then
        echo 0
        return
    fi
    printf '%s\n' "$1" | wc -l | tr -d ' '
}

# nm prints "<addr> <type> <symbol>", prefixed "<archive>:<member>:" with -A. A source
# of definitions with no GLOBAL definition at all is not one of ours, so requiring one
# is a positive control on nm itself.
NM_DEF_RE='^[0-9a-fA-F]+ [A-Z] '
NM_ARCHIVE_DEF_RE=':[0-9a-fA-F]+ [A-Z] '
# readelf -sW numbers every symbol-table row; a file it could not read has no row at all.
READELF_SYM_RE='^ *[0-9]+: '

# --- the rule, as parameters the self-test and the scan share -----------------
# EVERY ERE HERE USES BRACKET EXPRESSIONS AND NEVER A BACKSLASH ESCAPE: awk processes
# escape sequences in a -v value, so a `\.` would reach the matcher as a bare dot and a
# `\(` as a group opener.
#
# A local symbol carries a lowercase type, and of the uppercase ones only a real
# definition can take part in cross-member resolution.
DEF_TYPE_RE='^[A-Z]$'
DEF_SKIP_TYPES='N U'
# In readelf -SW a section header line is "[<nr>] <name> <type> <addr> <off> <size> <es>
# <flg> <lk> <inf> <al>", so the flags are the field before the last three. A section with
# NO flags prints an empty column and shifts those fields left onto the hex entry size,
# which cannot contain a G.
COMDAT_FLAG_OFF=3
# Only a GLOBAL or WEAK definition with a real section index can participate in
# cross-member resolution, which is the same set the nm inventory keeps.
COMDAT_BIND_RE='^(GLOBAL|WEAK)$'
FB_MEMBER_RE='_default[.]cc[.]o(bj)?$'
# Only an inclusion entry puts an `<archive>(<member>)` at column 0.
INCLUDE_RE='^[^[:blank:]].*[.]a[(].*[)]$'
MANGLED_RE='^_?_Z'
ELF_WEAK_SKIP_RE='^_?_Z|^DW[.]ref[.]'
KERNEL_ARCHIVE_GLOB='*libkickos_kernel.a'
# The RX psABI underscore is in the symbol table but not in what ld prints as the reason a
# member was extracted.
PSABI_PREFIX='_'

# --- the parsers, one per input format ----------------------------------------
# `nm -A` on an archive prints "<path>:<member>:<addr> <type> <symbol>".
nm_archive_defs() { # <nm -A output> <archive key> <type ere> <skip types>
    awk -v A="$2" -v TYPE="$3" -v SKIP="$4" '
        BEGIN { n = split(SKIP, s, " "); for (i = 1; i <= n; i++) { skip[s[i]] = 1 } }
        {
            split($1, p, ":")
            if (p[2] == "") { next }
            t = $(NF - 1)
            if (t !~ TYPE) { next }
            if (t in skip) { next }
            print A "\t" p[2] "\t" t "\t" $NF
        }' "$1"
}

nm_object_defs() { # <nm output> <object basename> <type ere> <skip types>
    awk -v O="$2" -v TYPE="$3" -v SKIP="$4" '
        BEGIN { n = split(SKIP, s, " "); for (i = 1; i <= n; i++) { skip[s[i]] = 1 } }
        {
            t = $(NF - 1)
            if (t !~ TYPE) { next }
            if (t in skip) { next }
            print "-\t" O "\t" t "\t" $NF
        }' "$1"
}

# A C++ vague-linkage definition lives in a section carrying readelf's G (SHF_GROUP) flag.
comdat_section_rows() { # <readelf -SW output> <archive key> <member when not an archive> <flag offset>
    awk -v A="$2" -v M0="$3" -v FOFF="$4" '
        BEGIN { m = M0 }
        /^File:/ {
            m = $2
            sub(/^.*\(/, "", m); sub(/\)$/, "", m)
            next
        }
        /^ *\[ *[0-9]+\]/ {
            line = $0
            sub(/^ *\[ */, "", line)
            if ($(NF - FOFF) ~ /G/) { print A "\t" m "\t" (line + 0) }
        }' "$1"
}

comdat_symbol_rows() { # <readelf -sW output> <archive key> <member when not an archive> <section rows> <bind ere>
    awk -v A="$2" -v M0="$3" -v SEC="$4" -v BIND="$5" '
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
        $5 ~ BIND && $7 ~ /^[0-9]+$/ {
            if ((m SUBSEP ($7 + 0)) in g) { print A "\t" m "\t" $8 }
        }' "$1"
}

# defs carries "<archive> <member> <nm-type> <symbol>"; the COMDAT key is columns 1, 2, 4.
strong_only() { # <defs> <comdat symbol rows>
    awk -F'\t' -v C="$2" '
        BEGIN { while ((getline line < C) > 0) { g[line] = 1 } }
        !((($1 "\t" $2 "\t" $4)) in g)' "$1"
}

# The map opens with the archive-member inclusion list:
#
#   arch/libkickos_arch_armv7m.a(arch_pinmux_set_default.cc.obj)
#                                 kernel/libkickos_kernel.a(syscall.cc.obj) (arch_pinmux_set)
#
# Selected structurally, never by the block's heading: the headings are translated by the
# host linker's locale, and the per-function section names the rest of the map would offer
# do not exist in the hosted (sim) build, which compiles without -ffunction-sections.
#
# An entry is flushed with an EMPTY reason when the next line is another entry or the file
# ends, so a member ld listed as included can never be missing from this table.
#
# THE REASON IS THE LINE'S LAST FIELD, so one carrying a space reads as empty and leg 3 then
# reports the member as satisfying '' instead of naming what it satisfied. That is loud and
# not silent, and every seam symbol is `extern "C"` (tests/static/check_extern_c_linkage.sh),
# so a C identifier never carries one. Reading the whole trailing parenthesised group instead
# would still not survive a nested paren, which is what a demangled name brings.
map_included() { # <map> <inclusion ere>
    awk -v INC="$2" '
        $0 ~ INC {
            if (pend != "") { print pend "\t" }
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
        }
        END { if (pend != "") { print pend "\t" } }' "$1" | sort -u
}

weak_rows() { # <readelf -sW output> <archive key>
    awk -v A="$2" '
        /^File:/ {
            m = $2
            sub(/^.*\(/, "", m); sub(/\)$/, "", m)
            next
        }
        $5 == "WEAK" { print A "\t" m "\t" $7 "\t" $8 }' "$1"
}

elf_weak_defs() { # <nm --defined-only output>
    awk '$(NF - 1) ~ /^[wWvV]$/ { print $NF }' "$1"
}

elf_weak_undefs() { # <nm --undefined-only output>
    awk '$(NF - 1) == "w" { print $NF }' "$1"
}

# --- the legs, each printing one finding per line ------------------------------
leg1_findings() { # <fb defs> <strong defs> <fb members> <kernel archive glob>
    _l1_fb="$1"; _l1_strong="$2"; _l1_glob="$4"
    while IFS="$TAB" read -r arch member; do
        [ -n "$member" ] || continue
        # kickos_kernel is scanned BEFORE kickos_chip_${KICKOS_CHIP} in the rescan group, so a fallback
        # placed there is extracted in the pass that first makes the symbol undefined and then
        # collides with the chip's definition. It belongs in the arch library.
        case "$arch" in
            $_l1_glob)
                printf 'leg 1: %s is in kickos_kernel; a fallback must sit in an archive the link scans AFTER the chip archive\n' "$member"
                ;;
        esac
        syms=$(awk -F'\t' -v a="$arch" -v m="$member" '$1 == a && $2 == m { print $4 }' "$_l1_fb" | sort -u)
        n=$(nlines "$syms")
        if [ "$n" -ne 1 ]; then
            printf 'leg 1: %s defines %s global symbols, expected exactly 1: %s\n' "$member" "$n" "$(echo $syms)"
            continue
        fi
        # Same archive, same symbol, another member: resolution would fall to member order.
        twin=$(awk -F'\t' -v a="$arch" -v m="$member" -v s="$syms" \
            '$1 == a && $2 != m && $4 == s { print $2 }' "$_l1_strong" | sort -u)
        if [ -n "$twin" ]; then
            printf 'leg 1: %s is defined by both %s and %s inside %s\n' "$syms" "$member" "$(echo $twin)" "$arch"
        fi
    done < "$3"
}

legs23_findings() { # <fb defs> <be defs> <seams> <included> <cmdline members> <map> <psabi prefix> <resolved out>
    _l23_fb="$1"; _l23_be="$2"; _l23_inc="$4"; _l23_cmd="$5"; _l23_map="$6"; _l23_pfx="$7"
    _l23_resolved=0
    while read -r sym; do
        [ -n "$sym" ] || continue
        fbmember=$(awk -F'\t' -v s="$sym" '$4 == s { print $2 }' "$_l23_fb" | sort -u)
        backends=$(awk -F'\t' -v s="$sym" '$4 == s { print $2 }' "$_l23_be" | sort -u)
        nbackends=$(nlines "$backends")
        if [ "$nbackends" -gt 1 ]; then
            printf 'leg 2: %s has %s backend definitions (%s); which one this link resolves to is member order\n' \
                "$sym" "$nbackends" "$(echo $backends)"
            continue
        fi
        in_link=$(awk -F'\t' -v m="$fbmember" '$1 == m { print "yes" }' "$_l23_inc" | head -1)
        if [ "$nbackends" -eq 1 ]; then
            backend_linked=""
            if grep -qxF "$backends" "$_l23_cmd"; then
                backend_linked=yes
            elif awk -F'\t' -v m="$backends" '$1 == m { print "yes" }' "$_l23_inc" | grep -q yes; then
                backend_linked=yes
            fi
            # A backend owns this seam, so its fallback member must not be in the image at
            # all. If it is, the backend's own member was not anchored, the fallback answered
            # the reference first, and the board SILENTLY DECLINES at runtime.
            if [ -n "$in_link" ]; then
                printf 'leg 2: %s entered the link although %s defines %s; that backend member is not anchored, so this board silently declines at runtime\n' \
                    "$fbmember" "$backends" "$sym"
                continue
            fi
            if [ -z "$backend_linked" ]; then
                printf 'leg 2: %s defines %s but never entered the link, and its fallback did not either\n' \
                    "$backends" "$sym"
            fi
            # The map must not mention the fallback member anywhere, not even as a discarded
            # section: an unextracted member cannot appear at all.
            if grep -qF "($fbmember)" "$_l23_map"; then
                printf 'leg 2: %s appears in the link map although %s owns %s\n' \
                    "$fbmember" "$backends" "$sym"
            fi
            continue
        fi
        # No backend at all: the fallback must be what answered the reference. A link that
        # succeeded with the fallback ABSENT proves nothing in the image referenced the seam
        # (an unresolved arch_* symbol is a link error), so that case has nothing to assert.
        if [ -z "$in_link" ]; then
            continue
        fi
        why=$(awk -F'\t' -v m="$fbmember" '$1 == m { print $2 }' "$_l23_inc" | head -1)
        if [ "$why" != "$sym" ] && [ "$_l23_pfx$why" != "$sym" ]; then
            printf "leg 3: %s entered the link to satisfy '%s', not %s\n" "$fbmember" "$why" "$sym"
            continue
        fi
        _l23_resolved=$((_l23_resolved + 1))
    done < "$3"
    printf '%s\n' "$_l23_resolved" > "$8"
}

# A non-numeric index (UND, ABS, COM) must not reach the COMDAT lookup: the arithmetic
# there would read it as a variable name and compare against section 0.
leg4_archive_findings() { # <weak rows> <allowlist> <section rows> <mangled ere>
    awk -F'\t' -v ALLOW="$2" -v SEC="$3" -v MANGLED="$4" '
        BEGIN {
            while ((getline s < ALLOW) > 0) { allow[s] = 1 }
            while ((getline s < SEC) > 0) { sec[s] = 1 }
        }
        {
            arch = $1; member = $2; ndx = $3; sym = $4
            if (sym == "") { next }
            if (sym in allow) { next }
            if (sym !~ MANGLED) {
                printf "leg 4: weak symbol %s in %s (%s) is not on the allowlist\n", sym, member, arch
                next
            }
            if (ndx == "UND") {
                printf "leg 4: weak undefined C++ reference %s in %s (%s)\n", sym, member, arch
                next
            }
            if (ndx !~ /^[0-9]+$/) {
                printf "leg 4: weak %s in %s (%s) has section index %s, which cannot be a COMDAT group\n", sym, member, arch, ndx
                next
            }
            if (!((arch "\t" member "\t" (ndx + 0)) in sec)) {
                printf "leg 4: weak %s in %s (%s) is NOT COMDAT, so it is a weak attribute and not C++ vague linkage\n", sym, member, arch
            }
        }' "$1"
}

leg4_elf_findings() { # <weak symbol list> <allowlist> <skip ere>
    awk -v ALLOW="$2" -v SKIP="$3" '
        BEGIN { while ((getline s < ALLOW) > 0) { allow[s] = 1 } }
        {
            sym = $1
            if (sym == "") { next }
            if (sym in allow) { next }
            if (sym ~ SKIP) { next }
            printf "leg 4: weak symbol %s in the image is not on the allowlist\n", sym
        }' "$1"
}

# --- self-test: prove every clause of the rule, one control per clause ---------
# Each control is a MINIMAL PAIR and every expected count is exact, so one an unrelated
# clause catches shows up as the wrong number. The tab-separated tables are written with `|`
# and translated, so no literal tab sits in this file for an edit to lose.
st_count() { # <file>
    wc -l < "$1" | tr -d ' '
}
st_alone() { # <corpus> <expected per-line count> <scanner> <its arguments after the file>
    _st_corpus="$1"; _st_want="$2"; _st_fn="$3"; shift 3
    _st_i=0
    while IFS= read -r _st_line; do
        _st_i=$((_st_i + 1))
        printf '%s\n' "$_st_line" > "$TMP/st_one"
        _st_n="$("$_st_fn" "$TMP/st_one" "$@" | wc -l | tr -d ' ')"
        [ "$_st_n" -eq "$_st_want" ] \
            || fail "control $_st_i of $_st_corpus reports $_st_n, expected $_st_want: $_st_line"
    done < "$_st_corpus"
    printf '%s\n' "$_st_i"
}
ST_NEVER='KICKOS_THIS_ERE_MATCHES_NOTHING'

# --- clause: the archive nm inventory keeps uppercase types only, minus N and U
cat > "$TMP/st_nmarch_pos" <<'EOF'
/p/lib.a:m1.cc.obj:00000000 T strong_text
/p/lib.a:m1.cc.obj:00000000 D strong_data
/p/lib.a:m2.cc.obj:00000000 W weak_def
EOF
cat > "$TMP/st_nmarch_neg" <<'EOF'
/p/lib.a:m1.cc.obj:00000000 t local_text
/p/lib.a:m1.cc.obj:00000000 N debug_only
/p/lib.a:m1.cc.obj:         U undefined_ref
00000000 T no_member_prefix
EOF
cat "$TMP/st_nmarch_pos" "$TMP/st_nmarch_neg" > "$TMP/st_nmarch"
nm_archive_defs "$TMP/st_nmarch" /p/lib.a "$DEF_TYPE_RE" "$DEF_SKIP_TYPES" > "$TMP/st_out"
ST_N="$(st_count "$TMP/st_out")"
[ "$ST_N" -eq 3 ] \
    || fail "the archive inventory kept $ST_N of 3 planted definitions; it would miss real ones"
# The MEMBER comes off the nm prefix, not off the archive path: legs 1 and 2 key on it.
grep -qxF "/p/lib.a${TAB}m1.cc.obj${TAB}T${TAB}strong_text" "$TMP/st_out" \
    || fail "the archive inventory does not key a definition on its nm member prefix"
ST_I="$(st_alone "$TMP/st_nmarch_neg" 0 nm_archive_defs /p/lib.a "$DEF_TYPE_RE" "$DEF_SKIP_TYPES")"
[ "$ST_I" -eq 4 ] || fail "$ST_I archive-inventory negative control(s) ran, expected 4"
# Disabled = a clause that cannot reject.
ST_N="$(nm_archive_defs "$TMP/st_nmarch" /p/lib.a '.' "$DEF_SKIP_TYPES" | wc -l | tr -d ' ')"
[ "$ST_N" -eq 4 ] \
    || fail "with the uppercase-type clause disabled the archive inventory kept $ST_N, expected 4;
      the local-symbol control is not a near miss and proves nothing"
ST_N="$(nm_archive_defs "$TMP/st_nmarch" /p/lib.a "$DEF_TYPE_RE" '' | wc -l | tr -d ' ')"
[ "$ST_N" -eq 5 ] \
    || fail "with the N/U clause disabled the archive inventory kept $ST_N, expected 5;
      the debug and undefined controls are not near misses and prove nothing"

# --- clause: the object nm inventory, keyed `-` so legs 2 and 3 read it as command line
cat > "$TMP/st_nmobj_pos" <<'EOF'
00000000 T obj_strong
00000000 V obj_vague
EOF
cat > "$TMP/st_nmobj_neg" <<'EOF'
00000000 t obj_local
00000000 N obj_debug
         U obj_undef
EOF
cat "$TMP/st_nmobj_pos" "$TMP/st_nmobj_neg" > "$TMP/st_nmobj"
nm_object_defs "$TMP/st_nmobj" main.cc.obj "$DEF_TYPE_RE" "$DEF_SKIP_TYPES" > "$TMP/st_out"
ST_N="$(st_count "$TMP/st_out")"
[ "$ST_N" -eq 2 ] \
    || fail "the object inventory kept $ST_N of 2 planted definitions; a seam the app defines would read as backend-less"
grep -qxF -- "-${TAB}main.cc.obj${TAB}T${TAB}obj_strong" "$TMP/st_out" \
    || fail "the object inventory does not key a link-line definition apart from an archive's"
ST_I="$(st_alone "$TMP/st_nmobj_neg" 0 nm_object_defs main.cc.obj "$DEF_TYPE_RE" "$DEF_SKIP_TYPES")"
[ "$ST_I" -eq 3 ] || fail "$ST_I object-inventory negative control(s) ran, expected 3"
ST_N="$(nm_object_defs "$TMP/st_nmobj" main.cc.obj '.' "$DEF_SKIP_TYPES" | wc -l | tr -d ' ')"
[ "$ST_N" -eq 3 ] \
    || fail "with the uppercase-type clause disabled the object inventory kept $ST_N, expected 3"

# --- clause: the COMDAT flag column, and the flagless line that shifts onto the entry size
# `.group` and a section whose NAME holds a G are the near misses: both must stay silent
# with the flags read at the right offset, and both are what a whole-line scan would take.
cat > "$TMP/st_resec_neg" <<'EOF'
  [ 0]                   NULL            00000000 000000 000000 00      0   0  0
  [ 1] .group            GROUP           00000000 000034 00000c 04     69  82  4
  [ 2] .text             PROGBITS        00000000 000050 000000 00  AX  0   0  2
  [ 6] .text.GROUP_NAME  PROGBITS        00000000 000070 000010 00      0   0  4
  [ 8] .bss              NOBITS          00000000 000080 000004 00  WA  0   0  1
EOF
cat > "$TMP/st_resec" <<'EOF'
File: /p/lib.a(m1.cc.obj)
  [ 0]                   NULL            00000000 000000 000000 00      0   0  0
  [ 1] .group            GROUP           00000000 000034 00000c 04     69  82  4
  [ 2] .text             PROGBITS        00000000 000050 000000 00  AX  0   0  2
  [ 3] .text._ZN1fEv     PROGBITS        00000000 000050 00000c 00 AXG  0   0  2
  [ 4] .rel.text._ZN1fEv REL             00000000 00c93c 000008 08  IG 69   3  4
  [ 6] .text.GROUP_NAME  PROGBITS        00000000 000070 000010 00      0   0  4
File: /p/lib.a(m2.cc.obj)
  [ 7] .text._ZN1gEv     PROGBITS        00000000 000060 00000c 00 AXG  0   0  2
  [ 8] .bss              NOBITS          00000000 000080 000004 00  WA  0   0  1
EOF
comdat_section_rows "$TMP/st_resec" /p/lib.a '' "$COMDAT_FLAG_OFF" > "$TMP/st_sec"
ST_N="$(st_count "$TMP/st_sec")"
[ "$ST_N" -eq 3 ] \
    || fail "the section scan found $ST_N of 3 planted group sections; every C++ weak symbol would read as non-COMDAT"
# The File: marker switches the member, or every section is attributed to the first one.
grep -qxF "/p/lib.a${TAB}m2.cc.obj${TAB}7" "$TMP/st_sec" \
    || fail "the section scan does not follow the File: member marker"
ST_I="$(st_alone "$TMP/st_resec_neg" 0 comdat_section_rows /p/lib.a m0.cc.obj "$COMDAT_FLAG_OFF")"
[ "$ST_I" -eq 5 ] || fail "$ST_I section-scan negative control(s) ran, expected 5"
# One field over reads the link index, and nothing there can hold a flag.
ST_N="$(comdat_section_rows "$TMP/st_resec" /p/lib.a '' 2 | wc -l | tr -d ' ')"
[ "$ST_N" -eq 0 ] || fail "with the flags read one field later the section scan found $ST_N, expected 0"
# Three of these lines hold a G outside the flag column: a section TYPE spelling PROGBITS, a
# group's own header spelling GROUP, and a section NAME carrying one.
ST_N="$(awk '/^ *\[ *[0-9]+\]/ && /G/' "$TMP/st_resec" | wc -l | tr -d ' ')"
[ "$ST_N" -eq 6 ] \
    || fail "a whole-line scan for the flag letter reports $ST_N of these lines, expected 6;
      the corpus does not discriminate the flag column from a G elsewhere on the line"
# An object has no File: line, so its member name is the one handed in.
printf '  [ 3] .text._ZN1fEv     PROGBITS        00000000 000050 00000c 00 AXG  0   0  2\n' > "$TMP/st_one"
comdat_section_rows "$TMP/st_one" - main.cc.obj "$COMDAT_FLAG_OFF" > "$TMP/st_out"
grep -qxF -- "-${TAB}main.cc.obj${TAB}3" "$TMP/st_out" \
    || fail "an object's group section is not attributed to the member name handed in"

# --- clause: a COMDAT symbol is GLOBAL or WEAK, in a group section, in ITS OWN archive
cat > "$TMP/st_resym_neg" <<'EOF'
    11: 00000001   304 FUNC    GLOBAL DEFAULT    2 plain_text
    12: 00000000     0 NOTYPE  GLOBAL DEFAULT  UND some_ref
    13: 00000000     0 NOTYPE  LOCAL  DEFAULT    3 local_in_group
EOF
cat > "$TMP/st_resym" <<'EOF'
File: /p/lib.a(m1.cc.obj)
    10: 00000001    12 FUNC    WEAK   DEFAULT    3 _ZN1fEv
    11: 00000001   304 FUNC    GLOBAL DEFAULT    2 plain_text
    12: 00000000     0 NOTYPE  GLOBAL DEFAULT  UND some_ref
    13: 00000000     0 NOTYPE  LOCAL  DEFAULT    3 local_in_group
File: /p/lib.a(m2.cc.obj)
    14: 00000001    12 FUNC    WEAK   DEFAULT    7 _ZN1gEv
EOF
ST_N="$(comdat_symbol_rows "$TMP/st_resym" /p/lib.a '' "$TMP/st_sec" "$COMDAT_BIND_RE" | wc -l | tr -d ' ')"
[ "$ST_N" -eq 2 ] \
    || fail "the COMDAT symbol scan found $ST_N of 2 planted vague-linkage definitions"
ST_I="$(st_alone "$TMP/st_resym_neg" 0 comdat_symbol_rows /p/lib.a m1.cc.obj "$TMP/st_sec" "$COMDAT_BIND_RE")"
[ "$ST_I" -eq 3 ] || fail "$ST_I COMDAT-symbol negative control(s) ran, expected 3"
# local_in_group sits in a REAL group section, so only the binding clause keeps it out.
ST_N="$(comdat_symbol_rows "$TMP/st_resym" /p/lib.a '' "$TMP/st_sec" '.' | wc -l | tr -d ' ')"
[ "$ST_N" -eq 3 ] \
    || fail "with the GLOBAL/WEAK clause disabled the COMDAT symbol scan found $ST_N, expected 3;
      the local-in-a-group control is not a near miss and proves nothing"
# The group table is keyed per archive, so another archive's sections must not match.
ST_N="$(comdat_symbol_rows "$TMP/st_resym" /p/other.a '' "$TMP/st_sec" "$COMDAT_BIND_RE" | wc -l | tr -d ' ')"
[ "$ST_N" -eq 0 ] \
    || fail "the COMDAT symbol scan matched $ST_N section(s) of a DIFFERENT archive, expected 0"

# --- clause: the strong-definition subtraction is keyed on archive, member AND symbol
tr '|' "$TAB" > "$TMP/st_defs" <<'EOF'
A1|m1.cc.obj|W|_ZN1fEv
A1|m2.cc.obj|W|_ZN1fEv
A1|m1.cc.obj|T|plain
EOF
tr '|' "$TAB" > "$TMP/st_comdat" <<'EOF'
A1|m1.cc.obj|_ZN1fEv
EOF
strong_only "$TMP/st_defs" "$TMP/st_comdat" > "$TMP/st_out"
ST_N="$(st_count "$TMP/st_out")"
[ "$ST_N" -eq 2 ] || fail "the COMDAT subtraction left $ST_N of 3 definitions, expected 2"
# The same symbol in ANOTHER member is not the same definition, and dropping it would hide
# every duplicate leg 1 exists to find.
grep -qxF "A1${TAB}m2.cc.obj${TAB}W${TAB}_ZN1fEv" "$TMP/st_out" \
    || fail "the COMDAT subtraction dropped a same-symbol definition from a DIFFERENT member"
ST_N="$(strong_only "$TMP/st_defs" /dev/null | wc -l | tr -d ' ')"
[ "$ST_N" -eq 3 ] || fail "with no COMDAT table the subtraction left $ST_N, expected 3"

# --- clause: the fallback/backend split, on both object suffixes
tr '|' "$TAB" > "$TMP/st_strong" <<'EOF'
A|arch_shutdown_default.cc.obj|T|arch_shutdown
A|kfault_terminate_default.cc.o|T|kfault_terminate
A|arch_shutdown_default.cc.obj.x|T|not_fb
A|defaults.cc.obj|T|not_fb2
A|chip.cc.obj|T|backend_sym
EOF
ST_N="$(awk -F'\t' -v RE="$FB_MEMBER_RE" '$2 ~ RE' "$TMP/st_strong" | wc -l | tr -d ' ')"
[ "$ST_N" -eq 2 ] \
    || fail "the fallback split matched $ST_N of 2 planted fallback members; legs 1 to 3 would go vacuous"
ST_N="$(awk -F'\t' -v RE="$FB_MEMBER_RE" '$2 !~ RE' "$TMP/st_strong" | wc -l | tr -d ' ')"
[ "$ST_N" -eq 3 ] || fail "the fallback split left $ST_N of 3 planted backend members"
# Without the `.obj` spelling every fallback this toolchain emits reads as a backend.
ST_N="$(awk -F'\t' -v RE='_default[.]cc[.]o$' '$2 ~ RE' "$TMP/st_strong" | wc -l | tr -d ' ')"
[ "$ST_N" -eq 1 ] \
    || fail "with the .obj suffix dropped the fallback split matched $ST_N, expected 1"

# --- clause: the map's inclusion entries, at column 0, with the reason on the next line
cat > "$TMP/st_map" <<'EOF'
Archive member included to satisfy reference by file (symbol)

arch/liba.a(arch_shutdown_default.cc.obj)
                              arch/libc.a(chip.cc.obj) (arch_shutdown)
arch/liba.a(switch.S.obj)
                              (PendSV_Handler)
arch/liba.a(adjacent_first.cc.obj)
arch/liba.a(adjacent_second.cc.obj)
                              other.o (pulled_second)
lib/libb.a(cpp_name.cc.obj)
                              main.cc.obj (kickos::f(int, long))

Linker script and memory map

 .text          0x00000000  0x10 arch/liba.a(arch_armv7m.cc.obj)
EOF
map_included "$TMP/st_map" "$INCLUDE_RE" > "$TMP/st_inc"
ST_N="$(st_count "$TMP/st_inc")"
[ "$ST_N" -eq 5 ] || fail "the map scan read $ST_N of 5 planted inclusion entries"
grep -qxF "arch_shutdown_default.cc.obj${TAB}arch_shutdown" "$TMP/st_inc" \
    || fail "the map scan does not take the extraction reason off the line below the entry"
grep -qxF "switch.S.obj${TAB}PendSV_Handler" "$TMP/st_inc" \
    || fail "the map scan cannot read a reason line that names no referencing file"
# An entry followed by another entry still has to reach the table: leg 2 reads MEMBERSHIP,
# so a dropped member is a finding that never fires.
grep -q "^adjacent_first.cc.obj${TAB}" "$TMP/st_inc" \
    || fail "the map scan dropped an inclusion entry that another entry followed"
# A placement line inside the memory map is indented and is not an inclusion.
grep -q '^arch_armv7m.cc.obj' "$TMP/st_inc" \
    && fail "the map scan read an indented memory-map placement as an inclusion entry"
ST_N="$(map_included "$TMP/st_map" '.*[.]a[(].*[)]$' | wc -l | tr -d ' ')"
[ "$ST_N" -eq 7 ] \
    || fail "with the column-0 anchor dropped the map scan read $ST_N entries, expected 7;
      the indented reason line and the memory-map placement are not near misses"

# --- clause: leg 1, one strong symbol per fallback member, unique in its archive
tr '|' "$TAB" > "$TMP/st_fb1" <<'EOF'
/p/libkickos_arch.a|two_syms_default.cc.obj|T|sym_a
/p/libkickos_arch.a|two_syms_default.cc.obj|T|sym_b
/p/libkickos_kernel.a|in_kernel_default.cc.obj|T|sym_k
/p/libkickos_arch.a|twinned_default.cc.obj|T|sym_t
/p/libkickos_arch.a|clean_default.cc.obj|T|sym_c
EOF
tr '|' "$TAB" > "$TMP/st_sd1" <<'EOF'
/p/libkickos_arch.a|two_syms_default.cc.obj|T|sym_a
/p/libkickos_arch.a|two_syms_default.cc.obj|T|sym_b
/p/libkickos_kernel.a|in_kernel_default.cc.obj|T|sym_k
/p/libkickos_arch.a|twinned_default.cc.obj|T|sym_t
/p/libkickos_arch.a|clean_default.cc.obj|T|sym_c
/p/libkickos_arch.a|twin_holder.cc.obj|T|sym_t
EOF
awk -F'\t' '{ print $1 "\t" $2 }' "$TMP/st_fb1" | sort -u > "$TMP/st_fbm1"
leg1_findings "$TMP/st_fb1" "$TMP/st_sd1" "$TMP/st_fbm1" "$KERNEL_ARCHIVE_GLOB" > "$TMP/st_out"
ST_N="$(st_count "$TMP/st_out")"
[ "$ST_N" -eq 3 ] || fail "leg 1 reported $ST_N of 3 planted violations; it would miss real ones"
# Read one by one, or three findings from one member cannot be told from one each.
grep -q 'two_syms_default.cc.obj defines 2 global symbols' "$TMP/st_out" \
    || fail "leg 1 does not report a fallback member with a second strong symbol"
grep -q 'in_kernel_default.cc.obj is in kickos_kernel' "$TMP/st_out" \
    || fail "leg 1 does not report a fallback member in the kernel archive"
grep -q 'sym_t is defined by both twinned_default.cc.obj and twin_holder.cc.obj' "$TMP/st_out" \
    || fail "leg 1 does not report a fallback symbol twinned inside its own archive"
grep -q 'clean_default.cc.obj' "$TMP/st_out" \
    && fail "leg 1 reports a fallback member that defines exactly one untwinned symbol"
# The kernel member is otherwise clean, so only the archive clause keeps it reported.
ST_N="$(leg1_findings "$TMP/st_fb1" "$TMP/st_sd1" "$TMP/st_fbm1" "$ST_NEVER" | wc -l | tr -d ' ')"
[ "$ST_N" -eq 2 ] \
    || fail "with the kernel-archive clause disabled leg 1 reported $ST_N, expected 2;
      the in-kernel control is not a near miss and proves nothing"

# --- clause: legs 2 and 3, per seam symbol
tr '|' "$TAB" > "$TMP/st_fb2" <<'EOF'
A|multi_default.cc.obj|T|seam_multi
A|unanchored_default.cc.obj|T|seam_unanchored
A|absent_default.cc.obj|T|seam_absent
A|wrongwhy_default.cc.obj|T|seam_wrongwhy
A|rxwhy_default.cc.obj|T|_seam_rxwhy
A|good_default.cc.obj|T|seam_good
A|mapleak_default.cc.obj|T|seam_mapleak
EOF
tr '|' "$TAB" > "$TMP/st_be2" <<'EOF'
A|be_one.cc.obj|T|seam_multi
A|be_two.cc.obj|T|seam_multi
A|be_unanchored.cc.obj|T|seam_unanchored
A|be_absent.cc.obj|T|seam_absent
A|be_mapleak.cc.obj|T|seam_mapleak
EOF
tr '|' "$TAB" > "$TMP/st_inc2" <<'EOF'
unanchored_default.cc.obj|seam_unanchored
wrongwhy_default.cc.obj|other_symbol
rxwhy_default.cc.obj|seam_rxwhy
good_default.cc.obj|seam_good
EOF
printf 'main.cc.obj\nbe_mapleak.cc.obj\n' > "$TMP/st_cmd2"
printf 'A(mapleak_default.cc.obj)\n' > "$TMP/st_map2"
awk -F'\t' '{ print $4 }' "$TMP/st_fb2" | sort -u > "$TMP/st_seams2"
legs23_findings "$TMP/st_fb2" "$TMP/st_be2" "$TMP/st_seams2" "$TMP/st_inc2" \
    "$TMP/st_cmd2" "$TMP/st_map2" "$PSABI_PREFIX" "$TMP/st_res2" > "$TMP/st_out"
ST_N="$(st_count "$TMP/st_out")"
[ "$ST_N" -eq 5 ] || fail "legs 2 and 3 reported $ST_N of 5 planted violations"
[ "$(cat "$TMP/st_res2")" -eq 2 ] \
    || fail "legs 2 and 3 resolved $(cat "$TMP/st_res2") of 2 planted fallback seams; leg 3 would be vacuous"
grep -q 'seam_multi has 2 backend definitions' "$TMP/st_out" \
    || fail "leg 2 does not report a seam with two backend definitions"
grep -q 'unanchored_default.cc.obj entered the link although be_unanchored.cc.obj defines seam_unanchored' "$TMP/st_out" \
    || fail "leg 2 does not report a fallback that answered a seam a backend owns"
grep -q 'be_absent.cc.obj defines seam_absent but never entered the link' "$TMP/st_out" \
    || fail "leg 2 does not report a backend that never entered the link"
grep -q 'mapleak_default.cc.obj appears in the link map although be_mapleak.cc.obj owns seam_mapleak' "$TMP/st_out" \
    || fail "leg 2 does not report a fallback member the map mentions while a backend owns the seam"
grep -q "wrongwhy_default.cc.obj entered the link to satisfy 'other_symbol', not seam_wrongwhy" "$TMP/st_out" \
    || fail "leg 3 does not report a fallback extracted for a different symbol"
# The map clause, as a minimal pair on the map alone: same tables, an empty map.
ST_N="$(legs23_findings "$TMP/st_fb2" "$TMP/st_be2" "$TMP/st_seams2" "$TMP/st_inc2" \
    "$TMP/st_cmd2" /dev/null "$PSABI_PREFIX" "$TMP/st_res2" | wc -l | tr -d ' ')"
[ "$ST_N" -eq 4 ] \
    || fail "with the fallback member absent from the map legs 2 and 3 reported $ST_N, expected 4;
      the map-mention control is not a near miss and proves nothing"
# The RX underscore tolerance, as a mutation: without it the rx seam reports instead of
# resolving, so the finding count and the resolved count move together.
ST_N="$(legs23_findings "$TMP/st_fb2" "$TMP/st_be2" "$TMP/st_seams2" "$TMP/st_inc2" \
    "$TMP/st_cmd2" "$TMP/st_map2" '' "$TMP/st_res2" | wc -l | tr -d ' ')"
[ "$ST_N" -eq 6 ] \
    || fail "with the psABI underscore clause disabled legs 2 and 3 reported $ST_N, expected 6"
[ "$(cat "$TMP/st_res2")" -eq 1 ] \
    || fail "with the psABI underscore clause disabled $(cat "$TMP/st_res2") seam(s) resolved, expected 1"

# --- clause: leg 4, the allowlist, the mangled test, the section index, COMDAT membership
tr '|' "$TAB" > "$TMP/st_w4" <<'EOF'
A1|m1.cc.obj|3|_ZN1fEv
A1|m1.cc.obj|5|_ZN1hEv
A1|m1.cc.obj|UND|_ZN1iEv
A1|m1.cc.obj|ABS|_ZN1jEv
A1|m1.cc.obj|7|plain_weak
A1|m1.cc.obj|9|allowed_weak
EOF
printf 'allowed_weak\n' > "$TMP/st_allow4"
tr '|' "$TAB" > "$TMP/st_sec4" <<'EOF'
A1|m1.cc.obj|3
EOF
leg4_archive_findings "$TMP/st_w4" "$TMP/st_allow4" "$TMP/st_sec4" "$MANGLED_RE" > "$TMP/st_out"
ST_N="$(st_count "$TMP/st_out")"
[ "$ST_N" -eq 4 ] || fail "leg 4 reported $ST_N of 4 planted weak violations"
# One message per clause, so a control silent for the WRONG reason is visible.
grep -q '_ZN1hEv in m1.cc.obj (A1) is NOT COMDAT' "$TMP/st_out" \
    || fail "leg 4 does not report a weak C++ symbol outside a COMDAT group"
grep -q 'weak undefined C++ reference _ZN1iEv' "$TMP/st_out" \
    || fail "leg 4 does not report a weak undefined C++ reference"
grep -q '_ZN1jEv in m1.cc.obj (A1) has section index ABS' "$TMP/st_out" \
    || fail "leg 4 does not report a weak C++ symbol whose section index cannot be a group"
grep -q 'weak symbol plain_weak in m1.cc.obj (A1) is not on the allowlist' "$TMP/st_out" \
    || fail "leg 4 does not report a plain weak symbol off the allowlist"
grep -q 'allowed_weak' "$TMP/st_out" \
    && fail "leg 4 reports an allowlisted weak symbol"
grep -q '_ZN1fEv' "$TMP/st_out" \
    && fail "leg 4 reports a weak C++ symbol that IS in a COMDAT group"
# _ZN1fEv is quiet only because its section is a group, and only because it is mangled.
# The two mutations reach the same count by DIFFERENT messages, so both are asserted.
leg4_archive_findings "$TMP/st_w4" "$TMP/st_allow4" /dev/null "$MANGLED_RE" > "$TMP/st_out"
ST_N="$(st_count "$TMP/st_out")"
[ "$ST_N" -eq 5 ] || fail "with no COMDAT table leg 4 reported $ST_N, expected 5"
grep -q '_ZN1fEv in m1.cc.obj (A1) is NOT COMDAT' "$TMP/st_out" \
    || fail "with no COMDAT table leg 4 does not report the group-section control"
leg4_archive_findings "$TMP/st_w4" "$TMP/st_allow4" "$TMP/st_sec4" "$ST_NEVER" > "$TMP/st_out"
ST_N="$(st_count "$TMP/st_out")"
[ "$ST_N" -eq 5 ] || fail "with the mangled-name clause disabled leg 4 reported $ST_N, expected 5"
grep -q 'weak symbol _ZN1fEv in m1.cc.obj (A1) is not on the allowlist' "$TMP/st_out" \
    || fail "with the mangled-name clause disabled leg 4 does not report a C++ weak symbol"

# --- clause: leg 4 over the image, where the groups are resolved away
cat > "$TMP/st_welf" <<'EOF'
_ZN1fEv
DW.ref.__gxx_personality_v0
allowed_weak
plain_weak
EOF
ST_N="$(leg4_elf_findings "$TMP/st_welf" "$TMP/st_allow4" "$ELF_WEAK_SKIP_RE" | wc -l | tr -d ' ')"
[ "$ST_N" -eq 1 ] || fail "leg 4 reported $ST_N of 1 planted image weak violation"
ST_N="$(leg4_elf_findings "$TMP/st_welf" "$TMP/st_allow4" "$ST_NEVER" | wc -l | tr -d ' ')"
[ "$ST_N" -eq 3 ] \
    || fail "with the mangled and DW.ref clause disabled leg 4 reported $ST_N over the image, expected 3"

# --- clause: which nm type letters are weak, defined and undefined
cat > "$TMP/st_nmelf" <<'EOF'
00000010 W weak_func
00000020 V weak_vague
00000030 v weak_vague_local
00000040 w weak_undef_typed
00000050 T strong_func
00000060 D strong_data
EOF
ST_N="$(elf_weak_defs "$TMP/st_nmelf" | wc -l | tr -d ' ')"
[ "$ST_N" -eq 4 ] || fail "the image weak scan found $ST_N of 4 planted weak definitions"
cat > "$TMP/st_nmundef" <<'EOF'
         w weak_ref
         U plain_ref
EOF
ST_N="$(elf_weak_undefs "$TMP/st_nmundef" | wc -l | tr -d ' ')"
[ "$ST_N" -eq 1 ] || fail "the image weak scan found $ST_N of 1 planted weak undefined reference"

# --- the allowlist ------------------------------------------------------------
# The RX psABI prefixes every C identifier with an underscore, so the allowlist carries
# both spellings and nothing here needs to know which target it is looking at.
grep -vE '^[[:space:]]*(#|$)' "$ALLOWLIST" | awk '{print $1}' | sort -u > "$TMP/allow"

# --- inventory: every defined symbol of every archive, member by member -------
: > "$TMP/defs"
for a in $ARCHIVES; do
    if [ ! -r "$a" ]; then
        bad "cannot read archive $a"
        continue
    fi
    tool_out "$TMP/tool" "$NM_ARCHIVE_DEF_RE" "$NM" -A --defined-only "$a"
    nm_archive_defs "$TMP/tool" "$a" "$DEF_TYPE_RE" "$DEF_SKIP_TYPES" >> "$TMP/defs"
done
# An object that cannot be read is the same failure the archive arm above reports, and
# skipping it classifies every seam the app itself defines (kickos_app_authority) as
# having NO backend: the fallback then reads as correct and the app's narrower authority
# mask is silently widened to the fallback's.
: > "$TMP/cmdline_members"
for o in $OBJECTS; do
    if [ ! -r "$o" ]; then
        bad "cannot read object $o"
        continue
    fi
    tool_out "$TMP/tool" "$NM_DEF_RE" "$NM" --defined-only "$o"
    nm_object_defs "$TMP/tool" "$(basename "$o")" "$DEF_TYPE_RE" "$DEF_SKIP_TYPES" >> "$TMP/defs"
    basename "$o" >> "$TMP/cmdline_members"
done
require_nonempty "$TMP/cmdline_members" \
    "no app object was inventoried; a seam the app itself defines would read as backend-less"

# --- inventory: COMDAT sections and the symbols they define -------------------
# Built once here; legs 1, 2, 3 subtract these symbols and leg 4 tests membership.
: > "$TMP/comdat_sec"
for a in $ARCHIVES; do
    tool_out "$TMP/tool" '^ *\[ *[0-9]+\]' "$READELF" -SW "$a"
    comdat_section_rows "$TMP/tool" "$a" '' "$COMDAT_FLAG_OFF" >> "$TMP/comdat_sec"
done
for o in $OBJECTS; do
    tool_out "$TMP/tool" '^ *\[ *[0-9]+\]' "$READELF" -SW "$o"
    comdat_section_rows "$TMP/tool" "-" "$(basename "$o")" "$COMDAT_FLAG_OFF" >> "$TMP/comdat_sec"
done

: > "$TMP/comdat_syms"
for a in $ARCHIVES; do
    tool_out "$TMP/syms" "$READELF_SYM_RE" "$READELF" -sW "$a"
    comdat_symbol_rows "$TMP/syms" "$a" '' "$TMP/comdat_sec" "$COMDAT_BIND_RE" >> "$TMP/comdat_syms"
done
for o in $OBJECTS; do
    tool_out "$TMP/syms" "$READELF_SYM_RE" "$READELF" -sW "$o"
    comdat_symbol_rows "$TMP/syms" "-" "$(basename "$o")" "$TMP/comdat_sec" "$COMDAT_BIND_RE" \
        >> "$TMP/comdat_syms"
done
sort -u "$TMP/comdat_syms" -o "$TMP/comdat_syms"

strong_only "$TMP/defs" "$TMP/comdat_syms" > "$TMP/strong_defs"

awk -F'\t' -v RE="$FB_MEMBER_RE" '$2 ~ RE' "$TMP/strong_defs" > "$TMP/fb_defs"
awk -F'\t' -v RE="$FB_MEMBER_RE" '$2 !~ RE' "$TMP/strong_defs" > "$TMP/be_defs"

# --- leg 1: one strong symbol per fallback member, unique in its archive ------
awk -F'\t' '{ print $1 "\t" $2 }' "$TMP/fb_defs" | sort -u > "$TMP/fb_members"
if [ ! -s "$TMP/fb_members" ]; then
    bad "no <symbol>_default.cc member found in any archive; the gate would be vacuous"
fi
leg1_findings "$TMP/fb_defs" "$TMP/strong_defs" "$TMP/fb_members" "$KERNEL_ARCHIVE_GLOB" \
    > "$TMP/leg1_out"
collect "$TMP/leg1_out"

# --- legs 2 and 3, per seam symbol -------------------------------------------
map_included "$MAP" "$INCLUDE_RE" > "$TMP/included"
awk -F'\t' '{ print $4 }' "$TMP/fb_defs" | sort -u > "$TMP/seams"
legs23_findings "$TMP/fb_defs" "$TMP/be_defs" "$TMP/seams" "$TMP/included" \
    "$TMP/cmdline_members" "$MAP" "$PSABI_PREFIX" "$TMP/resolved" > "$TMP/leg23_out"
collect "$TMP/leg23_out"
checked_fallback="$(cat "$TMP/resolved")"
if [ "$checked_fallback" -eq 0 ]; then
    bad "leg 3: this board resolved no seam from its fallback, so the fallback path is untested here"
fi

# --- leg 4: no weak symbol outside the allowlist ------------------------------
for a in $ARCHIVES; do
    tool_out "$TMP/syms" "$READELF_SYM_RE" "$READELF" -sW "$a"
    weak_rows "$TMP/syms" "$a" > "$TMP/weak_arch"
    leg4_archive_findings "$TMP/weak_arch" "$TMP/allow" "$TMP/comdat_sec" "$MANGLED_RE" \
        > "$TMP/leg4_out"
    collect "$TMP/leg4_out"
done

tool_out "$TMP/tool" "$NM_DEF_RE" "$NM" --defined-only "$ELF"
elf_weak_defs "$TMP/tool" > "$TMP/weak_elf"
# A fully linked image may legitimately carry no undefined symbol at all, so this one
# gets no landmark beyond nm having succeeded.
tool_out "$TMP/tool" '' "$NM" --undefined-only "$ELF"
elf_weak_undefs "$TMP/tool" >> "$TMP/weak_elf"
sort -u "$TMP/weak_elf" -o "$TMP/weak_elf"
leg4_elf_findings "$TMP/weak_elf" "$TMP/allow" "$ELF_WEAK_SKIP_RE" > "$TMP/leg4_elf_out"
collect "$TMP/leg4_elf_out"

if [ "$rc" -eq 0 ]; then
    nfb=$(wc -l < "$TMP/fb_members" | tr -d ' ')
    nseam=$(wc -l < "$TMP/seams" | tr -d ' ')
    echo "seam_defaults: OK ($nfb fallback members, $nseam seams, $checked_fallback resolved from a fallback)"
fi
exit "$rc"
