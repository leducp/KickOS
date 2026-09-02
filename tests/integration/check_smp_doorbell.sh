#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Gate on the cross-core doorbell and the kernel lock for a qemu-arm64 image built at more than
# one core. One boot, read through two independent channels.
#
#   the image     must print `# doorbell: <n> core(s) answered, rounds 0x<r>` once with n the
#                 configured count, raise no unanswered-doorbell refusal, and go on running.
#   the emulator  QEMU's own GIC model, through its trace events, must report a GICD_SGIR write
#                 and must report the CPU INTERFACE of every secondary acknowledging the
#                 doorbell INTID.
#
# THE EMULATOR-SIDE ARMS READ THE ROUND'S OWN WINDOW, which is the trace up to and including the
# LAST raise the round made. The round runs inside arch_init, ahead of any other raise, so its
# raises are the FIRST ones in the boot and the count the image printed is where they end. A
# running kernel raises the doorbell too, a cross-core wake being one, and such a raise would
# inflate the count the round is checked against and put the doorbell on core zero's own
# interface, which the round forbids.
#
# THE SECOND CHANNEL IS THE ORACLE: `gic_acknowledge_irq cpu 3 acknowledged irq 0` is QEMU's
# model stating that core 3's CPU interface read that INTID out of GICC_IAR, which guest code
# cannot write to this log, where a count the image printed says only that the image reached its
# own print. The two channels also CROSS-CHECK: the number of GICD_SGIR writes QEMU counted must
# equal the round count the image printed, two routes to one number.
#
# The refusal check runs before the banner check, so an image whose doorbell went unanswered is
# reported on the refusal it printed rather than on the line it did not.
#
# usage: check_smp_doorbell.sh <elf> <expect-cores> <live-ere>

set -u
. "$(dirname "$0")/../lib/gate.sh"
# The doorbell rounds run inside the arrival spin bound, and the trace log costs time.
: "${QEMU_TIMEOUT:=60}"

_usage="usage: check_smp_doorbell.sh <elf> <expect-cores> <live-ere> <backend>"
elf="${1:?$_usage}"
want="${2:?$_usage}"
live="${3:?$_usage}"
backend="${4:?$_usage}"

require_number "$want" "the expected core count"
require_literal "$live" "the liveness pattern"
if [ "$want" -le 1 ]; then
    fail "expected core count is $want. At one core the doorbell and the lock are empty macros
  and no round runs at all, so this gate belongs only on a preset whose core count exceeds one"
fi
[ -f "$elf" ] || fail "no image at $elf"
need_qemu_machine
need_qemu

# THE CHIP'S AND THE BACKEND'S OWN WORDING (arch/arm64/armv8a/klock_armv8a.cc). Each is matched
# as a literal and each passes require_literal first: an empty marker makes every
# absence-assertion below vacuous.
UNANSWERED="KickOS: armv8a doorbell unanswered by core "
EARLY_WAIT="KickOS: armv8a doorbell wait returned unanswered, rounds 0x"
NO_CONTEND="KickOS: armv8a kernel lock uncontended, peers "
NO_SPIN="KickOS: armv8a no peer reached the acquire loop, spinning mask 0x"
CHECK_HEAD="# doorbell: "
CHECK_TAIL=" core(s) answered, rounds 0x"
BANNER_HEAD="# smp: "
for _m in "$UNANSWERED" "$EARLY_WAIT" "$NO_CONTEND" "$NO_SPIN" "$CHECK_HEAD" "$CHECK_TAIL" \
          "$BANNER_HEAD"; do
    require_literal "$_m" "a doorbell marker"
done

