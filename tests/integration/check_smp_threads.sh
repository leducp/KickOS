#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Gate on threads actually RUNNING on every core of a qemu-arm64 image built at more than one
# core. One boot, read through two channels, both of them the emulator's own.
#
#   the oracle    QEMU's own execution log must show the APP's `svc` stub executed under every
#                 vCPU index. The stub is the unprivileged side of the trap seam
#                 (arch_syscall, in the app half), so a line under index N says vCPU N fetched
#                 and executed instructions that only a thread at EL0 reaches.
#   the check     QEMU's GIC model must report every core's CPU interface acknowledging an
#                 interrupt, which says each core is live and its own interface is enabled.
#
# THE INDEX ON A `Trace N:` LINE IS THE EMULATOR'S OWN cpu_index for the vCPU executing that
# translation block, which guest code cannot write, forge across vCPUs, or reach on a core the
# kernel never released (the machine's PSCI holds every secondary until CPU_ON). The filtered
# address is the APP half's stub, a separate symbol from the kernel's own trap leaf, so a
# kernel-side call cannot stand in for a thread.
#
# THE TWO CHANNELS ARE SAMPLED UNTIL THEY ARE COMPLETE, bounded by QEMU_TIMEOUT, and the run
# stops the moment they are. A bound that expires FAILS and names the vCPUs still outstanding:
# a core the workload has not reached YET and a core threads never reach are the same reading
# at any single instant, and only a bound separates them.
#
# WHAT THE WORKLOAD OWES. The app must keep more runnable threads than the machine has cores,
# so a core running no thread is a core the scheduler left idle beside a ready thread rather
# than a migration this boot happened not to make. An app whose threads hand off strictly one
# at a time offers ONE runnable thread, and which core takes it is then a race the host
# decides; under host saturation it is decided the same way for the whole run and no bound
# recovers it. `STRESS SKIP` is refused below for that reason: it says the pool was too small
# to size the soak, so the parallelism this gate rests on never existed.
#
# The image's `# smp sched:` line is a cross-check and never the verdict: its core figure comes
# from the configuration symbol this gate is handed, so the two agree whatever the scheduler
# did.
#
# The parse control runs before any absence is judged: an execution log with no stub line at
# all means the filter or the log format moved, which is UNKNOWN rather than a core that ran
# no thread.
#
# A GREEN SAYS EVERY vCPU EXECUTED A THREAD, AND NOTHING ABOUT BALANCE: how the work was split
# across the cores, in what order they were reached, and whether a core kept a thread for any
# length of time are all outside it. It licenses no claim about migration either: a thread per
# core and one thread visiting four cores read identically here.
#
# usage: check_smp_threads.sh <elf> <expect-cores> <live-ere> <nm>

set -u
. "$(dirname "$0")/../lib/gate.sh"
# The bound on the SAMPLING, not a fixed window: the run ends when both channels are complete,
# and a full suite under an execution log is what makes the failure path this long.
: "${QEMU_TIMEOUT:=120}"

_usage="usage: check_smp_threads.sh <elf> <expect-cores> <live-ere> <nm>"
elf="${1:?$_usage}"
want="${2:?$_usage}"
live="${3:?$_usage}"
nm="${4:?$_usage}"

require_number "$want" "the expected core count"
require_literal "$live" "the liveness pattern"
if [ "$want" -le 1 ]; then
    fail "expected core count is $want. At one core there is one vCPU index to find and the
  oracle is satisfied by any image at all, so this gate belongs only on a preset whose core
  count exceeds one"
fi
[ -f "$elf" ] || fail "no image at $elf"
[ -x "$nm" ] || fail "no nm at $nm; the stub's address cannot be read out of the image and
  every assertion below would rest on a hard-coded layout"
need_qemu_machine
need_qemu

# The unprivileged side of the trap seam, in the APP half. Named as a symbol: a gate carrying
# the layout would keep passing after the layout moved.
STUB_SYM=arch_syscall
# The two instructions of that stub (`svc`, then the return): a window around the address a
# translation block must be ENTERED at below.
STUB_WINDOW=8

