#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# QEMU smoke gate for the armv7m target: boot the `hello` image on a QEMU
# Cortex-M4 (mps2-an386) via semihosting and assert the two userspace threads
# ping-ponged.
#
# THE PLACEHOLDER CLAUSE AND assert_no_panic ARE WEAK ON A MULTI-WRITER POSTURE and neither can
# be made positive here: the banner's core name is per board and this gate reaches boards it
# carries no name for, so "not the placeholder" is all it can say, and a panic in a third thread
# leaves the ping-pong rounds standing. Both are sound on the one-core boards, which is most of
# what registers this gate; on qemu-arm64 above one core a shuffle can silence either.

set -u
. "$(dirname "$0")/../lib/gate.sh"

elf="${1:?usage: check_qemu_hello.sh <hello.elf>}"

# Round 3 of BOTH threads: one thread looping alone is not enough.
poll_image "$elf" "KickOS" "ping 3" "pong 3"

if [ "$POLL_OK" -ne 1 ]; then
    fail "expected banner + ping/pong rounds not observed"
fi

# Presence, then the PLACEHOLDER, so a board this gate reaches needs no core family added here.
# Both banner verbosities are matched.
if ! has_e '(^ *cpu +|^u )[A-Za-z0-9]'; then
    fail "the banner printed no CPU line"
fi
if has_e '(^ *cpu +|^u )unknown'; then
    fail "the banner resolved no core and printed its placeholder"
fi

assert_no_panic "hello panicked while ping-ponging"

echo "PASS: QEMU hello ping-ponged"
exit 0
