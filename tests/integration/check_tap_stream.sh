#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Verdict on a completed TAP stream, read from stdin. Shared by every selftest gate.
#
# EXPECT_SKIPS and EXPECT_PARTIALS (both default empty) are permission sets, not budgets:
# a skip or a partial whose name is not listed fails the gate, and a listed name that did
# not skip / go partial is a NOTE, never a failure.
#
# A PARTIAL is an arm that ran its invariant and left a sub-case unexercised on this
# board. It reports `ok`, so no plan/case reconciliation can see it and only the by-name
# set can. That matters because a mechanism regression makes an arm take its PARTIAL
# early return on EVERY board at once, which without this set is a green run with the
# contract gone.
#
# <expected-arms> is what makes the suite non-vacuous. tap.cc plans `1..N` from the
# RUNTIME registry, so a deleted arm shrinks the plan and the case count in lockstep and
# no self-consistent parse can see it. The caller owns the number because five arms are
# #if-conditional and the total is therefore per-posture.
#
# usage: <tap stream> | check_tap_stream.sh <label> <expected-arms>

set -u
. "$(dirname "$0")/../lib/gate.sh"
label="${1:?usage: check_tap_stream.sh <label> <expected-arms>}"
want_arms="${2:?usage: check_tap_stream.sh <label> <expected-arms>}"
# A KICKOS_CONSOLE_CRLF board emits CR, which defeats the end-anchored parses below.
out="$(tr -d '\r')"

# A raw CMake list arrives semicolon-separated. Flattened to single-space separation
# because the membership tests below are `case` globs against " $name ".
expect_skips="$(printf '%s' "${EXPECT_SKIPS:-}" | tr ',;\t\n' '    ')"
expect_partials="$(printf '%s' "${EXPECT_PARTIALS:-}" | tr ',;\t\n' '    ')"

if echo "$out" | grep -q "not ok"; then
    echo "$out" | grep "not ok"
    fail "a TAP test reported not ok"
fi
if ! echo "$out" | grep -q "# all tests passed"; then
    fail "TAP completion marker missing (crash / hang / truncated run?)"
fi
# THE SELFTEST NEVER FAULTS. The deliberate cross-domain fault is a separate binary
# (faultsurvive), so a thread-fault record in this stream is an arm whose thread died the
# wrong way -- and thread-scoped isolation means it dies anyway, so every plan, case and
# directive check above still reconciles and the run reads green. A slay redirect that
# rebuilds an UNPRIVILEGED context faults the stub on its first kernel access, is caught
# by kickos_fault_kill_thread, and reaches the same observable end state as a correct one.
if echo "$out" | grep -q "=== THREAD FAULT ==="; then
    echo "$out" | grep "=== THREAD FAULT ==="
    fail "a thread faulted during the suite: this stream's arms must never fault"
fi

# Parsed after the completion marker so a truncated run is reported as truncated.
plan="$(echo "$out" | sed -n 's/^1\.\.\([0-9][0-9]*\)$/\1/p' | tail -1)"
if [ -z "$plan" ]; then
    fail "no '1..N' plan line in the TAP stream (the whole stream was dropped?)"
fi
cases="$(echo "$out" | grep -c '^\(not \)\?ok [0-9]')"
if [ "$plan" -ne "$cases" ]; then
    fail "TAP plan claims $plan case(s) but $cases were reported"
fi
if [ "$plan" -ne "$want_arms" ]; then
    fail "TAP plan is $plan, expected exactly $want_arms: an arm was added or deleted"
fi

# One directive class. The harness spells both as a passing case carrying a directive,
# `ok <n> - <name> # <DIRECTIVE> <reason>`, plus a matching `# <label>: N` summary line;
# parse and permission are identical, only the tokens differ. Sets N to the count.
check_directive() { # <DIRECTIVE> <summary-label> <permitted names>
    _dir="$1"
    _label="$2"
    _expect="$3"

    N="$(echo "$out" | sed -n "s/^# $_label: \([0-9][0-9]*\)\$/\1/p" | tail -1)"
    if [ -z "$N" ]; then
        fail "no '# $_label: N' summary in the TAP stream (harness regression?)"
    fi

    _names="$(echo "$out" \
        | sed -n "s/^ok [0-9][0-9]* - \([A-Za-z0-9_]*\) # $_dir.*/\1/p" \
        | tr '\n' ' ')"
    _parsed=0
    for _x in $_names; do _parsed=$((_parsed + 1)); done

    # If the parse and the harness disagree, the permission set below is checking nothing.
    if [ "$_parsed" -ne "$N" ]; then
        echo "FAIL: harness reported $N $_label but $_parsed $_dir line(s) parsed."
        echo "      The 'ok N - name # $_dir' format moved, so the expected-$_label set is vacuous."
        echo "$out" | grep "# $_dir"
        exit 1
    fi

    _unexpected=""
    for _x in $_names; do
        case " $_expect " in
            *" $_x "*) ;;
            *) _unexpected="$_unexpected $_x" ;;
        esac
    done
    if [ -n "$_unexpected" ]; then
        echo "FAIL: $_dir not on the expected list:$_unexpected"
        echo "      expected:${_expect:+ $_expect}"
        echo "$out" | grep "# $_dir"
        exit 1
    fi

    for _x in $_expect; do
        case " $_names " in
            *" $_x "*) ;;
            *) echo "NOTE: '$_x' is on the expected-$_label list but no arm reported it; trim it" ;;
        esac
    done
}

check_directive SKIP skipped "$expect_skips"
skipped="$N"
check_directive PARTIAL partial "$expect_partials"
partial="$N"

echo "PASS: $label TAP suite clean ($plan arms, $skipped skipped, $partial partial, all expected)"
exit 0
