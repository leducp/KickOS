#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# One boot of libcmix (on QEMU when QEMU_MACHINE is set): main and its peer, two images of one
# system, each linking the newlib profile its CMake names, each run their libc arms in their own
# task. Each names the profile its printf answers to, in main and in a thread it spawns, which
# must be the one asked for.
#
#   check_libcmix.sh <image> --main <full|nano> --peer <full|nano>
#   KOS_CAPTURE=<log> check_libcmix.sh <board-build> <kickos-source> <cmake> --main <p> --peer <p>
#
# The peer's exit() ends its task with status 5, which main reads, then main's exit(0) ends the
# system. main's exit runs its atexit handler, the peer's its static object's destructor.

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=20}"

if judging_capture; then
    shift 3
    judge_capture libcmix
else
    elf="${1:?usage: check_libcmix.sh <image> --main <profile> --peer <profile>}"
    shift
    run_image "$elf"
fi
main_profile=""
peer_profile=""
while [ $# -gt 0 ]; do
    case "$1" in
        --main) main_profile="$2"; shift 2 ;;
        --peer) peer_profile="$2"; shift 2 ;;
        *) fail "unknown argument $1" ;;
    esac
done
[ -n "$main_profile" ] && [ -n "$peer_profile" ] || fail "names no --main and --peer profile"
[ "$main_profile" != "$peer_profile" ] || fail "main and the peer ask for one profile, $main_profile"

# Whether `libcmix <role>: <line>` follows that role's done line.
after_done() {
    printf '%s\n' "$OUT" | sed -n "/^libcmix $1: done\$/,\$p" | grep -q "^libcmix $1: $2\$"
}

assert_no_panic "panic in libcmix"
if has "libcmix [a-z]*: FAIL"; then
    cfail arm "an arm failed: $(printf '%s\n' "$OUT" | grep 'libcmix [a-z]*: FAIL' | head -n 3)"
fi
for role in main peer; do
    want="$main_profile"
    [ "$role" = peer ] && want="$peer_profile"
    has "^libcmix $role: start" || cfail "$role-start" "$role never started"
    has "^libcmix $role: profile $want\$" \
        || cfail "$role-profile" "$role's printf does not answer as the $want profile"
    has "^libcmix $role: thread profile $want\$" \
        || cfail "$role-thread-profile" "$role's spawned thread's printf does not answer as the $want profile"
    for arm in printf profile malloc file_lock thread; do
        has "^libcmix $role: ok $arm\$" || cfail "$role-ok-$arm" "$role never passed its $arm arm"
    done
    has "^libcmix $role: done\$" || cfail "$role-done" "$role never reached its exit()"
done
after_done main atexit || cfail main-atexit "main's exit() ran no atexit handler after its arms"
after_done peer "static destructor" \
    || cfail peer-destructor "the peer's exit() ran no static destructor after its arms"
has "^libcmix main: ok peer_exit\$" || cfail peer-exit "main did not read the peer's exit status 5"
status_clause "main's exit(0) ended the system with status 0" 0
echo "PASS: main ($main_profile) and its peer ($peer_profile) each ran printf, malloc, a FILE lock and exit() in their own profile"
exit 0
