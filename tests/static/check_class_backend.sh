#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Driver-class shadowing gate. The public class symbols of <kickos/driver/*.h>
# (kos_uart_open... , kos_spi_bus_open...) are ordinary strong C symbols, and an image may
# legitimately hold a MOCK definition of them: the selftest compiles spi_mock.cc straight
# into its executable.
#
# A static-archive member is extracted ONLY to satisfy a still-undefined symbol, so a mock
# already on the link command line answers every reference first, the backend's member is
# never extracted, and the link reports nothing. The image then runs the mock over the live
# register file: kernel banner, then silence.
#
# Usage:
#   check_class_backend.sh <nm> <headerdirs> <map> <expect-app-definition> <source>...
#
# <headerdirs> is a ';'-separated list of public-header directories, each read at depth 1.
# It covers the SYSCALL set as well as the driver classes, because the same shadowing works
# through either: a U-seam gate defines public kos_* names, and today only the fact that
# user/src/syscall_stubs.cc is ONE archive member keeps a target image from resolving them
# out of its own executable. Splitting that file per subsystem is an ordinary refactor, and
# the derived set is what has to survive it.
#
# The symbol set is DERIVED from the headers, so over-capture is safe: a name never defined
# twice can never fail. <expect-app-definition> is 1 on an image that compiles a mock, and is
# the positive control: green then means "no second definer" rather than "found nothing".

set -u
# Every path arrives as an argument and is re-split unquoted below; a glob character in a
# build path must not expand against the cwd and inventory a different file.
set -f
. "$(dirname "$0")/../lib/gate.sh"

# The map and the tool output are parsed structurally, never by translated headings.
export LC_ALL=C

if [ "$#" -lt 5 ]; then
    echo "usage: $0 <nm> <headerdirs> <map> <expect-app-definition> <source>..." >&2
    exit 2
fi

NM="$1"; shift
HEADERDIRS="$1"; shift
MAP="$1"; shift
EXPECT_APP="$1"; shift

[ -n "$HEADERDIRS" ] || fail "no header directory given"
[ -r "$MAP" ] || fail "cannot read $MAP"

# Split on ';' as well as on argument boundaries: add_test does NOT split a
# $<TARGET_OBJECTS:> expansion, so the app's whole object list arrives as ONE
# semicolon-joined argument. A path that does not exist would read downstream as a source
# with no definitions, so it is refused here.
ARCHIVES=""
OBJECTS=""
_oldifs=$IFS
IFS=';'
for _arg in "$@"; do
    for _in in $_arg; do
        [ -n "$_in" ] || continue
        [ -e "$_in" ] || fail "definition source does not exist: $_in"
        # ARCHIVES/OBJECTS are space-joined lists that every loop re-splits, so a path with
        # whitespace is refused here instead of being read as two paths.
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

scratch_dir
rc=0
# A shadowing violation is accumulated so one run names them all; a broken tool takes
# gate.sh's hard exit instead.
bad() { echo "FAIL: $*" >&2; rc=1; }

# nm prints "<addr> <type> <symbol>", prefixed "<archive>:<member>:" with -A. Requiring at
# least one match is a positive control on the nm invocation itself.
NM_DEF_RE='^[0-9a-fA-F]+ [A-Z] '
NM_ARCHIVE_DEF_RE=':[0-9a-fA-F]+ [A-Z] '

# --- the rule, as named patterns ---------------------------------------------
# Handed to the scanners below, so the self-test and the real run cannot disagree about what
# the rule is. AN ERE REACHING awk THROUGH -v IS ESCAPE-PROCESSED FIRST, so a metacharacter
# is bracketed rather than backslashed: `[.]a[(]` survives that pass and `\.a\(` does not.
CLASS_DECL_ERE='\bkos_[a-z0-9_]+[[:space:]]*\('
LINE_COMMENT_SED='s://.*::'
NM_TYPE_ERE='^[A-Z]$'
NM_TYPE_SKIP_ERE='^[NU]$'
NM_MEMBER_ERE='.'
APP_NS_ERE='^_?kos_[a-z0-9_]+$'
MAP_INCLUDE_ERE='^[^ \t].*[.]a[(].*[)]$'

