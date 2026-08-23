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

# --- the class symbol set, derived from the headers --------------------------
# Line comments go first: the prose in these headers names the calls, and "see
# kos_uart_open" must not become a symbol.
#
# `find`, not a shell glob: `set -f` above would leave "$_d"/*.h unexpanded and the set empty.
: > "$TMP/class_syms"
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
while IFS= read -r h; do
    [ -r "$h" ] || continue
    sed -e 's://.*::' "$h" \
        | grep -oE '\bkos_[a-z0-9_]+[[:space:]]*\(' \
        | sed -e 's/[[:space:]]*(//' >> "$TMP/class_syms"
done < "$TMP/headers"
sort -u "$TMP/class_syms" -o "$TMP/class_syms"
require_nonempty "$TMP/class_syms" \
    "no class symbol was parsed out of the headers in $HEADERDIRS; this gate would be vacuous"
ndeclared=$(wc -l < "$TMP/class_syms" | tr -d ' ')
# The RX psABI prefixes every C identifier with an underscore, so the set carries BOTH
# spellings. Without them the inventory comes back empty on such a target and legs 1 and 2
# pass vacuously.
sed -e 's/^/_/' "$TMP/class_syms" > "$TMP/class_syms_u"
cat "$TMP/class_syms_u" >> "$TMP/class_syms"
sort -u "$TMP/class_syms" -o "$TMP/class_syms"

# --- inventory: every class symbol defined by every source -------------------
# `nm -A` on an archive prints "<path>:<member>:<addr> <type> <symbol>". Only an uppercase
# type takes part in cross-member resolution, and N (debug) and U (undefined) do not. A
# class symbol is extern "C", so it is never COMDAT.
: > "$TMP/defs"
for a in $ARCHIVES; do
    if [ ! -r "$a" ]; then
        bad "cannot read archive $a"
        continue
    fi
    tool_out "$TMP/tool" "$NM_ARCHIVE_DEF_RE" "$NM" -A --defined-only "$a"
    awk -v A="$a" -v C="$TMP/class_syms" '
        BEGIN { while ((getline s < C) > 0) { k[s] = 1 } }
        {
            split($1, p, ":")
            if (p[2] == "") { next }
            t = $(NF - 1)
            if (t !~ /^[A-Z]$/ || t == "N" || t == "U") { next }
            if (!($NF in k)) { next }
            print $NF "\t" A "(" p[2] ")" "\t" p[2] "\tarchive"
        }' "$TMP/tool" >> "$TMP/defs"
done
: > "$TMP/app_kos"
for o in $OBJECTS; do
    if [ ! -r "$o" ]; then
        bad "cannot read object $o"
        continue
    fi
    tool_out "$TMP/tool" "$NM_DEF_RE" "$NM" --defined-only "$o"
    # AK is appended, never truncated: this awk runs once PER OBJECT, and `>` would reopen
    # and empty the file on every one of them, leaving only the last object's symbols.
    awk -v O="$o" -v B="$(basename "$o")" -v C="$TMP/class_syms" -v AK="$TMP/app_kos" '
        BEGIN { while ((getline s < C) > 0) { k[s] = 1 } }
        {
            t = $(NF - 1)
            if (t !~ /^[A-Z]$/ || t == "N" || t == "U") { next }
            if ($NF ~ /^_?kos_[a-z0-9_]+$/) { print $NF "\t" B >> AK }
            if (!($NF in k)) { next }
            print $NF "\t" O "\t" B "\tobject"
        }' "$TMP/tool" >> "$TMP/defs"
done
sort -u "$TMP/defs" -o "$TMP/defs"
sort -u "$TMP/app_kos" -o "$TMP/app_kos"

# --- leg 3: the header parse did not under-capture ---------------------------
# kos_* is the KickOS public API namespace, so an app TU defining a symbol in it is
# supplying a driver class. A miss here is a class header this gate did not read, or an app
# squatting on the namespace; either lets a backend be shadowed with the gate still green.
while IFS="$TAB" read -r sym member; do
    [ -n "$sym" ] || continue
    if ! grep -qxF "$sym" "$TMP/class_syms"; then
        bad "leg 3: $member defines the public symbol $sym, which is not declared by any header in $HEADERDIRS; the class symbol set this gate derives is incomplete"
    fi
done < "$TMP/app_kos"

# The positive control. Without it a broken header parse, a broken nm invocation or an
# empty source list all read as "no shadowing found".
napp=$(awk -F'\t' '$4 == "object"' "$TMP/defs" | wc -l | tr -d ' ')
if [ "$EXPECT_APP" = "1" ] && [ "$napp" -eq 0 ]; then
    bad "no class symbol is defined by any object on the link line, but this image compiles the mocks; the inventory is not seeing them"
fi

# --- map: which archive members entered the link -----------------------------
# Only an inclusion entry puts an `<archive>(<member>)` at column 0. Selected structurally:
# the block's heading is translated by the host linker's locale.
awk '/^[^ \t].*\.a\(.*\)$/ {
        m = $1
        sub(/^.*\(/, "", m); sub(/\)$/, "", m)
        print m
    }' "$MAP" | sort -u > "$TMP/included"

# --- legs 1 and 2, per class symbol ------------------------------------------
nchecked=0
while read -r sym; do
    [ -n "$sym" ] || continue
    sources=$(awk -F'\t' -v s="$sym" '$1 == s { print $2 }' "$TMP/defs" | sort -u)
    [ -n "$sources" ] || continue
    nchecked=$((nchecked + 1))
    n=$(printf '%s\n' "$sources" | wc -l | tr -d ' ')
    if [ "$n" -gt 1 ]; then
        # The object always wins, being on the command line, so the archive member is the
        # one that vanished.
        winner=$(awk -F'\t' -v s="$sym" '$1 == s && $4 == "object" { print $2 }' "$TMP/defs" | head -1)
        if [ -n "$winner" ]; then
            losers=$(awk -F'\t' -v s="$sym" '$1 == s && $4 == "archive" { print $2 }' "$TMP/defs" | sort -u)
            bad "leg 1: $sym is defined by the link-line object $winner AND by $(echo $losers); the archive member is never extracted, no duplicate symbol is reported, and that backend's own calls bind to the object's definition. Either keep the second definition out of every target image (what the UART class does: its mock is host-only, tests/unit/uartclass), or rename the backend's class symbols in its CMakeLists (what the SPI services do, because t_bus_device_slots needs a mock in the image: target_compile_definitions kos_*=<driver>_*)"
        else
            bad "leg 1: $sym has $n definition sources ($(echo $sources)); which one this link resolves to is archive order"
        fi
        continue
    fi
    # Leg 2: a lone archive definition must have been extracted, or the image is using a
    # definition this inventory never saw.
    kind=$(awk -F'\t' -v s="$sym" '$1 == s { print $4 }' "$TMP/defs" | head -1)
    if [ "$kind" = "archive" ]; then
        member=$(awk -F'\t' -v s="$sym" '$1 == s { print $3 }' "$TMP/defs" | head -1)
        if ! grep -qxF "$member" "$TMP/included"; then
            bad "leg 2: $member holds the only inventoried definition of $sym but never entered the link; something outside this inventory answered the reference"
        fi
    fi
done < "$TMP/class_syms"

if [ "$rc" -eq 0 ]; then
    echo "class_backend: OK ($ndeclared class symbols declared, $nchecked defined in this image, $napp of them on the link line)"
fi
exit "$rc"
