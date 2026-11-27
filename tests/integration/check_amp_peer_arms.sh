#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Runs the arms that need a peer that is running against one, those ARMS names below. Each
# decides at RUNTIME whether it has one: a service crossing once the peer's own serviced count
# moved.
#
# Any `not ok` line in the capture fails the gate, listed arm or not, a `# TODO` failure being
# the harness's permitted one. The named set adds what a `not ok` cannot show: that each of these
# arms ran against the peer rather than skipping.
#
# usage: check_amp_peer_arms.sh <unused.elf> <cmake> <build-dir> <artefact>
#        check_amp_peer_arms.sh --controls     judges planted captures only
#        KOS_CAPTURE=<log> check_amp_peer_arms.sh <board-build> <kickos-source> <cmake> <arm>...
#            judges a silicon capture of the selftest partition: each <arm> of the five above,
#            the ones the image registers, must run, and any other <arm> is ignored
#        check_amp_peer_arms.sh --peer-arms|--other-skips <skips>
#            the arms of a comma-separated skip set that are the five above, `;`-separated, or
#            the others, comma-separated: in a partition the first must run and leave the set
#            (tools/bench/bench.sh)
#
# The own-image posture expects these arms to skip (KICKOS_EXPECT_SKIPS in
# tests/integration/gates/selftest.cmake), because the same image runs standalone and inside
# a merged partition and only the second has a peer.
# This gate is what bounds that permission: here the arms must report `ok` and must NOT be
# skipped, since a runtime skip that never stops skipping is a lapsed arm wearing a permission.
# Every name that permission grants is therefore in the enforced set below.
#
# A two-kernel gate draws fresh every run on the console interleaving, on which kernel reaches
# its first publication first, and on which is inside a masked handler when the other rings, so
# a change to this gate or to what it boots owes TEN green runs before it is believed:
#
#   for i in $(seq 1 10); do ctest --test-dir <build> -R 'amp_partition|amp_peer_arms' || break; done
#
# It may not join a `--repeat until-pass` set: a retry masks exactly the class of defect this
# vehicle exists to find.
#
# AND IT MAY NOT RUN UNDER `ctest -j`. It begins by running `cmake --build`, and two concurrent
# ninja invocations on ONE build directory race on the intermediates they share, leaving
# kernel/libkickos_kernel.a truncated mid-archive. The tell is a gate failing in about a tenth
# of a second, far too fast to have built or booted anything. RUN_SERIAL is set where these are
# registered (user/apps/common/ampping/CMakeLists.txt); a gate copied from this one owes the same.

set -u
here="$(dirname "$0")"
. "$here/../lib/gate.sh"
: "${QEMU_TIMEOUT:=120}"

ARMS="amp_far_call amp_far_call_held amp_far_send_held amp_share_crossing amp_far_reply_unmapped amp_far_wall amp_far_share amp_far_server_gone"

# `ok N - <arm>` with no SKIP directive: a skipped arm reports `ok` too, so the directive is
# what has to be excluded.
#
# A VACUITY skip (`# SKIP VACUOUS`, permitted by name nowhere) is caught here as well, and that
# is deliberate rather than an oversight of the category: these arms are run against a peer that
# answers, so one of them declining for want of a window is this vehicle's own defect and may
# not be waved through on a permission the rest of the fleet carries.
tap_ok() { # <arm> <armed|skipped>
    printf '%s\n' "$OUT" | tr -d '\r' | awk -v arm="$1" -v want="$2" '
        $1 == "ok" && $3 == "-" && $4 == arm {
            skip = ($5 == "#" && toupper($6) ~ /^SKIP/)
            if ((want == "skipped") == skip) { found = 1 }
        }
        END { exit !found }'
}
armed() { # <arm>
    tap_ok "$1" armed
}
skipped() { # <arm>
    tap_ok "$1" skipped
}