# WHAT THE EMULATOR CAN SEE OF THE DOORBELL, PER BACKEND, AND ONE BACKEND SEES LESS.
#
#   arm64  the GIC model traces both halves: the RAISE, as a GICD_SGIR write, and the
#          ACKNOWLEDGEMENT, as a GICC_IAR read. The two cross-check.
#   rv64   the CLINT msip store is a plain MMIO write with no trace event, so THE RAISE HALF
#          HAS NO ORACLE HERE AT ALL. What the trap log does carry is stronger on the other
#          half: its line names the hart AND the cause, where the GIC's event names the
#          interface and the INTID. This gate therefore runs with ONE channel on rv64 and says
#          so; it does not check a raise count and must not read as though it had.
#
# HAS_RAISE is what gates the arms that need the missing half.
case "$backend" in
    armv8a)
        # The controller is a POSTURE of this board, so the events come from the machine string
        # the gate itself hands the emulator: a parse cannot then disagree with what booted.
        case " ${QEMU_MACHINE:-} " in
            *gic-version=3*)
                TRACE_ACK=gicv3_icc_iar1_read
                TRACE_WRITE=gicv3_icc_generate_sgi
                SGIR_LINE="gicv3_icc_generate_sgi" ;;
            *)
                TRACE_ACK=gic_acknowledge_irq
                TRACE_WRITE=gic_dist_write
                SGIR_LINE="dist write at 0x00000f00" ;;
        esac
        CHAN_ITEMS="trace:$TRACE_ACK,trace:$TRACE_WRITE"
        HAS_RAISE=1
        CTL_NAME="timer acknowledgement"
        CHAN_NAME="QEMU's GIC model"
        ;;
    rv64imac)
        CHAN_ITEMS="int"
        HAS_RAISE=0
        SGIR_LINE=""
        CTL_NAME="privilege-probe trap"
        CHAN_NAME="QEMU's trap log"
        ;;
    *)
        fail "check_smp_doorbell.sh knows no backend '$backend'. What the emulator can see of
  a doorbell is a property of its device model, so an unlisted backend would read an empty
  channel as a core that never answered" ;;
esac

# One acknowledgement line, as the modelled controller spells it. BOTH THE CORE NUMBER AND THE
# INTID ARE PRINTED IN THE LOG'S OWN RADIX, GICv3 spelling each in hexadecimal, so the
# conversion is made rather than assumed.
ack_ere() { # <cpu|*> <intid>
    _cpu="$1"
    _id="$2"
    if [ "${TRACE_ACK:-}" = gicv3_icc_iar1_read ]; then
        if [ "$_cpu" = "*" ]; then
            _cpu="[0-9a-f][0-9a-f]*"
        else
            _cpu="$(printf '%x' "$_cpu")"
        fi
        printf '^%s GICv3 ICC_IAR1 read cpu 0x%s value 0x%x$' "$TRACE_ACK" "$_cpu" "$_id"
        return
    fi
    if [ "$_cpu" = "*" ]; then
        _cpu="[0-9][0-9]*"
    fi
    printf '^%s cpu %s acknowledged irq %d$' "$TRACE_ACK" "$_cpu" "$_id"
}

# The channel's readers, per backend. Each takes the file to read.
kos_ctl_count() { # <file>: an event every boot produces, INDEPENDENT of the doorbell
    case "$backend" in
        armv8a)
            grep -c "$(ack_ere '*' 30)" "$1" ;;
        rv64imac)
            # kickos_rv64_privilege_probe reads a machine CSR from supervisor mode on purpose,
            # once per hart, so every boot produces exactly one of these per core.
            grep -c 'desc=illegal_instruction' "$1" ;;
    esac
}
kos_doorbell_count() { # <file> <core>: that core taking the CROSS-CORE doorbell
    case "$backend" in
        armv8a)
            grep -c "$(ack_ere "$2" 0)" "$1" ;;
        rv64imac)
            # cause 3 is the machine software interrupt a peer's msip write raises.
            grep -c "^riscv_cpu_do_interrupt: hart:$2, async:1, cause:0*3," "$1" ;;
    esac
}

