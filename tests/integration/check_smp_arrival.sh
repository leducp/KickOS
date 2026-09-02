#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Gate on secondary-core arrival for an image built at more than one core. Two arms over the
# same ELF, both reading the chip's own release lines.
#
# THE CHIP'S WORDING AND THE MECHANISM THAT REFUSES ARE PER BACKEND, so both are parameters and
# an unlisted backend REFUSES rather than running with another one's vocabulary: a gate that
# greps for lines an image never prints asserts nothing and passes.
#
#   full machine    the emulator gets the configured core count. The image must print
#                   `# smp: <n> core(s) online` exactly once with n that count, raise none of
#                   the three release refusals, and go on running: <live-ere> is a line the
#                   app under test emits after the release, so a core zero wedged by the
#                   bring-up is caught rather than read as a clean banner.
#   short machine   the emulator gets one core FEWER, so the image must refuse by name at core
#                   n-1 and exit KICKOS_FATAL_STATUS. WHICH refusal differs by backend and the
#                   binding is the same either way: on arm64 PSCI CPU_ON answers
#                   INVALID_PARAMETERS for a core the machine does not carry; on rv64 there is
#                   no firmware to answer at all, so the missing hart simply never publishes
#                   arrival and the bounded wait names it.
#
# THE SHORT-MACHINE ARM IS WHAT BINDS THE RELEASE LOOP TO THE COUNT. The banner reads the same
# whether the loop released every secondary or only the first, its number coming from the same
# configuration symbol this gate is handed; the short machine is an oracle the image does not
# supply, and only a loop that reaches core n-1 finds the missing core.
#
# The refusal checks run before the banner check, so an image whose secondaries never publish
# arrival is reported on the arrival refusal it printed rather than on the line it did not.
#
# usage: check_smp_arrival.sh <elf> <expect-cores> <live-ere> <backend>

set -u
. "$(dirname "$0")/../lib/gate.sh"
# The arrival spin bound is large and the refusal path has to be reached rather than killed.
: "${QEMU_TIMEOUT:=45}"

_usage="usage: check_smp_arrival.sh <elf> <expect-cores> <live-ere> <backend>"
elf="${1:?$_usage}"
want="${2:?$_usage}"
live="${3:?$_usage}"
backend="${4:?$_usage}"

require_number "$want" "the expected core count"
require_literal "$live" "the liveness pattern"
if [ "$want" -le 1 ]; then
    fail "expected core count is $want. A single-core image prints no arrival banner at all,
  so this gate belongs only on a preset whose core count exceeds one"
fi
[ -f "$elf" ] || fail "no image at $elf"
need_qemu_machine

# THE CHIP'S OWN WORDING. Each is matched as a literal, and each passes require_literal first:
# an empty marker makes every absence-assertion below vacuous.
#
# SHORT_MARK is the refusal the short machine must produce and SHORT_FMT the printf the chip
# formats the core index with. BAD_EXTRA is a second refusal that must be absent on the full
# machine; a backend with only one such refusal repeats SHORT_MARK there rather than leaving a
# marker empty.
BANNER_HEAD="# smp: "
BANNER_TAIL=" core(s) online"
case "$backend" in
    armv8a)
        BAD_ENTRY="KickOS: qemu-arm64 secondary entry is not identity-linked: 0x"
        BAD_EXTRA="KickOS: qemu-arm64 secondary never reached its entry: core "
        SHORT_MARK="KickOS: qemu-arm64 PSCI CPU_ON refused core "
        SHORT_TAIL=", status 0x"
        SHORT_FMT="%02x"
        ;;
    rv64imac)
        # No firmware answers a release here, so the missing hart is found by the bounded wait
        # rather than by a refused call. The duplicate-id claim is the second refusal: it fires
        # where mhartid is not the dense index the rows are keyed by.
        BAD_ENTRY="KickOS: two harts report the same mhartid"
        BAD_EXTRA="KickOS: hart never reached the supervisor park: hart "
        SHORT_MARK="KickOS: hart never reached the supervisor park: hart "
        SHORT_TAIL=""
        SHORT_FMT="%d"
        ;;
    *)
        fail "check_smp_arrival.sh knows no backend '$backend'. The chip's release wording and
  the mechanism that refuses a missing core are both per backend, so an unlisted one would
  grep for lines the image never prints and pass without asserting anything" ;;
esac
for _m in "$BAD_ENTRY" "$BAD_EXTRA" "$SHORT_MARK" "$BANNER_HEAD" "$BANNER_TAIL"; do
    require_literal "$_m" "a release marker"
done

# The image's exit status on kfault_terminate (arch/common/fatal_status.ld.h). A literal, so
# this gate asserts the number rather than restating whatever the tree computes.
FATAL_STATUS=132

# The machine has to carry the count this gate was handed, or both arms measure a posture
# nobody asked for. The two values reach here by different routes and are checked against each
# other before either is used.
case " ${QEMU_EXTRA:-} " in
    *" -smp $want "*) ;;
    *)
        fail "QEMU_EXTRA does not give the machine $want cores: [${QEMU_EXTRA:-}]. The
  expected count and the machine's core count come from one configuration symbol by two
  routes, and a disagreement makes both arms below measure the wrong machine" ;;
