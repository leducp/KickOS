#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Verdict on a completed TAP stream, read from stdin. Shared by every selftest gate.
#
# EXPECT_SKIPS, EXPECT_PARTIALS and EXPECT_FAULTS (all default empty) are permission sets,
# not budgets: a skip, a partial or a faulting thread whose name is not listed fails the
# gate, and a listed name that did not skip / go partial / fault is a NOTE, never a failure.
#
# A PARTIAL is an arm that ran its invariant and left a sub-case unexercised on this
# board. It reports `ok`, so no plan/case reconciliation can see it and only the by-name
# set can. That matters because a mechanism regression makes an arm take its PARTIAL
# early return on EVERY board at once, which without this set is a green run with the
# contract gone.
#
# <expected-arms> is what makes the suite non-vacuous. tap.cc plans `1..N` from the
# RUNTIME registry, so a deleted arm shrinks the plan and the case count in lockstep and
# no self-consistent parse can see it. The caller owns the number because a large minority
# of the arms are #if-conditional (posture, MPU, self-test syscalls) and the split image
# cuts the set again by KICKOS_SELFTEST_PART, so the total is per-posture AND per-image.
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
expect_faults="$(printf '%s' "${EXPECT_FAULTS:-}" | tr ',;\t\n' '    ')"

if echo "$out" | grep -q "not ok"; then
    echo "$out" | grep "not ok"
    fail "a TAP test reported not ok"
fi
if ! echo "$out" | grep -q "# all tests passed"; then
    fail "TAP completion marker missing (crash / hang / truncated run?)"
fi
# ONLY A NAMED THREAD MAY FAULT. A thread-fault record from any other thread is an arm whose
# thread died the wrong way, and thread-scoped isolation means the plan, case and directive
# checks above still reconcile and read green, so only this clause can see it. A slay redirect
# that rebuilds an UNPRIVILEGED context faults the stub on its first kernel access and reaches
# the same observable end state as a correct one.
#
# The list is by THREAD and not by arm, because the record names the thread and nothing else:
# the containment arm's own worker is the one deliberate fault in the stream (F5, T8), and
# every neighbouring arm's worker stays forbidden. A listed thread that did NOT fault is a
# NOTE here rather than a failure: the arm's join is what asserts the death, and a fault that
# never happened times that join out and reports `not ok` above.
_faulted="$(echo "$out" \
    | sed -n "s/.*=== THREAD FAULT === thread '\([^']*\)'.*/\1/p")"
_badfault=""
for _t in $_faulted; do
    case " $expect_faults " in
        *" $_t "*) ;;
        *) _badfault="$_badfault $_t" ;;
    esac
done
if [ -n "$_badfault" ]; then
    echo "$out" | grep "=== THREAD FAULT ==="
    echo "      expected to fault:${expect_faults:+ $expect_faults}"
    fail "thread(s)$_badfault faulted during the suite and are not declared"
fi
for _t in $expect_faults; do
    case " $_faulted " in
        *" $_t "*) ;;
        *) echo "NOTE: '$_t' is on the expected-fault list but never faulted; trim it" ;;
    esac
done

# The arm numbers, in order, one per line.
arm_numbers() { sed -n 's/^\(not \)\?ok \([0-9][0-9]*\).*/\2/p'; }

# The first place the numbers are not strictly +1, or empty. STRICTLY +1 AND NOT A COUNT: one
# duplicated number paired with one missing number leaves both the case count and the span
# untouched, so neither of those tests can see it.
seq_break() { awk 'NR == 1 { prev = $1; next } { if ($1 != prev + 1) { print prev "->" $1; exit } prev = $1 }'; }

# Proven on planted input before it is trusted, because a checker that never fires reports
# every stream clean.
_probe="$(printf 'ok 1\nok 1\nok 3\n' | arm_numbers | seq_break)"
[ "$_probe" = "1->1" ] \
    || fail "seq_break did not catch a duplicated arm number on planted input (got '$_probe')"