# The verdict over $OUT.
judge() {
    # The peer is witnessed through node 0's own reading and not through the peer's banner.
    # Every peer named, not one of them: a peer that never answered is the failure.
    printf '%s\n' "$OUT" | grep -E 'amp far call: [0-9]+ of [0-9]+ peer node\(s\) answered' \
        | sed 's/.*amp far call: \([0-9]*\) of \([0-9]*\) peer.*/\1 \2/' \
        | awk '{ if ($1 >= 1 && $1 == $2) ok = 1 } END { exit !ok }' \
        || fail "not every peer answered node 0's far call: the arms below would skip or mean
  nothing. This is node 0's own line, not one the peer printed."

    # Anywhere on a line.
    _failing="$(printf '%s\n' "$OUT" | grep -E 'not ok [0-9]+' | grep -v ' # TODO ')"
    if [ -n "$_failing" ]; then
        printf '%s\n' "$_failing"
        fail "an arm reported not ok in the merged partition"
    fi

    n_arms=0
    for arm in $ARMS; do
        n_arms=$((n_arms + 1))
        if skipped "$arm"; then
            fail "$arm SKIPPED against a live peer.
  This artefact exists to give it one, so a skip here means the arm cannot see the peer and the
  permission the standalone run carries has quietly become permanent."
        fi
        armed "$arm" || fail "$arm did not report ok against a live peer"
        echo "== $arm: ok, not skipped"
    done
}

# <mode> <skips>: --peer-arms or --other-skips over the skip set.
split_skips() {
    _peer=""
    _rest=""
    for _arm in $(printf '%s' "$2" | tr ',;' '  '); do
        case " $ARMS " in
            *" $_arm "*) _peer="$_peer;$_arm" ;;
            *) _rest="$_rest,$_arm" ;;
        esac
    done
    if [ "$1" = --peer-arms ]; then
        echo "${_peer#;}"
    else
        echo "${_rest#,}"
    fi
}
if [ "${1:-}" = --peer-arms ] || [ "${1:-}" = --other-skips ]; then
    split_skips "$1" "${2:-}"
    exit 0
fi

if [ "${1:-}" = "--controls" ]; then
    good() { # [skip|drop <arm>]
        echo "amp far call: 1 of 1 peer node(s) answered"
        _n=0
        for _arm in $ARMS; do
            _n=$((_n + 1))
            if [ "${2:-}" != "$_arm" ]; then
                echo "ok $_n - $_arm"
            elif [ "$1" = skip ]; then
                echo "ok $_n - $_arm # SKIP no peer"
            fi
        done
    }
    # <name> <pass|refuse> <capture>
    ctl() {
        _ctl_out="$( (OUT="$3"; judge) 2>&1 )"
        _ctl_rc=$?
        if [ "$2" = pass ] && [ "$_ctl_rc" -ne 0 ]; then
            fail "the '$1' control is REFUSED, and it is a capture this gate must accept:
$_ctl_out"
        fi
        if [ "$2" = refuse ] && [ "$_ctl_rc" -eq 0 ]; then
            fail "the '$1' control PASSES, so a capture carrying it reads as clean"
        fi
        echo "== $1: $2"
    }
    ctl 'every arm ok' pass "$(good)"
    ctl 'an unlisted arm not ok' refuse "$(good; echo 'not ok 9 - amp_unlisted')"
    ctl 'a failing check' refuse "$(good; echo 'not ok 9 - amp_unlisted # main.cc:3662: rc == 0')"
    ctl 'a failing check, terse' refuse "$(good; echo 'not ok 9 - amp_unlisted # main.cc:3662')"
    ctl 'a not ok behind a peer byte' refuse "$(good; echo 'Knot ok 9 - amp_unlisted')"
    ctl 'a TODO arm not ok' pass "$(good; echo 'not ok 9 - amp_unlisted # TODO owed')"
    ctl 'every arm ok with CR line ends' pass "$(good | sed 's/$/\r/')"
    ctl 'a listed arm skipped' refuse "$(good skip amp_far_call)"
    ctl 'a listed arm absent' refuse "$(good drop amp_far_call_held)"
    ctl 'no peer answered' refuse "$(good | grep -v 'peer node(s) answered')"
    ctl 'one peer of two answered' refuse "$(good | sed 's/1 of 1 peer/1 of 2 peer/')"
    ctl 'no peer of one answered' refuse "$(good | sed 's/1 of 1 peer/0 of 1 peer/')"
    # The selftest partition's route over an esp32c6-wroom capture, with selftest_p4's manifest
    # row: its skip set loses the peer arms, which this judge then requires to run.
    scratch_dir
    _cap="$here/app_captures/amp-partition-selftest-c6.capture"
    _row=amp_far_call,amp_far_call_held,amp_share_crossing
    _split="$(split_skips --peer-arms "$_row,planted_other")"
    [ "$_split" = "amp_far_call;amp_far_call_held;amp_share_crossing" ] \
        || fail "the peer arms of a skip set are not split out of it"
    [ "$(split_skips --other-skips "$_row,planted_other")" = planted_other ] \
        || fail "a skip set without its peer arms keeps another set"
    # <capture>: the route's TAP verdict and this judge's, status 0 when both pass.
    route() {
        EXPECT_SKIPS="$(split_skips --other-skips "$_row")" \
            EXPECT_PARTIALS=irq_kernel_line_reserved EXPECT_FAULTS=txtrap \
            sh "$here/check_tap_stream.sh" partition 28 < "$1" > "$TMP/route.out" 2>&1 || return 1
        KOS_CAPTURE="$1" sh "$0" "$TMP" "$here/../.." cmake amp_far_call amp_far_call_held \
            amp_share_crossing >> "$TMP/route.out" 2>&1
    }
    route "$_cap" || fail "the selftest partition route refuses a capture whose peer arms ran:
