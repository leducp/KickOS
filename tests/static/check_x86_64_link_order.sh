#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The order of the objects in every x86_64 application image under a build tree.
#
#   check_x86_64_link_order.sh <build-dir> <pe-script>
#   check_x86_64_link_order.sh --controls
#
# arch/x86/x86_64/pe_image.ld gathers the app half's sections in input order, so the order the
# link reads its objects in decides the image's bytes. CMake places an object a usage
# requirement carries by the depth of the target that carries it, not where it is written, so a
# reshaped leaf moves the app half with no source change. Read from each image's map, its objects
# must come as: the image's own, then the other objects of its app half (its system target's),
# then the C++ runtime's (kickos_cxx_rt), if any. An image's own objects are its target's,
# CMakeFiles/<image>.dir/, but for selftest_rebased, linked from selftest's. The objects
# <pe-script> claims for the kernel by target name land in the kernel half wherever they come,
# and are not ranked.
#
# --controls runs the whole check over build trees and section scripts written here, each
# passed or refused as named.
set -u

fail() { echo "FAIL: x86_64-link-order: $*" >&2; exit 1; }

TMP="$(mktemp -d)" || fail "mktemp failed"
trap 'rm -rf "$TMP"' EXIT INT TERM

# classes <map> <claims-file>: one letter per object the map loads, O the image's own, S
# another of its app half, R the C++ runtime, X one named by a relative path; a kernel-claimed
# object prints nothing.
classes() {
    image="$(basename "$1" .efi.map)"
    if [ "$image" = "selftest_rebased" ]; then
        image=selftest
    fi
    own="$(dirname "$1")/CMakeFiles/$image.dir/"
    sed -n 's/^LOAD \(.*\)$/\1/p' "$1" | while IFS= read -r p; do
        case "$p" in
            *.krel.o) continue ;;
            *.o|*.obj) ;;
            *) continue ;;
        esac
        case "$p" in
            /*) ;;
            *) printf 'X'; continue ;;
        esac
        if printf '%s\n' "$p" | grep -qF -f "$2"; then
            continue
        fi
        case "$p" in
            */kickos_cxx_rt.dir/*) printf 'R' ;;
            "$own"*) printf 'O' ;;
            *) printf 'S' ;;
        esac
    done
}

# check_tree <build-dir> <pe-script>: every map under <build-dir> in order, or a refusal.
check_tree() {
    build="$1"
    pescript="$2"
    [ -d "$build" ] || fail "$build is no directory"
    [ -f "$pescript" ] || fail "$pescript is no file"
    build="$(cd "$build" && pwd)"
    grep -oE '\*[a-z0-9_]+\.dir/' "$pescript" | sed 's|^\*|/|' | sort -u > "$TMP/claims"
    [ -s "$TMP/claims" ] || fail "$pescript claims no object library for the kernel, so every
      kernel object would be ranked as the app's"
    find "$build" -name '*.efi.map' -type f | sort > "$TMP/maps"
    n=0
    n_rt=0
    bad=0
    while IFS= read -r map; do
        c="$(classes "$map" "$TMP/claims")"
        n=$((n + 1))
        case "$c" in
            *X*) fail "$map loads an object by a relative path, so which target's it is cannot
      be told from the map" ;;
            *S*R) n_rt=$((n_rt + 1)) ;;
        esac
        if ! printf '%s\n' "$c" | grep -qE '^O+S*R?$'; then
            echo "FAIL: x86_64-link-order: $map loads its objects as '$c' (O own, S its" \
                 "system's, R the C++ runtime), not own, then system, then runtime" >&2
            bad=1
        fi
    done < "$TMP/maps"
    [ "$n" -gt 0 ] || fail "no application image map under $build: the links did not run"
    [ "$n_rt" -gt 0 ] || fail "no image under $build loads a system target's object and the C++
      runtime's, so the order this pins is not in the corpus"
    [ "$bad" -eq 0 ] || exit 1
    echo "x86_64-link-order: $n image(s) in order, $n_rt with a system target and the C++ runtime"
}

if [ "${1:-}" != "--controls" ]; then
    check_tree "${1:?usage: check_x86_64_link_order.sh <build-dir> <pe-script> | --controls}" \
               "${2:?usage: check_x86_64_link_order.sh <build-dir> <pe-script> | --controls}"
    exit 0
fi

# map <tree> <image> <object>...: the image's map in <tree>/app, its objects in that order. An
# object under CMakeFiles/ is the image directory's, and one starting rel/ is written relative.
map() {
    mkdir -p "$1/app"
    m="$1/app/$2.efi.map"
    : > "$m"
    shift 2
    for o in "$@"; do
        case "$o" in
            CMakeFiles/*) printf 'LOAD %s\n' "$(dirname "$m")/$o" >> "$m" ;;
            rel/*) printf 'LOAD %s\n' "${o#rel/}" >> "$m" ;;
            *) printf 'LOAD %s\n' "$o" >> "$m" ;;
        esac
    done
}

# control <name> pass|refuse <claimed target dir> <setup>: <setup> writes the tree's maps.
control() {
    name="$1"
    want="$2"
    tree="$TMP/control.$name"
    mkdir -p "$tree"
    printf '    *%s*(.text .text.*)\n' "$3" > "$tree.ld"
    "$4" "$tree"
    got=refuse
    if (check_tree "$tree" "$tree.ld") > "$tree.log" 2>&1; then
        got=pass
    fi
    [ "$got" = "$want" ] || fail "control '$name' gave $got, expected $want: $(cat "$tree.log")"
    echo "control $name: $got"
}

sys="/b/CMakeFiles/sys_table.dir/table.c.obj"
boot="/b/CMakeFiles/kickos_x86_64_boot.dir/entry.cc.obj"
rt="/b/CMakeFiles/kickos_cxx_rt.dir/vterminate.cc.obj"
claim="kickos_x86_64_boot.dir/"
other_claim="kickos_x86_64_landed_kernel.dir/"

in_order() { map "$1" app CMakeFiles/app.dir/main.cc.obj "$sys" "$boot" "$rt" /b/lib/libk.a; }
kernel_first() { map "$1" app "$boot" CMakeFiles/app.dir/main.cc.obj "$sys" "$rt"; }
no_runtime() { map "$1" app CMakeFiles/app.dir/main.cc.obj "$sys" "$boot"; }
runtime_first() {
    in_order "$1"
    map "$1" bad CMakeFiles/bad.dir/main.cc.obj "$rt" "$sys" "$boot"
}
system_first() {
    in_order "$1"
    map "$1" bad "$sys" CMakeFiles/bad.dir/main.cc.obj "$boot"
}
another_target() {
    in_order "$1"
    map "$1" bad CMakeFiles/app.dir/main.cc.obj "$sys" "$boot"
}
rebased() {
    in_order "$1"
    map "$1" selftest_rebased CMakeFiles/selftest.dir/main.cc.obj "$sys" "$boot"
}
relative() {
    in_order "$1"
    map "$1" bad CMakeFiles/bad.dir/main.cc.obj rel/b/CMakeFiles/sys_table.dir/table.c.obj
}
no_maps() { :; }

control in_order pass "$claim" in_order
control kernel_first_claimed pass "$claim" kernel_first
control kernel_first_unclaimed refuse "$other_claim" kernel_first
control no_claims refuse "" in_order
control no_runtime_floor refuse "$claim" no_runtime
control runtime_before_system refuse "$claim" runtime_first
control system_before_own refuse "$claim" system_first
control another_targets_objects refuse "$claim" another_target
control rebased_reads_selftest pass "$claim" rebased
control relative_path refuse "$claim" relative
control no_maps refuse "$claim" no_maps
