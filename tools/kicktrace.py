#!/usr/bin/env python3
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# KickOS telemetry decoder (telemetry.md deliverable 8). Pure bytes -> CSV /
# summary / Chrome-trace JSON. Decodes the little-endian, fixed-length-by-type
# record stream produced by the kernel frontend (see include/kickos/trace/record.h)
# and the RTT ch1 sink; the sim flushes that ring to a file at shutdown.
#
# What it does, and why:
#   * self-delimiting parse: type -> length, never guesses; an unknown type
#     triggers a resync scan for the next SESSION magic.
#   * two-anchor resync: two SESSION records each pair a u32 trace stamp with a
#     u64 ns clock anchor; two points define the tick->ns rate AND let us unwrap
#     the u32 trace clock across its ~wrap without knowing the frequency a priori.
#   * loss accounting: the seq number is monotonic per record; a gap means the
#     sink dropped (ring full) or the link lost records. Cross-checked against the
#     SESSION records_attempted count.
#   * rule (f): a seq gap POISONS any open ENTER/EXIT pair straddling it (the pair
#     is discarded, not mis-measured) and EXCLUDES that time span from CPU%.
#   * CPU%: from the SWITCH chain (tid 0 == idle); ISR time that ran while idle
#     was current is subtracted from idle (the CPU was busy in the ISR).
#   * latency stats for switch / syscall / IRQ, and a Chrome-trace JSON dump.

import sys
import struct
import argparse
import json

# --- wire constants (keep in sync with include/kickos/trace/record.h) --------
EV_SESSION = 1
EV_SWITCH = 2
EV_SYSCALL_ENTER = 3
EV_SYSCALL_EXIT = 4
EV_IRQ_ENTER = 5
EV_IRQ_EXIT = 6

TYPE_NAME = {
    EV_SESSION: "SESSION",
    EV_SWITCH: "SWITCH",
    EV_SYSCALL_ENTER: "SYSCALL_ENTER",
    EV_SYSCALL_EXIT: "SYSCALL_EXIT",
    EV_IRQ_ENTER: "IRQ_ENTER",
    EV_IRQ_EXIT: "IRQ_EXIT",
}

REC_LEN = {
    EV_SESSION: 28,
    EV_SWITCH: 11,
    EV_SYSCALL_ENTER: 11,
    EV_SYSCALL_EXIT: 11,
    EV_IRQ_ENTER: 9,
    EV_IRQ_EXIT: 9,
}

ARCH_NAME = {0: "sim", 1: "armv7m", 2: "armv6m", 3: "xtensa", 4: "rx", 5: "riscv"}

# syscall numbers -> names (keep in sync with user/include/kickos/sys/abi.h). Used
# to break syscall cost down per-call. *sleep_ns* and *sem_wait* are BLOCKING: their
# ENTER..EXIT span includes intended block time, so judge them by on-CPU overhead.
SYSCALL_NAME = {
    1: "kconsole_write", 2: "yield", 3: "sleep_ns", 4: "sem_create", 5: "sem_wait",
    6: "sem_post", 7: "thread_spawn", 8: "exit", 9: "irq_inject", 10: "guard_addr",
    11: "irq_attach", 12: "clock_now", 13: "ram_alloc", 14: "irq_register",
    15: "irq_wait", 16: "irq_ack", 17: "sem_destroy", 18: "irq_spurious",
    19: "diag_led_set", 20: "diag_led_toggle",
}

TRACE_MAGIC = 0x4B545243
TIMER_LINE = 0xFFFE
NO_THREAD = 0xFFFF
# Note: KOS_SYS_exit is a no-return syscall, recorded as SYSCALL_ENTER with no
# matching EXIT (one per exiting thread). The structural check tolerates unmatched
# ENTERs (exit(), or threads blocked at shutdown) and flags only orphan EXITs.
SEQ_MOD = 1 << 16
TS_MOD = 1 << 32


class Record:
    __slots__ = ("type", "seq", "t", "fields", "abs_t")

    def __init__(self, type_, seq, t, fields):
        self.type = type_
        self.seq = seq
        self.t = t
        self.fields = fields  # dict of payload fields
        self.abs_t = t

    def canon(self):
        # Canonical one-line form (golden-vector round-trip compares this).
        name = TYPE_NAME.get(self.type, "UNKNOWN")
        f = self.fields
        if self.type == EV_SESSION:
            return ("SESSION seq=%d t=%d ver=%d arch=%d ts_bits=%d probe=%d "
                    "attempted=%d anchor=%d" % (
                        self.seq, self.t, f["ver"], f["arch"], f["ts_bits"],
                        f["probe_overhead"], f["records_attempted"], f["t_anchor"]))
        if self.type == EV_SWITCH:
            return "SWITCH seq=%d t=%d from=%d to=%d" % (
                self.seq, self.t, f["from"], f["to"])
        if self.type in (EV_SYSCALL_ENTER, EV_SYSCALL_EXIT):
            return "%s seq=%d t=%d tid=%d nr=%d" % (
                name, self.seq, self.t, f["tid"], f["nr"])
        if self.type in (EV_IRQ_ENTER, EV_IRQ_EXIT):
            return "%s seq=%d t=%d line=%d" % (name, self.seq, self.t, f["line"])
        return "UNKNOWN seq=%d t=%d" % (self.seq, self.t)