# CAN THIS EMULATOR REPORT THE ORACLE AT ALL. The trace REGISTRY is build-global rather than
# target-scoped, so a listed event proves the binary was compiled with it and never that this
# machine emits it; the parse control below, on the real log, is what settles that.
case "$CHAN_ITEMS" in
    trace:*)
        for _ev in $(printf '%s' "$CHAN_ITEMS" | tr ',' ' '); do
            _ev="${_ev#trace:}"
            if ! "$QEMU_BIN" -d 'trace:help' 2>&1 | grep -q "^$_ev\$"; then
                echo "SKIP: $QEMU_BIN reports no '$_ev' trace event, so the emulator-side
  oracle is unavailable and the image's own count would be the only channel"
                exit 77
            fi
        done ;;
    *)
        if ! "$QEMU_BIN" -d 'help' 2>&1 | grep -qE "^$CHAN_ITEMS[ ,]"; then
            echo "SKIP: $QEMU_BIN reports no '$CHAN_ITEMS' log item, so $CHAN_NAME is
  unavailable and the image's own count would be the only channel"
            exit 77
        fi ;;
esac

# The machine has to carry the count this gate was handed, or the run measures a posture nobody
# asked for. The two values reach here by different routes.
case " ${QEMU_EXTRA:-} " in
    *" -smp $want "*) ;;
    *)
        fail "QEMU_EXTRA does not give the machine $want cores: [${QEMU_EXTRA:-}]. The
  expected count and the machine's core count come from one configuration symbol by two
  routes, and a disagreement makes this gate measure the wrong machine" ;;
esac

scratch_dir
TRACE="$TMP/gic.log"

# --- The boot, with QEMU's GIC model logging beside the console ----------------
echo "== $want core(s), doorbell round with the GIC model traced =="
QEMU_EXTRA="${QEMU_EXTRA:-} -d $CHAN_ITEMS -D $TRACE"
poll_image "$elf" "$CHECK_HEAD$want core\\(s\\) answered" "$live"

# --- Channel 1: what the image said ------------------------------------------
count_literal "$UNANSWERED"
if [ "$KOS_COUNT" -ne 0 ]; then
    printf '%s\n' "$OUT" | grep -F -e "$UNANSWERED"
    fail "a raise went unanswered: the wait reached its bound with a core still silent"
fi
# THE WAIT'S POSTCONDITION, read per round and under the lock by the image itself: a peer
# catching up afterwards would satisfy any count taken at the end of the run.
count_literal "$EARLY_WAIT"
if [ "$KOS_COUNT" -ne 0 ]; then
    printf '%s\n' "$OUT" | grep -F -e "$EARLY_WAIT"
    fail "the wait returned with a peer still behind this round's request, so it is not a
  rendezvous: the raise is fire-and-forget and every caller that needs an answer is broken"
fi
# THE OVERLAP THE POLL PHASE IS BUILT ON, observed by the image with the lock held before it
# raised: a peer inside the acquire loop under its own interrupt mask is the only state in which
# the acquire loop's own servicing can be what answered.
count_literal "$NO_SPIN"
if [ "$KOS_COUNT" -ne 0 ]; then
    printf '%s\n' "$OUT" | grep -F -e "$NO_SPIN"
    fail "no peer reached the lock's acquire loop inside the bound, with the initiator holding
  the word the whole time. A peer that published intent cannot leave that window until the
  release, so this is a core that never got there rather than a race the image lost"
fi
# CONTENTION HAPPENED, which is what puts the coupling through its exercise: a peer that
# completed an acquisition is one the lock was actually handed to.
count_literal "$NO_CONTEND"
if [ "$KOS_COUNT" -ne 0 ]; then
    printf '%s\n' "$OUT" | grep -F -e "$NO_CONTEND"
    fail "a peer never completed a kernel-lock acquisition inside the bound, with the word left
  free for it the whole time: the lock is never handed over"
fi
assert_no_panic "the image panicked during the doorbell round"