# The trace event this gate's second channel reads, as QEMU's model spells it, and which
# controller is modelled read off QEMU_MACHINE: the same string this gate hands the emulator, so
# the parse cannot be looking for one model while the run boots the other. ACK_PREFIX runs up to
# the core number and ACK_SUFFIX is what a planted line puts after it.
#
# THE TIMER PPI HAS ONE SOURCE HERE, and the suffixes are PRINTED from it rather than typed:
# the two models spell the same INTID in different radixes, so a second hand-written copy could
# disagree with the first and the planted control would then prove the parse against a line the
# emulator never emits.
TIMER_INTID=30
case " ${QEMU_MACHINE:-} " in
    *gic-version=3*)
        TRACE_ACK=gicv3_icc_iar1_read
        ACK_PREFIX="gicv3_icc_iar1_read GICv3 ICC_IAR1 read cpu 0x"
        ACK_SUFFIX="$(printf ' value 0x%x' "$TIMER_INTID")" ;;
    *)
        TRACE_ACK=gic_acknowledge_irq
        ACK_PREFIX="gic_acknowledge_irq cpu "
        ACK_SUFFIX="$(printf ' acknowledged irq %d' "$TIMER_INTID")" ;;
esac

# The core number as the modelled controller's log spells it: GICv3's is HEXADECIMAL, and the
# conversion is made rather than assumed. Nothing here rests on the two radixes coinciding, so
# no core count below sixteen is assumed either.
ack_cpu_token() { # <core>
    if [ "$TRACE_ACK" = gicv3_icc_iar1_read ]; then
        printf '%x' "$1"
        return
    fi
    printf '%d' "$1"
}

# The image's own cross-check line, the app's refusal and the chip's, each as the emitter
# spells it (kernel/init/kmain.cc, user/apps/common/stress/main.cc). Each is matched as a
# literal and each passes require_literal first: an empty marker makes every
# absence-assertion below vacuous.
SCHED_HEAD="# smp sched: "
SCHED_TAIL=" core(s) in the scheduler"
PEER_STUCK="KickOS: kernel core never reached its scheduler: core "
NO_SPREAD="STRESS SKIP (board thread/sem pool too small)"
RUNNABLE="stress: runnable "
for _m in "$SCHED_HEAD" "$SCHED_TAIL" "$PEER_STUCK" "$NO_SPREAD" "$RUNNABLE" "$STUB_SYM" \
          "$TRACE_ACK" "$ACK_PREFIX" "$ACK_SUFFIX"; do
    require_literal "$_m" "a gate marker"
done

# CAN THIS EMULATOR REPORT THE ORACLE AT ALL. Asked of the binary: an absent facility would
# otherwise read as a core that ran no thread, which is the finding this gate exists to make.
if ! "$QEMU_BIN" -d 'help' 2>&1 | grep -qE '^exec[ ,]'; then
    echo "SKIP: $QEMU_BIN reports no 'exec' log item, so the per-core execution oracle is
  unavailable and only the image's own count would be left"
    exit 77
fi
if ! "$QEMU_BIN" -d 'trace:help' 2>&1 | grep -q "^$TRACE_ACK\$"; then
    echo "SKIP: $QEMU_BIN reports no '$TRACE_ACK' trace event"
    exit 77
fi

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
LOG="$TMP/exec.log"

# --- The stub's address, out of the image itself -------------------------------
tool_out "$TMP/nm" "[0-9a-fA-F]" "$nm" --defined-only "$elf"
stub_hex="$(sed -n "s/^\\([0-9a-fA-F][0-9a-fA-F]*\\) [Tt] $STUB_SYM\$/\\1/p" "$TMP/nm" \
    | head -n1)"
case "$stub_hex" in
    ''|*[!0-9a-fA-F]*) fail "no text symbol '$STUB_SYM' in $elf. The trap stub is what an
  unprivileged thread executes, so without it there is nothing to filter on" ;;
