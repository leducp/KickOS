#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Every libc system call an image makes reaches KickOS's porting layer (user/src/newlib_stubs.cc):
# every archive member the link map lists taken comes from KickOS or the compiler's own runtime, so
# no libnosys or other libgloss stub is. A libnosys write drops stdout without a word, and the image
# still runs. --images reads every map a list file names, one per line. --self-test reads planted
# maps.
#
# The map's headings are matched in English: a binutils built with NLS writes them in the link's
# locale, which fails this read loudly as a member list not read.
#
#   check_no_libnosys.sh <map>...
#   check_no_libnosys.sh --images <list>
#   check_no_libnosys.sh --self-test

set -u
. "$(dirname "$0")/../lib/gate.sh"

# Matched by file name alone, so a stub archive named like one of these passes.
RUNTIME_ARCHIVES='(^|/)lib(kickos_[A-Za-z0-9_]+|(c|g|m|gcc|gcc_eh|stdc\+\+|supc\+\+)(_nano)?)\.a\('

# <map> <out>: each member the map lists taken from an archive outside RUNTIME_ARCHIVES, with the
# symbol that took it; status 2 when the member list was not read. A member line is the member,
# then on that line or the next the file and symbol that took it. A path may hold spaces, so the
# member ends at its first `.a(...)`.
nosys_members() {
    awk '/^Archive member included/ { on = 1; next }
         on && /^(Discarded input sections|Memory Configuration|Allocating common symbols)/ { exit }
         on && /^[^ \t]/ {
             if (!match($0, /^[^(]*\.a\([^)]*\)/)) { member = ""; next }
             member = substr($0, 1, RLENGTH)
             rest = substr($0, RLENGTH + 1)
             if (rest ~ /\([^()]*\)[ \t]*$/) { print member " <- " sym(rest); member = "" }
             next
         }
         on && member != "" { print member " <- " sym($0); member = "" }
         function sym(line) {
             match(line, /\([^()]*\)[ \t]*$/)
             return substr(line, RSTART, RLENGTH)
         }' \
        "$1" | sed 's/[[:space:]]*$//' > "$TMP/members"
    # An image takes kickos_user and the kernel from archives, so an empty list is a map this read
    # no longer understands.
    grep -q 'libkickos_kernel\.a(' "$TMP/members" || return 2
    grep -vE "$RUNTIME_ARCHIVES" "$TMP/members" > "$2"
    return 0
}

scratch_dir

