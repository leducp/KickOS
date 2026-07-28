#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# QEMU TAP gate: boot the `selftest` image on QEMU (semihosting console), let the
# TAP suite run to completion, and assert a clean run -- "# all tests passed" with
# no "not ok". This is the SAME binary/suite that runs on the sim, now exercised on
# real ISA mechanism (armv7m/armv6m PendSV, rv32imac msip). selftest self-terminates
# (arch_shutdown when done), so QEMU_TIMEOUT is only a hang backstop.
#
# EXPECTED SKIPS (EXPECT_SKIPS, default empty). A test whose board cannot host it
# reports a real TAP skip -- `ok N - name # SKIP reason` -- and the harness prints
# `# skipped: N`. A skip is NOT a pass, so it must not be free: the default empty list
# makes EVERY registered test required, and a board that legitimately cannot run some
# of them NAMES them at its call site (microbit's 2-thread pool, and the tests whose
# subject is the privileged posture on a KICKOS_ROOT_PRIVILEGED=OFF image). The harness
# cannot make this judgement -- only the board can. This is the hole that let the mutex
# / domain_share suites skip themselves on microbit for milestones while CI stayed green.
#
# NAMES, not a number, and that is the point. A budget of N admits ANY N skips, so a
# test that silently stopped running is indistinguishable from one the board genuinely
# cannot host -- it just takes a slot someone else vacated. The list is a permission
# set: a skip whose name is not on it fails the gate and is printed.
#
# An expected skip that did NOT occur is reported as a NOTE, not a failure. Fixing a
# test so it stops skipping must not turn CI red; the note is what stops the list from
# quietly rotting into a description of a state that no longer exists.
#
# The parsed names are cross-checked against the harness's own `# skipped: N` count. A
# mismatch means the SKIP line format moved out from under the parse, which would make
# the whole list vacuous, so it is a failure rather than a silent pass.

set -u
elf="${1:?usage: check_qemu_selftest.sh <selftest.elf>}"
qemu="${QEMU:-qemu-system-arm}"
machine="${QEMU_MACHINE:-mps2-an386}"
extra_arg="${QEMU_EXTRA:-}"             # e.g. -bios none (RISC-V virt)
# Accept commas, semicolons (a raw CMake list) or whitespace, flattened to single-space
# separation so the `case` globs below can match on " $name ".
expect_skips="$(printf '%s' "${EXPECT_SKIPS:-}" | tr ',;\t\n' '    ')"

if ! command -v "$qemu" >/dev/null 2>&1; then
    # Exit 77 -> CTest SKIP (not PASS), so a QEMU-less box doesn't green-light it.
    echo "SKIP: $qemu not found"
    exit 77
fi

# Strip CR: a board with KICKOS_CONSOLE_CRLF cooks '\n' on the wire, which would
# otherwise defeat the end-anchored parse of the summary line below.
out="$(timeout "${QEMU_TIMEOUT:-30}" "$qemu" -M "$machine" $extra_arg -nographic -semihosting -kernel "$elf" 2>&1 | tr -d '\r')"
echo "$out"

if echo "$out" | grep -q "not ok"; then
    echo "FAIL: a TAP test reported not ok"
    exit 1
fi
if ! echo "$out" | grep -q "# all tests passed"; then
    echo "FAIL: TAP completion marker missing (crash / hang / truncated run?)"
    exit 1
fi

# Parsed AFTER the completion marker, so a truncated run is reported as truncated
# rather than as a missing skip count. run_all() emits this line unconditionally
# (zero included), so absence here means the harness itself regressed.
skipped="$(echo "$out" | sed -n 's/^# skipped: \([0-9][0-9]*\)$/\1/p' | tail -1)"
if [ -z "$skipped" ]; then
    echo "FAIL: no '# skipped: N' summary in the TAP stream (harness regression?)"
    exit 1
fi

# The names behind that count. TAP spells a skip as a passing case with a directive:
# `ok <n> - <name> # SKIP <reason>`.
# Flattened to single-space separation: both membership tests below are `case` globs
# against " $list ", which cannot match across the newlines sed emits.
skipped_names="$(echo "$out" \
    | sed -n 's/^ok [0-9][0-9]* - \([A-Za-z0-9_]*\) # SKIP.*/\1/p' \
    | tr '\n' ' ')"
parsed=0
for _n in $skipped_names; do parsed=$((parsed + 1)); done

# Anti-vacuity: if the parse and the harness disagree, the list below is checking
# nothing. Fail rather than pass an unexamined run.
if [ "$parsed" -ne "$skipped" ]; then
    echo "FAIL: harness reported $skipped skip(s) but $parsed SKIP line(s) parsed."
    echo "      The 'ok N - name # SKIP' format moved; the expected-skip list is vacuous."
    echo "$out" | grep "# SKIP"
    exit 1
fi

unexpected=""
for n in $skipped_names; do
    case " $expect_skips " in
        *" $n "*) ;;
        *) unexpected="$unexpected $n" ;;
    esac
done
if [ -n "$unexpected" ]; then
    echo "FAIL: skip(s) not on the expected list:$unexpected"
    echo "      expected:${expect_skips:+ $expect_skips}"
    echo "$out" | grep "# SKIP"
    exit 1
fi

# Listed but not skipped: the good direction (something got fixed, or a board grew a
# capability). Reported so the list can be trimmed, never failed.
for e in $expect_skips; do
    case " $skipped_names " in
        *" $e "*) ;;
        *) echo "NOTE: '$e' is on the expected-skip list but did not skip -- trim it" ;;
    esac
done

echo "PASS: selftest TAP suite clean ($skipped skipped, all expected)"
exit 0