esac

short=$((want - 1))
short_extra="$(printf '%s' "${QEMU_EXTRA:-}" \
    | sed "s/\\(^\\|[[:space:]]\\)-smp[[:space:]]\\{1,\\}$want\\([[:space:]]\\|\$\\)/\\1-smp $short\\2/")"
case " $short_extra " in
    *" -smp $short "*) ;;
    *) fail "could not rewrite the machine's core count to $short: [$short_extra]" ;;
esac
case " $short_extra " in
    *" -smp $want "*)
        fail "the rewritten machine still carries $want cores: [$short_extra]. The short arm
  would run on the full machine and assert nothing" ;;
    *) ;;
esac

full_extra="${QEMU_EXTRA:-}"

# --- Arm 1: the full machine brings every core online -------------------------
# Polled, not run to completion: the app under test need not terminate, and the two patterns
# are the banner and a line it emits afterwards.
echo "== $want core(s) requested, machine given $want =="
QEMU_EXTRA="$full_extra"
poll_image "$elf" "$BANNER_HEAD$want core\\(s\\) online" "$live"

count_literal "$BAD_ENTRY"
if [ "$KOS_COUNT" -ne 0 ]; then
    printf '%s\n' "$OUT" | grep -F -e "$BAD_ENTRY"
    fail "the secondary entry is not identity-linked, so no core was released"
fi
count_literal "$SHORT_MARK"
if [ "$KOS_COUNT" -ne 0 ]; then
    printf '%s\n' "$OUT" | grep -F -e "$SHORT_MARK"
    fail "the release refused a core on a machine that carries all $want cores"
fi
# BEFORE THE BANNER. A secondary that is started and never reaches its own code refuses here,
# and that refusal is the finding; the missing banner is only its consequence.
count_literal "$BAD_EXTRA"
if [ "$KOS_COUNT" -ne 0 ]; then
    printf '%s\n' "$OUT" | grep -F -e "$BAD_EXTRA"
    fail "a released core never reached its entry: it was started and published no arrival"
fi
assert_no_panic "the image panicked while bringing the secondaries up"

count_literal "$BANNER_HEAD$want$BANNER_TAIL"
if [ "$KOS_COUNT" -eq 0 ]; then
    fail "no '$BANNER_HEAD$want$BANNER_TAIL' line. The release either did not run or did not
  reach its positive statement, and an absent refusal is not a witness: an image that
  released nobody prints nothing and boots clean"
fi
if [ "$KOS_COUNT" -ne 1 ]; then
    printf '%s\n' "$OUT" | grep -F -e "$BANNER_HEAD"
    fail "the arrival banner appeared $KOS_COUNT times; the release runs once per machine"
fi
# Any other count on the line is a build whose banner and whose posture disagree.
count_literal "$BANNER_HEAD"
if [ "$KOS_COUNT" -ne 1 ]; then
    printf '%s\n' "$OUT" | grep -F -e "$BANNER_HEAD"
    fail "$KOS_COUNT '$BANNER_HEAD' line(s) on the wire, one of them not naming $want cores"
fi
if [ "$POLL_OK" -ne 1 ]; then
    fail "the banner is on the wire but /$live/ is not: the image did not get past its own
  bring-up in ${QEMU_TIMEOUT}s"
fi
echo "   $want core(s) online, and the image runs on"

# --- Arm 2: one core short, and the release must refuse by name ---------------
# The release loop is bound to the configured count here and nowhere else: on a machine of
# $short cores, only a loop that reaches index $short asks PSCI for a core that does not
# exist. An image that released just the first secondary boots this arm clean.
echo "== $want core(s) requested, machine given $short =="
short_index="$(printf "$SHORT_FMT" "$short")"
QEMU_EXTRA="$short_extra"
run_image "$elf"

# THE REFUSAL FIRST. An image that boots clean on the short machine never terminates, so its
# run ends in a timeout; reading the timeout first would report a hang instead of the loop
# bound that caused it.
count_literal "$SHORT_MARK$short_index$SHORT_TAIL"
if [ "$KOS_COUNT" -eq 0 ]; then
    printf '%s\n' "$OUT" | grep -F -e "KickOS: "
    fail "no '$SHORT_MARK$short_index$SHORT_TAIL' refusal on a machine of $short cores.
  The release loop never asked for core $short_index, so it does not run to the configured
  count of $want and a build that releases one core of $want reads the same as this one"
fi
if [ "$RC" -eq 124 ]; then
    fail "core $short_index was refused and the image did not exit within ${QEMU_TIMEOUT}s:
  the refusal path reported and then hung instead of terminating"
fi
count_literal "$BANNER_HEAD"
if [ "$KOS_COUNT" -ne 0 ]; then
    printf '%s\n' "$OUT" | grep -F -e "$BANNER_HEAD"
    fail "the image announced cores online on a machine that cannot carry $want of them"
fi
if [ "$RC" -ne "$FATAL_STATUS" ]; then
    fail "expected exit $FATAL_STATUS from the release refusal, got $RC"
fi
echo "   release of core $short_index refused, exit $RC"

echo "PASS: $want core(s) arrive and are announced; the release loop reaches core
  $short_index, refusing when the machine carries only $short"
exit 0