count_literal "$CHECK_HEAD"
if [ "$KOS_COUNT" -eq 0 ]; then
    fail "no '$CHECK_HEAD' line. The round either did not run or did not reach its positive
  statement, and an absent refusal is not a witness: an image that raised nothing prints
  nothing and boots clean"
fi
if [ "$KOS_COUNT" -ne 1 ]; then
    printf '%s\n' "$OUT" | grep -F -e "$CHECK_HEAD"
    fail "the doorbell line appeared $KOS_COUNT times; the round runs once per machine"
fi
count_literal "$CHECK_HEAD$want$CHECK_TAIL"
if [ "$KOS_COUNT" -ne 1 ]; then
    printf '%s\n' "$OUT" | grep -F -e "$CHECK_HEAD"
    fail "the doorbell line does not name $want cores answering"
fi
if [ "$POLL_OK" -ne 1 ]; then
    fail "the doorbell line is on the wire but /$live/ is not: the image did not get past its
  own bring-up in ${QEMU_TIMEOUT}s"
fi
echo "   the image reports $want core(s) answered"

# The round count the image printed, as its own hex digits. Read back so the emulator's raise
# count below is checked against a number this run actually carried.
rounds_hex="$(printf '%s\n' "$OUT" \
    | sed -n "s/^.*$CHECK_TAIL\\([0-9a-f][0-9a-f]*\\).*\$/\\1/p" | head -n1)"
case "$rounds_hex" in
    ''|*[!0-9a-f]*) fail "could not read the round count out of the image's own line" ;;
esac
rounds=$(printf '%d' "0x$rounds_hex")
require_number "$rounds" "the round count"
if [ "$rounds" -eq 0 ]; then
    fail "the image reports 0 rounds, so it raised nothing and every assertion below is
  vacuously satisfied"
fi

# --- Channel 2: what the EMULATOR said ----------------------------------------
require_nonempty "$TRACE" "QEMU wrote no trace log at $TRACE, so the oracle is UNKNOWN rather
  than negative and every assertion below it would read as an absence"

# POSITIVE CONTROL ON THE PARSE, before any absence is judged. The timer PPI is acknowledged on
# every boot of this board, so a parse that cannot find THAT cannot be trusted to report a
# missing doorbell acknowledgement. Matched on ANY core: the PPI is banked and only a core whose
# current thread carries a deadline arms it, so which core takes one is the scheduler's.
ctl="$(kos_ctl_count "$TRACE")" || ctl=0
require_number "$ctl" "the control event count"
if [ "$ctl" -eq 0 ]; then
    sed -n '1,5p' "$TRACE"
    fail "the trace carries no $CTL_NAME line at all, which every boot of this board produces.
  The event names or the log format have moved, so this parse reports an absence it cannot
  distinguish from a failure to read"
fi
echo "   trace parse control: $ctl $CTL_NAME(s)"

HEAD="$TMP/chan.head"
if [ "$HAS_RAISE" -eq 0 ]; then
    # NO RAISE ORACLE ON THIS BACKEND, so the raise arm and the cross-check against the round
    # count are both absent and this gate does not pretend otherwise. The window is cut at the
    # emulator's own first record of userspace instead: the bring-up round runs before any
    # thread has issued a syscall, so everything before the first one is the boot's.
    awk '/desc=user_ecall/ { exit } { print }' "$TRACE" > "$HEAD"
    require_nonempty "$HEAD" "the boot window is empty, so the cut found no userspace entry to
  stop at and every assertion below it would read a window that is not the boot's"
    echo "   NO RAISE ORACLE: the CLINT store carries no trace event, so this run checks that
   every peer TOOK the doorbell and not that the initiator raised it. Window cut at the first
   userspace entry: $(wc -l < "$HEAD") of $(wc -l < "$TRACE") traced line(s)"
else

