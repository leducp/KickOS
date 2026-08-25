<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# Design -- KickCAT as the driver-API reality check

> **Status: BACKEND WRITTEN AND COMPILED, NEVER LINKED, NEVER RUN.** The gap was measured against
> both trees at KickOS `3c3967e3` and KickCAT `43ad3e9`; the ruling in section 3 is made and the
> backend implements it. Section 8 records what writing it found, including one correction to
> section 3 and one retraction from section 1. Silicon is owed: `frdmk64f` was not on the bus.

KickCAT (`../KickCAT`) was delinked from KickOS in M4.1 and set aside through the whole driver era,
to be brought back at the end as the thing that JUDGES the driver APIs rather than as a consumer
that adapts to them. This is that judgement. `design-kickcat-k64f.md` holds the original K64F
hardware path and is not superseded by this page.

## 1. What the delink actually left

The delink was a **link-time break, not a source removal**, and that is why the port is smaller
than its age suggests. All of KickCAT's KickOS build glue is live and correct: the `KICKOS` backend
selection, the `OS/KickOS/types/os_types.h` branch taken BEFORE `__unix__` (deliberate, because the
sim host also defines `__unix__`), the bare-metal no-PIC arm, and the standalone slave app at
`../../KickCAT/examples/slave/lan9252/freedom-k64f/kickos/`, which is a proper out-of-tree consumer using
`find_package(KickOS)` and sharing the CTT-proven slave core verbatim with the NuttX example.

What evaporated is the far side of one ABI. `../../KickCAT/lib/slave/driver/src/kickos/SPI.cc` declares three
`extern "C"` symbols and the app calls a fourth: `spi_transfer`, `spi_enable_cs`, `spi_disable_cs`,
`spi_driver_start`. **None of the four exists anywhere in KickOS today.** The `user/apps/k64dspi`
that provided them is gone; what replaced it is `system/driver/mk64f/k64dspi/` (a service) plus
`user/apps/frdmk64f/k64dspi/` (a client).

Time survived the same delink intact: `OS/KickOS/Time.cc` binds `kos_clock_now` and `kos_sleep_ns`,
both of which still exist and still link. `AbstractESC` needs nothing from KickOS at all, because a
slave never sees a frame -- the ESC chip does the MAC, and the raw-socket seam is master-side only.

So the port is exactly one seam wide, and it is SPI.

## 2. The collision

`kickcat::AbstractSPI` exposes `enableChipSelect()` and `disableChipSelect()` as caller-visible
primitives, SEPARATE from `transfer()`. `Lan9252::readInternalRegister` is the whole question:

    enableChipSelect();
    write(&cmd, CSR_CMD_HEADER_SIZE);   // 3 bytes, instruction + 16-bit address
    read(payload, size);                // N bytes, chip select still asserted
    disableChipSelect();

Two transfers inside one chip-select bracket. KickOS refuses that shape at two independent levels,
and both refusals are deliberate rulings rather than gaps:

- `user/include/kickos/driver/spi.h` states that one call IS one complete transaction: the driver
  asserts its chip select, clocks all `nseg` segments, and releases it, with "ONE CHIP-SELECT
  BRACKET SPANS ALL SEGMENTS" and "THERE IS NO SHORT TRANSFER", because a half-clocked transaction
  cannot be resumed once its chip select is gone. The client never sees CS at all.
- `reference/ipc-call-reply.md` states there is NO cross-call state hold, and that a coherent
  multi-phase transaction is expressed as ONE call with multiple segments.

Under the current API those two `transfer()` calls become two `kos_call`s, each releasing CS at the
end, so the LAN9252 would see two unrelated transactions and the read would return garbage.

## 3. The ruling: NEITHER interface moves, and the backend absorbs it

**KickOS is right not to offer a cross-call chip-select hold, and `AbstractSPI` does not have to
change either.** The mechanism the port needs already exists on the KickOS side: `nseg` up to
`KOS_BUS_SEG_MAX` under one chip-select bracket is exactly the LAN9252 header-plus-payload shape.

The thing that makes the collision dissolve is that **`enableChipSelect()` and `disableChipSelect()`
are PURE VIRTUALS**. The bracket is already a backend concept, so a backend is entitled to decide
what it means, and the KickOS one means "accumulate":

- `enableChipSelect()` starts a segment list and touches no hardware.
- `transfer()` appends a segment.
- the list goes out as ONE `kos_spi_transfer`, and `disableChipSelect()` closes the list.