if [ "${1:-}" = --self-test ]; then
    # <name> <member line>...: a map taking the kernel archive's member and each named one, each
    # with its reference on the next line, or on its own line when it already carries one.
    plant() {
        _pl_out="$TMP/$1.map"
        shift
        {
            echo 'Archive member included to satisfy reference by file (symbol)'
            echo
            echo '/opt/kickos/lib/libkickos_kernel.a(kmain.cc.obj)'
            echo '                              main.c.obj (kmain)'
            for _pl_m in "$@"; do
                echo "$_pl_m"
                case "$_pl_m" in
                    *')'*' ('*')') ;;
                    *) printf '%30s%s\n' '' '/opt/gcc/lib/libc.a(libc_a-writer.o) (write)' ;;
                esac
            done
            echo
            echo 'Discarded input sections'
            echo
            echo '/opt/gcc/lib/libnosys.a(write.o) .text.unused'
        } > "$_pl_out"
    }
    # <map> <expected member and symbol>: the read finds exactly that.
    finds() {
        nosys_members "$TMP/$1.map" "$TMP/nosys" \
            || fail "the planted $1 map's member list was not read"
        grep -qxF -- "$2" "$TMP/nosys" \
            || fail "the read misses '$2' in the planted $1 map: $(cat "$TMP/nosys")"
    }
    plant none '/opt/kickos/lib/libkickos_user.a(newlib_stubs.cc.obj)' \
        '/opt/gcc/lib/thumb/libc_nano.a(lib_a-printf.o)' '/opt/gcc/lib/libstdc++.a(new_op.o)' \
        '/opt/gcc/lib/libgcc.a(_udivsi3.o)'
    nosys_members "$TMP/none.map" "$TMP/nosys" || fail "the planted map's member list was not read"
    if [ -s "$TMP/nosys" ]; then
        fail "the read finds a stub member in a map taking none"
    fi
    plant nosys '/opt/gcc/lib/rxv3/libnosys.a(write.o)'
    finds nosys '/opt/gcc/lib/rxv3/libnosys.a(write.o) <- (write)'
    plant oneline 'libnosys.a(write.o)           main.o (write)'
    if grep -q 'libc_a-writer' "$TMP/oneline.map"; then
        fail "the planted one-line member gained a reference line of its own"
    fi
    finds oneline 'libnosys.a(write.o) <- (write)'
    plant spaced '/opt/my tools/lib/libnosys.a(write.o)'
    finds spaced '/opt/my tools/lib/libnosys.a(write.o) <- (write)'
    plant sim '/opt/gcc/lib/rxv3/libsim.a(write.o)'
    finds sim '/opt/gcc/lib/rxv3/libsim.a(write.o) <- (write)'
    plant rdimon '/opt/gcc/lib/thumb/librdimon.a(rdimon-syscalls.o)'
    finds rdimon '/opt/gcc/lib/thumb/librdimon.a(rdimon-syscalls.o) <- (write)'
    plant unlisted '/opt/vendor/lib/libcs3.a(write.o)'
    finds unlisted '/opt/vendor/lib/libcs3.a(write.o) <- (write)'
    plant lookalike '/opt/gcc/lib/libcrdimon.a(write.o)'
    finds lookalike '/opt/gcc/lib/libcrdimon.a(write.o) <- (write)'
    grep -v 'libkickos_kernel' "$TMP/none.map" > "$TMP/unread.map"
    if nosys_members "$TMP/unread.map" "$TMP/nosys"; then
        fail "a map taking no kernel member was read as a member list"
    fi
    # --images, the form every build runs: a list naming a missing map, and an empty list.
    # <list> <refusal text> <what>: --images refuses the list with that text.
    refuses() {
        if sh "$0" --images "$1" > "$TMP/run.out" 2>&1; then
            fail "--images passed $3"
        fi
        grep -qF -- "$2" "$TMP/run.out" \
            || fail "--images refused $3 without '$2': $(tail -n 1 "$TMP/run.out")"
    }
    echo "$TMP/absent.map" > "$TMP/absent.list"
    refuses "$TMP/absent.list" "no link map at $TMP/absent.map" \
        "a list naming a map that does not exist"
    : > "$TMP/empty.list"
    refuses "$TMP/empty.list" "names no image" "a list naming no image"
    echo "$TMP/nosys.map" > "$TMP/nosys.list"
    refuses "$TMP/nosys.list" "not on the runtime list" "a map taking a libnosys member"
    echo "PASS: the read finds a member of an archive off the runtime list in either map form,"
    echo "  through a spaced path, and none in a map taking only runtime archives; --images"
    echo "  refuses a missing map, an empty list and a stub member, each by name"
    exit 0
fi

if [ "${1:-}" = --images ]; then
    [ "$#" -eq 2 ] || fail "usage: check_no_libnosys.sh --images <list>"
    [ -f "$2" ] || fail "no image list at $2"
    grep -v '^$' "$2" > "$TMP/maps"
    [ -s "$TMP/maps" ] || fail "$2 names no image"
else
    [ "$#" -ge 1 ] || fail "usage: check_no_libnosys.sh <map>..."
    printf '%s\n' "$@" > "$TMP/maps"
fi
COUNT=0
BAD=0
while IFS= read -r MAP; do
    [ -f "$MAP" ] || fail "no link map at $MAP"
    nosys_members "$MAP" "$TMP/nosys" \
        || fail "$MAP lists no member of the kernel archive taken, so the member list was not read"
    COUNT=$((COUNT + 1))
    if [ -s "$TMP/nosys" ]; then
        echo "$(basename "$MAP"):" >&2
        sed 's/^/  /' "$TMP/nosys" >&2
        BAD=$((BAD + 1))
    fi
done < "$TMP/maps"
if [ "$BAD" -ne 0 ]; then
    fail "$BAD of $COUNT image(s) take a member of an archive not on the runtime list: one
  outside KickOS and the compiler's runtime answered a symbol, which KickOS must define first"
fi
echo "PASS: none of $COUNT image(s) takes a member of an archive outside KickOS and the runtime"
