#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# No app, whatever console its compositions name, writes through a raw kernel console write
# (kos_kconsole_write, or its syscall number), which takes what the ring can and leaves the rest to
# the caller, unless the line ends in a comment that says its drop, or the write's answer, is the
# measurement. Every other line goes through kos_print or stdio, which wait out a full ring. An
# app is a directory under user/apps/<group>/ holding a CMakeLists.txt. Planted copies are
# refused: a raw write with no mark, one in an app whose console is a published driver, an empty
# tree, and an app with no source.

set -u
. "$(dirname "$0")/../lib/gate.sh"

require_repo_root
scratch_dir
rc=0
DROP_RE='(^|[^A-Za-z0-9_])(kos_kconsole_write[[:space:]]*\(|KOS_SYS_KCONSOLE_WRITE)'
EXEMPT_RE='// (a dropped (beat|line)|its answer) is the measurement$'

# <apps dir>: each app under <apps dir>/<group>/, one per line, into $TMP/apps. Returns 1, saying
# why, for a tree with no app.
apps() {
    : > "$TMP/apps"
    for _ap_cm in "$1"/*/*/CMakeLists.txt; do
        [ -f "$_ap_cm" ] || continue
        dirname "$_ap_cm" >> "$TMP/apps"
    done
    [ -s "$TMP/apps" ] || { echo "no app under $1"; return 1; }
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
        bad "the app $1 has no C/C++ source to check"
        return
    fi
    # shellcheck disable=SC2046
    if dropping $(cat "$TMP/app.srcs") > "$TMP/hits"; then
        while read -r hit; do
            bad "$hit writes the kernel console raw, dropping what a full ring cannot take, with no mark saying the drop is the measurement"
        done < "$TMP/hits"
    fi
}

# <apps dir>: check_app over every app under it, their count in $napps.
scan() {
    why=$(apps "$1") || fail "$why"
    napps=0
    while read -r app; do
        check_app "$app"
        napps=$((napps + 1))
    done < "$TMP/apps"
}

scan user/apps

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
mkdir -p "$A"
if apps "$A" > "$TMP/why"; then
    bad "a tree with no app is not refused"
fi
mkdir -p "$A/common/drv" "$A/b1/nosrc"
printf 'kickos_app_system(drv COMPOSITION system.yaml)\n' > "$A/common/drv/CMakeLists.txt"
printf 'version: 1\nstdout: /svc/console\n' > "$A/common/drv/system.yaml"
printf 'void f() { kos_kconsole_write(s, n);\n}\n' > "$A/common/drv/main.cc"
printf 'add_executable(nosrc main.cc)\n' > "$A/b1/nosrc/CMakeLists.txt"
rc_before=$rc
napps_before=$napps
rc=0
scan "$A" 2> "$TMP/planted.err"
planted_rc=$rc
rc=$rc_before
grep -qF "$A/common/drv/main.cc:1:" "$TMP/planted.err" \
    || bad "a raw write with no mark in an app whose console is a published driver is not refused"
grep -qF "the app $A/b1/nosrc has no C/C++ source" "$TMP/planted.err" \
    || bad "an app with no source passes"
[ "$planted_rc" -ne 0 ] || bad "the planted tree passes"
napps=$napps_before

[ "$rc" -eq 0 ] || exit 1
echo "PASS: none of the $napps apps writes the kernel console raw without its mark"
