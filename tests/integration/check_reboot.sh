#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The shutdown outcomes past halt, riding rebootdemo. Its bootloader request must come back
# -KOS_ENOSYS on every machine here (the whole path: main's kos_shutdown, the AUTH_SYSTEM and
# AUTH_BOOTLOADER gates, the synchronous console flush, the fallback). Its reset request then
# either declines too and the app shuts down cleanly (`declined`), or restarts the machine
# (`reset`): the image boots again and asks again, so the run ends at its bound with the
# app's first line printed by more than one boot. On an AMP node that does not own the chip's
# reset both requests are refused -KOS_EPERM and the app shuts down cleanly (`refused`).
# Native run for the sim; QEMU when QEMU_MACHINE is set.
#
# rc=-38 is -KOS_ENOSYS and rc=-1 -KOS_EPERM (system/include/kickos/sys/errno.h).

set -u
. "$(dirname "$0")/../lib/gate.sh"

elf="${1:?usage: check_reboot.sh <rebootdemo.elf> declined|reset|refused}"
expect="${2:?usage: check_reboot.sh <rebootdemo.elf> declined|reset|refused}"
# A machine that resets loops until this bound: three boots fit inside it.
: "${QEMU_TIMEOUT:=12}"
# The machine must be let reset rather than stop on the request (gate.sh, boot_args).
KOS_ALLOW_RESET=1

run_image "$elf"

assert_no_panic "panic/fault during rebootdemo"
if [ "$expect" = refused ]; then
    [ "$RC" -eq 0 ] || fail "rebootdemo exit status $RC (expected a clean shutdown)"
    has "reboot declined: rc=-1" || fail "the bootloader request's -KOS_EPERM line is missing"
    has "reset declined: rc=-1" || fail "the reset request's -KOS_EPERM line is missing"
    echo "PASS: both outcomes refused -KOS_EPERM on a node that does not own the reset"
    exit 0
fi
if ! has "reboot declined: rc=-38"; then
    fail "the bootloader request's -KOS_ENOSYS line is missing"
fi
boots=$(printf '%s\n' "$OUT" | grep -c "^\[rebootdemo\] KickOS reboot-to-bootloader demo")
case "$expect" in
    declined)
        [ "$RC" -ne 124 ] || fail "rebootdemo timed out (no exit status forwarded)"
        [ "$RC" -eq 0 ] || fail "rebootdemo exit status $RC (expected a clean shutdown)"
        has "reset declined: rc=-38" || fail "the reset request's -KOS_ENOSYS line is missing"
        echo "PASS: both outcomes declined with -KOS_ENOSYS and the app shut down cleanly"
        ;;
    reset)
        if has "reset declined"; then
            fail "the reset request returned: $(printf '%s\n' "$OUT" | grep -m1 'reset declined')"
        fi
        [ "$boots" -ge 2 ] || fail "the image booted $boots time(s): the reset did not restart it"
        echo "PASS: the bootloader declined -KOS_ENOSYS and the reset booted the image $boots times"
        ;;
    *)
        fail "unknown expectation '$expect'"
        ;;
esac
exit 0
