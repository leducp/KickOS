#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Smoke gate for `hello` and `hello_c`: the image booted and its two userspace threads
# ping-ponged, under QEMU or, with KOS_CAPTURE, in a silicon capture.
#
#   check_qemu_hello.sh <hello.elf>
#   KOS_CAPTURE=<log> check_qemu_hello.sh <board-build> <kickos-source> <cmake>
#
# THE PLACEHOLDER CLAUSE AND assert_no_panic ARE WEAK ON A MULTI-WRITER POSTURE and neither can
# be made positive here: the banner's core name is per board and this gate reaches boards it
# carries no name for, so "not the placeholder" is all it can say, and a panic in a third thread
# leaves the ping-pong rounds standing. Both are sound on the one-core boards, which is most of
# what registers this gate; on qemu-arm64 above one core a shuffle can silence either.

set -u
. "$(dirname "$0")/../lib/gate.sh"

# Round 3 of BOTH threads: one thread looping alone is not enough.
if judging_capture; then
    judge_capture hello
    for _round in "ping 3" "pong 3"; do
        if ! has_e "^ *$_round\$"; then
            jfail rounds "no '$_round' in the capture, so the two threads did not ping-pong"
        fi
    done
else
    elf="${1:?usage: check_qemu_hello.sh <hello.elf>}"
    poll_image "$elf" "KickOS" "ping 3" "pong 3"
    if [ "$POLL_OK" -ne 1 ]; then
        fail "expected banner + ping/pong rounds not observed"
    fi
fi

# Presence, then the PLACEHOLDER, so a board this gate reaches needs no core family added here.
# Both banner verbosities are matched.
if ! has_e '(^ *cpu +|^u )[A-Za-z0-9]'; then
    cfail cpu "the banner printed no CPU line"
fi
if has_e '(^ *cpu +|^u )unknown'; then
    cfail cpu "the banner resolved no core and printed its placeholder"
fi

assert_no_panic "hello panicked while ping-ponging"

echo "PASS: hello ping-ponged"
exit 0
