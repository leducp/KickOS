#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Verdict on a completed TAP stream, read from stdin. Shared by every selftest gate.
#
# EXPECT_SKIPS, EXPECT_PARTIALS and EXPECT_FAULTS (all default empty) are permission sets, not
# budgets: a skip, a partial or a faulting thread whose name is not listed fails the gate, and
# a listed name that did not skip, go partial or fault is a note, never a failure.
#
# A vacuity skip is the exception and takes no list at all: it is permitted whatever its name
# and expected nowhere. The stream says which kind of skip it is; see VACUITY_MARK below for
# why it cannot be a fourth set here.
#
# A partial is an arm that ran its invariant and left a sub-case unexercised on this board. It
# reports `ok`, so no plan/case reconciliation can see it and only the by-name set can. That
# matters because a mechanism regression makes an arm take its partial early return on every
# board at once, which without this set is a green run with the contract gone.
#
# <expected-arms> is what makes the suite non-vacuous. tap.cc plans `1..N` from the runtime
# registry, so a deleted arm shrinks the plan and the case count in lockstep and no
# self-consistent parse can see it. The caller owns the number because a large minority of the
# arms are #if-conditional (posture, MPU, self-test syscalls) and a split image carries only
# its own run of registry regions, so the total is per-posture and per-image.
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

# The producer says when it dropped output, and it is the only thing that can: every count
# below reconciles against the lines that survived, so a capture missing whole lines can
# satisfy all of them and still not be the run it claims to be. <kickos/sys/emit.h> gives the
# kernel console ring one full ring of wire time to take a write and then drops the remainder,
# counting the bytes; the count reaches the wire as this marker on the first write that fits
# after the loss.
#
# Not anchored, and counted by occurrence rather than by line: the write that was cut can end
# mid-line, and a second writer on a shared console prepends its bytes, so the marker is not
# always the start of a capture line. It is checked first so a truncated capture is named as
# one instead of being reported as a plan that does not add up.
DROP_MARK="# console dropped"

# Planted before it is trusted, as a minimal pair: a parse that matches nothing reports every
# capture whole, which is the reading this clause exists to end.
literal_count "ok 1 - a
$DROP_MARK 48 byte(s)
ok 2 - b" "$DROP_MARK"
[ "$KOS_LITERAL_N" = 1 ] \
    || fail "the drop-marker parse reads $KOS_LITERAL_N marker(s) out of a planted stream
  carrying one, so a producer reporting lost output would be read as a clean capture"
literal_count "ok 1 - a
ok 2 - b" "$DROP_MARK"
[ "$KOS_LITERAL_N" = 0 ] \
    || fail "the drop-marker parse fires on a stream carrying no marker ($KOS_LITERAL_N)"

literal_count "$out" "$DROP_MARK"
if [ "$KOS_LITERAL_N" -gt 0 ]; then
    printf '%s\n' "$out" | grep -F -- "$DROP_MARK"
    fail "the producer reports console output it could not deliver ($KOS_LITERAL_N marker(s)):
  this capture is MISSING LINES and is a witness for nothing, whatever the counts below
  reconcile to. The ring refused a write for a whole ring's wire time, which is a console the
  run outran or one that stopped draining, not a test result"
fi

# The verdict is the harness's own tally. Above one core arch_console_write is a byte-at-a-time
# device loop under no lock, so a peer's status line lands inside another line character by
# character and carries its own newline in with it: `not ok 7 - x` reaches the wire as `not `
# plus a foreign line, then `ok 7 - x` on the next one. A grep for the forbidden literal cannot
# see that, and every way it can be wrong ends in a pass. run_all() emits exactly one final
# verdict line whatever the outcome, so reading that is a positive claim: a shredded one does
# not parse, which is a red.
#
# The `not ok` lines are printed as the diagnostic and never consulted for the verdict.
verdict="$(printf '%s\n' "$out" | sed -n \
    -e 's/^# all tests passed.*$/passed/p' \
    -e 's/^# \([0-9][0-9]*\) test(s) failed$/failed \1/p' | tail -n1)"
