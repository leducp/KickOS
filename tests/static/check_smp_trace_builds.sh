#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# KICKOS_SMP_TRACE is OFF in every preset, so its enabled arm is compiled by nothing and would
# rot unnoticed: a field added to the record, a renamed hook, a signature moved. This compiles
# THAT ARM, with the preset's own flags, and runs no image.
#
# THE FLAGS COME FROM compile_commands.json rather than from a list here, so they cannot drift
# from what the preset actually uses; only -DKICKOS_SMP_TRACE is overridden.
#
# An empty corpus is a FAILURE: a TU this cannot find is a rename, not a pass.
#
#   control  the real source guards its enabled arm with `#if defined(KICKOS_SMP_TRACE) &&
#            KICKOS_SMP_TRACE`, so a TU compiled with NO -D at all takes the disabled branch
#            and compiles exactly as clean as one compiled with the arm truly forced on: the
#            real TUs below cannot tell a working force-on from a broken one, so this run
#            asserted syntax and nothing else. Only a probe reacting to an UNDEFINED macro
#            can tell them apart, and it has to run derive_cmd(), the SAME function the
#            loop below calls and not a copy of it, or a break in how the knob is forced on
#            could diverge between what the control checks and what the loop runs. Proven
#            both ways: the probe must REFUSE the base command (no forced define, the shape
#            of a dropped `-D`) and must accept derive_cmd()'s own output.
#
# usage: check_smp_trace_builds.sh <compile_commands.json>

set -eu
. "$(dirname "$0")/../lib/gate.sh"

_usage="usage: check_smp_trace_builds.sh <compile_commands.json>"
cc_json="${1:?$_usage}"
[ -f "$cc_json" ] || fail "no compile_commands.json at $cc_json"

# Every TU that carries a hook or the rings themselves.
TUS="smptrace.cc sync.cc sched.cc"
scratch_dir

# The compile line compile_commands.json carries for <tu>, exactly as CMake wrote it.
tu_line() { # <tu> -> the line on stdout
    _tl="$(sed -n 's/.*"command": *"\(.*\)".*/\1/p' "$cc_json" \
        | grep -F -- "$1" | grep -v -- '-fsyntax-only' | head -n1)"
    [ -n "$_tl" ] || fail "no compile command for '$1' in $cc_json. It was renamed or dropped
  from the build, so this gate would compile nothing and pass"
    printf '%s\n' "$_tl"
}

# Drop the object output and the dep file, drop any EXISTING -DKICKOS_SMP_TRACE (the preset
# already defines the knob OFF), syntax only rather than codegen. Leaves the knob OFF: only
# the negative control below calls this directly, to get the shape of a dropped `-D`.
force_off_and_syntax_only() { # <line> -> the derived command on stdout, knob still off
    printf '%s' "$1" | sed 's/\\"/"/g; s/-o [^ ]*//; s/-c /-fsyntax-only /;
        s/-M[TFD] [^ ]*//g; s/-MD//; s/-DKICKOS_SMP_TRACE=[0-9]*//g'
}

# THE COMMAND THE LOOP BELOW ACTUALLY RUNS. The positive control calls this same function
# for the same TU, so a change to how the knob gets forced on cannot diverge from what the
# control just proved: a second -D here would be a redefinition and -Werror refuses it, which
# is why the existing one is dropped first rather than shadowed.
derive_cmd() { # <tu> -> the final compile command on stdout, knob forced on
    printf '%s -DKICKOS_SMP_TRACE=1' "$(force_off_and_syntax_only "$(tu_line "$1")")"
}

# --- control: the forcing mechanism actually lands the value, proven both ways --------
mkdir -p "$TMP/ctl"
cat > "$TMP/ctl/probe.cc" <<'EOF'
#ifndef KICKOS_SMP_TRACE
#error "KICKOS_SMP_TRACE not defined: the -D did not reach the compiler"
#endif
#if KICKOS_SMP_TRACE != 1
#error "KICKOS_SMP_TRACE is not 1: the enabled arm was not selected"
#endif
typedef int kos_probe_smp_trace_tu;
EOF

# Negative: nothing appended, the shape of a `-D` that got dropped. The real source's own
# `#if defined(...) && ...` guard would take this silently; the probe must not.
_ctl_base="$(force_off_and_syntax_only "$(tu_line smptrace.cc)")"
if eval "$_ctl_base -x c++ -" < "$TMP/ctl/probe.cc" 2>"$TMP/ctl/neg.err"; then
    fail "the probe compiles with KICKOS_SMP_TRACE left undefined, so it cannot tell a
  forced-on build from the preset's own OFF posture: every real TU below would report PASS
  whether or not the enabled arm was ever selected"
fi

# Positive: derive_cmd()'s own output, the same call the loop below makes.
_ctl_on="$(derive_cmd smptrace.cc)"
if ! eval "$_ctl_on -x c++ -" < "$TMP/ctl/probe.cc" 2>"$TMP/ctl/pos.err"; then
    sed -n '1,6p' "$TMP/ctl/pos.err" >&2
    fail "the probe does not compile against derive_cmd()'s own output, so the real TUs below
  are not being compiled with KICKOS_SMP_TRACE=1 either, whatever they report"
fi

echo "== control: derive_cmd() lands KICKOS_SMP_TRACE=1, and the probe refuses the unforced command =="

built=0
for tu in $TUS; do
    cmd="$(derive_cmd "$tu")"
    if ! eval "$cmd" 2>"$TMP/err.$tu"; then
        sed -n '1,25p' "$TMP/err.$tu"
        fail "'$tu' does not compile with KICKOS_SMP_TRACE=1. The enabled arm has rotted:
  nothing else in the tree compiles it"
    fi
    built=$((built + 1))
done

require_number "$built" "the count of translation units compiled"
if [ "$built" -ne 3 ]; then
    fail "compiled $built translation unit(s), expected 3"
fi
echo "PASS: the KICKOS_SMP_TRACE arm compiles in $built translation unit(s), with the preset's
  own flags and no image run"
exit 0