def _u16(b, o):
    return b[o] | (b[o + 1] << 8)


def _u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def _u64(b, o):
    return struct.unpack_from("<Q", b, o)[0]


def parse(data):
    """Walk the byte stream into Records. Returns (records, parse_errors)."""
    recs = []
    errors = []
    i = 0
    n = len(data)
    while i < n:
        t = data[i]
        rl = REC_LEN.get(t)
        if rl is None or i + rl > n:
            # Unknown type or truncated tail: resync to the next SESSION magic.
            j = _find_magic(data, i + 1)
            if j is None:
                errors.append("unparseable tail at offset %d (%d bytes)" % (i, n - i))
                break
            errors.append("resync: skipped %d bytes at offset %d" % (j - i, i))
            i = j
            continue
        seq = _u16(data, i + 1)
        ts = _u32(data, i + 3)
        fields = {}
        if t == EV_SESSION:
            magic = _u32(data, i + 7)
            if magic != TRACE_MAGIC:
                j = _find_magic(data, i + 1)
                if j is None:
                    errors.append("bad SESSION magic at %d, no resync" % i)
                    break
                errors.append("bad SESSION magic at %d; resync +%d" % (i, j - i))
                i = j
                continue
            fields = {
                "ver": data[i + 11],
                "arch": data[i + 12],
                "ts_bits": data[i + 13],
                "probe_overhead": _u16(data, i + 14),
                "records_attempted": _u32(data, i + 16),
                "t_anchor": _u64(data, i + 20),
            }
        elif t == EV_SWITCH:
            fields = {"from": _u16(data, i + 7), "to": _u16(data, i + 9)}
        elif t in (EV_SYSCALL_ENTER, EV_SYSCALL_EXIT):
            fields = {"tid": _u16(data, i + 7), "nr": _u16(data, i + 9)}
        elif t in (EV_IRQ_ENTER, EV_IRQ_EXIT):
            fields = {"line": _u16(data, i + 7)}
        recs.append(Record(t, seq, ts, fields))
        i += rl
    return recs, errors


def _find_magic(data, start):
    # SESSION magic bytes (LE) 0x4B545243 sit at offset +7 of a session record;
    # scan for them and return the record start (magic_pos - 7), or None.
    needle = struct.pack("<I", TRACE_MAGIC)
    j = data.find(needle, start)
    while j != -1:
        if j - 7 >= 0 and data[j - 7] == EV_SESSION:
            return j - 7
        j = data.find(needle, j + 1)
    return None


def unwrap(records):
    """Reconstruct a monotonic 64-bit trace time for each record (u32 wrap) and
    attach it as .abs_t. Records are in emit (file) order."""
    hi = 0
    last = None
    for r in records:
        if last is not None and r.t < last:
            # a backward step within the modulus => a u32 wrap occurred
            hi += TS_MOD
        last = r.t
        r.abs_t = hi + r.t


def clock_model(records):
    """Two-anchor tick->ns model from the first and last SESSION records.
    Returns (rate_ns_per_tick, base_abs_t, base_anchor_ns) or None."""
    sess = [r for r in records if r.type == EV_SESSION]
    if len(sess) >= 2:
        a, b = sess[0], sess[-1]
        dt = b.abs_t - a.abs_t
        if dt > 0:
            rate = (b.fields["t_anchor"] - a.fields["t_anchor"]) / dt
            return (rate, a.abs_t, a.fields["t_anchor"])
    if len(sess) == 1:
        # Single anchor: arch_trace_now (the tick counter) and arch_clock_now (the
        # t_anchor ns) read the SAME clock zeroed at the same origin, so the anchor's
        # ns/tick = t_anchor / abs_t -- a real rate from one SESSION (no delta needed).
        a = sess[0]
        if a.abs_t > 0:
            return (a.fields["t_anchor"] / a.abs_t, a.abs_t, a.fields["t_anchor"])
    return None  # zero anchors (or a t=0 anchor): rate unknown -> ticks, or --clock-hz


def to_ns(model, abs_t):
    rate, base_t, base_ns = model
    return base_ns + (abs_t - base_t) * rate