esac
stub_dec="$(printf '%d' "0x$stub_hex")"
require_number "$stub_dec" "the stub address"
# QEMU prints a translation block's PC as sixteen zero-padded hex digits.
stub_pc="$(printf '%016x' "$stub_dec")"
require_literal "$stub_pc" "the stub's printed program counter"
echo "== $want core(s), $STUB_SYM at 0x$stub_pc under an execution log =="

# --- What the sampling waits for ----------------------------------------------
# Both channels, re-read out of the growing log on every tick. The two lists are what an
# expired bound reports, so they are rebuilt from scratch each time rather than accumulated:
# a stale entry would name a core that has since been witnessed.
#
# Nothing here reaches a verdict. An unreadable log, a log QEMU has not created yet and a log
# holding no line of either shape all mean NOT YET, and the assertions below the poll are
# where an absence is judged.
KOS_MISS_STUB=""
KOS_MISS_ACK=""

kos_spread_seen() {
    KOS_MISS_STUB=""
    KOS_MISS_ACK=""
    _seen_stub=" $(sed -n "s/^Trace \\([0-9][0-9]*\\):.*\\/$stub_pc\\/.*/\\1/p" "$LOG" \
        2>/dev/null | sort -u | tr '\n' ' ')"
    _seen_ack=" $(sed -n "s/^$ACK_PREFIX\\([0-9a-f][0-9a-f]*\\) .*/\\1/p" "$LOG" \
        2>/dev/null | sort -u | tr '\n' ' ')"
    _core=0
    while [ "$_core" -lt "$want" ]; do
        case "$_seen_stub" in
            *" $_core "*) ;;
            *) KOS_MISS_STUB="$KOS_MISS_STUB $_core" ;;
        esac
        case "$_seen_ack" in
            *" $(ack_cpu_token "$_core") "*) ;;
            *) KOS_MISS_ACK="$KOS_MISS_ACK $_core" ;;
        esac
        _core=$((_core + 1))
    done
    if [ -n "$KOS_MISS_STUB" ] || [ -n "$KOS_MISS_ACK" ]; then
        return 1
    fi
    return 0
}

# The control on that predicate, on a planted log, before a run is judged by it. Both
# directions: a predicate satisfied by a log that is one vCPU short would end the sampling
# early and report a core as witnessed, and one that a complete log cannot satisfy would burn
# the whole bound on every green run.
kos_spread_control() {
    _keep="$LOG"
    LOG="$TMP/control.log"
    _last=$((want - 1))
    _core=0
    while [ "$_core" -lt "$want" ]; do
        printf 'Trace %s: 0xdeadbeef [ffffffffffffffff/%s/0x0/0x0] \n' "$_core" "$stub_pc"
        printf '%s%s%s\n' "$ACK_PREFIX" "$(ack_cpu_token "$_core")" "$ACK_SUFFIX"
        _core=$((_core + 1))
    done > "$TMP/control.full"
    cp "$TMP/control.full" "$LOG"
    kos_spread_seen \
        || fail "the sampling predicate is not satisfied by a planted log naming every one of
      $want core(s) on both channels (stub short of [$KOS_MISS_STUB], acknowledgement short
      of [$KOS_MISS_ACK]), so every run would burn its whole bound"
    # One core dropped from each channel, one channel at a time: a predicate reading only the
    # other one passes this and reports nothing.
    grep -v "^Trace $_last:" "$TMP/control.full" > "$LOG"
    if kos_spread_seen; then
        fail "the sampling predicate is satisfied by a log carrying no stub block for vCPU
      $_last, so it would stop the sampling with a core unwitnessed and the run would be
      judged on a log nobody waited for"
    fi
    grep -v "^$ACK_PREFIX$(ack_cpu_token "$_last") " "$TMP/control.full" > "$LOG"
    if kos_spread_seen; then
        fail "the sampling predicate is satisfied by a log carrying no acknowledgement by cpu
      $_last, so it ignores its second channel"
    fi
    rm -f "$TMP/control.full" "$LOG"
    LOG="$_keep"
}
kos_spread_control
echo "   sampling predicate control: satisfied by a complete log, refused by either channel
   one core short"

