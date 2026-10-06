#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# No app one of whose compositions names the kernel console as stdout, on any board, writes
# through a raw kernel console write (kos_kconsole_write, or its syscall number), which takes what
# the ring can and leaves the rest to the caller, unless the line ends in a comment that says its
# drop, or the write's answer, is the measurement. Every other line goes through kos_print or
# stdio, which wait out a full ring. An app's compositions are the YAML files under its directory,
# and for an app linking KickOS::system_default the default composition of each board it builds
# on. Planted copies are refused: a raw write with no mark, an empty derivation, an app with no
# composition, and an app with no source.

set -u
. "$(dirname "$0")/../lib/gate.sh"

require_repo_root
scratch_dir
rc=0
DROP_RE='(^|[^A-Za-z0-9_])(kos_kconsole_write[[:space:]]*\(|KOS_SYS_KCONSOLE_WRITE)'
EXEMPT_RE='// (a dropped (beat|line)|its answer) is the measurement$'
KERNEL_RE='^stdout:[[:space:]]*kernel[[:space:]]*$'

# <apps dir> <boards dir>: each app under <apps dir>/<group>/ one of whose compositions has
# `stdout: kernel`, one per line. A group other than `common` is the one board its apps build
# on. Returns 1, naming it, for an app with no composition to read.
kconsole_apps() {
    for _ka_cm in "$1"/*/*/CMakeLists.txt; do
        [ -f "$_ka_cm" ] || continue
        _ka_app=$(dirname "$_ka_cm")
        _ka_group=$(basename "$(dirname "$_ka_app")")
        find "$_ka_app" -type f -name '*.yaml' | sort > "$TMP/yamls"
        if grep -q 'KickOS::system_default' "$_ka_cm"; then
            if [ "$_ka_group" = common ]; then
                for _ka_d in "$2"/*/composition.yaml; do
                    [ -f "$_ka_d" ] && echo "$_ka_d" >> "$TMP/yamls"
                done
            else
                [ -f "$2/$_ka_group/composition.yaml" ] || {
                    echo "$_ka_cm links KickOS::system_default, and $2/$_ka_group has no" \
                         "composition.yaml"
                    return 1
                }
                echo "$2/$_ka_group/composition.yaml" >> "$TMP/yamls"
            fi
        fi
        [ -s "$TMP/yamls" ] || {
            echo "$_ka_cm names no composition under its directory and links no" \
                 "KickOS::system_default"
            return 1
        }
        while read -r _ka_y; do
            if grep -qE "$KERNEL_RE" "$_ka_y"; then
                echo "$_ka_app"
                break
            fi
        done < "$TMP/yamls"
    done
}

# <apps dir> <boards dir>: kconsole_apps into $TMP/apps. Returns 1, saying why, for an app with
# no composition or no app at all.
probes() {
    kconsole_apps "$1" "$2" > "$TMP/apps" || { cat "$TMP/apps"; return 1; }
    [ -s "$TMP/apps" ] || { echo "no app under $1 has a composition with stdout: kernel"; return 1; }
}

# <app dir>: its C/C++ sources, one per line. Returns 1 for an app with none.
app_sources() {
    find "$1" -type f \( -name '*.cc' -o -name '*.c' -o -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) | sort > "$TMP/srcs"
    [ -s "$TMP/srcs" ] || return 1
    cat "$TMP/srcs"
}

# <file>...: each dropping write no exception marks, as <file>:<line>. Exits on a grep error.
dropping() {
    grep -nHE "$DROP_RE" "$@" > "$TMP/drop" 2> "$TMP/drop.err"
    [ $? -le 1 ] || fail "grep could not read the sources: $(cat "$TMP/drop.err")"
    grep -vE "$EXEMPT_RE" "$TMP/drop"
}

# <app dir>: bad for each dropping write in it, and for no source at all.
check_app() {
    if ! app_sources "$1" > "$TMP/app.srcs"; then
        bad "the kernel-console app $1 has no C/C++ source to check"
        return
    fi
    # shellcheck disable=SC2046
    if dropping $(cat "$TMP/app.srcs") > "$TMP/hits"; then
        while read -r hit; do
            bad "$hit writes the kernel console raw, dropping what a full ring cannot take, with no mark saying the drop is the measurement"
        done < "$TMP/hits"
    fi
}

why=$(probes user/apps boards) || fail "$why"
napps=0
while read -r app; do
    check_app "$app"
    napps=$((napps + 1))
done < "$TMP/apps"

# <writer>: inprstorm with its MARKER line through <writer> instead, in $TMP/planted.cc.
plant_marker() {
    awk -v w="$1" '!done && sub(/kos::print\("\[inprstorm\] MARKER/, w "(\"[inprstorm] MARKER") {
             done = 1
         }
         { print }' user/apps/xmc4800-relax/inprstorm/main.cc > "$TMP/planted.cc"
    cmp -s user/apps/xmc4800-relax/inprstorm/main.cc "$TMP/planted.cc" \
        && fail "the planted $1 was not written"
}
plant_marker kos_kconsole_write
dropping "$TMP/planted.cc" > /dev/null || bad "a planted raw write with no mark is not refused"
plant_marker 'arch_syscall(KOS_SYS_KCONSOLE_WRITE, '
dropping "$TMP/planted.cc" > /dev/null || bad "a planted raw syscall with no mark is not refused"
for writer in 'kos_print' 'printf'; do
    plant_marker "$writer"
    if dropping "$TMP/planted.cc" > /dev/null; then
        bad "a planted $writer, which waits out the ring, is taken for a dropping write"
    fi
done
printf 'int f() { printf("x"); puts("x"); return snprintf(b, 4, "x") + ksnprintf(b, 4, "x"); }\n' \
    > "$TMP/stdio.cc"
if dropping "$TMP/stdio.cc" > /dev/null; then
    bad "a stdio write, which waits out the ring, is taken for a dropping one"
fi
printf 'void f() { kos_kconsole_write(s, n); // a dropped line is the measurement\n}\n' \
    > "$TMP/exempt.cc"
if dropping "$TMP/exempt.cc" > /dev/null; then
    bad "a line marked as its drop being the measurement is refused"
fi
printf 'void f() { kos_kconsole_write(s, n); // a dropped line is the measurement, maybe\n}\n' \
    > "$TMP/notexempt.cc"
dropping "$TMP/notexempt.cc" > /dev/null || bad "a mark not ending its line exempts the write"
unread=$( (dropping "$TMP/absent.cc") 2>&1 )
case "$unread" in
    *"grep could not read"*) ;;
    *) bad "a source grep cannot read is read as no dropping write" ;;
esac

A="$TMP/tree/apps"
B="$TMP/tree/boards"
mkdir -p "$A/common/none" "$B/b1" "$B/b2"
printf 'stdout: /svc/console\n' > "$B/b1/composition.yaml"
printf 'stdout: /svc/console\n' > "$B/b2/composition.yaml"
printf 'kickos_app_system(none COMPOSITION system.yaml)\n' > "$A/common/none/CMakeLists.txt"
printf 'version: 1\nstdout: /svc/console\n' > "$A/common/none/system.yaml"
if probes "$A" "$B" > "$TMP/why"; then
    bad "a tree with no kernel-console app is not refused: $(cat "$TMP/apps")"
fi

mkdir -p "$A/common/bare"
printf 'add_executable(bare main.cc)\n' > "$A/common/bare/CMakeLists.txt"
if kconsole_apps "$A" "$B" > "$TMP/derived"; then
    bad "an app with no composition to read is not refused"
fi
rm -r "$A/common/bare"

mkdir -p "$A/b9/orphan"
printf 'target_link_libraries(orphan PRIVATE KickOS::system_default)\n' \
    > "$A/b9/orphan/CMakeLists.txt"
if kconsole_apps "$A" "$B" > "$TMP/derived"; then
    bad "a board app whose board has no default composition is not refused"
fi
rm -r "$A/b9"

mkdir -p "$A/common/dflt" "$A/b1/onb1" "$A/b2/onb2" "$A/common/part/systems/b1" \
    "$A/common/nosrc/systems"
printf 'target_link_libraries(dflt PRIVATE KickOS::kernel KickOS::system_default)\n' \
    > "$A/common/dflt/CMakeLists.txt"
printf 'target_link_libraries(onb1 PRIVATE KickOS::kernel KickOS::system_default)\n' \
    > "$A/b1/onb1/CMakeLists.txt"
printf 'target_link_libraries(onb2 PRIVATE KickOS::kernel KickOS::system_default)\n' \
    > "$A/b2/onb2/CMakeLists.txt"
printf 'kickos_app_system(part PARTITION ${_nodes})\n' > "$A/common/part/CMakeLists.txt"
printf 'version: 1\nstdout: /svc/console\n' > "$A/common/part/systems/b1/node0.yaml"
printf 'version: 1\nstdout: kernel\n' > "$A/common/part/systems/b1/node1.yaml"
printf 'add_executable(nosrc main.cc)\n%s\n' \
    'kickos_app_system(nosrc COMPOSITION systems/${KICKOS_BOARD}.yaml)' \
    > "$A/common/nosrc/CMakeLists.txt"
printf 'version: 1\nstdout: kernel\n' > "$A/common/nosrc/systems/b2.yaml"
printf 'version: 1\nstdout: kernel\n' > "$B/b2/composition.yaml"
kconsole_apps "$A" "$B" > "$TMP/derived" \
    || bad "the planted tree fails the derivation: $(cat "$TMP/derived")"
for want in common/dflt b2/onb2 common/part common/nosrc; do
    grep -qx "$A/$want" "$TMP/derived" || bad "the kernel-console app $want is not derived"
done
for unwanted in b1/onb1 common/none; do
    if grep -qx "$A/$unwanted" "$TMP/derived"; then
        bad "$unwanted, no composition of which has stdout: kernel, is derived"
    fi
done
rc_before=$rc
rc=0
check_app "$A/common/nosrc" 2> /dev/null
planted_rc=$rc
rc=$rc_before
[ "$planted_rc" -ne 0 ] || bad "a kernel-console app with no source passes"

[ "$rc" -eq 0 ] || exit 1
echo "PASS: none of the $napps kernel-console apps writes the kernel console raw without its mark"
