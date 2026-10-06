#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The XMC4800 probes whose composition names the kernel console as stdout print through
# kickos::emit, which waits out a full console ring, and never through a writer that drops what
# the ring cannot take (kos::print, kos_print, a bare kos_kconsole_write). The one exception is a
# line carrying `// a dropped beat is the measurement`. Planted copies are refused: a burst line
# through each dropping writer, an empty derivation, and a probe with no source.

set -u
. "$(dirname "$0")/../lib/gate.sh"

require_repo_root
scratch_dir
rc=0
BOARD_APPS=user/apps/xmc4800-relax
DROP_RE='(^|[^A-Za-z0-9_])(print|kos_print|kos_kconsole_write)[[:space:]]*\('
EXEMPT_RE='// a dropped beat is the measurement$'

# <apps dir>: each app under it one of whose compositions has `stdout: kernel`, one per line.
# Returns 1, naming it, for a composition its CMakeLists names that is not there.
kconsole_apps() {
    for _ka_cm in "$1"/*/CMakeLists.txt; do
        [ -f "$_ka_cm" ] || continue
        _ka_app=$(dirname "$_ka_cm")
        if grep -qE 'kickos_app_system\(.*PARTITION' "$_ka_cm"; then
            echo "$_ka_cm composes a partition, whose stdout this gate does not read"
            return 1
        fi
        for _ka_y in $(sed -nE 's/.*[[:space:](]COMPOSITION[[:space:]]+([^[:space:])]+).*/\1/p' "$_ka_cm"); do
            [ -f "$_ka_app/$_ka_y" ] || { echo "$_ka_cm names the composition $_ka_y, which is not there"; return 1; }
            if grep -qE '^stdout:[[:space:]]*kernel[[:space:]]*$' "$_ka_app/$_ka_y"; then
                echo "$_ka_app"
                break
            fi
        done
    done
}

# <apps dir>: kconsole_apps into $TMP/apps. Returns 1, saying why, for a missing composition or
# no probe at all.
probes() {
    kconsole_apps "$1" > "$TMP/apps" || { cat "$TMP/apps"; return 1; }
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
        bad "the kernel-console probe $1 has no C/C++ source to check"
        return
    fi
    # shellcheck disable=SC2046
    if dropping $(cat "$TMP/app.srcs") > "$TMP/hits"; then
        while read -r hit; do
            bad "$hit prints through a writer that drops a line the full console ring cannot take"
        done < "$TMP/hits"
    fi
}

[ -d "$BOARD_APPS" ] || fail "no apps at $BOARD_APPS"
why=$(probes "$BOARD_APPS") || fail "$why"
while read -r app; do
    check_app "$app"
done < "$TMP/apps"

for writer in 'kos::print' 'kos_print' 'kos_kconsole_write' 'print'; do
    awk -v w="$writer" '!done && sub(/kickos::emit\("\[inprstorm\] MARKER/, w "(\"[inprstorm] MARKER") {
             done = 1
         }
         { print }' user/apps/xmc4800-relax/inprstorm/main.cc > "$TMP/planted.cc"
    cmp -s user/apps/xmc4800-relax/inprstorm/main.cc "$TMP/planted.cc" \
        && fail "the planted $writer was not written"
    dropping "$TMP/planted.cc" > /dev/null || bad "a planted $writer of a judged line is not refused"
done
printf 'int f() { printf("x"); puts("x"); return snprintf(b, 4, "x") + ksnprintf(b, 4, "x"); }\n' \
    > "$TMP/stdio.cc"
if dropping "$TMP/stdio.cc" > /dev/null; then
    bad "a stdio write, which waits out the ring, is taken for a dropping one"
fi
unread=$( (dropping "$TMP/absent.cc") 2>&1 )
case "$unread" in
    *"grep could not read"*) ;;
    *) bad "a source grep cannot read is read as no dropping write" ;;
esac

mkdir -p "$TMP/board/none" "$TMP/board/missing" "$TMP/board/nosrc/sub"
printf 'kickos_app_system(none COMPOSITION system.yaml)\n' > "$TMP/board/none/CMakeLists.txt"
printf 'version: 1\nstdout: /svc/console\n' > "$TMP/board/none/system.yaml"
if probes "$TMP/board" > "$TMP/why"; then
    bad "a board with no kernel-console probe is not refused: $(cat "$TMP/apps")"
fi

printf 'kickos_app_system(missing COMPOSITION systems/gone.yaml)\n' > "$TMP/board/missing/CMakeLists.txt"
if kconsole_apps "$TMP/board" > "$TMP/derived"; then
    bad "a CMakeLists naming a composition that is not there is not refused"
fi
rm -r "$TMP/board/missing"
mkdir -p "$TMP/partboard/ok" "$TMP/partboard/part"
printf 'kickos_app_system(ok COMPOSITION k.yaml)\n' > "$TMP/partboard/ok/CMakeLists.txt"
printf 'version: 1\nstdout: kernel\n' > "$TMP/partboard/ok/k.yaml"
probes "$TMP/partboard" > "$TMP/why" || bad "a board with one kernel-console probe is refused: $(cat "$TMP/why")"
printf 'kickos_app_system(part PARTITION n0.yaml n1.yaml)\n' > "$TMP/partboard/part/CMakeLists.txt"
if probes "$TMP/partboard" > "$TMP/why"; then
    bad "a partition, whose stdout the gate does not read, is not refused"
fi

printf 'add_executable(nosrc main.cc)\nkickos_app_system(nosrc COMPOSITION systems/k.yaml)\n' \
    > "$TMP/board/nosrc/CMakeLists.txt"
mkdir -p "$TMP/board/nosrc/systems"
printf 'version: 1\nstdout: kernel\n' > "$TMP/board/nosrc/systems/k.yaml"
kconsole_apps "$TMP/board" > "$TMP/derived" || bad "the planted board fails the derivation: $(cat "$TMP/derived")"
grep -qx "$TMP/board/nosrc" "$TMP/derived" || bad "a probe whose composition lives under systems/ is not derived"
rc_before=$rc
rc=0
check_app "$TMP/board/nosrc" 2> /dev/null
planted_rc=$rc
rc=$rc_before
[ "$planted_rc" -ne 0 ] || bad "a kernel-console probe with no source passes"

[ "$rc" -eq 0 ] || exit 1
echo "PASS: the kernel-console XMC4800 probes print every judged line through kickos::emit"
