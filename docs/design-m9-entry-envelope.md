<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# M9 entry envelope: decision boundary

> **Status: MEASUREMENT RECORD.** The M8.12 tables, recomputation, source labels and
> term-by-term audit are in
> [`archive/M9_entry_envelope_recompute.md`](archive/M9_entry_envelope_recompute.md).
> M9.1's later two-core LX6 measurement is in
> [`design-m9-lock-bound.md`](design-m9-lock-bound.md).

The envelope asks four questions of the frozen M8.12 captures: the locked fraction,
contention wait, IRQ-to-user span, and IPC round-trip distribution. Each derived
quantity must name its workload, board, sample population and instrument correction.
A percentile bucket edge is a floor, and the short swept captures cannot support a
p99 claim. One lock-hold sample can enclose multiple waits, so subtracting medians
from those two columns does not yield a critical-section duration.

The M8.12 numbers justify measuring the lock's own bound and identifying missing
inputs. They do not, by themselves, price the M9.4/M9.5 partition stop condition:
its benefit needs the call/reply workload's locked fraction, and its overhead needs a
tail distribution for a single-flow IPC call. The four-core emulator collapse is a
workload observation, not a projected saving. M9.1 supplied the first shared-kernel
silicon lock-fraction evidence; its narrower result supersedes the old statement that
no such run existed.

This page approves no concurrency change. Consult the archived capture for any
numeric comparison and the later stage record for decisions actually landed.