# --- The boot ------------------------------------------------------------------
# nochain, so a block reached through a chain from an earlier one is still logged: short of it
# a core that entered the stub only through a chain leaves no line. dfilter bounds the log to
# the stub's own window.
QEMU_EXTRA="${QEMU_EXTRA:-} -d exec,nochain,trace:$TRACE_ACK"
QEMU_EXTRA="$QEMU_EXTRA -dfilter 0x$stub_hex+$STUB_WINDOW -D $LOG"
KOS_POLL_UNTIL=kos_spread_seen
poll_image "$elf" "$live"

# HOW THE SAMPLING ENDED, for the refusals below. A bound that ran out and an image that
# stopped before the channels were complete are different findings, and reporting either as the
# other sends the reader to the wrong place.
sampling_end="the sampling bound of ${QEMU_TIMEOUT}s ran out after ${POLL_MS} ms"
if [ "$POLL_ALIVE" -eq 0 ]; then
    sampling_end="the image ended after ${POLL_MS} ms of sampling, inside the
  ${QEMU_TIMEOUT}s bound"
fi

# THE REFUSALS FIRST, so a core that never reached its scheduler and a workload that never
# sized up are reported on the line the image printed rather than on the vCPU index the log
# then lacks.
count_literal "$PEER_STUCK"
if [ "$KOS_COUNT" -ne 0 ]; then
    printf '%s\n' "$OUT" | grep -F -e "$PEER_STUCK"
    fail "a core never reached its scheduler: the boot core waited out its bound with a peer
  still parked, so that peer has no idle thread of its own to fall back on"
fi
assert_no_panic "the image panicked while its threads were being counted"
count_literal "$NO_SPREAD"
if [ "$KOS_COUNT" -ne 0 ]; then
    printf '%s\n' "$OUT" | grep -F -e "$NO_SPREAD"
    fail "the app sized its soak down to nothing, so it never held more runnable threads than
  the machine has cores. Every core running a thread would then be a migration the host
  happened to make and not an obligation the scheduler owes, whichever way the count came out"
fi

# THE WORKLOAD IS READ, NOT ASSUMED. The app's budget probe shrinks its soak independently of
# the compile-time maxima, and a soak at or below the core count makes every arm below a
# lottery outcome rather than a scheduling obligation. The app reports what it realized.
runnable="$(printf '%s\n' "$OUT" \
    | sed -n "s/^${RUNNABLE}\([0-9][0-9]*\).*$/\1/p" | tail -n 1)"
if [ -z "$runnable" ]; then
    fail "the image printed no '${RUNNABLE}<n>' line, so the realized soak size is UNKNOWN
  rather than sufficient. This gate rests on the app holding more runnable threads than the
  machine has cores, and nothing here can check that without the app saying so"
fi
require_number "$runnable" "the realized runnable count"
if [ "$runnable" -le "$want" ]; then
    fail "the app realized $runnable runnable thread(s) on a $want-core machine, so a core
  running a thread is a migration the host happened to make rather than an obligation the
  scheduler owes. Raise the soak's pairs and sleepers, or the board's thread and semaphore
  pools, until the count exceeds the core count"
fi

# --- The parse control, before any absence is judged --------------------------
require_nonempty "$LOG" "QEMU wrote no log at $LOG, so the oracle is UNKNOWN rather than
  negative and every assertion below it would read as an absence"
stub_lines="$(grep -c "^Trace [0-9][0-9]*:.*/$stub_pc/" "$LOG")" || stub_lines=0
require_number "$stub_lines" "the stub execution count"
if [ "$stub_lines" -eq 0 ]; then
    sed -n '1,5p' "$LOG"
    fail "the execution log carries no block at /$stub_pc/, which every boot of this image
  produces the moment root makes its first syscall. The log format, the filter or the symbol
  has moved, so this parse reports an absence it cannot distinguish from a failure to read"
fi
echo "   parse control: $stub_lines block(s) entered at the stub over ${POLL_MS} ms"

