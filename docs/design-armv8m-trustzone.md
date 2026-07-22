<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->

# ARMv8-M TrustZone as the armv8-M kernel-confinement backend

Status: DESIGN SPIKE. Not implemented. Forward-looking. IMPLEMENT POST-M6 (after
the M4 driver era and M5 SMP settle, since the secure-services angle needs the
service model to exist first). Requires a fable design review before any merge.
Per-chip capability: only armv8-M cores that implement the Security Extension
(M23/M33/M55/M85 MAY) can use this backend; detect and fall back everywhere else.

This spike answers one question: what is the RIGHT role for ARMv8-M TrustZone in
KickOS's memory-protection story? The short answer -- corrected below from a first,
wrong framing -- is that TrustZone is not a per-task isolation mechanism and does
NOT replace the MPU; it is the armv8-M-with-Security-Extension MECHANISM for
kernel confinement (kernel in Secure state, apps in Non-secure), the strongest
realization of the already-parked "confine the kernel / drop PRIVDEFENA" goal on
the chips that have it. Companion to `docs/design-rp2350-mpu-armv8m.md` (the
PMSAv8 per-task MPU backend, which TrustZone layers on top of, not instead of) and
the post-M6 "Option B" item in `TODO.md`.

## What TrustZone-M is

The ARMv8-M Security Extension adds a Secure / Non-secure state split that is
ORTHOGONAL to the existing privileged / unprivileged split -- a 2x2, not a new
point on the privilege axis. A thread runs in one of four combinations
(Secure-privileged, Secure-unprivileged, Non-secure-privileged,
Non-secure-unprivileged). The machinery:

- **SAU / IDAU** partition the physical address space into Secure (S),
  Non-secure (NS), and Non-secure-callable (NSC) regions. The SAU is the
  software-programmable half; the IDAU is the fixed, implementation-defined half
  (on many parts an address-bit map). Their union decides the security attribute
  of every access.
- **Two banked MPUs -- MPU_S and MPU_NS.** Each security state has its own full
  MPU register file. The MPU is NOT shared across states; it is duplicated.
- **Banked stack pointers per state** (MSP_S/PSP_S and MSP_NS/PSP_NS), banked
  SysTick, banked fault handling, banked control registers.
- **The secure gateway** -- SG instructions sit in NSC regions; NS code calls
  Secure code only by branching to an SG (via veneers marked
  `cmse_nonsecure_entry`), and Secure code returns to / calls out to NS via BXNS /
  BLXNS. This is the only legal S<->NS control-flow transfer.
- **NVIC ITNS** routes each interrupt line to a target security state, so an IRQ
  is delivered to S or NS handling independently per line.

## The key limit (why the first framing was wrong)

TrustZone is ONE strong boundary, not a per-task isolation mechanism. Putting
apps in Non-secure state does NOT make the apps isolated from EACH OTHER -- NS
tasks are still separated only by MPU_NS, which must be reprogrammed on every
NS-task switch, at exactly the same per-switch cost KickOS pays today with the
single MPU. TrustZone therefore:

- does NOT reduce the per-switch MPU reprogram cost (the skip-if-unchanged
  optimization tracked in `TODO.md` is the lever for that, orthogonally);
- does NOT replace the MPU -- MPU_NS still does all per-task work; MPU_S protects
  the kernel's own internal partitioning if the kernel wants it;
- adds a SECOND MPU to program, not zero.

So TrustZone buys a boundary, not cheaper switching. It is a security / assurance
play, not a performance one.

## The reframed thesis: uniform GOAL, per-arch MECHANISM

The correction that makes TrustZone fit cleanly: kernel confinement is ALREADY
arch-dependent in KickOS. The confinement mechanism is per-arch and always has
been --

- PMSAv7 background-region drop on armv7m/armv6m (XMC / F411 / RP2040 / microbit),
- PMP locked entries on RISC-V (C6),
- RX-MPU supervisor region set on RXv3 (RX72M),
- SYSMPU RGD0 restriction on K64F.

Uniformity in KickOS does not live in the register mechanism. It lives in the
GOAL: a confined kernel / TCB, per-task isolation, and capability authority. Each
arch reaches that goal through its own hardware. This is the same discipline the
MPU backend already follows (`{base,size,attr}` is the uniform seam; the register
encoding is per-arch).

