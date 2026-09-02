#!/usr/bin/env python3
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Boot an image built with -DKICKOS_SMP_TRACE=ON, wait for it to stall, and answer the ONE
# question a stall cannot answer for itself: of the three ways a parked thread never runs
# again, which one happened.
#
#   no waker ran           a PARK on a queue with NO later SEARCH of that queue
#   a waker found nothing  a SEARCH of it that came back empty, with the queue's head beside it
#                          so an empty queue is told apart from a search of the wrong one
#   the switch never took  a SEARCH that found the thread, a READY for it, and no RUN
#
# The rings are read out of guest memory after the run, so nothing here runs on the recording
# path. Records are ordered by the timebase every hart reads, never by a shared counter.
#
# usage: smptrace_decode.py <elf> <nm> <cores> [seconds]

import os, re, socket, subprocess, struct, sys, time

# Held to the header by static_assert in kickos/smptrace.h: a field added there moves every
# offset read here, and the build is what refuses first.
REC = struct.Struct("<IHHIIQ")          # seq, kind, core, a, b, ts
DEPTH = 512
RING = 12352
KIND = {1: "PARK", 2: "SEARCH", 3: "EMPTY", 4: "READY", 5: "REFUSED", 6: "ASK", 7: "RUN"}
VA_BASE = 0xFFFFFFFF00000000

def sym(nm, elf, name):
    out = subprocess.run([nm, "--defined-only", elf], capture_output=True, text=True).stdout
    for line in out.splitlines():
        f = line.split()
        if len(f) == 3 and f[2].endswith(name):
            return int(f[0], 16)
    sys.exit("no symbol matching %s in %s" % (name, elf))