# --- the scanners ------------------------------------------------------------
# Line comments go first: the prose in these headers names the calls, and "see
# kos_uart_open" must not become a symbol. A `/* */` block is NOT stripped, so a name inside
# one is captured; over-capture cannot fail a symbol nothing defines twice.
parse_class_syms() { # <headers-list-file> <out>
    : > "$2"
    while IFS= read -r _h; do
        [ -r "$_h" ] || continue
        sed -e "$LINE_COMMENT_SED" "$_h" \
            | grep -oE "$CLASS_DECL_ERE" \
            | sed -e 's/[[:space:]]*(//' >> "$2"
    done < "$1"
    sort -u "$2" -o "$2"
}

# The RX psABI prefixes every C identifier with an underscore, so the set carries BOTH
# spellings. Without them the inventory comes back empty on such a target and legs 1 and 2
# pass vacuously.
add_underscore_spellings() { # <syms-file>
    sed -e 's/^/_/' "$1" > "$1.u"
    cat "$1.u" >> "$1"
    sort -u "$1" -o "$1"
}

# `nm -A` on an archive prints "<path>:<member>:<addr> <type> <symbol>". Only an uppercase
# type takes part in cross-member resolution, and N (debug) and U (undefined) do not. A
# class symbol is extern "C", so it is never COMDAT.
# Through tool_out and then appended, rather than awk redirected straight at <defs-out>: an
# awk that died leaves the table short, and legs 1 and 2 over a short table report nothing.
inventory_archive() { # <nm-output> <archive-key> <class-syms> <defs-out>
    tool_out "$TMP/inv_a" '' \
        awk -v A="$2" -v C="$3" -v TYPE="$NM_TYPE_ERE" -v SKIP="$NM_TYPE_SKIP_ERE" \
        -v MEMBER="$NM_MEMBER_ERE" '
        BEGIN { while ((getline s < C) > 0) { k[s] = 1 } }
        {
            split($1, p, ":")
            if (p[2] !~ MEMBER) { next }
            t = $(NF - 1)
            if (t !~ TYPE || t ~ SKIP) { next }
            if (!($NF in k)) { next }
            print $NF "\t" A "(" p[2] ")" "\t" p[2] "\tarchive"
        }' "$1"
    cat "$TMP/inv_a" >> "$4"
}

inventory_object() { # <nm-output> <object-path> <class-syms> <app-kos-out> <defs-out>
    # AK is appended, never truncated: this awk runs once PER OBJECT, and `>` would reopen
    # and empty the file on every one of them, leaving only the last object's symbols.
    tool_out "$TMP/inv_o" '' \
        awk -v O="$2" -v B="$(basename "$2")" -v C="$3" -v AK="$4" \
        -v TYPE="$NM_TYPE_ERE" -v SKIP="$NM_TYPE_SKIP_ERE" -v NS="$APP_NS_ERE" '
        BEGIN { while ((getline s < C) > 0) { k[s] = 1 } }
        {
            t = $(NF - 1)
            if (t !~ TYPE || t ~ SKIP) { next }
            if ($NF ~ NS) { print $NF "\t" B >> AK }
            if (!($NF in k)) { next }
            print $NF "\t" O "\t" B "\tobject"
        }' "$1"
    cat "$TMP/inv_o" >> "$5"
}

# Only an inclusion entry puts an `<archive>(<member>)` at column 0. Selected structurally:
# the block's heading is translated by the host linker's locale.
map_included() { # <map> <out>
    tool_out "$TMP/map_raw" '' awk -v INC="$MAP_INCLUDE_ERE" '
        $0 ~ INC {
            m = $1
            sub(/^.*\(/, "", m); sub(/\)$/, "", m)
            print m
        }' "$1"
    tool_out "$2" '' sort -u "$TMP/map_raw"
}

# A miss here is a class header this gate never read, or an app squatting on kos_*; either
# leaves a backend shadowable with this gate still green.
leg3_scan() { # <app-kos> <class-syms> <findings-out>
    : > "$3"
    while IFS="$TAB" read -r _sym _member; do
        [ -n "$_sym" ] || continue
        if ! grep -qxF "$_sym" "$2"; then
            printf '%s\n' "leg 3: $_member defines the public symbol $_sym, which is not declared by any header in $HEADERDIRS; the class symbol set this gate derives is incomplete" >> "$3"
        fi
    done < "$1"
}