Read that way, TrustZone is simply THE armv8-M-with-Security-Extension mechanism
for kernel confinement: the kernel and TCB live in Secure state, applications live
in Non-secure state. It is NOT a uniformity violation -- it is the natural per-arch
backend for the confinement goal, exactly as PMP locked entries are RISC-V's and
RGD0 restriction is K64F's. TrustZone is one member of that family, distinguished
only by being the STRONGEST realization of the goal on hardware that offers it.

Chips WITHOUT the Security Extension (all v7-M, armv8-M-baseline parts without TZ)
reach the SAME goal through the parked "Option B": explicit kernel MPU map + drop
PRIVDEFENA (see the post-M6 fleet-wide item in `TODO.md`). So the fleet plan is:

- **uniform goal** -- confine the kernel, isolate tasks, gate by capability;
- **per-chip mechanism** -- Option B (MPU regions + drop-PRIVDEFENA) everywhere;
  TrustZone additionally, where the silicon implements the Security Extension.

Do Option B first, fleet-wide (it is the portable floor). TrustZone then layers on
as the armv8-M mechanism on the parts that have it -- it does not replace Option B
there, it strengthens the boundary from a privilege-based one (kernel is
privileged, background dropped) to a state-based hardware one.

## Where TrustZone actually helps

Two concrete gains beyond what Option B alone gives on an armv8-M part:

1. **A hardware TCB boundary.** With the kernel in Secure state, NS code cannot
   touch Secure kernel memory EVEN WHEN NS code is privileged. Option B relies on
   the kernel being privileged and the background dropped -- a privilege-based
   boundary that a kernel bug running privileged can still cross. The S/NS split is
   a separate hardware axis: a fault in privileged NS code faults on Secure memory
   regardless of its privilege. That is a strictly stronger TCB boundary than
   MPU + dropped-PRIVDEFENA background.

2. **A PSA-style secure-services partition for roots-of-trust.** Keys, secure
   boot, attestation, and similar roots-of-trust fit naturally in the Secure
   partition, reached from NS only through capability-gated secure-gateway entry
   points. This FITS the KickOS capability-gated-services model rather than
   fighting it: an NS client holds a capability that authorizes an SG call into a
   Secure service, mirroring how a client holds a badged endpoint to reach a
   userspace driver. The secure services become the highest-assurance tier of the
   same service model -- which is why this work waits until the M4 service model
   exists.

## Cost and complexity

This is a lot of machinery for the assurance it buys:

- SAU (and IDAU-awareness) configuration of the S / NS / NSC partition;
- secure-gateway veneers plus the S<->NS call ABI (`cmse_nonsecure_entry`, the
  argument/return conventions across the boundary, non-secure function pointers);
- banked stack pointers -- the switch path and fault path must be TrustZone-aware;
- S / NS interrupt targeting via NVIC ITNS (every line's target state decided);
- a SEPARATE Secure build and link (the Secure image and the NS image are distinct
  link units with a shared NSC veneer table), i.e. a second linker script and a
  two-image boot.

None of this is on the performance path; it is boundary and assurance work. It
also interacts with SMP (M5): the MPUs are banked and per-core, and so is the SAU
-- a TrustZone SMP story must set up S/NS per core, which is one more reason to
sequence it after M5.

## Recommendation

- **Spike now (this document); IMPLEMENT POST-M6.** Sequence it after M4 drivers
  and M5 SMP settle: the secure-services angle needs the service model, and the
  banked-per-core machinery wants the SMP model proven first.
- **Do Option B first, fleet-wide.** The uniform kernel-confinement floor
  (MPU regions + drop-PRIVDEFENA) is the portable goal realization; TrustZone is
  the armv8-M mechanism that layers on top of it, not a substitute.
- **RP2350's Cortex-M33 has the Security Extension -- a concrete first target.**
  It is already the PMSAv8 and SMP target, so the TrustZone work would build on a
  board whose per-task MPU and dual-core story are already in flight.
- **Per-chip capability: detect and fall back.** M23 / M33 / M55 / M85 MAY
  implement the extension; a part without it uses Option B alone. The confinement
  goal is uniform; whether TrustZone backs it is a per-silicon fact discovered at
  bring-up, exactly like whether a chip has a CPU-side MPU at all.

The one-line summary: uniform goal, per-chip mechanism -- TrustZone is one such
mechanism, the strongest armv8-M realization of kernel confinement, layered on the
portable Option-B floor rather than replacing the MPU or the confinement goal.