case "$verdict" in
    passed) ;;
    'failed '*)
        printf '%s\n' "$out" | grep "not ok"
        fail "the harness reports ${verdict#failed } arm(s) failed" ;;
    *)
        printf '%s\n' "$out" | grep "not ok"
        fail "no final verdict line in the TAP stream (crash / hang / truncated run, or a wire
  that shredded it): tests/tap/tap.cc emits one whatever the outcome, so its absence is never
  'nothing to report'" ;;
esac
# A line the harness could not hold is cut and marked, and nothing else here can see it.
# tests/tap/tap.cc assembles each line in a fixed buffer and restores the newline it would
# otherwise have lost, so the stream stays countable and every reconciliation below reads
# clean while a reason, a diagnostic or a failure's own file and line sit truncated. The
# marker is the only trace, and a cut reason is exactly the text a reader needs most.
TRUNC_MARK='<TRUNCATED>'

# Planted as a minimal pair before the search is trusted: a checker that matches nothing
# reports every stream whole.
_planted="ok 7 - an_arm # SKIP a reason that ran out of roo$TRUNC_MARK"
printf '%s\n' "$_planted" | grep -qF "$TRUNC_MARK" \
    || fail "the truncation search does not see a planted '$TRUNC_MARK' line, so a cut line
  would pass every clause in this gate"
printf '%s\n' "ok 7 - an_arm # SKIP a reason with room to spare" | grep -qF "$TRUNC_MARK" \
    && fail "the truncation search fires on a line that was NOT cut, so it says nothing"

if printf '%s\n' "$out" | grep -qF "$TRUNC_MARK"; then
    printf '%s\n' "$out" | grep -F "$TRUNC_MARK"
    fail "the line(s) above overran the harness's assembly buffer and were emitted CUT:
  shorten what they format. The stream itself is intact, so no other clause here refuses it"
fi

# Only a named thread may fault. A thread-fault record from any other thread is an arm whose
# thread died the wrong way, and thread-scoped isolation means the plan, case and directive
# checks above still reconcile and read green, so only this clause can see it. A slay redirect
# that rebuilds an unprivileged context faults the stub on its first kernel access and reaches
# the same observable end state as a correct one.
#
# The list is by thread and not by arm, because the record names the thread and nothing else:
# the containment arm's own worker is the one deliberate fault in the stream, and every
# neighbouring arm's worker stays forbidden. A listed thread that did not fault is a note here
# rather than a failure: the arm's join is what asserts the death, and a fault that never
# happened times that join out and reports `not ok` above.
#
# The parse is reconciled against the two halves of the record it reads. kernel/init/fault.cc
# emits the banner as one write opening with FAULT_HEAD and closing with FAULT_TAIL, so an
# intact record contributes one of each and one parsed name. A peer's status line landing
# inside it splits the line at the newline it brings with it, the sed then matches nothing, and
# the permission set below judges an empty list: the one fault this clause exists to catch
# reads as no fault at all. One break cannot destroy both fragments, so a head or a tail that
# does not pair with a parsed name is that record, and it is a refusal rather than a silence.
#
# The three counts must agree exactly, in both directions. A head or a tail in excess of the
# names is the fragment a break left behind; a shortfall is the same record with the fragment
# that carries no name gone, which is the reading that let an incomplete record through. The
# two halves are counted as occurrences, so two records sharing one physical line are two
# records here and not one, which is the reading a line count gets wrong.
FAULT_HEAD='=== THREAD FAULT ==='
FAULT_TAIL="' killed"
fault_names() { sed -n "s/.*$FAULT_HEAD thread '\([^']*\)'.*/\1/p"; }