# Without this a broken header parse, a broken nm invocation or an empty source list all
# read as "no shadowing found".
expect_app_scan() { # <expect-app> <napp> <findings-out>
    : > "$3"
    if [ "$1" = "1" ] && [ "$2" -eq 0 ]; then
        printf '%s\n' "no class symbol is defined by any object on the link line, but this image compiles the mocks; the inventory is not seeing them" >> "$3"
    fi
}

legs12_scan() { # <class-syms> <defs> <included> <findings-out> <count-out>
    : > "$4"
    _checked=0
    while read -r _sym; do
        [ -n "$_sym" ] || continue
        tool_out "$TMP/l12_src" '' awk -F'\t' -v s="$_sym" '$1 == s { print $2 }' "$2"
        tool_out "$TMP/l12_srcu" '' sort -u "$TMP/l12_src"
        _sources=$(cat "$TMP/l12_srcu")
        [ -n "$_sources" ] || continue
        _checked=$((_checked + 1))
        _c=$(printf '%s\n' "$_sources" | wc -l | tr -d ' ')
        if [ "$_c" -gt 1 ]; then
            # The object always wins, being on the command line, so the archive member is
            # the one that vanished.
            _winner=$(awk -F'\t' -v s="$_sym" '$1 == s && $4 == "object" { print $2 }' "$2" | head -1)
            if [ -n "$_winner" ]; then
                _losers=$(awk -F'\t' -v s="$_sym" '$1 == s && $4 == "archive" { print $2 }' "$2" | sort -u)
                printf '%s\n' "leg 1: $_sym is defined by the link-line object $_winner AND by $(echo $_losers); the archive member is never extracted, no duplicate symbol is reported, and that backend's own calls bind to the object's definition. Either keep the second definition out of every target image (what the UART class does: its mock is host-only, tests/unit/uartclass), or rename the backend's class symbols in its CMakeLists (what the SPI services do, because t_bus_device_slots needs a mock in the image: target_compile_definitions kos_*=<driver>_*)" >> "$4"
            else
                printf '%s\n' "leg 1: $_sym has $_c definition sources ($(echo $_sources)); which one this link resolves to is archive order" >> "$4"
            fi
            continue
        fi
        # Leg 2: a lone archive definition must have been extracted, or the image is using a
        # definition this inventory never saw.
        tool_out "$TMP/l12_kind" '' awk -F'\t' -v s="$_sym" '$1 == s { print $4 }' "$2"
        [ -s "$TMP/l12_kind" ] \
            || fail "no kind column came back for $_sym, which this inventory says has a
      definition source; leg 2 would be skipped for it"
        _kind=$(head -1 "$TMP/l12_kind")
        if [ "$_kind" = "archive" ]; then
            tool_out "$TMP/l12_mem" '' awk -F'\t' -v s="$_sym" '$1 == s { print $3 }' "$2"
            _member=$(head -1 "$TMP/l12_mem")
            if ! grep -qxF "$_member" "$3"; then
                printf '%s\n' "leg 2: $_member holds the only inventoried definition of $_sym but never entered the link; something outside this inventory answered the reference" >> "$4"
            fi
        fi
    done < "$1"
    printf '%s\n' "$_checked" > "$5"
}

# --- self-test: prove every clause of the rule, one control per clause --------
# Every control below is a MINIMAL PAIR with an exact expected count, and every negative is
# also read ON ITS OWN: a whole-corpus zero cannot tell "every clause works" from "one
# clause swallowed the corpus".
ST="$TMP/selftest"
mkdir -p "$ST/hdr" || fail "cannot create the self-test scratch directory"

cat > "$ST/hdrlines" <<'EOF'
// see kos_comment_only() for the contract
int kos_uart_open(int fd);
int kos_spi_bus_open (int bus);
/* kos_in_block_comment(void) */
int not_kos_prefixed(void);
int kos_no_paren;
int KOS_UPPER(void);
int kos_uart_close(void);
EOF
cp "$ST/hdrlines" "$ST/hdr/a.h"
printf '%s\n' "$ST/hdr/a.h" > "$ST/hdrlist"

