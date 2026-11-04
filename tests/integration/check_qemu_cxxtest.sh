#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Gate for the full-C++ opt-in: boot the cxxtest image on QEMU via semihosting, or read a
# silicon capture of it, and assert that exceptions, STL and RTTI all executed (every check
# printed PASS, and the "ALL PASS" summary). Proves the toolchain libstdc++/libsupc++ over
# newlib runs on the target ISA.
#
#   check_qemu_cxxtest.sh <cxxtest.elf>
#   KOS_CAPTURE=<log> check_qemu_cxxtest.sh <board-build> <kickos-source> <cmake>
#
# getentropy has a source only on an x86_64 processor model offering RDSEED; no capture is of one.

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=15}"

entropy=none
if judging_capture; then
    judge_capture cxxtest
else
    elf="${1:?usage: check_qemu_cxxtest.sh <cxxtest.elf>}"
    case ",${KICKOS_X86_64_CPU:-}," in
        *,+rdseed,*) entropy=rdseed ;;
    esac
    need_qemu_machine
    run_image "$elf"
fi

assert_no_panic "the image faulted during the full-C++ checks"
if has_e "FAIL:|SOME FAILED"; then
    cfail error "a full-C++ check failed"
fi
if ! has "ALL PASS"; then
    cfail verdict "'ALL PASS' summary not observed (image did not complete)"
fi
# Every check that every board compiles, by its line: one compiled out or never reached leaves
# "ALL PASS" standing.
need_arms() {
    while IFS= read -r arm; do
        if ! has_f "PASS: $arm"; then
            cfail arm "no PASS line for: $arm"
        fi
    done
}
if [ "$entropy" = rdseed ]; then
    need_arms <<'ARMS'
getentropy answers a request from its source
getentropy fills two requests with different bytes
getentropy serves 256 bytes and refuses 257 with EINVAL
random_device constructs and draws from getentropy
ARMS
else
    need_arms <<'ARMS'
getentropy refuses with ENOSYS
random_device refuses to construct with no source
ARMS
fi
need_arms <<'ARMS'
exception caught by exact type
exception caught by std::exception base
stack unwind runs local dtors
std::vector<int> push_back + sum == 5050
std::string concat == KickOS/full-c++
virtual dispatch picks override
dynamic_cast to real type succeeds
dynamic_cast to wrong type yields nullptr
typeid reflects dynamic type
two threads' first throws at once are both caught
a static two threads reach at once is built once
a contended call_once runs once and every caller sees its effect
a call_once whose function threw runs again, then never
a shared_ptr two threads copy and drop keeps its count
two std::threads take turns over a condition variable
a timed condition wait ends at its deadline
a timed lock of a held mutex ends at its deadline
a recursive timed lock waits until every level is released
a recursive wait held two deep gives both levels back
a thread inheriting its priority ignores the one it names
each thread rethrows its own caught exception while another holds one
each thread counts only its own uncaught exception
a deleted key's index serves a new key
a value a deleted key left behind is never the new key's
a thread a non-creator joined has its slot free again
a join returns for a thread that ended by kos_exit
a join returns for a thread its creator cancelled
a thread_local destructor a key destructor registers runs
a detached thread that starts and detaches threads keeps its own record
a detached thread that returned leaves no record behind
hardware_concurrency counts the task's cores
ARMS

echo "PASS: full-C++ exceptions/STL/RTTI executed"
exit 0