# KOS_FAULT_HEADS, KOS_FAULT_TAILS, KOS_FAULT_NAMES: what <text> carries. 0 when the three
# agree. A predicate, so the controls below judge the very clause the stream is judged by.
faults_reconcile() { # <text>
    literal_count "$1" "$FAULT_HEAD"
    KOS_FAULT_HEADS="$KOS_LITERAL_N"
    literal_count "$1" "$FAULT_TAIL"
    KOS_FAULT_TAILS="$KOS_LITERAL_N"
    KOS_FAULT_NAMES=0
    for _fr in $(printf '%s\n' "$1" | fault_names); do
        KOS_FAULT_NAMES=$((KOS_FAULT_NAMES + 1))
    done
    if [ "$KOS_FAULT_NAMES" -ne "$KOS_FAULT_HEADS" ]; then
        return 1
    fi
    if [ "$KOS_FAULT_NAMES" -ne "$KOS_FAULT_TAILS" ]; then
        return 1
    fi
    return 0
}

# Proven on planted records: a parse that matches nothing reconciles with itself at zero and
# reports every stream clean, so a wording change in fault.cc would retire this clause silently.
# Each control below is a minimal pair against the intact one, differing in the half it drops or
# repeats, so a refusal cannot be credited to the wrong direction.
_planted="$FAULT_HEAD thread 'planted$FAULT_TAIL, system continues"
_probe="$(printf '%s\n' "$_planted" | fault_names)"
[ "$_probe" = planted ] \
    || fail "the thread-fault name parse reads no name out of a planted record (got '$_probe')"
faults_reconcile "$_planted" \
    || fail "the reconciliation refuses an INTACT planted record ($KOS_FAULT_HEADS banner(s),
  $KOS_FAULT_TAILS kill line(s), $KOS_FAULT_NAMES name(s)), so every stream would read broken"
if faults_reconcile "$_planted
$FAULT_HEAD thread 'shortfall'"; then
    fail "the reconciliation accepts a record with a NAME and a banner and no kill line
  ($KOS_FAULT_HEADS banner(s), $KOS_FAULT_TAILS kill line(s), $KOS_FAULT_NAMES name(s)): an
  incomplete record passes and the expected-fault set judges a list that is already short"
fi
if faults_reconcile "$_planted
  $FAULT_TAIL"; then
    fail "the reconciliation accepts a kill line in EXCESS of the names parsed
  ($KOS_FAULT_HEADS banner(s), $KOS_FAULT_TAILS kill line(s), $KOS_FAULT_NAMES name(s))"
fi
if faults_reconcile "$_planted$FAULT_HEAD thread 'shared$FAULT_TAIL, system continues"; then
    fail "the reconciliation accepts two records sharing ONE physical line
  ($KOS_FAULT_HEADS banner(s), $KOS_FAULT_TAILS kill line(s), $KOS_FAULT_NAMES name(s)): the
  second thread is never named and the expected-fault set cannot see it"
fi

_faulted="$(echo "$out" | fault_names)"
if ! faults_reconcile "$out"; then
    printf '%s\n' "$out" | grep -F -e "$FAULT_HEAD" -e "$FAULT_TAIL"
    fail "the stream carries $KOS_FAULT_HEADS thread-fault banner(s) and $KOS_FAULT_TAILS kill
  line(s) against $KOS_FAULT_NAMES name(s) parsed: a fault record reached the wire whose thread
  this gate cannot name, so the expected-fault set below is judging an incomplete list"
fi

_badfault=""
for _t in $_faulted; do
    case " $expect_faults " in
        *" $_t "*) ;;
        *) _badfault="$_badfault $_t" ;;
    esac
done
if [ -n "$_badfault" ]; then
    printf '%s\n' "$out" | grep -F -e "$FAULT_HEAD"
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

# The first place the numbers are not strictly +1, or empty. Strictly +1 and not a count: one
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
# Every case, and separately every case that passed. The plan reconciliation below is what says
# the suite is whole; this pair is what says the arms in it reported, and neither is the absence
# of a string.
passing="$(echo "$out" | grep -c '^ok [0-9]')"
require_number "$cases" "the reported case count"
require_number "$passing" "the passing case count"
# Parsed after the verdict line so a truncated run is reported as truncated.
plan="$(echo "$out" | sed -n 's/^1\.\.\([0-9][0-9]*\)$/\1/p' | tail -1)"