parse_class_syms "$ST/hdrlist" "$ST/syms_parsed"
POS="$(wc -l < "$ST/syms_parsed" | tr -d ' ')"
[ "$POS" -eq 4 ] || fail "the header parse took $POS of 4 planted declarations; it would miss a real class symbol"
for _w in kos_uart_open kos_spi_bus_open kos_in_block_comment kos_uart_close; do
    grep -qxF "$_w" "$ST/syms_parsed" || fail "the header parse missed the planted declaration $_w"
done
for _w in kos_comment_only kos_prefixed kos_no_paren KOS_UPPER; do
    if grep -qxF "$_w" "$ST/syms_parsed"; then
        fail "the header parse took $_w, which no header declares; the derived set is over-wide in a way that hides a real miss"
    fi
done

_i=0
while IFS= read -r _line; do
    _i=$((_i + 1))
    printf '%s\n' "$_line" > "$ST/hdr/one.h"
    printf '%s\n' "$ST/hdr/one.h" > "$ST/onelist"
    parse_class_syms "$ST/onelist" "$ST/one_syms"
    _n="$(wc -l < "$ST/one_syms" | tr -d ' ')"
    case "$_i" in
        2|3|4|8) [ "$_n" -eq 1 ] || fail "planted declaration $_i parsed to $_n symbol(s), expected 1: $_line" ;;
        *)       [ "$_n" -eq 0 ] || fail "header control $_i is not silent on its own: $_line" ;;
    esac
done < "$ST/hdrlines"
[ "$_i" -eq 8 ] || fail "$_i header control(s) ran, expected 8"

# Disable one clause and the count must move to an EXACT number: that is what proves each
# control is a near miss rather than one kept quiet by the wrong clause.
hdr_mutate() { # <clause> <comment-sed> <decl-ere> <expect-count>
    _m="$( ( LINE_COMMENT_SED="$2"; CLASS_DECL_ERE="$3"
             parse_class_syms "$ST/hdrlist" "$ST/mut_syms"
             wc -l < "$ST/mut_syms" | tr -d ' ' ) )"
    [ "$_m" -eq "$4" ] || fail "with the $1 clause disabled the header parse took $_m symbol(s), expected $4;
      the control for it is not a near miss and proves nothing"
}
hdr_mutate "line-comment"  's:$::'             "$CLASS_DECL_ERE"                     5
hdr_mutate "word-boundary" "$LINE_COMMENT_SED" 'kos_[a-z0-9_]+[[:space:]]*\('        5
hdr_mutate "space-tolerant-paren" "$LINE_COMMENT_SED" '\bkos_[a-z0-9_]+\('           3

cp "$ST/syms_parsed" "$ST/syms_doubled"
add_underscore_spellings "$ST/syms_doubled"
POS="$(wc -l < "$ST/syms_doubled" | tr -d ' ')"
[ "$POS" -eq 8 ] || fail "the RX spelling pass produced $POS of 8 entries; a target whose ABI underscores every identifier would inventory nothing"
grep -qxF _kos_uart_open "$ST/syms_doubled" || fail "the RX spelling pass did not add the underscored spelling"

cat > "$ST/syms" <<'EOF'
kos_uart_open
kos_uart_close
kos_uart_read
kos_uart_write
kos_spi_bus_open
_kos_uart_open
EOF

# One planted nm record per clause of the archive inventory: an ordinary definition, a local
# type, a debug type, an undefined type, a second member, a record carrying no member name
# at all, and a name outside the class set.
cat > "$ST/nm_arch" <<'EOF'
libx.a:m1.obj:00000010 T kos_uart_open
libx.a:m1.obj:00000020 t kos_uart_close
libx.a:m1.obj:00000030 N kos_spi_bus_open
libx.a:m1.obj:                 U kos_uart_write
libx.a:m2.obj:00000040 D kos_uart_read
00000050 T kos_uart_open
libx.a:m2.obj:00000060 T not_a_class_symbol
EOF
: > "$ST/defs_arch"
inventory_archive "$ST/nm_arch" libx.a "$ST/syms" "$ST/defs_arch"
POS="$(wc -l < "$ST/defs_arch" | tr -d ' ')"
[ "$POS" -eq 2 ] || fail "the archive inventory took $POS of 2 definitions out of 7 planted nm records; a backend definition would read as absent"