The single wrinkle is that `transfer()` must fill `data_read` before returning, so a read cannot be
deferred. It does not need to be: the backend FLUSHES AT THE READ, issuing everything accumulated
plus that segment, filling the buffer, and leaving `disableChipSelect()` a no-op. That is exactly
`Lan9252::readInternalRegister` -- write the three-byte command, read the payload, release --
which becomes `nseg == 2`. `writeInternalRegister` is `nseg == 1` and needs nothing.

**The shape the backend cannot express is a read followed by another transfer in the SAME bracket**,
because the flush has already released the chip select. It must REFUSE that loudly rather than
silently splitting the transaction; `THROW_ERROR` is what KickCAT uses. No such shape exists in the
LAN9252 path today.

### 3.1 Why not change `AbstractSPI` anyway

A segmented `transfer(segments)` primitive is defensible on its own terms: the split form encodes
an assumption that the CALLER owns the bus across calls, which suits a memory-mapped
single-address-space backend and composes badly with an OS-mediated one. NuttX has `SPI_LOCK` and
Linux `spidev` has the `SPI_IOC_MESSAGE` multi-transfer ioctl for that reason.

**It is still refused, for now, because nothing forces it.** Changing a public interface moves every
existing backend and consumer; the buffering backend is contained in the one file this port writes
anyway, and it enforces its limit with a throw rather than a silent wrong answer. If a real ESC
driver later needs read-then-write inside one bracket, THAT is the forcing consumer and that is when
the interface moves, on evidence rather than on a prediction.

**This entry previously said the opposite** -- that `AbstractSPI` was what had to move, and that
declining KickCAT's demand was the milestone's finding. That was reasoning from "KickOS cannot hold
a chip select across calls" straight to "the caller's interface is wrong", without noticing that the
bracket was already a customisation point. The correct output of the reality check is narrower and
better: the impedance mismatch is real, it is absorbable in a backend, and NEITHER public interface
needs to change to absorb it.

One consequence worth stating because it is a genuine semantic change: deferring means a `write()`
inside a bracket clocks no bytes until the flush. For a device with timing requirements BETWEEN
phases of one transaction that would matter. It does not for LAN9252 CSR access, which is one
indivisible transaction by construction, and the accumulated bytes must in any case fit
`KOS_SPI_XFER_MAX` and `KOS_BUS_SEG_MAX` -- 212 bytes and 8 segments, against a worst case here of
about 35 bytes and 2.

### 3.2 What the port therefore is

Three discrete pieces, plus one open question:

1. A new `kickcat::SPI` backend over `kos_spi_bus_open` / `kos_spi_device_open` /
   `kos_spi_transfer`, accumulating across the chip-select bracket per section 3 and throwing on
   the one shape it cannot express. `AbstractSPI` is untouched.
2. `Lan9252` is untouched too: its read path already brackets the command and the payload, which is
   what the backend turns into `nseg == 2`.
3. `KICKOS_MULTI_INSTANCE` is wired into the build (see section 5).

The buffer discipline differs mechanically and is not a design question: KickCAT passes distinct
`tx` and `rx` pointers and allows either to be null, where `kos_spi_transfer` is one in-place full
duplex buffer. That is a copy and a fill, not a negotiation.

## 4. Two things that are NOT constraints, stated so nobody widens them

- **`KOS_EP_MSG_MAX` is not binding.** `KOS_SPI_XFER_MAX` is 212 bytes after the request header and
  the segment table. Every LAN9252 transfer fits well inside it: the CSR header is 3 bytes, an
  internal-register write payload is at most 64 (67 on the wire), and both PRAM FIFOs are 32 bytes,
  so the largest transfer is around 35 bytes. The 1024-byte mailbox is NOT one transfer; `Lan9252`
  already chops it into FIFO-sized chunks. **Do not raise `KOS_EP_MSG_MAX` for KickCAT.**
- **`KOS_BUS_SEG_MAX` is not binding either.** It is 8; the maximum useful `nseg` here is 2.

## 4.1 The open question, which is latency and not shape

Fixing the transaction shape does not address cost, and the cost is real. `Lan9252::readData` is
three chip-select brackets for one 4-byte register read: write `ECAT_CSR_CMD`, then `waitCSR()`,
then read `ECAT_CSR_DATA`. Under KickOS each bracket becomes a `kos_call` round trip, and
`waitCSR` is a POLL LOOP, so it becomes one IPC round trip per iteration until the busy bit clears.