if [ -n "${TAP_HEADLESS_LAST:-}" ]; then
    # A console that is the device cannot deliver its own head, so the plan line is gone.
    # The caller naming the last arm replaces it as the anti-truncation guard: that plus the
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
    # The counts reconcile and the numbering still may not. `1..3` with `ok 1, ok 1, ok 3`
    # satisfies both tests above, so the plan is checked against the sequence as well.
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

# The stream says which category a skip is, and nothing here looks it up. Two kinds reach the
# wire under the SKIP directive and are judged by opposite rules:
#
#   provisioning, `# SKIP <reason>`: this board cannot host the arm. Declared in EXPECT_SKIPS;
#     an undeclared one fails and a declared one that did not skip is a note.
#   vacuity, `# SKIP VACUOUS <reason>`: the timing window the arm's claim rests on did not hold
#     on this run, so the arm asserted nothing. Permitted whatever its name, never expected.
#
# A vacuity skip may not join the declared set: that set is a measurement and not slack, and an
# arm gone permanently vacuous would read green forever with a listed-but-unskipped arm being
# only a note. Nor may there be a separate list of the arms allowed to go vacuous, since that
# would be a second authority beside the arms, stale the moment one of them is repaired. So the
# category travels on the wire and is read here.
#
# It stays a SKIP directive on the wire on purpose: a directive of its own would read as a
# plain pass to every reader not taught the category, check_amp_peer_arms.sh's armed() among
# them, where a peer-dependent arm silently declining is exactly what that gate exists to
# refuse.
#
# Permitted is not silent: each one is printed by name with its reason, and the count is
# reconciled against the harness's own `# vacuous: N`. Without that reconciliation a marker
# this parse no longer matched would report every run free of vacuity, the same false green
# wearing a new name.
VACUITY_MARK='SKIP VACUOUS'

# The names carrying <DIRECTIVE>, one per line. `SKIP` is a prefix of `SKIP VACUOUS`, so the
# ordinary-skip caller subtracts the vacuity names instead of trusting this to separate them.
directive_names() { # <DIRECTIVE>
    sed -n "s/^ok [0-9][0-9]* - \([A-Za-z0-9_]*\) # $1.*/\1/p"
}

# Planted before either parse is trusted, as minimal pairs: the two differ only in the marker,
# so a probe passing for the wrong reason cannot be credited to the right one.
_planted="ok 7 - an_arm # $VACUITY_MARK the waiter reaching its lock: due 3 us PAST a 120 us span"
_probe="$(printf '%s\n' "$_planted" | directive_names "$VACUITY_MARK")"
[ "$_probe" = an_arm ] \
    || fail "the vacuity parse reads no name out of a planted '# $VACUITY_MARK' line (got
  '$_probe'), so every run would report no vacuity at all"
_probe="$(printf '%s\n' "$_planted" | directive_names SKIP)"
[ "$_probe" = an_arm ] \
    || fail "the ordinary-skip parse no longer reads a vacuity line (got '$_probe'), so the
  subtraction below is dead weight and the two categories are being told apart by something
  this gate does not state"
_planted="ok 7 - an_arm # SKIP this part hosts no console"
_probe="$(printf '%s\n' "$_planted" | directive_names "$VACUITY_MARK")"
[ -z "$_probe" ] \
    || fail "the vacuity parse reads an ORDINARY skip as a vacuity skip (got '$_probe'), so an
  undeclared provisioning skip would be permitted"

# One directive class. The harness spells both as a passing case carrying a directive,
# `ok <n> - <name> # <DIRECTIVE> <reason>`, plus a matching `# <label>: N` summary line.
# Sets N to the count.
check_directive() { # <DIRECTIVE> <summary-label> <permitted names> [<names a sub-category took>]
    _dir="$1"
    _label="$2"
    _expect="$3"
    _claimed="${4:-}"

    N="$(echo "$out" | sed -n "s/^# $_label: \([0-9][0-9]*\)\$/\1/p" | tail -1)"
    if [ -z "$N" ]; then
        fail "no '# $_label: N' summary in the TAP stream (harness regression?)"
    fi

    _names=""
    for _x in $(echo "$out" | directive_names "$_dir"); do
        case " $_claimed " in
            *" $_x "*) ;;
            *) _names="$_names $_x" ;;
        esac
    done
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