_i=0
while IFS= read -r _rec; do
    _i=$((_i + 1))
    printf '%s\n' "$_rec" > "$ST/one_arch"
    : > "$ST/one_defs"
    inventory_archive "$ST/one_arch" libx.a "$ST/syms" "$ST/one_defs"
    _n="$(wc -l < "$ST/one_defs" | tr -d ' ')"
    case "$_i" in
        1|5) [ "$_n" -eq 1 ] || fail "planted archive definition $_i inventoried $_n row(s), expected 1: $_rec" ;;
        *)   [ "$_n" -eq 0 ] || fail "archive control $_i is not silent on its own: $_rec" ;;
    esac
done < "$ST/nm_arch"
[ "$_i" -eq 7 ] || fail "$_i archive control(s) ran, expected 7"

arch_mutate() { # <clause> <type-ere> <skip-ere> <member-ere> <expect-count>
    _m="$( ( NM_TYPE_ERE="$2"; NM_TYPE_SKIP_ERE="$3"; NM_MEMBER_ERE="$4"
             : > "$ST/mut_defs"
             inventory_archive "$ST/nm_arch" libx.a "$ST/syms" "$ST/mut_defs"
             wc -l < "$ST/mut_defs" | tr -d ' ' ) )"
    [ "$_m" -eq "$5" ] || fail "with the $1 clause disabled the archive inventory took $_m row(s), expected $5;
      the control for it is not a near miss and proves nothing"
}
arch_mutate "uppercase-type"  '.'            "$NM_TYPE_SKIP_ERE" "$NM_MEMBER_ERE" 3
arch_mutate "undefined-type"  "$NM_TYPE_ERE" '^N$'               "$NM_MEMBER_ERE" 3
arch_mutate "debug-type"      "$NM_TYPE_ERE" '^U$'               "$NM_MEMBER_ERE" 3
arch_mutate "archive-member"  "$NM_TYPE_ERE" "$NM_TYPE_SKIP_ERE" '^'              3

# TWO objects, each defining a distinct public name, because the app table is APPENDED once
# per object: a truncating redirection leaves only the last object's symbols and the count
# below is what says so.
cat > "$ST/nm_obj1" <<'EOF'
00000010 T kos_uart_open
00000020 T kos_squatter
00000030 t kos_local
00000040 T _kos_uart_open
00000050 T ordinary_symbol
00000060 T xkos_uart_open
00000070 N kos_uart_read
EOF
cat > "$ST/nm_obj2" <<'EOF'
00000080 T kos_uart_write
EOF
: > "$ST/defs_obj"
: > "$ST/app_kos"
inventory_object "$ST/nm_obj1" "$ST/main.cc.obj" "$ST/syms" "$ST/app_kos" "$ST/defs_obj"
inventory_object "$ST/nm_obj2" "$ST/mock.cc.obj" "$ST/syms" "$ST/app_kos" "$ST/defs_obj"
POS="$(wc -l < "$ST/app_kos" | tr -d ' ')"
[ "$POS" -eq 4 ] || fail "the object inventory left $POS of 4 planted public names in the app table; leg 3 would be blind to an app squatting on the namespace"
POS="$(wc -l < "$ST/defs_obj" | tr -d ' ')"
[ "$POS" -eq 3 ] || fail "the object inventory took $POS of 3 planted class definitions; a mock on the link line would read as absent"

_i=0
while IFS= read -r _rec; do
    _i=$((_i + 1))
    printf '%s\n' "$_rec" > "$ST/one_obj"
    : > "$ST/one_app"
    : > "$ST/one_defs"
    inventory_object "$ST/one_obj" "$ST/main.cc.obj" "$ST/syms" "$ST/one_app" "$ST/one_defs"
    _n="$(wc -l < "$ST/one_app" | tr -d ' ')"
    case "$_i" in
        1|2|4) [ "$_n" -eq 1 ] || fail "planted public name $_i reached the app table $_n time(s), expected 1: $_rec" ;;
        *)     [ "$_n" -eq 0 ] || fail "object control $_i is not silent on its own: $_rec" ;;
    esac
done < "$ST/nm_obj1"
[ "$_i" -eq 7 ] || fail "$_i object control(s) ran, expected 7"