def main():
    elf, nm, cores = sys.argv[1], sys.argv[2], int(sys.argv[3])
    wait = float(sys.argv[4]) if len(sys.argv) > 4 else 25.0
    va = sym(nm, elf, "g_kos_traceE")
    pa = va - VA_BASE
    sock, dump = "/tmp/kos_smptrace.sock", "/tmp/kos_smptrace.bin"
    for f in (sock, dump):
        if os.path.exists(f):
            os.remove(f)
    con = "/tmp/kos_smptrace.console"
    p = subprocess.Popen(
        ["qemu-system-riscv64", "-M", "virt", "-smp", str(cores), "-m", "128M",
         "-bios", "none", "-kernel", elf, "-nographic",
         # The finisher write ENDS the machine, which would take the monitor with it before the
         # rings could be read. Stopped rather than exited, memory stays readable either way,
         # so a run that completes and a run that stalls are dumped by the same path.
         "-no-shutdown",
         "-serial", "file:" + con, "-monitor", "unix:%s,server,nowait" % sock])
    for _ in range(100):
        if os.path.exists(sock):
            break
        time.sleep(0.1)
    done = 0.0
    while done < wait:
        time.sleep(0.5); done += 0.5
        try:
            txt = open(con, errors="replace").read()
        except Exception:
            continue
        if "all tests passed" in txt or "test(s) failed" in txt:
            break
    s = socket.socket(socket.AF_UNIX); s.connect(sock); time.sleep(0.3)
    try:
        s.recv(1 << 20)
    except Exception:
        pass
    s.sendall(b"pmemsave 0x%x 0x%x \"%s\"\n" % (pa, RING * cores, dump.encode()))
    time.sleep(2.0)
    p.kill()

    blob = open(dump, "rb").read()
    recs, wrapped = [], set()
    for c in range(cores):
        base = c * RING
        nxt = struct.unpack_from("<I", blob, base)[0]
        held = min(nxt, DEPTH)
        for k in range(held):
            i = (nxt - held + k) % DEPTH
            seq, kind, core, a, b, ts = REC.unpack_from(blob, base + 8 + i * REC.size)
            recs.append((ts, seq, kind, core, a, b))
        if nxt > DEPTH:
            wrapped.add(c)
            print("core %d WRAPPED: %d records, ring holds %d, the start is gone"
                  % (c, nxt, DEPTH))
    recs.sort()
    if not recs:
        sys.exit("every ring is empty: the instrument recorded nothing, so this run says "
                 "nothing about the stall")

    console = open(con, errors="replace").read() if os.path.exists(con) else ""
    last_ok = [l for l in console.splitlines() if l.startswith("ok ")]
    print("== %d record(s) over %d core(s); console last arm: %s"
          % (len(recs), cores, last_ok[-1] if last_ok else "<none>"))

    # A thread is STALLED when it parked and never ran again after that park.
    parked, ran, readied, refused = {}, {}, {}, {}
    for ts, seq, kind, core, a, b in recs:
        k = KIND.get(kind, "?")
        if k == "PARK":
            parked[a] = (ts, b, core)
        elif k == "RUN":
            ran[a] = ts
        elif k == "READY":
            readied[a] = ts
        elif k == "REFUSED":
            refused.setdefault(a, []).append((ts, b))

    stalled = [(t, v) for t, v in parked.items() if ran.get(t, -1) < v[0]]
    if stalled and wrapped:
        print("NOTE: core(s) %s wrapped their rings, so every ABSENCE below is a maybe: the "
              "record may have been overwritten. Presence readings stay sound."
              % sorted(wrapped))
    if not stalled:
        print("no thread parked and stayed parked: this run did not stall")
        return
    for t, (ts, q, core) in sorted(stalled, key=lambda x: x[1][0]):
        print("\n-- thread 0x%08x parked on queue 0x%08x from core %d" % (t, q, core))
        q_searched = [r for r in recs if KIND.get(r[2]) == "SEARCH" and r[4] == q and r[0] > ts]
        if not q_searched:
            if wrapped:
                print("   INCONCLUSIVE: no search of that queue is in the rings, but core(s) %s "
                      "WRAPPED, so the search may have been overwritten rather than absent. An "
                      "absence verdict needs a run where no ring wrapped; raise KOS_TRACE_DEPTH "
                      "or narrow the workload." % sorted(wrapped))
                continue
            print("   VERDICT: NO WAKER RAN. Nothing searched that queue after the park, so "
                  "the question is who owed the wake, not why it failed.")
            continue
        hit = [r for r in q_searched if r[5] == t]
        if not hit and wrapped:
            print("   INCONCLUSIVE: no search found this thread, but core(s) %s WRAPPED, so a "
                  "search that did may have been overwritten." % sorted(wrapped))
            continue
        if not hit:
            empt = [r for r in recs if KIND.get(r[2]) == "EMPTY" and r[4] == q and r[0] > ts]
            head = ("queue head 0x%08x" % empt[0][5]) if empt and empt[0][5] else "queue empty"
            print("   VERDICT: A WAKER SEARCHED AND DID NOT FIND IT. %d search(es), %s. A "
                  "non-zero head with the thread absent means it is not on the queue the "
                  "waker read." % (len(q_searched), head))
            continue
        if t in readied and readied[t] > ts:
            if wrapped:
                print("   INCONCLUSIVE: readied with no RUN after it, but core(s) %s WRAPPED, "
                      "so a RUN may have been overwritten." % sorted(wrapped))
                continue
            print("   VERDICT: FOUND AND READIED, THE SWITCH NEVER TOOK. Readied at %d and "
                  "no RUN after it." % readied[t])
            asks = [r for r in recs if KIND.get(r[2]) == "ASK" and r[0] >= readied[t]][:1]
            if asks:
                print("   the ask that followed named core mask 0x%x at priority %d"
                      % (asks[0][4], asks[0][5]))
            else:
                print("   and NO ask followed it at all")
            continue
        if t in refused:
            print("   VERDICT: THE WAKE WAS REFUSED, state %d. Not a stall of this kind."
                  % refused[t][-1][1])
            continue
        print("   VERDICT: found by a search and never readied, which is neither of the three "
              "and wants the raw records below.")
        for r in recs[-20:]:
            print("     ts=%d core=%d %-7s a=0x%08x b=0x%08x" % (r[0], r[3], KIND.get(r[2], "?"), r[4], r[5]))

main()