# The expected-failure category, read before the tally because it subtracts from it. An arm
# carrying `# TODO` states a claim the tree does not meet yet, with the change that will close
# it named in the reason. Its failure is permitted and is not the run's; its pass is the fix
# arriving and is reported, since the whole point of the category is that the arm announces its
# own repair instead of somebody remembering to look.
_todo_owed="$(printf '%s\n' "$out" | grep -c '^not ok [0-9][0-9]* - .* # TODO ')"
_todo_fixed="$(printf '%s\n' "$out" | grep -c '^ok [0-9][0-9]* - .* # TODO ')"
_todo_said="$(printf '%s\n' "$out" | sed -n 's/^# todo: \([0-9][0-9]*\)$/\1/p' | tail -1)"
_fixed_said="$(printf '%s\n' "$out" | sed -n 's/^# todo-fixed: \([0-9][0-9]*\)$/\1/p' | tail -1)"
if [ -z "$_todo_said" ] || [ -z "$_fixed_said" ]; then
    fail "no '# todo: N' and '# todo-fixed: N' summary in the TAP stream (harness regression?).
  Both are emitted zero included, so an absent line is a truncated run and never 'none owed'"
fi
if [ "$_todo_owed" -ne "$_todo_said" ] || [ "$_todo_fixed" -ne "$_fixed_said" ]; then
    fail "the stream carries $_todo_owed owed and $_todo_fixed fixed TODO arm(s) against a
  harness count of $_todo_said and $_fixed_said: one of the two was written by something that
  did not also write the other"
fi
if [ "$_todo_fixed" -ne 0 ]; then
    printf '%s\n' "$out" | grep '^ok [0-9][0-9]* - .* # TODO '
    fail "$_todo_fixed arm(s) marked TODO now PASS. That is the fix arriving: take the marker
  off the arm and close the item its reason names, or the next reader learns nothing from it"
fi

# The tally closes here: <expected-arms> arms were planned, that many cases reported, they are
# numbered 1..N without a repeat or a hole, and every one of them reported ok or is owed.
if [ $((passing + _todo_owed)) -ne "$cases" ]; then
    printf '%s\n' "$out" | grep "not ok" | grep -v ' # TODO '
    fail "$cases case(s) reported, $passing of them ok and $_todo_owed owed"
fi

# The vacuity category first, because the ordinary-skip check subtracts what it claims.
_vac_names=""
for _x in $(printf '%s\n' "$out" | directive_names "$VACUITY_MARK"); do
    _vac_names="$_vac_names $_x"
done
vacuous=0
for _x in $_vac_names; do vacuous=$((vacuous + 1)); done
_vac_said="$(printf '%s\n' "$out" | sed -n 's/^# vacuous: \([0-9][0-9]*\)$/\1/p' | tail -1)"
if [ -z "$_vac_said" ]; then
    fail "no '# vacuous: N' summary in the TAP stream (harness regression?). This category is
  permitted by rule and named by no list, so the harness's own count is the only thing that
  holds the parse above to something"
fi
if [ "$vacuous" -ne "$_vac_said" ]; then
    printf '%s\n' "$out" | grep "# SKIP"
    fail "the harness reports $_vac_said vacuity skip(s) against $vacuous '# $VACUITY_MARK'
  line(s) parsed: the marker moved, or a line carrying one was dropped before this gate read it"
fi
if [ "$vacuous" -gt 0 ]; then
    echo "VACUITY: $vacuous arm(s) asserted nothing on this run (permitted, never expected):"
    # The same anchor the count used, so the list and the count cannot disagree.
    printf '%s\n' "$out" \
        | sed -n "s/^\(ok [0-9][0-9]* - [A-Za-z0-9_]* # $VACUITY_MARK .*\)/  \1/p"
fi

check_directive SKIP skipped "$expect_skips" "$_vac_names"
skipped="$N"
check_directive PARTIAL partial "$expect_partials"
partial="$N"

echo "PASS: $label TAP suite clean ($plan arms, $skipped skipped, $vacuous vacuous," \
     "$partial partial, all expected)"
exit 0