obj_mutate() { # <clause> <ns-ere> <expect-count>
    _m="$( ( APP_NS_ERE="$2"
             : > "$ST/mut_app"; : > "$ST/mut_defs"
             inventory_object "$ST/nm_obj1" "$ST/main.cc.obj" "$ST/syms" "$ST/mut_app" "$ST/mut_defs"
             inventory_object "$ST/nm_obj2" "$ST/mock.cc.obj" "$ST/syms" "$ST/mut_app" "$ST/mut_defs"
             wc -l < "$ST/mut_app" | tr -d ' ' ) )"
    [ "$_m" -eq "$3" ] || fail "with the $1 clause disabled the object inventory left $_m name(s) in the app table, expected $3;
      the control for it is not a near miss and proves nothing"
}
obj_mutate "rx-underscore"    '^kos_[a-z0-9_]+$'  3
obj_mutate "namespace-anchor" '_?kos_[a-z0-9_]+$' 5

# One planted map record per clause: an inclusion entry, the indented line naming what
# pulled it, an indented placement, a second inclusion, a column-zero line that is not an
# archive member, and an inclusion entry with an operand after it.
cat > "$ST/map" <<'EOF'
libx.a(m1.obj)
                              libk.a(syscall.cc.obj) (kos_uart_open)
 .text          0x1000  0x20 libx.a(m2.obj)
libx.a(m3.obj)
some_function(void)
libx.a(m4.obj) extra
EOF
map_included "$ST/map" "$ST/included_ctl"
POS="$(wc -l < "$ST/included_ctl" | tr -d ' ')"
[ "$POS" -eq 2 ] || fail "the map parse read $POS of 2 planted inclusion entries out of 6 records; a member that entered the link would read as absent"
for _w in m1.obj m3.obj; do
    grep -qxF "$_w" "$ST/included_ctl" || fail "the map parse missed the planted inclusion entry $_w"
done
for _w in m2.obj syscall.cc.obj m4.obj void .text; do
    if grep -qxF "$_w" "$ST/included_ctl"; then
        fail "the map parse read $_w as an inclusion entry; leg 2 would take an unextracted member for a linked one"
    fi
done

map_mutate() { # <clause> <include-ere> <expect-count>
    _m="$( ( MAP_INCLUDE_ERE="$2"
             map_included "$ST/map" "$ST/mut_inc"
             wc -l < "$ST/mut_inc" | tr -d ' ' ) )"
    [ "$_m" -eq "$3" ] || fail "with the $1 clause disabled the map parse read $_m entry(ies), expected $3;
      the control for it is not a near miss and proves nothing"
}
map_mutate "column-zero"  '^.*[.]a[(].*[)]$'   4
map_mutate "line-end"     '^[^ \t].*[.]a[(]'   3
map_mutate "archive-name" '^[^ \t].*[(].*[)]$' 3

# Legs 1 and 2 over a planted inventory: a symbol shadowed by a link-line object, one with
# two archive definitions and no object, one whose lone archive member never entered the
# link, one whose member did, one defined only on the link line, and one nothing defines.
printf '%s\n' kos_never_defined kos_spi_bus_open kos_uart_close kos_uart_open \
    kos_uart_read kos_uart_write > "$ST/legsyms"
{
    printf '%s\t%s\t%s\t%s\n' kos_uart_open    main.cc.obj      main.cc.obj object
    printf '%s\t%s\t%s\t%s\n' kos_uart_open    'libx.a(m1.obj)' m1.obj      archive
    printf '%s\t%s\t%s\t%s\n' kos_spi_bus_open 'liba.a(p.obj)'  p.obj       archive
    printf '%s\t%s\t%s\t%s\n' kos_spi_bus_open 'libb.a(q.obj)'  q.obj       archive
    printf '%s\t%s\t%s\t%s\n' kos_uart_close   'libx.a(m9.obj)' m9.obj      archive
    printf '%s\t%s\t%s\t%s\n' kos_uart_read    'libx.a(m2.obj)' m2.obj      archive
    printf '%s\t%s\t%s\t%s\n' kos_uart_write   main.cc.obj      main.cc.obj object
} > "$ST/legdefs"
printf '%s\n' m1.obj m2.obj > "$ST/legincluded"