$(cat "$TMP/route.out")"
    echo "== the selftest partition route, peer arms ok: pass"
    sed 's/^\(ok [0-9]* - amp_far_call\)$/\1 # SKIP no peer/' "$_cap" > "$TMP/skipped.capture"
    cmp -s "$_cap" "$TMP/skipped.capture" && fail "the planted skip leaves the capture unchanged"
    if route "$TMP/skipped.capture"; then
        fail "the selftest partition route passes a capture whose peer arm skipped"
    fi
    echo "== the selftest partition route, a peer arm skipped: refuse"
    echo "PASS: the planted captures are judged as stated"
    exit 0
fi

if judging_capture; then
    shift 3
    _registered=""
    for _arm in "$@"; do
        case " $ARMS " in
            *" $_arm "*) _registered="$_registered $_arm" ;;
            *) ;;
        esac
    done
    [ -n "$_registered" ] || fail "no arm that needs a running peer is named, so nothing would be judged"
    ARMS="${_registered# }"
    judge_capture "the selftest partition"
    judge
    echo "PASS: no arm reported not ok, and $n_arms arm(s) that needed a running peer ran against one and none of them skipped"
    exit 0
fi

_unused="${1:?usage: check_amp_peer_arms.sh <unused.elf> <cmake> <build> <artefact>}"
CMAKE="${2:?}"
BUILD="${3:?}"
ART="${4:?}"
: "$_unused"

need_qemu_machine

echo "== building the selftest partition artefact =="
"$CMAKE" --build "$BUILD" --target amp_partition_selftest >/dev/null 2>&1 \
    || fail "the amp_partition_selftest target did not build"
[ -f "$ART" ] || fail "no artefact at $ART"

scratch_dir
serial1_capture
run_image "$ART"
judge
if printf '%s\n' "$(serial1_out)" | grep -qE "$KOS_PANIC_RE"; then
    fail "a peer on serial 1, its own UART, panicked"
fi

echo "PASS: no arm reported not ok, and $n_arms arm(s) that needed a running peer ran against one and none of them skipped"