The measured cost of a round trip is in `design-m5-ipc-fastpath.md`: about 4640 cycles of fixed
cost, of which roughly 90 percent is generic syscall and switch machinery rather than anything
IPC-specific. A register poll across that boundary is therefore expensive in a way no segmenting
fixes.

KickCAT's slave states no cyclic deadline -- `freedom_slave.cc` is a bare `while (true)
{ slave.routine(); }` with no sleep and no period, and the only stated timeouts are a 10 ms CSR
busy-wait bound and a retry budget -- so nothing formally MISSES a deadline. What is at risk is
throughput, and it is not knowable from the code.

Three options, and the choice is not to be made before a measurement:

- a poll-until op on the bus wire, which puts a loop inside the driver;
- a batched multi-transaction op;
- moving the LAN9252 register-access layer BEHIND the endpoint as its own driver, which is what
  the `Descriptor` and `bring_up` machinery exists for.

The third is the architecturally honest one and it changes what "porting KickCAT" means, so it is a
scoping decision rather than an implementation detail.

## 5. Multi-instance: KickCAT is ready and KickOS is not

The requirement is tens of KickOS+KickCAT slaves in ONE host process against a KickCAT emulator.

**KickCAT is already instantiable N times.** A sweep for file-scope mutables, singletons and
thread-locals over `lib/` finds essentially nothing on the slave path: `Slave` holds every piece of
state as a member including the four ESM state objects, `Lan9252` holds its SPI by `shared_ptr`,
and `EmulatedESC::Memory` is a member. The only process-globals are a `since_start` epoch (shared
is arguably correct), a log-timestamp static in `EmulatedESC` (cosmetic), and a mock clock in the
TEST time backend that is not selected here. The one real gap is logging: `debug.h` writes to
stderr with no instance tag, so tens of instances interleave unattributably. That is usability,
not correctness.

**The emulator exists and is better than expected.** `EmulatedESC` implements `AbstractESC` on both
the PDI and ECAT sides with injectable clock drift in ppm and injectable jitter; `EmulatedNetwork`
is the bus, wiring ESCs as a daisy chain or an explicit topology, routing frames in real physical
port order with loopback at open ports, supporting redundancy and RUNTIME wire break and heal
injection. It is driven by `simulation/network_simulator.cc` from JSON configs which already
include a `freedom-k64f` one.

So the blocker is entirely on the KickOS side, and it is the one `design-m7-state-inventory.md`
describes. **That blocker has since been taken down for the sim**: `KICKOS_MULTI_INSTANCE` is a
real build knob (`Kconfig`, `CMakeLists.txt`), the selector is a thread-local
(`include/kickos/instance_local.h`), `arch/sim/sim.cc` carries the guarded per-instance state, and
`tests/integration/check_sim_multi_instance.sh` is a registered gate. Read
`design-m7-state-inventory.md` section 6 for what implementing it corrected, including the shared
`sigaltstack` this paragraph called the sharpest item, which was a live bug and is fixed.
What this section recorded as the state before that work: the seam was authored (`kernel()` and
`SimInstance`) but unreachable, no thread-local storage existed anywhere in the tree, and around
twenty file-scope mutable objects sat outside `Kernel` and `SimInstance`.

## 6. Recovered history, and a regression in KickCAT HEAD

`STATE.md` records two KickCAT fixes for the master relaunch: `8bc3d63` (retry `bus.init` to
survive a warm restart) and `cf7ec6f` (widen the bring-up link timeout for the SPI-PDI slave),
both 2026-07-18, together verified over 65 warm relaunches reaching and holding OP.

**Both were unreachable from every branch.** The `kickos-backend` branch was rewritten and its tip
squashed them away; `kickos-backend-backup` does not carry them either. They survived only in the
reflog, whose unreachable-object expiry defaults to 30 days, and they were exactly 30 days old.
They are now held by `backup/k64f-master-relaunch-20260817` in the KickCAT repo, local and
unpushed, in the same spirit as the KickOS backup branches `STATE.md` lists under *History that
must not be garbage-collected*.

**The behaviour is back in KickCAT HEAD**: all three `freedom_k64f_*_map_example.cc` variants call
a bare `bus.init(100ms)` with no retry wrapper, which is the pre-fix shape.

**OUT OF SCOPE for this milestone, and the lost fix was not the right one anyway (maintainer
ruling).** A bounded retry around `bus.init` on the MASTER is a workaround for a defect that lives
in the SLAVE: the standard requires a slave to handle a return to INIT properly, and one that
raises `INVALID_MAILBOX_CONFIGURATION` because it validated its mailbox SyncManager mid-reset is
not doing that. Widening the master's link timeout is the same shape of answer. So the two
recovered commits are worth keeping as EVIDENCE of the failure mode and of what was measured
(65 of 65 warm relaunches once masked), and they are NOT worth restoring as the fix.

The real work is in the slave's ESM, it belongs to KickCAT rather than to KickOS, and it is not
this milestone's. What this page owes it is only that the failure mode is written down so the next
person to hit it during a KickCAT bring-up recognises it instead of re-deriving it.

## 7. Size

**Small to medium, and smaller than section 3 first made it.** The build glue is intact and correct,
KickCAT is already multi-instance clean, `AbstractESC` needs nothing, time still links, KickOS
already has the segmented mechanism and two working SPI drivers, and now that the bracket is
absorbed in the backend neither public interface moves and `Lan9252` is untouched.

What is left is one new backend file, plus `KICKOS_MULTI_INSTANCE`, plus the latency question in
4.1 which is a measurement rather than a change. The four vanished `spi_*` symbols still have no
drop-in replacement, but the gap between them and `kos_spi_*` is now entirely inside the file the
port writes.

## 8. What the port found once it was written

The backend is `ea7ea4d` on KickCAT `master`, unpushed: `lib/slave/driver/{include,src}/kickcat/SPI.{h,cc}`
over `kos_spi_bus_open` / `kos_spi_device_open` / `kos_spi_transfer` / `kos_spi_bus_close`, with
`AbstractSPI`, `AbstractESC` and `Lan9252` untouched as section 3 ruled. It compiles clean for
cortex-m4 against the real KickOS headers at `-Wall -Wextra -Wshadow -Wundef -Werror`, and its only
undefined symbols are the four `kos_spi_*` names. **It has never linked and never run on silicon**:
`frdmk64f` was not on the bus. That is the state to carry into the bring-up, not a completed port.

Section 3 needs one correction and section 1 needs one retraction.

- **`disableChipSelect()` is not always a no-op.** Section 3 reads as if the flush always happens at
  the read. For a write-only bracket -- `writeInternalRegister`, `nseg == 1` -- there is no read to
  flush at, so the end of the bracket IS the flush point. The backend keys the three behaviours on
  three different facts (bracket open, bracket already flushed, segments pending), because one flag
  cannot distinguish them.
- **Section 1's "all of KickCAT's KickOS build glue is live and correct" was too generous.** Beyond
  the four vanished `spi_*` symbols, the example assigned a `kos::thread::Handle` to an `int`. It
  could not have compiled even with the SPI symbols present, so the glue was never being built.

Three findings are about KickOS rather than KickCAT, which is what the reality check was for.

1. **`spawn_caps` cannot take a custom stack** (`user/include/kickos/kos.h`, `spawn_caps`). It hard-passes
   `nullptr, 0` for both mmio and stack, so a consumer wanting delegated caps AND a custom stack has
   to abandon the helper and call `spawn` positionally through all eighteen arguments. The in-tree
   `k64dspi` app never hit this because it takes the default stack; KickCAT needs 16 KB, so it does.
   Not a defect -- the helper is honest about being a shortcut -- but it is a real gap, and the
   remedy is to give `spawn_caps` the two stack parameters rather than to document the workaround.
2. **A capability has no home in `AbstractSPI`.** `open()` carries a device *string*, and a
   capability is not a string, so the endpoint arrives through a non-virtual setter the consumer
   calls concretely. That works and it is the honest answer, but it is the one place the two
   interfaces genuinely do not meet: a KickOS SPI consumer is **not** substitutable through
   `AbstractSPI` alone, because it must know it is on KickOS to hand the cap over. Section 3 ruled
   that neither interface moves; this is the price of that ruling, and it is worth paying.
3. Section 4's non-constraints are confirmed against the built code, not just read off the headers:
   worst case ~35 bytes over 2 segments against 212 and 8.

Section 4.1's latency question is untouched by any of this. `waitCSR()` is still one IPC round trip
per poll iteration, and the baseline in `design-m5-ipc-fastpath.md` is what prices it.