legs12_scan "$ST/legsyms" "$ST/legdefs" "$ST/legincluded" "$ST/legfind" "$ST/legcount"
POS="$(wc -l < "$ST/legfind" | tr -d ' ')"
[ "$POS" -eq 3 ] || fail "legs 1 and 2 reported $POS of 3 planted shadowings; a shadowed backend would read as clean"
POS="$(cat "$ST/legcount")"
[ "$POS" -eq 5 ] || fail "legs 1 and 2 checked $POS of the 5 planted symbols that carry a definition"
# The two leg-1 arms are separate clauses, and only the message tells them apart.
grep -q 'leg 1: kos_uart_open is defined by the link-line object' "$ST/legfind" \
    || fail "leg 1 did not name the link-line object as the winner over the archive member"
grep -q 'leg 1: kos_spi_bus_open has 2 definition sources' "$ST/legfind" \
    || fail "leg 1 did not report two archive definitions with no object among them"

_i=0
while IFS= read -r _s; do
    _i=$((_i + 1))
    printf '%s\n' "$_s" > "$ST/onesym"
    legs12_scan "$ST/onesym" "$ST/legdefs" "$ST/legincluded" "$ST/onefind" "$ST/onecount"
    _n="$(wc -l < "$ST/onefind" | tr -d ' ')"
    case "$_s" in
        kos_spi_bus_open|kos_uart_close|kos_uart_open)
            [ "$_n" -eq 1 ] || fail "planted shadowing $_s reported $_n finding(s), expected 1" ;;
        *)
            [ "$_n" -eq 0 ] || fail "leg control $_s is not silent on its own: $(cat "$ST/onefind")" ;;
    esac
done < "$ST/legsyms"
[ "$_i" -eq 6 ] || fail "$_i leg control(s) ran, expected 6"

legs12_scan "$ST/legsyms" "$ST/legdefs" "/dev/null" "$ST/mutfind" "$ST/mutcount"
POS="$(wc -l < "$ST/mutfind" | tr -d ' ')"
[ "$POS" -eq 4 ] || fail "with an empty inclusion list legs 1 and 2 reported $POS finding(s), expected 4;
      the leg-2 negative control is not a near miss and proves nothing"
printf '%s\n' m1.obj m2.obj m9.obj > "$ST/wideinc"
legs12_scan "$ST/legsyms" "$ST/legdefs" "$ST/wideinc" "$ST/mutfind" "$ST/mutcount"
POS="$(wc -l < "$ST/mutfind" | tr -d ' ')"
[ "$POS" -eq 2 ] || fail "with every member linked legs 1 and 2 reported $POS finding(s), expected 2;
      the leg-2 positive control is caught by another clause and proves nothing about leg 2"

{
    printf '%s\t%s\n' kos_uart_open  main.cc.obj
    printf '%s\t%s\n' kos_squatter   main.cc.obj
    printf '%s\t%s\n' _kos_uart_open main.cc.obj
} > "$ST/legapp"
printf '%s\n' kos_uart_open _kos_uart_open > "$ST/leg3syms"
leg3_scan "$ST/legapp" "$ST/leg3syms" "$ST/leg3find"
POS="$(wc -l < "$ST/leg3find" | tr -d ' ')"
[ "$POS" -eq 1 ] || fail "leg 3 reported $POS of 1 planted undeclared public symbol; a class header this gate never read would leave a backend shadowable"
grep -q kos_squatter "$ST/leg3find" || fail "leg 3 reported a finding, but not for the planted undeclared symbol"
leg3_scan "$ST/legapp" "/dev/null" "$ST/leg3mut"
POS="$(wc -l < "$ST/leg3mut" | tr -d ' ')"
[ "$POS" -eq 3 ] || fail "with an empty class symbol set leg 3 reported $POS finding(s), expected 3;
      its negative controls are not near misses and prove nothing"

expect_app_scan 1 0 "$ST/eafind"
POS="$(wc -l < "$ST/eafind" | tr -d ' ')"
[ "$POS" -eq 1 ] || fail "the expect-app control stayed silent on an image that compiles a mock and inventoried none of it"
expect_app_scan 1 4 "$ST/eafind"
POS="$(wc -l < "$ST/eafind" | tr -d ' ')"
[ "$POS" -eq 0 ] || fail "the expect-app control fired although the inventory saw the mock"
expect_app_scan 0 0 "$ST/eafind"
POS="$(wc -l < "$ST/eafind" | tr -d ' ')"
[ "$POS" -eq 0 ] || fail "the expect-app control fired on an image that compiles no mock"