_probe="$(printf 'ok 1\nok 2\nok 3\n' | arm_numbers | seq_break)"
[ -z "$_probe" ] \
    || fail "seq_break fired on a clean planted sequence (got '$_probe')"

cases="$(echo "$out" | grep -c '^\(not \)\?ok [0-9]')"
# Parsed after the completion marker so a truncated run is reported as truncated.
plan="$(echo "$out" | sed -n 's/^1\.\.\([0-9][0-9]*\)$/\1/p' | tail -1)"

if [ -n "${TAP_HEADLESS_LAST:-}" ]; then
    # A console that IS the device cannot deliver its own head, so the plan line is gone.
    # The caller naming the LAST arm replaces it as the anti-truncation guard: that plus the
    # completion marker brackets the tail, and contiguity closes the middle. An arm deleted
    # before the first captured line stays invisible, which is why this is opt-in.
    if [ -n "$plan" ]; then
        fail "TAP_HEADLESS_LAST is set but the stream HAS a plan line ($plan): drop the
  variable and let the ordinary reconciliation run, which is strictly stronger"
    fi
    last="$(echo "$out" | sed -n 's/^\(not \)\?ok \([0-9][0-9]*\).*/\2/p' | tail -1)"
    first="$(echo "$out" | sed -n 's/^\(not \)\?ok \([0-9][0-9]*\).*/\2/p' | head -1)"
    [ -n "$last" ] || fail "no ok/not-ok lines at all: the whole stream was dropped"
    if [ "$last" -ne "$TAP_HEADLESS_LAST" ]; then
        fail "the last arm is $last, expected $TAP_HEADLESS_LAST: the run was cut short"
    fi
    if [ "$cases" -ne "$want_arms" ]; then
        fail "$cases case(s) captured, expected exactly $want_arms"
    fi
    # Contiguous, so a gap in the middle cannot pass by reconciling against the ends.
    if [ "$((last - first + 1))" -ne "$cases" ]; then
        fail "arms $first..$last span $((last - first + 1)) numbers but $cases were reported:
  the stream has a HOLE, which a head-truncated capture must never have"
    fi
    seq_bad="$(printf '%s\n' "$out" | arm_numbers | seq_break)"
    if [ -n "$seq_bad" ]; then
        fail "arm numbers are not strictly consecutive at $seq_bad: a repeat or an
  out-of-order line, either of which a span test cannot see"
    fi
    plan="$last"
    echo "NOTE: no plan line; head-truncated transport. Arms 1..$((first - 1)) are NOT" >&2
    echo "  covered by this verdict; $first..$last are." >&2
else
    if [ -z "$plan" ]; then
        fail "no '1..N' plan line in the TAP stream (the whole stream was dropped?)"
    fi
    if [ "$plan" -ne "$cases" ]; then
        fail "TAP plan claims $plan case(s) but $cases were reported"
    fi
    if [ "$plan" -ne "$want_arms" ]; then
        fail "TAP plan is $plan, expected exactly $want_arms: an arm was added or deleted"
    fi
    # THE COUNTS RECONCILE AND THE NUMBERING STILL MAY NOT. `1..3` with `ok 1, ok 1, ok 3`
    # satisfies both tests above, so the plan is checked against the SEQUENCE as well.
    first="$(printf '%s\n' "$out" | arm_numbers | head -1)"
    if [ "$first" != "1" ]; then
        fail "the first arm is numbered $first, not 1, against a plan of 1..$plan"
    fi
    seq_bad="$(printf '%s\n' "$out" | arm_numbers | seq_break)"
    if [ -n "$seq_bad" ]; then
        fail "arm numbers are not strictly consecutive at $seq_bad: a repeat or an
  out-of-order line, which neither the plan nor the case count can see"
    fi
fi

# One directive class. The harness spells both as a passing case carrying a directive,
# `ok <n> - <name> # <DIRECTIVE> <reason>`, plus a matching `# <label>: N` summary line.
# Sets N to the count.
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