# --- The oracle: every vCPU executed the unprivileged trap stub ---------------
cpus="$(sed -n "s/^Trace \\([0-9][0-9]*\\):.*\\/$stub_pc\\/.*/\\1/p" "$LOG" | sort -u)"
if [ -z "$cpus" ]; then
    fail "the stub blocks carry no vCPU index at all, so the log's own attribution could not
  be read and no per-core claim can rest on it"
fi
missing=""
core=0
while [ "$core" -lt "$want" ]; do
    n="$(grep -c "^Trace $core:.*/$stub_pc/" "$LOG")" || n=0
    require_number "$n" "the stub execution count for vCPU $core"
    if [ "$n" -eq 0 ]; then
        missing="$missing $core"
    else
        echo "   vCPU $core executed the unprivileged trap stub $n time(s)"
    fi
    core=$((core + 1))
done
if [ -n "$missing" ]; then
    echo "      vCPU indices seen: $(printf '%s' "$cpus" | tr '\n' ' ')"
    fail "$sampling_end, and QEMU's execution log attributes no unprivileged trap stub to
  vCPU(s)$missing, so no thread ran there. A core that reaches no scheduler, an idle thread
  with no peer to schedule and a wake that never leaves the waking core all present exactly
  this way"
fi

# --- The second channel: every core's interface took an interrupt -------------
# Its own parse control first, on ANY core: the timer PPI is acknowledged on every boot of
# this board, so a trace carrying no acknowledgement at all is a format that moved rather than
# four interfaces that took nothing.
acks="$(grep -c "^$ACK_PREFIX[0-9a-f][0-9a-f]* " "$LOG")" || acks=0
require_number "$acks" "the acknowledgement count"
if [ "$acks" -eq 0 ]; then
    grep "^$TRACE_ACK" "$LOG" | sed -n '1,5p'
    fail "the log carries no acknowledgement line on any core, which every boot of this board
  produces. The event name or the log format has moved, so the per-core reading below reports
  an absence it cannot distinguish from a failure to read"
fi

ack_missing=""
core=0
while [ "$core" -lt "$want" ]; do
    n="$(grep -c "^$ACK_PREFIX$(ack_cpu_token "$core") " "$LOG")" || n=0
    require_number "$n" "the acknowledgement count for cpu $core"
    if [ "$n" -eq 0 ]; then
        ack_missing="$ack_missing $core"
    fi
    core=$((core + 1))
done
if [ -n "$ack_missing" ]; then
    grep "^$TRACE_ACK" "$LOG" | sort -u | sed -n '1,10p'
    fail "$sampling_end, and QEMU's GIC model reports no acknowledgement by cpu(s)$ack_missing.
  Those interfaces took no interrupt at all, so whatever ran on them was never preempted or
  woken through the controller"
fi
echo "   every core's interface acknowledged an interrupt"

# The poll's own verdict on the same two channels, which the two loops above have just
# re-derived off the final log. A disagreement is this gate's parse against itself.
if [ "$POLL_UNTIL_OK" -ne 1 ]; then
    fail "the per-core loops found both channels complete and the poll's own predicate does
  not (stub short of [$KOS_MISS_STUB], acknowledgement short of [$KOS_MISS_ACK]): the two read
  one log and one of them is wrong"
fi

# --- The image's own line, as a cross-check and never as the verdict ----------
count_literal "$SCHED_HEAD$want$SCHED_TAIL"
if [ "$KOS_COUNT" -ne 1 ]; then
    printf '%s\n' "$OUT" | grep -F -e "$SCHED_HEAD"
    fail "the image does not report $want core(s) in the scheduler exactly once. The oracle
  above says threads ran on every core, so the two channels disagree and one of them is wrong"
fi
if [ "$POLL_OK" -ne 1 ]; then
    fail "the per-core stub executions are on the wire but /$live/ is not: the app half never
  reached its own first line inside ${QEMU_TIMEOUT}s"
fi

echo "PASS: $want core(s) each executed the unprivileged trap stub within ${POLL_MS} ms of
  sampling, so a thread ran on every one; QEMU's own execution log is the witness and the
  image supplied none of it"
exit 0