# --- the class symbol set, derived from the headers --------------------------
# `find`, not a shell glob: `set -f` above would leave "$_d"/*.h unexpanded and the set empty.
: > "$TMP/headers"
_oldifs=$IFS
IFS=';'
for _d in $HEADERDIRS; do
    IFS=$_oldifs
    [ -n "$_d" ] || continue
    [ -d "$_d" ] || fail "header directory does not exist: $_d"
    find "$_d" -maxdepth 1 -name '*.h' -print >> "$TMP/headers"
    IFS=';'
done
IFS=$_oldifs
require_nonempty "$TMP/headers" "no header found in $HEADERDIRS"
parse_class_syms "$TMP/headers" "$TMP/class_syms"
require_nonempty "$TMP/class_syms" \
    "no class symbol was parsed out of the headers in $HEADERDIRS; this gate would be vacuous"
ndeclared=$(wc -l < "$TMP/class_syms" | tr -d ' ')
add_underscore_spellings "$TMP/class_syms"

# --- inventory: every class symbol defined by every source -------------------
: > "$TMP/defs"
for a in $ARCHIVES; do
    if [ ! -r "$a" ]; then
        bad "cannot read archive $a"
        continue
    fi
    tool_out "$TMP/tool" "$NM_ARCHIVE_DEF_RE" "$NM" -A --defined-only "$a"
    inventory_archive "$TMP/tool" "$a" "$TMP/class_syms" "$TMP/defs"
done
: > "$TMP/app_kos"
for o in $OBJECTS; do
    if [ ! -r "$o" ]; then
        bad "cannot read object $o"
        continue
    fi
    tool_out "$TMP/tool" "$NM_DEF_RE" "$NM" --defined-only "$o"
    inventory_object "$TMP/tool" "$o" "$TMP/class_syms" "$TMP/app_kos" "$TMP/defs"
done
sort -u "$TMP/defs" -o "$TMP/defs"
sort -u "$TMP/app_kos" -o "$TMP/app_kos"

# --- the inventory floor -----------------------------------------------------
require_nonempty "$TMP/defs" \
    "not one class symbol is defined by any archive or object on this link line; the
      inventory read nothing, so legs 1 and 2 would check nothing"
NARCH="$(grep -c "${TAB}archive$" "$TMP/defs")" || NARCH=0
[ "$NARCH" -gt 0 ] || fail "no archive member on this link line defines a class symbol, so
      there is nothing for a link-line definition to shadow and every leg below passes
      vacuously. The archives inventoried were:$ARCHIVES"

# --- leg 3: the header parse did not under-capture ---------------------------
leg3_scan "$TMP/app_kos" "$TMP/class_syms" "$TMP/find3"
while IFS= read -r _msg; do
    bad "$_msg"
done < "$TMP/find3"

tool_out "$TMP/appdefs" '' awk -F'\t' '$4 == "object"' "$TMP/defs"
napp=$(wc -l < "$TMP/appdefs" | tr -d ' ')
expect_app_scan "$EXPECT_APP" "$napp" "$TMP/findapp"
while IFS= read -r _msg; do
    bad "$_msg"
done < "$TMP/findapp"

# --- map: which archive members entered the link -----------------------------
map_included "$MAP" "$TMP/included"

# --- legs 1 and 2, per class symbol ------------------------------------------
legs12_scan "$TMP/class_syms" "$TMP/defs" "$TMP/included" "$TMP/find12" "$TMP/count12"
nchecked=$(cat "$TMP/count12")
[ "$nchecked" -gt 0 ] || fail "legs 1 and 2 checked no class symbol at all, although the
      inventory holds $(wc -l < "$TMP/defs" | tr -d ' ') definition(s); the per-symbol lookup
      is reading nothing"
while IFS= read -r _msg; do
    bad "$_msg"
done < "$TMP/find12"

if [ "$rc" -eq 0 ]; then
    echo "class_backend: OK ($ndeclared class symbols declared, $nchecked defined in this image, $napp of them on the link line)"
fi
exit "$rc"