def loss_scan(records):
    """Walk seq numbers; return (lost_total, gap_indices) where gap_indices are
    record positions AFTER which a gap was detected (rule (f) uses these)."""
    lost = 0
    gaps = set()
    expected = None
    for idx, r in enumerate(records):
        if expected is not None and r.seq != expected:
            miss = (r.seq - expected) % SEQ_MOD
            lost += miss
            gaps.add(idx)  # a gap sits immediately before record idx
        expected = (r.seq + 1) % SEQ_MOD
    return lost, gaps


def stats(vals):
    if not vals:
        return None
    s = sorted(vals)
    n = len(s)
    return {
        "count": n,
        "min": s[0],
        "max": s[-1],
        "mean": sum(s) / n,
        "p50": s[n // 2],
        "p95": s[min(n - 1, int(n * 0.95))],
    }


def syscall_overheads(records, slices, ns, gaps, probe_ns=0.0):
    """True on-CPU cost of each syscall: the issuing thread's RUNNING time within
    its SYSCALL_ENTER..EXIT window. A blocking syscall (sleep_ns, sem_wait) yields
    the CPU to idle/others and resumes later, so its raw enter->exit SPAN includes
    that block -- NOT a cost. Intersecting the window with the thread's run slices
    (the SWITCH-derived on-CPU intervals) subtracts the blocked-away time, leaving
    the real overhead. Returns [(nr, overhead_ns)] (ENTER/EXIT matched per tid, FIFO;
    pairs straddling a seq gap dropped)."""
    pending = {}  # tid -> [(idx, nr, t_enter)]
    out = []
    for idx, r in enumerate(records):
        if r.type == EV_SYSCALL_ENTER:
            pending.setdefault(r.fields["tid"], []).append((idx, r.fields["nr"], ns(r)))
        elif r.type == EV_SYSCALL_EXIT:
            q = pending.get(r.fields["tid"])
            if not q:
                continue
            i0, nr, t0 = q.pop(0)
            if _crossed_gap(i0, idx, gaps):
                continue
            tid = r.fields["tid"]
            t1 = ns(r)
            on_cpu = 0.0
            for (stid, s, e) in slices:
                if stid != tid:
                    continue
                lo = s if s > t0 else t0
                hi = e if e < t1 else t1
                if hi > lo:
                    on_cpu += hi - lo
            out.append((nr, max(0.0, on_cpu - probe_ns)))
    return out


def analyze(records, model, gaps):
    """CPU% + latency stats. Honours rule (f): pairs straddling a gap are dropped,
    and gap spans are excluded from the CPU% denominator."""
    def ns(r):
        if model is None:
            return float(r.abs_t)
        return to_ns(model, r.abs_t)

    # probe_overhead (from the first SESSION) is the cost of one arch_trace_now
    # read, in TRACE TICKS. Convert to ns via the clock model and subtract it once
    # per measured interval (each interval spans two reads; one read's bias is the
    # dominant correction) so reported latencies approach the true cost.
    probe_ns = 0.0
    sess = [r for r in records if r.type == EV_SESSION]
    if sess:
        ticks = sess[0].fields["probe_overhead"]
        if model is not None:
            probe_ns = ticks * model[0]
        else:
            probe_ns = float(ticks)

    # --- run slices from the SWITCH chain (carry record indices for gap tests) --
    switches = []        # (idx, record)
    for idx, r in enumerate(records):
        if r.type == EV_SWITCH:
            switches.append((idx, r))
    run_ns = {}          # tid -> ns run
    slices = []          # (tid, start_ns, end_ns) for chrome/cpu
    # rule (f): a run slice whose index range crosses a seq gap is poisoned --
    # drop it from BOTH the numerator (run_ns) AND the denominator (excluded).
    excluded = 0.0
    for (ia, a), (ib, b) in zip(switches, switches[1:]):
        start, end = ns(a), ns(b)
        if end < start:
            continue
        if _crossed_gap(ia, ib, gaps):
            excluded += (end - start)
            continue
        tid = a.fields["to"]
        run_ns[tid] = run_ns.get(tid, 0.0) + (end - start)
        slices.append((tid, start, end))

    total = 0.0
    if len(records) >= 2:
        total = ns(records[-1]) - ns(records[0])
    denom = total - excluded
    idle = run_ns.get(0, 0.0)

    # ISR-during-idle: IRQ enter/exit while idle (tid 0) was current -> not idle.
    # Union the spans (an IRQ pair nesting inside another must be counted once,
    # not summed twice -- summing overstates ISR time and thus CPU%).
    idle_isr_spans = []
    for line, dur, start, tid_at in irq_pairs(records, switches, ns, gaps, probe_ns):
        if tid_at == 0:
            idle_isr_spans.append((start, start + dur))
    isr_during_idle = _union_len(idle_isr_spans)
    idle_busy = max(0.0, idle - isr_during_idle)
    cpu = None
    if denom > 0:
        cpu = 100.0 * (denom - idle_busy) / denom

    # --- latency stats (probe-overhead-corrected) -----------------------------
    sys_lat = pair_latencies(records, EV_SYSCALL_ENTER, EV_SYSCALL_EXIT, "tid", "nr",
                             ns, gaps, probe_ns)
    # true on-CPU cost (block time subtracted), overall + per syscall number. The
    # CPU% `slices` cover only switch-to-switch runs; add the leading + trailing
    # runs (before the first / after the last SWITCH) so a syscall in the boundary
    # run is still counted -- these are local to the overhead calc, not CPU%.
    over_slices = list(slices)
    if switches and len(records) >= 2:
        _, first_sw = switches[0]
        _, last_sw = switches[-1]
        frm = first_sw.fields["from"]
        if frm != NO_THREAD:
            over_slices.append((frm, ns(records[0]), ns(first_sw)))
        over_slices.append((last_sw.fields["to"], ns(last_sw), ns(records[-1])))
    sys_over = syscall_overheads(records, over_slices, ns, gaps, probe_ns)
    by_nr = {}
    for nr, oc in sys_over:
        by_nr.setdefault(nr, []).append(oc)
    irq_lat = [d for (_l, d, _s, _t) in irq_pairs(records, switches, ns, gaps, probe_ns)]
    # switch "response": IRQ_ENTER -> next SWITCH (wake-to-switch), same gap rule.
    sw_lat = wake_latencies(records, ns, gaps, probe_ns)

    return {
        "total_ns": total,
        "excluded_ns": excluded,
        "idle_ns": idle,
        "isr_during_idle_ns": isr_during_idle,
        "cpu_pct": cpu,
        "run_ns": run_ns,
        "slices": slices,
        "syscall_lat": stats(sys_lat),
        "syscall_overhead": stats([o for _, o in sys_over]),
        "syscall_by_nr": {nr: stats([o for n, o in sys_over if n == nr])
                          for nr in sorted(by_nr)},
        "irq_lat": stats(irq_lat),
        "switch_wake_lat": stats(sw_lat),
        "switch_count": len(switches),
    }


def _crossed_gap(i0, i1, gaps):
    # any gap index strictly between (i0, i1] poisons a pair opened at i0.
    for g in gaps:
        if i0 < g <= i1:
            return True
    return False


def _union_len(spans):
    """Total time covered by [start, end) spans, merging overlapping/nested
    intervals so a span nested inside another is counted once (not summed)."""
    if not spans:
        return 0.0
    total = 0.0
    cur_s, cur_e = None, None
    for s, e in sorted(spans):
        if cur_e is None:
            cur_s, cur_e = s, e
        elif s <= cur_e:
            if e > cur_e:
                cur_e = e
        else:
            total += cur_e - cur_s
            cur_s, cur_e = s, e
    total += cur_e - cur_s
    return total


def _corrected(d, probe_ns):
    # subtract one probe read's cost, clamped at 0.
    d -= probe_ns
    if d < 0.0:
        return 0.0
    return d


def pair_latencies(records, enter_t, exit_t, k0, k1, ns, gaps, probe_ns=0.0):
    """Match ENTER->EXIT by (k0,k1) key, drop pairs straddling a gap, subtract
    the probe overhead from each interval."""
    lat = []
    open_ = {}  # key -> (idx, ns)
    for idx, r in enumerate(records):
        if r.type == enter_t:
            key = (r.fields[k0], r.fields[k1])
            open_[key] = (idx, ns(r))
        elif r.type == exit_t:
            key = (r.fields[k0], r.fields[k1])
            if key in open_:
                i0, t0 = open_.pop(key)
                if not _crossed_gap(i0, idx, gaps):
                    d = ns(r) - t0
                    if d >= 0:
                        lat.append(_corrected(d, probe_ns))
    return lat


def irq_pairs(records, switches, ns, gaps, probe_ns=0.0):
    """Yield (line, dur_ns, start_ns, tid_at_start) for matched IRQ pairs
    (probe-overhead-corrected)."""
    out = []
    open_ = {}
    for idx, r in enumerate(records):
        if r.type == EV_IRQ_ENTER:
            open_[r.fields["line"]] = (idx, ns(r))
        elif r.type == EV_IRQ_EXIT:
            line = r.fields["line"]
            if line in open_:
                i0, t0 = open_.pop(line)
                if not _crossed_gap(i0, idx, gaps):
                    d = ns(r) - t0
                    if d >= 0:
                        out.append((line, _corrected(d, probe_ns), t0,
                                    _current_tid_at(switches, ns, t0)))
    return out


def _current_tid_at(switches, ns, when_ns):
    # switches is a list of (idx, record).
    tid = NO_THREAD
    for _idx, s in switches:
        if ns(s) <= when_ns:
            tid = s.fields["to"]
        else:
            break
    return tid


def _syscall_open_at(records, tid, upto):
    """True if `tid`'s most recent SYSCALL_ENTER before record index `upto` has no
    matching SYSCALL_EXIT since -- i.e. a syscall is mid-flight on that thread, so a
    switch here is cooperative/blocking rather than an ISR-caused wake."""
    depth = 0
    for r in records[:upto]:
        if r.fields.get("tid") != tid:
            continue
        if r.type == EV_SYSCALL_ENTER:
            depth += 1
        elif r.type == EV_SYSCALL_EXIT:
            if depth > 0:
                depth -= 1
    return depth > 0


def wake_latencies(records, ns, gaps, probe_ns=0.0):
    """IRQ_ENTER -> the SWITCH that ISR caused. KickOS defers the switch to ISR
    exit (PendSV tail-chains), so an ISR-caused switch appears as the ISR group's
    IRQ_EXIT *immediately* followed by SWITCH. Anchor on the SWITCH and require its
    immediate predecessor to be IRQ_EXIT (else the switch was cooperative / syscall-
    driven, not a wake); then walk back through the possibly-nested ISR group to its
    outermost IRQ_ENTER and measure from there.

    Anchoring on the switch -- rather than scanning forward from each IRQ_ENTER -- is
    what keeps a non-waking IRQ (an idle timer tick, or a console TX-drain IRQ) from
    being paired with a far-later switch: such an IRQ's EXIT is NOT immediately
    before a SWITCH, so it is simply never counted. (Forward-scanning produced
    sleep-beat/multi-second outliers because it skipped past intervening IRQs.)
    Gap-guarded, probe-overhead-corrected."""
    lat = []
    for j, r in enumerate(records):
        if r.type != EV_SWITCH or j == 0:
            continue
        if records[j - 1].type != EV_IRQ_EXIT:
            continue  # cooperative / syscall-driven switch, not ISR-caused
        # A non-waking IRQ (e.g. a bare SysTick) can tail-chain immediately ahead
        # of a switch the outgoing thread requested from inside a syscall (a
        # blocking sem_wait/sleep). That switch is cooperative, not ISR-caused: if
        # the `from` thread has a SYSCALL_ENTER still open (no matching EXIT) at
        # this point, exclude it.
        frm = records[j].fields["from"]
        if frm != NO_THREAD and _syscall_open_at(records, frm, j):
            continue
        # Walk back through the (nesting-aware) ISR group to its outermost ENTER.
        depth = 0
        i = j - 1
        found = -1
        while i >= 0:
            ti = records[i].type
            if ti == EV_IRQ_EXIT:
                depth += 1
            elif ti == EV_IRQ_ENTER:
                depth -= 1
                if depth == 0:
                    found = i
                    break
            else:
                break  # non-IRQ inside the group (dropped record): bail
            i -= 1
        if found < 0 or _crossed_gap(found, j, gaps):
            continue
        d = ns(records[j]) - ns(records[found])
        if d >= 0:
            lat.append(_corrected(d, probe_ns))
    return lat


# --- output modes ------------------------------------------------------------
def cmd_csv(records):
    for r in records:
        print(r.canon())


def _fmt_stat(s, unit):
    if s is None:
        return "  (none)"
    return ("  count=%d min=%.1f%s max=%.1f%s mean=%.1f%s p50=%.1f%s p95=%.1f%s"
            % (s["count"], s["min"], unit, s["max"], unit, s["mean"], unit,
               s["p50"], unit, s["p95"], unit))


def cmd_summary(records, model, lost, gaps, errors):
    sess = [r for r in records if r.type == EV_SESSION]
    a = analyze(records, model, gaps)
    unit = "ns"
    if model is None:
        unit = "tick"
    print("== KickOS telemetry summary ==")
    print("records decoded : %d" % len(records))
    if sess:
        s0 = sess[0].fields
        print("arch            : %s (id %d)  ver %d  ts_bits %d"
              % (ARCH_NAME.get(s0["arch"], "?"), s0["arch"], s0["ver"], s0["ts_bits"]))
        print("probe overhead  : %d ticks" % s0["probe_overhead"])
        # The cross-check reconciles decoded+lost against the CLOSING SESSION's final
        # records_attempted -- only meaningful with a clean shutdown (>=2 SESSIONs).
        # A running capture has just the opening SESSION (attempted=1, stale), so
        # skip it there rather than always report a false MISMATCH.
        if len(sess) >= 2:
            attempted = sess[-1].fields["records_attempted"]
            print("records_attempted (final SESSION): %d" % attempted)
            print("cross-check     : decoded=%d + lost=%d = %d  vs attempted=%d  -> %s"
                  % (len(records), lost, len(records) + lost, attempted,
                     "OK" if len(records) + lost == attempted else "MISMATCH"))
        else:
            print("cross-check     : (no closing SESSION -- needs a clean shutdown)")
    print("seq gaps        : %d (records lost: %d)" % (len(gaps), lost))
    if model is not None:
        print("clock rate      : %.4f ns/tick" % model[0])
    print("total span      : %.1f %s (excluded by gaps: %.1f %s)"
          % (a["total_ns"], unit, a["excluded_ns"], unit))
    if a["cpu_pct"] is not None:
        print("CPU utilization : %.2f%%  (idle %.1f %s, ISR-in-idle %.1f %s)"
              % (a["cpu_pct"], a["idle_ns"], unit, a["isr_during_idle_ns"], unit))
    print("switches        : %d" % a["switch_count"])
    print("per-thread run  :")
    for tid in sorted(a["run_ns"]):
        label = "idle" if tid == 0 else ("tid %d" % tid)
        print("  %-8s %.1f %s" % (label, a["run_ns"][tid], unit))
    # On-CPU overhead is the real syscall cost; the raw enter->exit SPAN includes
    # intended block time for blocking calls (sleep_ns/sem_wait) and is not a cost.
    print("syscall overhead (on-CPU) :")
    print(_fmt_stat(a["syscall_overhead"], unit))
    if a["syscall_by_nr"]:
        print("  by syscall (on-CPU overhead):")
        for nr in sorted(a["syscall_by_nr"]):
            s = a["syscall_by_nr"][nr]
            if s is None:
                continue
            name = SYSCALL_NAME.get(nr, "nr%d" % nr)
            print("    %-16s n=%-5d mean=%.1f%s p95=%.1f%s"
                  % (name, s["count"], s["mean"], unit, s["p95"], unit))
    print("syscall span (incl. block time) :")
    print(_fmt_stat(a["syscall_lat"], unit))
    print("IRQ latency     :")
    print(_fmt_stat(a["irq_lat"], unit))
    print("wake->switch    :")
    print(_fmt_stat(a["switch_wake_lat"], unit))
    if errors:
        print("parse notes     :")
        for e in errors:
            print("  " + e)


def cmd_chrome(records, model, gaps, out_path):
    a = analyze(records, model, gaps)

    def us(ns):
        return ns / 1000.0
    events = []
    for tid, start, end in a["slices"]:
        name = "idle" if tid == 0 else ("thread %d" % tid)
        events.append({"name": name, "ph": "X", "ts": us(start),
                       "dur": us(end - start), "pid": 1, "tid": tid})
    # syscall + irq as their own tracks
    open_ = {}
    for r in records:
        if r.type == EV_SYSCALL_ENTER:
            open_[("s", r.fields["tid"], r.fields["nr"])] = to_time(model, r)
        elif r.type == EV_SYSCALL_EXIT:
            k = ("s", r.fields["tid"], r.fields["nr"])
            if k in open_:
                t0 = open_.pop(k)
                events.append({"name": "syscall %d" % r.fields["nr"], "ph": "X",
                               "ts": us(t0), "dur": us(to_time(model, r) - t0),
                               "pid": 1, "tid": 1000 + r.fields["tid"]})
        elif r.type == EV_IRQ_ENTER:
            open_[("i", r.fields["line"])] = to_time(model, r)
        elif r.type == EV_IRQ_EXIT:
            k = ("i", r.fields["line"])
            if k in open_:
                t0 = open_.pop(k)
                events.append({"name": "irq %d" % r.fields["line"], "ph": "X",
                               "ts": us(t0), "dur": us(to_time(model, r) - t0),
                               "pid": 1, "tid": 2000})
    with open(out_path, "w") as f:
        json.dump({"traceEvents": events, "displayTimeUnit": "ms"}, f, indent=1)
    print("wrote Chrome trace: %s (%d events)" % (out_path, len(events)))


def to_time(model, r):
    if model is None:
        return float(r.abs_t)
    return to_ns(model, r.abs_t)


def cmd_assert(records, model, lost, gaps, errors):
    """Structural CI assertions. Return exit code (0 ok)."""
    problems = []
    if errors:
        problems += ["parse: " + e for e in errors]
    # 1. SWITCH chain: each from == previous to (first from == NO_THREAD).
    prev_to = None
    for r in records:
        if r.type == EV_SWITCH:
            frm, to = r.fields["from"], r.fields["to"]
            if prev_to is None:
                if frm != NO_THREAD:
                    problems.append("first SWITCH from=%d != 0xFFFF" % frm)
            elif frm != prev_to:
                problems.append("SWITCH chain broken: from=%d != prev.to=%d (seq %d)"
                                % (frm, prev_to, r.seq))
            prev_to = to
    # 2. no seq gaps.
    if lost != 0:
        problems.append("seq gaps present: %d records lost" % lost)
    # 3. records_attempted cross-check.
    sess = [r for r in records if r.type == EV_SESSION]
    if len(sess) < 2:
        problems.append("expected >= 2 SESSION records, got %d" % len(sess))
    else:
        attempted = sess[-1].fields["records_attempted"]
        if len(records) + lost != attempted:
            problems.append("cross-check: decoded+lost=%d != attempted=%d"
                            % (len(records) + lost, attempted))
    # 4. pairing: no ORPHAN EXIT (an EXIT with no prior matching ENTER == stream
    # corruption). Unmatched ENTERs are legitimate -- a no-return exit() syscall,
    # or a thread blocked mid-syscall when the system shut down -- so they are not
    # errors. Orphan exits are.
    orphan_sys = _orphan_exits(records, EV_SYSCALL_ENTER, EV_SYSCALL_EXIT, ("tid", "nr"))
    if orphan_sys != 0:
        problems.append("%d orphan SYSCALL_EXIT (no matching ENTER)" % orphan_sys)
    orphan_irq = _orphan_exits(records, EV_IRQ_ENTER, EV_IRQ_EXIT, ("line",))
    if orphan_irq != 0:
        problems.append("%d orphan IRQ_EXIT (no matching ENTER)" % orphan_irq)
    unmatched_irq = _unmatched(records, EV_IRQ_ENTER, EV_IRQ_EXIT, ("line",))
    if unmatched_irq != 0:
        problems.append("%d unmatched IRQ_ENTER" % unmatched_irq)

    if problems:
        print("STRUCTURAL FAIL:")
        for p in problems:
            print("  - " + p)
        return 1
    print("STRUCTURAL OK: %d records, %d switches, chain intact, no gaps, pairs balanced"
          % (len(records), sum(1 for r in records if r.type == EV_SWITCH)))
    return 0


def cmd_assert_atomic(records, data, lost, errors):
    """CI gate 2: the file is a clean sequence of WHOLE records (record-atomic),
    with contiguous sequence numbers (drops, if any, are a clean tail -- never a
    torn record or an internal corruption). Prints 'decoded=N' for the caller's
    drop-accounting cross-check."""
    problems = []
    if errors:
        problems += ["parse: " + e for e in errors]
    consumed = sum(REC_LEN[r.type] for r in records)
    if consumed != len(data):
        problems.append("torn tail: %d bytes not consumed by whole records"
                        % (len(data) - consumed))
    if lost != 0:
        problems.append("internal seq gap: %d (drops must be a clean tail)" % lost)
    if not records:
        problems.append("no records decoded")
    if problems:
        print("ATOMIC FAIL:")
        for p in problems:
            print("  - " + p)
        return 1
    print("ATOMIC OK: decoded=%d records, whole-record-clean, contiguous seq"
          % len(records))
    return 0


def _unmatched(records, enter_t, exit_t, keys):
    open_ = {}
    for r in records:
        if r.type == enter_t:
            k = tuple(r.fields[x] for x in keys)
            open_[k] = open_.get(k, 0) + 1
        elif r.type == exit_t:
            k = tuple(r.fields[x] for x in keys)
            if open_.get(k, 0) > 0:
                open_[k] -= 1
    return sum(open_.values())


def _orphan_exits(records, enter_t, exit_t, keys):
    # count EXIT records with no currently-open matching ENTER (stream corruption).
    open_ = {}
    orphans = 0
    for r in records:
        if r.type == enter_t:
            k = tuple(r.fields[x] for x in keys)
            open_[k] = open_.get(k, 0) + 1
        elif r.type == exit_t:
            k = tuple(r.fields[x] for x in keys)
            if open_.get(k, 0) > 0:
                open_[k] -= 1
            else:
                orphans += 1
    return orphans


_LOCK_RUN = 3  # consecutive valid, seq-contiguous records needed to lock alignment


def _lock_offset(buf):
    """Find the smallest offset where a run of _LOCK_RUN self-delimiting, seq-
    contiguous records begins. This aligns a live stream WITHOUT needing a SESSION
    record: SESSION is emitted only at boot/shutdown, so a capture that starts (or
    resyncs) mid-stream must lock onto the record grid itself -- valid type bytes +
    a u16 seq that increments by 1. Returns the offset, or None if no run is
    confirmable in the current buffer (wait for more bytes)."""
    n = len(buf)
    for off in range(n):
        i = off
        prev = None
        run = 0
        while run < _LOCK_RUN:
            if i >= n:
                break  # ran out before confirming -> cannot lock at this off yet
            rl = REC_LEN.get(buf[i])
            if rl is None or i + rl > n:
                break
            seq = buf[i + 1] | (buf[i + 2] << 8)
            if prev is not None and seq != ((prev + 1) & 0xFFFF):
                break  # seq gap within the lock window -> not a clean run here
            prev = seq
            i += rl
            run += 1
        if run >= _LOCK_RUN:
            return off
    return None


def follow(src):
    """Live per-record decode of a STREAMING ch1 capture (e.g.
    `JLinkRTTLogger -RTTChannel 1 <fifo> | kicktrace --follow <fifo>`). Records are
    self-delimiting, so each is decoded + printed (canonical form, same as --csv) as
    its bytes arrive; a partial record at the tail waits for more. Alignment is by
    seq-contiguity (see _lock_offset), so it locks onto a mid-stream capture and
    re-locks after any desync -- it does NOT depend on a SESSION marker. A seq GAP
    after locking is fine (dropped records under ring overflow leave the byte grid
    intact -- emit is whole-record-atomic); only an invalid type byte forces a
    re-lock. Time stays in raw ticks (the ns model needs both SESSION anchors, the
    last only at shutdown); aggregate views (--summary/--chrome/--assert) are
    batch-only. Ctrl-C or stream close ends it."""
    buf = bytearray()
    synced = False
    try:
        while True:
            chunk = src.read(256)
            if not chunk:
                break  # stream closed / EOF
            buf += chunk
            while buf:
                if not synced:
                    off = _lock_offset(buf)
                    if off is None:
                        # keep a bounded tail (a few records' worth) to retry once
                        # more bytes arrive; drop older un-lockable bytes.
                        if len(buf) > 256:
                            del buf[:len(buf) - 256]
                        break
                    if off:
                        del buf[:off]
                    synced = True
                rl = REC_LEN.get(buf[0])
                if rl is None:
                    synced = False  # desync -> re-lock via seq-scan (no SESSION needed)
                    continue
                if len(buf) < rl:
                    break  # partial record: wait for the next read
                recs, _ = parse(bytes(buf[:rl]))
                if not recs:
                    # a SESSION-typed slice with a bad magic => mis-locked; re-lock
                    synced = False
                    del buf[0]
                    continue
                print(recs[0].canon())
                sys.stdout.flush()  # line-live even when piped
                del buf[:rl]
    except (KeyboardInterrupt, BrokenPipeError):
        pass


def main():
    ap = argparse.ArgumentParser(description="KickOS telemetry decoder")
    ap.add_argument("file", help="binary trace (ch1 ring dump); '-' = stdin (with --follow)")
    ap.add_argument("--csv", action="store_true", help="canonical per-record lines")
    ap.add_argument("--follow", action="store_true",
                    help="live per-record decode of a streaming capture (stdin/fifo)")
    ap.add_argument("--summary", action="store_true", help="CPU%%/latency summary")
    ap.add_argument("--clock-hz", type=float, metavar="HZ",
                    help="trace-clock frequency (armv7m: the DWT/core clock, i.e. "
                         "SystemCoreClock) -- converts ticks to ns even with no "
                         "SESSION anchor in the capture; overrides the derived rate")
    ap.add_argument("--chrome", metavar="OUT.json", help="write Chrome-trace JSON")
    ap.add_argument("--assert-structural", action="store_true",
                    help="CI structural checks (exit nonzero on failure)")
    ap.add_argument("--assert-atomic", action="store_true",
                    help="CI record-atomicity checks under ring overflow")
    args = ap.parse_args()

    if args.follow:
        src = sys.stdin.buffer if args.file == "-" else open(args.file, "rb")
        follow(src)
        return

    with open(args.file, "rb") as f:
        data = f.read()
    records, errors = parse(data)
    unwrap(records)
    model = clock_model(records)
    if args.clock_hz:
        # Explicit trace-clock frequency: ns = tick * 1e9 / hz, from the clock
        # origin. Exact, and works with zero SESSION anchors (mid-stream capture).
        model = (1e9 / args.clock_hz, 0.0, 0.0)
    lost, gaps = loss_scan(records)

    if args.csv:
        cmd_csv(records)
    if args.chrome:
        cmd_chrome(records, model, gaps, args.chrome)
    if args.assert_structural:
        rc = cmd_assert(records, model, lost, gaps, errors)
        sys.exit(rc)
    if args.assert_atomic:
        rc = cmd_assert_atomic(records, data, lost, errors)
        sys.exit(rc)
    if args.summary or not (args.csv or args.chrome):
        cmd_summary(records, model, lost, gaps, errors)


if __name__ == "__main__":
    main()