# A raise happened at all, and at least as many as the image says rounds it ran.
raises="$(grep -c -F -e "$SGIR_LINE" "$TRACE")" || raises=0
require_number "$raises" "the GICD_SGIR write count"
if [ "$raises" -eq 0 ]; then
    fail "QEMU's GIC model logged no write to GICD_SGIR ($SGIR_LINE). No software-generated
  interrupt was ever raised, whatever the image printed about cores answering"
fi
if [ "$raises" -lt "$rounds" ]; then
    fail "QEMU counted $raises GICD_SGIR write(s) and the image reports $rounds round(s). The
  two numbers reach this gate by different routes and one raise per round is what the send
  owes; fewer means rounds completed without reaching the controller"
fi

# THE ROUND'S OWN WINDOW: every line up to and including the round's last raise. Its raises are
# the first in the boot, so counting them off is what finds the boundary, and everything the
# round owes, each peer's acknowledgement of the raise it answered, lies inside it.
awk -v want="$rounds" -v pat="$SGIR_LINE" '
    index($0, pat) > 0 { seen = seen + 1; if (seen > want) { exit } }
    { print }
' "$TRACE" > "$HEAD"
require_nonempty "$HEAD" "the round's window is empty, so the cut found no raise to count off"
head_raises="$(grep -c -F -e "$SGIR_LINE" "$HEAD")" || head_raises=0
require_number "$head_raises" "the windowed GICD_SGIR write count"
if [ "$head_raises" -ne "$rounds" ]; then
    fail "the round's window holds $head_raises raise(s) for $rounds round(s), so the cut did
  not land: every assertion below it would read a window that is not the round's"
fi
echo "   the emulator counted $raises GICD_SGIR write(s), $head_raises of them the round's,
   over $(wc -l < "$HEAD") of $(wc -l < "$TRACE") traced line(s)"
fi

# EVERY SECONDARY'S CPU INTERFACE ACKNOWLEDGED THE DOORBELL, which the image cannot state: the
# interface number is the controller's and the acknowledgement is a GICC_IAR read QEMU
# observed.
missing=""
core=1
while [ "$core" -lt "$want" ]; do
    n="$(kos_doorbell_count "$HEAD" "$core")" || n=0
    require_number "$n" "the doorbell count for core $core"
    if [ "$n" -eq 0 ]; then
        missing="$missing $core"
    else
        echo "   core $core took the doorbell $n time(s)"
    fi
    core=$((core + 1))
done
if [ -n "$missing" ]; then
    sed -n '1,10p' "$HEAD"
    fail "$CHAN_NAME reports no doorbell taken by core(s)$missing.
      A core already spinning in the lock's acquire loop answers by POLLING and acknowledges
      nothing, so this arm cannot separate an interrupt path that never ran from a core the host
      starved. The test is registered RUN_SERIAL for that reason; a parallel ctest reopens it.
  Those interfaces never took the doorbell, so whatever answered the initiator did not arrive
  through the controller"
fi

# THE INITIATOR SERVICES ITS OWN BIT INLINE AND MUST NOT RAISE ON ITSELF: a self-raise taken
# while the initiator waits with interrupts masked is a deadlock.
self="$(kos_doorbell_count "$HEAD" 0)" || self=0
require_number "$self" "the self-doorbell count"
if [ "$self" -ne 0 ]; then
    fail "core 0 took the doorbell $self time(s) inside the boot window: the initiator raised
  it on itself instead of servicing its own bit in the send"
fi

if [ "$HAS_RAISE" -eq 1 ]; then
    echo "PASS: $want core(s) answer the doorbell over $rounds round(s) held under the kernel
  lock; $CHAN_NAME witnesses the raise and every secondary acknowledging it"
else
    echo "PASS: $want core(s) answer the doorbell over $rounds round(s) held under the kernel
  lock; $CHAN_NAME witnesses every secondary TAKING it and the initiator taking none. THE
  RAISE ITSELF IS UNWITNESSED on this backend: the CLINT store carries no trace event, so no
  arm here counts raises and the image's own round count is the only statement about them"
fi
exit 0
