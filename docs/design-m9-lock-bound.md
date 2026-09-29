<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# M9.1 kernel-lock bound

> **Status: LANDED.** The derivation, per-backend evidence and capture interpretation are in
> [the M9.1 record](archive/M9.1_lock_bound_record.md). M9.5 changed x86_64 arbitration to
> CLH while retaining one big kernel lock; see [the M9.5 decision](design-m9.5-bkl-options.md).

The ticket version assigns one ticket per core per acquisition. It tests whether its turn has
arrived before polling the doorbell; a waiting core must continue servicing doorbell requests.
The interrupt mask stays in force across lock acquisition and its wait, preventing a nested
handler from drawing a second ticket behind the suspended first one.

For `N` contenders, the wait model is `W = D + (N - 1) * (C + H + S)`: ticket draw `D`, longest
critical section `C`, handoff visibility `H`, and one possible doorbell service `S` at each
handoff. On rv64imac an AMO draw has no retry instruction outcome, though fabric arbitration
remains. On armv8a the exclusive draw has an architecturally unbounded retry term, measured
under the recorded workload. On LX6 the conditional-store draw has no derivable progress bound;
the ticket removes overtaking but does not turn that term into a bound.

Measured maxima in the record are samples, not worst-case bounds. It also records why
multi-core lock traffic and the measured locked fraction cannot be compared directly with
single-core captures.
