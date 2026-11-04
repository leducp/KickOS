#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Each image of a link takes its C library from the newlib profile it asked for, and from that
# profile alone: every member its link map lists taken from newlib, libstdc++, libsupc++ or the
# KickOS user library comes from that profile's archive, and the libc carve the merged link
# states for the image is that profile's. --self-test reads planted maps.
#
#   check_libc_profile.sh <nm> <image> [--carve <profile>=<bytes>]... <n>=<full|nano>...
#   check_libc_profile.sh --self-test
#
# <image> is the merged ELF; image n's map is <image>.kimg/image<n>.map, and its carve the merged
# ELF's __kickos_image<n>_reent_carve, or where each image is a program of its own its
# <image>.kimg/image<n>.elf's __kickos_reent_carve. The map's headings are matched in English.

set -u
. "$(dirname "$0")/../lib/gate.sh"

# <map>: one line per archive member the map lists taken, `<profile> <archive>`, for the archives
# whose profile is in their name; status 2 when the member list was not read.
profile_members() {
    awk '/^Archive member included/ { on = 1; next }
         on && /^(Discarded input sections|Memory Configuration|Allocating common symbols)/ { on = 0 }
         on && /^[^ \t]/ && match($0, /lib[A-Za-z0-9_+]+\.a\(/) {
             a = substr($0, RSTART, RLENGTH - 1)
             if (a ~ /^lib(c|g|stdc\+\+|supc\+\+)_nano\.a$/ || a == "libkickos_user_nano.a") {
                 print "nano " a
             } else if (a ~ /^lib(c|g|stdc\+\+|supc\+\+)\.a$/ || a == "libkickos_user_full.a") {
                 print "full " a
             } else if (a == "libkickos_kernel.a" || a == "libkickos_lib.a") {
                 print "kernel " a
             }
         }' "$1" | sort -u > "$TMP/members"
    grep -q '^[a-z]* libc\(_nano\)\?\.a$' "$TMP/members" || return 2
    return 0
}

# <map> <profile> <what>: refuses an image whose libc or user library is not <profile> alone.
judge_map() {
    profile_members "$1" || fail "$3: no libc member read from $1, so its profile is unknown"
    if ! grep -q "^[a-z]* libkickos_user_\(full\|nano\)\.a$" "$TMP/members"; then
        fail "$3: $1 takes no member of the KickOS user library"
    fi
    _jm_other=$(grep -v "^$2 \|^kernel " "$TMP/members" | sed 's/^[a-z]* //' | tr '\n' ' ')
    [ -z "$_jm_other" ] \
        || fail "$3 asks for the $2 profile and links members of $_jm_other"
}

# <nm> <image> <n> <profile> <bytes>: refuses image n of <image> unless its link states a libc carve
# of <bytes>, the <profile> profile's.
judge_carve() {
    [ -n "$5" ] || fail "image $3 asks for the $4 profile and no --carve names that profile's"
    if [ -f "$2.kimg/image$3.elf" ]; then
        _jc_stated=$("$1" "$2.kimg/image$3.elf" | sed -n 's/^\([0-9a-f]*\) A _*__kickos_reent_carve$/\1/p')
    else
        _jc_stated=$("$1" "$2" | sed -n "s/^\\([0-9a-f]*\\) [A-Za-z] _*__kickos_image${3}_reent_carve\$/\\1/p")
    fi
    [ -n "$_jc_stated" ] || fail "image $3 of $2 states no libc carve"
    [ $((0x$_jc_stated)) -eq "$5" ] \
        || fail "image $3 asks for the $4 profile and its link states a libc carve of $((0x$_jc_stated)), not $5"
}

scratch_dir

if [ "${1:-}" = --self-test ]; then
    plant() { # <name> <archive>...
        _pl_out="$TMP/$1.map"
        shift
        {
            echo 'Archive member included to satisfy reference by file (symbol)'
            echo
            for _pl_a in "$@"; do
                echo "/opt/gcc/lib/thumb/$_pl_a(member.o)"
                echo '                              main.o (symbol)'
            done
            echo
            echo 'Discarded input sections'
            echo
            echo '/opt/gcc/lib/thumb/libc.a(other.o) .text.unused'
        } > "$_pl_out"
    }
    refused() { # <name> <profile> <text>
        if (judge_map "$TMP/$1.map" "$2" "planted $1") > "$TMP/run.out" 2>&1; then
            fail "the planted $1 map passed as $2"
        fi
        grep -qF -- "$3" "$TMP/run.out" \
            || fail "the planted $1 map was refused without '$3': $(tail -n 1 "$TMP/run.out")"
    }
    plant nano libc_nano.a libstdc++_nano.a libkickos_user_nano.a libkickos_kernel.a
    (judge_map "$TMP/nano.map" nano "planted nano") || fail "a nano image's map was refused"
    plant full libc.a libsupc++.a libkickos_user_full.a
    (judge_map "$TMP/full.map" full "planted full") || fail "a full image's map was refused"
    refused nano full "links members of libc_nano.a"
    plant both libc.a libc_nano.a libkickos_user_full.a
    refused both full "links members of libc_nano.a"
    plant user libc_nano.a libkickos_user_full.a
    refused user nano "links members of libkickos_user_full.a"
    plant cxx libc.a libstdc++_nano.a libkickos_user_full.a
    refused cxx full "links members of libstdc++_nano.a"
    plant nouser libc.a
    refused nouser full "takes no member of the KickOS user library"
    plant blind libkickos_user_full.a
    refused blind full "no libc member read"

    # An nm that prints the planted listing beside the file it is given.
    printf '#!/bin/sh\ncat "$1.nm"\n' > "$TMP/nm"
    chmod +x "$TMP/nm"
    : > "$TMP/merged"
    printf '%s\n' '000000a0 A __kickos_image1_reent_carve' '00000140 A __kickos_image2_reent_carve' \
        '000000c0 a __kickos_image3_reent_carve' > "$TMP/merged.nm"
    mkdir -p "$TMP/program.kimg"
    : > "$TMP/program"
    : > "$TMP/program.kimg/image1.elf"
    printf '%s\n' '00000160 A __kickos_reent_carve' > "$TMP/program.kimg/image1.elf.nm"
    : > "$TMP/program.kimg/image2.elf"
    printf '%s\n' '00000160 T __kickos_reent_carve' > "$TMP/program.kimg/image2.elf.nm"
    carve_refused() { # <image> <n> <want> <text>
        if (judge_carve "$TMP/nm" "$TMP/$1" "$2" nano "$3") > "$TMP/run.out" 2>&1; then
            fail "image $2 of the planted $1 passed a carve of '$3'"
        fi
        grep -qF -- "$4" "$TMP/run.out" \
            || fail "image $2 of the planted $1 was refused without '$4': $(tail -n 1 "$TMP/run.out")"
    }
    (judge_carve "$TMP/nm" "$TMP/merged" 1 nano 160) || fail "a merged link's stated carve was refused"
    (judge_carve "$TMP/nm" "$TMP/merged" 2 nano 320) || fail "a merged link's second carve was refused"
    (judge_carve "$TMP/nm" "$TMP/program" 1 nano 352) || fail "a program's stated carve was refused"
    carve_refused merged 1 161 "states a libc carve of 160, not 161"
    carve_refused merged 2 160 "states a libc carve of 320, not 160"
    (judge_carve "$TMP/nm" "$TMP/merged" 3 nano 192) || fail "a merged link's local carve was refused"
    carve_refused merged 4 160 "states no libc carve"
    carve_refused program 2 352 "states no libc carve"
    carve_refused merged 1 "" "no --carve names that profile's"
    echo "PASS: the read refuses an image linking the other profile's libc, libstdc++ or user"
    echo "  library, or both profiles, one taking no user library, a map it reads no libc in, and"
    echo "  a carve that is not its profile's, a program's carve that is not absolute, another"
    echo "  image's carve, and a profile with no carve"
    exit 0
fi

nm_tool="${1:?usage: check_libc_profile.sh <nm> <image> [--carve <profile>=<bytes>]... <n>=<profile>...}"
image="${2:?usage: check_libc_profile.sh <nm> <image> [--carve <profile>=<bytes>]... <n>=<profile>...}"
shift 2
[ -f "$image" ] || fail "no image at $image"
carves=""
judged=0
while [ $# -gt 0 ]; do
    case "$1" in
        --carve) carves="$carves $2"; shift 2; continue ;;
        [0-9]*=full|[0-9]*=nano) ;;
        *) fail "not <n>=<full|nano>: $1" ;;
    esac
    n="${1%%=*}"
    profile="${1#*=}"
    shift
    map="$image.kimg/image$n.map"
    [ -f "$map" ] || fail "image $n of $image has no link map at $map"
    judge_map "$map" "$profile" "image $n"
    want=""
    for c in $carves; do
        [ "${c%%=*}" = "$profile" ] && want="${c#*=}"
    done
    judge_carve "$nm_tool" "$image" "$n" "$profile" "$want"
    judged=$((judged + 1))
done
[ "$judged" -gt 0 ] || fail "names no image to judge"
echo "PASS: each of the $judged image(s) of $(basename "$image") takes its C library, C++ library and KickOS user library from its own newlib profile alone"
exit 0
