<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# Design -- KickCAT as the driver-API reality check

> **Status: SURVEYED, not ported.** The gap is measured against both trees at KickOS `3c3967e3`
> and KickCAT `43ad3e9`. The ruling in section 3 is made; the work is not started.

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

## 3. The ruling: KickCAT's seam is what moves

**KickOS is right here and should not be changed to accommodate the split form.** The mechanism the
port needs already exists: `nseg` up to `KOS_BUS_SEG_MAX` under one chip-select bracket is exactly
the LAN9252 header-plus-payload shape. `writeInternalRegister` maps to `nseg == 1` unchanged; the
read path maps to `nseg == 2`, header then payload.

`AbstractSPI`'s split form encodes an assumption that the CALLER owns the bus across calls. That is
natural for exactly one kind of backend, a memory-mapped single-address-space driver such as the
RPi bcm2835 one, and it does not survive contact with any OS-mediated SPI stack. Both of the other
backends KickCAT would want are already better served by the segmented form: NuttX has `SPI_LOCK`
and Linux `spidev` has the `SPI_IOC_MESSAGE` multi-transfer ioctl precisely because the split form
does not compose. So the change is not a concession to KickOS; it is a fix KickCAT wants anyway.

The shape: add a segmented `transfer(segments)` primitive to `AbstractSPI` and keep the existing
two-call form as a default-implemented convenience over it, so no existing backend breaks.

**This is the milestone's intended output.** The reality check was set up so that a KickCAT demand
would drive an API change; what it produced instead is the inverse, and declining the demand is the
finding. Recording it that way matters, because "the consumer asked and we said no" is only a
defensible answer with the reason written down.

### 3.1 What the port therefore is

Three discrete pieces, plus one open question:

1. `AbstractSPI` grows the segmented primitive; a new `kickcat::SPI` backend implements it over
   `kos_spi_bus_open` / `kos_spi_device_open` / `kos_spi_transfer`.
2. `Lan9252`'s read path is rewritten from bracket-plus-two-calls to one segmented call.
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

So the blocker is entirely on the KickOS side, and it is the one `design-m6-state-inventory.md`
describes: the seam is authored (`kernel()` and `SimInstance`) but unreachable, the two pointers it
selects do not exist, there is no thread-local storage of any kind in the tree, and around twenty
file-scope mutable objects sit outside `Kernel` and `SimInstance`. The sim's shared `sigaltstack`
is the sharpest of them, because `sigaltstack` is a per-thread POSIX property.

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

**Medium.** Smaller than its age suggests: the build glue is intact and correct, KickCAT is already
multi-instance clean, `AbstractESC` needs nothing, time still links, and KickOS already has the
segmented mechanism plus two working SPI drivers. Larger than a symbol rename: the four `spi_*`
symbols have no drop-in replacement precisely because the seam that named them encodes an
assumption the current API refuses on purpose.
