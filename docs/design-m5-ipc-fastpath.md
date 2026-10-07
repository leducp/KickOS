<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# M5 IPC fastpath: design constraints

> **Status: MEASURED, mechanism open.** No new IPC fastpath is specified or landed here.
> The baseline, phase measurements, corrected instrument and candidate analysis are in
> archived `M5_ipc_fastpath_study.md`. Its section
> numbers are retained for the measurements cited by other milestones.

The measured call/reply cost is mostly fixed path length at small driver payloads.
Copying eight or twelve bytes is a small share. A useful fastpath must avoid generic
syscall and scheduler work; replacing only the byte copy is insufficient. The
measurement correction matters: leaves are adjusted by `PH_NULL`, nested composites
by `PH_NEST`, and a composite shared between different paths cannot report an honest
minimum for either. Use the archived corrected values, not its early composites.

## Constraints on a candidate

- `kos_call` remains the public API. Its stub can select a register-payload trap when
  both request and reply fit; larger payloads keep the buffer path. The consumer does
  not choose a second call API.
- The handler decides eligibility before mutating any IPC or capability state, so a
  refusal falls through to the existing slowpath. Correctness remains in that path.
- A fast rendezvous needs a receiver already parked. A higher-priority caller can
  arrive before the server parks; scheduling and priority donation, rather than fewer
  copy instructions, determine that case's latency.
- A handler-level switch needs an arch-specific frame handoff. Capability checks may
  be a shared C leaf, but exception-frame surgery cannot be one portable C body.
- Donation under bursty traffic, reply-cap storage and teardown remain design
  questions. The saturated equal-priority benchmark cannot settle them.

No cycle figure here is an architecture-independent promise. The archived captures
name their board, build posture, payload and instrument correction.
