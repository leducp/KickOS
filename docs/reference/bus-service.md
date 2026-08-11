<!-- SPDX-License-Identifier: CECILL-C -->
# The SPI / I2C bus-service contract

The exact wire contract a client and an unprivileged bus driver exchange 1:1 over a
`kos_call` endpoint (`ipc-call-reply.md` is the transport). Code source of truth:
`user/include/kickos/sys/bus.h` (the wire ABI), `user/include/kickos/driver/spi.h` (the SPI
CLASS this protocol serialises) + `user/lib/spi_proxy/spi_proxy.cc` (the proxy backend of that
class), `user/include/kickos/sys/spi_service.h` (the transport),
`system/driver/mk64f/k64dspi/k64dspi.cc` + `system/driver/xmc4800/xmcssc/xmcssc.cc` (the two
reference SPI services) and their engines `spi_dspi.cc` / `spi_usic.cc`. If a page and the code
disagree, the page is the bug.

## Layering

A bus transaction is one `kos_call`: the client frames a request in a stack buffer, calls,
and reads the reply back from the SAME buffer (call is in-place). The kernel copies the
bounded payload between the two domains; the driver runs the transaction under its own
MMIO grant and `kos_reply`s the result. There are TWO status channels, and they are
distinct:

- **`kos_call`'s return** -- the kernel-level outcome (delivery, `EPIPE`, `ENOMEM`, ...),
  per `ipc-call-reply.md`.
- **`kos_bus_rsp.status`** -- the SERVICE-level outcome (the driver's own result), only
  meaningful once `kos_call` returned `>= 0`.

## Wire structs

Fixed-width, naturally aligned, no compiler pragmas; native endianness (the payload is
kernel-copied within one machine and never crosses a link). Sizes are locked by
`static_assert` in the header.

    struct kos_bus_req            // 12 bytes -- every request starts here
    {
        uint8_t  proto;           // enum kos_bus_proto: KOS_BUS_SPI=0 / KOS_BUS_I2C=1
        uint8_t  op;              // enum kos_bus_op:    KOS_BUS_OP_XFER=0 / KOS_BUS_OP_CONFIG=1
        uint8_t  device;          // device slot, < KOS_BUS_DEV_MAX (4) (CS index / I2C addr slot)
        uint8_t  nseg;            // 1 .. KOS_BUS_SEG_MAX (8)
        int32_t  region_cap;      // -1 = inline payload follows; else granted region (DEFERRED)
        uint32_t offset;          // byte offset into the region (region path); 0 inline
    };

    struct kos_bus_seg            // 4 bytes -- nseg of these follow the header
    {
        uint16_t len;             // bytes clocked in this segment
        uint8_t  flags;           // enum kos_bus_seg_flags (I2C phase controls)
        uint8_t  rsv;
    };

    struct kos_bus_rsp            // 4 bytes -- reply header; rx bytes follow
    {
        int16_t  status;          // 0, or a negative KOS_E*-taxonomy service error
        uint16_t len;             // rx bytes following
    };

    struct kos_bus_cfg            // 12 bytes -- op == KOS_BUS_OP_CONFIG payload
    {
        uint32_t hz;              // target clock; driver rounds DOWN, replies achieved hz
        uint16_t addr;            // I2C 7/10-bit address (width in mode); SPI 0
        uint8_t  mode;            // enum kos_bus_mode
        uint8_t  word_bits;       // SPI frame size (8 default)
        uint8_t  cs_policy;       // enum kos_bus_cs_policy
        uint8_t  cs_index;        // HW PCS/SELO line index, or the driver's GPIO pin slot
        uint8_t  rsv[2];
    };

Enums (`bus.h`): `KOS_BUS_SEG_MAX = 8`; `KOS_BUS_DEV_MAX = 4`;
`KOS_BUS_SEG_RD = 1<<0` / `KOS_BUS_SEG_STOP = 1<<1`;
`KOS_BUS_CS_NONE=0` / `CS_HW=1` / `CS_GPIO=2`; mode bits `CPOL=1<<0` / `CPHA=1<<1` /
`LSB_FIRST=1<<2` (SPI) and `ADDR_10BIT=1<<0` (I2C -- overlaps CPOL; distinct protos).
`proto` is a sanity tag: a driver rejects a mismatch with `status = -KOS_EINVAL`.

## Framing and the inline budget

A request is `kos_bus_req` (12 B) + `nseg * kos_bus_seg` (4 B each) + the inline payload
bytes, all in one message capped by `KOS_EP_MSG_MAX` (256). Worst-case framing is
12 + 8*4 = 44 B, leaving ~212 B of inline data. A typical 2-segment SPI command carries
~236 B. This covers every first consumer (LAN9252 CSR ops are <= 7 B; sensor/EEPROM
traffic is tens of bytes).

The inline path is selected by **`region_cap == -1`**. The region path (a granted shared
buffer named by `region_cap` + `offset`, lifting the byte bound) is DEFERRED: the fields
exist in the struct so it lands later without an ABI break, and a driver returns
`status = -KOS_ENOSYS` for any `region_cap != -1` today.

A client must size `recv_cap` (its `kos_call` reply capacity) at least
`sizeof(kos_bus_rsp) + expected_rx`.

## The segment model

`nseg` segments follow the header, then the inline payload:

- **SPI** clocks full duplex, so every segment shifts `len` bytes in BOTH directions and
  CS spans the whole message (no per-segment SPI flag is defined). The inline request
  bytes are the tx bytes for every segment concatenated; the reply rx bytes are the
  full-duplex data for all segments concatenated (same total length).
- **I2C** treats each segment as one addressed phase: START + the `device` slot's
  configured address + direction (`KOS_BUS_SEG_RD` set = read, absent = write) + `len`
  bytes. `KOS_BUS_SEG_STOP` closes the transaction after that segment; its absence means a
  repeated START into the next segment. Only write segments carry inline request bytes;
  read segments carry none. The reply rx bytes are the read-segment data concatenated.

## SPI

Full-duplex `transfer(tx, rx, len)` is `nseg = 1`. The coherent command+payload shape a
length-sensitive target needs (e.g. LAN9252) is `nseg = 2` under ONE CS bracket: a write
phase then a read phase. `word_bits > 8` frames require each `seg.len` to be a whole-word
multiple (native word layout).

## I2C (contract only -- no driver body ships yet)

The canonical register access is a write-then-read: `nseg = 2`, segment 0
`{ len = reg-addr bytes, flags = 0 }` (write), segment 1
`{ len = n, flags = KOS_BUS_SEG_RD | KOS_BUS_SEG_STOP }` (read-then-STOP), with a repeated
START between them (STOP absent on segment 0). A NACK aborts the transaction and surfaces
as a negative `kos_bus_rsp.status` with `len` = bytes actually transferred before the NACK
(partial). No CS concept exists on I2C. Controllers that realise this contract are the
same many-modes blocks the SPI services use (XMC USIC IIC mode, RX SCI simple-I2C);
mode-select resolves inside the class, not the wire.

> **Taxonomy gap (flagged).** The bus design intends an EIO-class code for a NACK, but the
> current `system/include/kickos/sys/errno.h` taxonomy has no I/O-error code at all. Because
> no I2C driver body exists yet, nothing defines one. The code that lands the first I2C service
> must add an EIO-class code to the taxonomy (or map NACK onto an existing code) and this
> page must be updated to name it exactly. Until then "a negative service status" is the
> honest contract.

## Chip-select policy (SPI, driver-internal)

CS is chosen per DEVICE in `kos_bus_cfg.cs_policy`; the client NEVER sees CS. The service
picks a mechanism it can realise on its controller:

- **`CS_HW`** -- the controller's own chip-select line spanning the message. The XMC USIC
  service holds MSLS/SELO0 across the software-paced multi-word frame via `PCR.FEM = 1`
  (Frame End Mode) + `SCTR.FLE = 63` (software SOF/EOF govern the frame end), so the line
  does not drop between TBUF refills. A DSPI HW-PCS `CONT` window is the K64F equivalent,
  but its release clocks a trailing dummy byte -- HW CS on DSPI is for per-frame-CS-tolerant
  devices only.
- **`CS_GPIO`** -- a software GPIO the driver owns, asserted before the first clock and
  released after the last drain (the coherent-transaction answer). The K64F DSPI service
  drives PTC4 via `GPIOC` `PSOR`/`PCOR` (write-only atomic set/clear) directly from the
  unprivileged thread -- K64F GPIO is an ungated crossbar slave, so no grant is needed.
- **`CS_NONE`** -- no chip-select managed.

## Device slots

`kos_bus_req.device` names one of `KOS_BUS_DEV_MAX` (4) slots on that bus -- the multi-drop
case, a flash on slot 0 and a sensor on slot 1. A service keeps ONE folded profile per slot
and re-applies the named slot's profile at the head of every transfer, because a controller
has a single live profile register set (DSPI `CTAR0`/`MCR`; USIC `SCTR`/`PCR`): a slot table
that were only written at CONFIG time would still let the last CONFIG win.

Slots are indexed by the CALLER's own `device` byte, so the slot table trusts the client.
That holds because **one client reaches a bus service**: the client's cap is `KOS_CAP_SIGNAL`
only, and spawn-time delegation refuses a source cap without `CAP_TRANSFER`
(`ipc-call-reply.md`), so a client cannot pass its copy on -- the reachable set is exactly
what the bring-up delegated, and the bring-up hands the endpoint out ONCE
(`xmc_spi0_take_endpoint` / `k64dspi_take_endpoint` are one-shot). Several devices behind one
client is supported; several MUTUALLY-UNTRUSTING clients on one bus is NOT -- that needs
badged endpoints (`badge` is `KOS_BADGE_NONE` today; see `roadmap.md`, service publication).

Refusals: `device >= KOS_BUS_DEV_MAX` is `status = -KOS_EINVAL`, and so is a
`KOS_BUS_OP_XFER` naming an in-range slot that has had no `KOS_BUS_OP_CONFIG` yet -- there is
no profile to apply, and a service must never clock a device on another device's profile.

## The `KOS_BUS_OP_CONFIG` op

Stores the profile for `kos_bus_req.device`; the service re-applies it before every transfer
naming that slot. The driver folds `{hz, mode, word_bits}` into its controller registers
(DSPI CTAR, USIC BRG/FDR + SCTR, ...), re-derives dividers from `KOS_SYS_PERIPH_CLOCK_HZ`
(base-keyed, never a baked-in clock), rounds the bit clock DOWN, and replies the ACHIEVED hz
truthfully. Wire shape of the reply: a `kos_bus_rsp` header (`status = 0`, `len = 4`)
immediately followed by a 4-byte `uint32_t` achieved-hz value. Both reference services emit
exactly this.

What is actually per-device depends on where the controller keeps each field, because the
driver thread is UNPRIVILEGED:

- **K64F DSPI0** -- rate, CPOL/CPHA, word size and bit order are ALL in `CTAR0`, inside the
  granted window once AIPS slot 44 is open, so all four are per-device.
- **XMC4800 USIC0-CH1** -- word size, bit order and the CS framing are in `SCTR`/`PCR` and
  are per-device. Rate (`FDR`, `BRG`) and CPOL/CPHA (`BRG.SCLKCFG`) are NOT: those registers
  are write-privileged-only at the bus and an unprivileged store to them is silently
  discarded (measured on silicon: `../design-unprivileged-root.md` section 9,
  `user/apps/xmc4800-relax/pvprobe`). `kos_spi_bus_open` fixes them, and every device on that
  bus shares one rate and mode 0, so the engine REFUSES (`-KOS_ENOTSUP`) any non-zero `cfg.hz`
  and any `CPOL`/`CPHA` bit rather than dropping them. `cfg.hz == 0` asks for the rate the
  channel is already programmed to, which it reads back out of FDR + BRG.

Re-applying costs 2 register writes on XMC and 3 on K64F per transfer -- orders of magnitude
under one SPI byte time -- so no dirty-tracking of the live slot is worth its state.

## Service-side discipline (invariant)

**The reply cap is consumed on EVERY loop path.** A malformed request (bad `proto` / `op`
/ `nseg`, segment lengths exceeding the message, a reply that would not fit the wire, a
`region_cap != -1`) gets an immediate `kos_reply` carrying `status = -KOS_EINVAL` (or
`-KOS_ENOSYS` for the region path). A leaked reply cap parks the client forever (until the
driver dies). The service loop is: `kos_recv` a request with a `kos_recv_info`; if
`reply_cap < 0` it is a plain send (not this protocol) -- ignore; otherwise run the class
transaction under the MMIO grant and `kos_reply` a `kos_bus_rsp`.

## The transport (`<kickos/sys/spi_service.h>`)

The service is a THIN TRANSPORT over the SPI class and there is no engine interface and no
template: `kickos::spi::serve_loop(bus)` blocks on the delegated WAIT recv cap
(`KOS_SPAWN_DELEGATED_CAP0`), drops plain sends (no reply cap), and hands every call to
`serve_one`, which parses the `kos_bus_req` / `seg` / `cfg` framing, enforces the inline budget,
routes `req.device` to its slot, calls the class, and ALWAYS consumes the reply cap (the
invariant above), returning when the endpoint dies (`n < 0` -> `EPIPE`) so the driver thread can
exit and let root respawn. It owns the slot store (`kickos::spi::SlotTable`, a `serve_loop`
local, so per-slot state costs driver STACK and no `.bss`) and nothing else.

The slot store holds one `kos_spi_device` HANDLE per slot, and every transfer names its handle:
with one live profile register set, a transfer that did not name its device would silently clock
on the previous one's profile, which is exactly the bug the slots fix. The class it calls is
chosen by the LINK, so the driver thread runs the same engine a local consumer of the same bus
would run; the driver's own copy carries private symbols (its `CMakeLists.txt`) so a proxy
consumer in the same image is not a duplicate definition. `k64dspi` and `xmcssc` are the two
reference services; a new bus driver writes an engine against `<kickos/driver/spi.h>` and calls
`serve_loop`.

## The bring-up (`<kickos/sys/driver_service.h>`)

A bus service's whole bring-up is `kickos::driver::bring_up(desc, cfg, &g_ep)` over a
`constexpr Descriptor` authored in the driver's own TU, which is also the only TU that sees the
chip's register directory. It creates the endpoint, claims the descriptor's IRQ lines, and spawns
each thread with its own MMIO window, memory grant and cap list, `caps[i]` landing at child cap
index `KOS_SPAWN_DELEGATED_CAP0 + i`; every failure unwinds the steps already taken. A bus takes
the `KOS_DRV_EP_RETAIN` posture: root keeps the full-rights cap so the app can delegate a narrowed
copy per client, which also means root never stops being a receiver, so NO failure path in the
driver thread may `exit()` under it. The privileged, per-class bring-up (pinmux, clock, register
init) stays in the caller.

## The client side is the CLASS, not a wrapper (`<kickos/driver/spi.h>`)

There is no separate client API. A client calls the SPI class, and its `SPI_BACKEND` is
`kickos_spi_proxy`, whose four bodies marshal onto this protocol. The mapping IS the 1:1 rule:

| class call | request |
|---|---|
| `kos_spi_bus_open` / `kos_spi_bus_close` | nothing on the wire: the peripheral's lifecycle belongs to whoever holds its window grant, and the driver thread performs it before `serve_loop` |
| `kos_spi_device_open` | `KOS_BUS_OP_CONFIG`, replying the achieved bit clock |
| `kos_spi_transfer` | `KOS_BUS_OP_XFER` |

The wire has exactly those two ops and the class exactly those four calls, so neither side can
express something the other cannot. `kos_bus_req.device` is the slot, which is
`kos_spi_device_config.slot`; a transfer naming a slot no `CONFIG` opened is refused. A client
holds a `SIGNAL`-bearing cap on the service endpoint; any thread may call, root included, and a
caller parked in a call has no timeout (`ipc-call-reply.md`).

`KOS_SPI_XFER_MAX` (212 B) is this protocol's inline budget stated in the class header, and BOTH
the proxy and the local engines refuse a longer transfer. That is what keeps an API written
against the class serialisable: an SPI transaction cannot be resumed after its chip select has
been released, so there is no short-count answer to give.

## K64F DSPI0 -- the register map behind the reference service

Everything below is instance-relative to `DSPI0_BASE = 0x4002_C000` (K64 Sub-Family RM Rev.4
Oct 2019, chapter 50), which is the whole window `k64dspi` is granted. Reset values are the RM's
and they are load-bearing twice over: they are what the bring-up shim must undo, and they are
what a read-back is compared against.

| Register | Offset | Reset | Fields the service uses |
|---|---|---|---|
| `MCR` | `0x00` | `0000_4001h` | `MSTR` b31, `CONT_SCKE` b30, `DCONF` [29:28], `FRZ` b27, `MTFE` b26, `PCSSE` b25, `ROOE` b24, `PCSIS` [21:16], `DOZE` b15, `MDIS` b14, `DIS_TXF` b13, `DIS_RXF` b12, `CLR_TXF` b11, `CLR_RXF` b10, `SMPL_PT` [9:8], `HALT` b0 |
| `TCR` | `0x08` | `0000_0000h` | transfer counter |
| `CTAR0` | `0x0C` | `7800_0000h` | `DBR` b31, `FMSZ` [30:27], `CPOL` b26, `CPHA` b25, `LSBFE` b24, `PCSSCK` [23:22], `PASC` [21:20], `PDT` [19:18], `PBR` [17:16], `CSSCK` [15:12], `ASC` [11:8], `DT` [7:4], `BR` [3:0] |
| `SR` | `0x2C` | `0200_0000h` | `TCF` b31 (w1c), `TXRXS` b30 (**read-only**), `EOQF` b28 (w1c), `TFUF` b27 (w1c), `TFFF` b25 (w1c, resets **1**), `RFOF` b19 (w1c), `RFDF` b17 (w1c), `TXCTR` [15:12], `RXCTR` [7:4] |
| `RSER` | `0x30` | `0000_0000h` | `TCF_RE` b31, `EOQF_RE` b28, `TFUF_RE` b27, `TFFF_RE` b25, `TFFF_DIRS` b24, `RFOF_RE` b19, `RFDF_RE` b17, `RFDF_DIRS` b16 |
| `PUSHR` | `0x34` | `0000_0000h` | `CONT` b31, `CTAS` [30:28], `EOQ` b27, `CTCNT` b26, `PCS` [21:16], `TXDATA` [15:0] |
| `POPR` | `0x38` | `0000_0000h` | `RXDATA` [31:0], **read-only** (the frame is right-justified per `CTAR.FMSZ`; an 8-bit service masks `0xFF`) |
| `TXFR0..3` | `0x3C..0x48` | `0000_0000h` | TX FIFO shadow, read-only (debug visibility) |
| `RXFR0..3` | `0x7C..0x88` | `0000_0000h` | RX FIFO shadow, read-only (debug visibility) |

**`MCR` resets to `MDIS = 1, HALT = 1`**: the module boots DISABLED and HALTED. A shim must clear
`MDIS`, flush both FIFOs (`CLR_TXF | CLR_RXF`) and only then release `HALT`. `PCSIS` bit 16 set
makes `PCS0` inactive-HIGH, which is what a CS-idle-high target wants. Both FIFOs are 4 entries
deep on SPI0, so `SR.TXCTR`/`SR.RXCTR` are how the service paces a multi-word transfer.

**`SR` flags are w1c and must be cleared BEFORE a line is re-armed.** An uncleared level
re-asserts on unmask and STORMS the line -- the same hazard the PIT `TIF` case teaches. `TXRXS` is
the exception: it is read-only status (RUNNING vs STOPPED), and setting `EOQF` auto-CLEARS it,
because end-of-queue stops the module. An `EOQ`-paced design therefore clears `EOQF` and lets the
next `PUSHR` restart the queue, which is exactly a request/response exchange. `EOQF` over `TCF` is
the reason: `TCF` fires per frame (N interrupts for an N-frame command) while `EOQ` marked on the
last frame fires ONCE per logical transfer, so the wake model does not change when a FIFO-burst
optimisation lands.

**Enable and reach**, all outside the granted window and therefore the shim's or the seam's:

| What | Register | Value |
|---|---|---|
| clock gate | `SIM_SCGC6` @ `0x4004_803C` | `SPI0` = bit **12** (RM 12.2.13) |
| bus supervisor-protect | `AIPS0_PACRF` @ `0x4000_0044` | slot 44, field 4 = bits `[15:12]`, `SP4` = bit **14**; the register resets to `0x4444_4444`, i.e. supervisor-only (RM 20.2.3) |
| NVIC | -- | IRQ **26**, vector **42**, `0x0000_00A8`; ONE vector for every DSPI0 source, so the enabled `RSER` bit alone decides what wakes it |
| pins (ALT2 of PORTD) | `PORTD_PCR0..3` | `PTD0 = SPI0_PCS0`, `PTD1 = SPI0_SCK`, `PTD2 = SPI0_SOUT`, `PTD3 = SPI0_SIN` |

The `PACR` register and `SP` bit are DERIVED from the block base rather than tabled
(`slot_of`/`pacr_of`/`pacr_sp_bit`, `arch/arm/chip/mk64f/regs/aips.h`, `static_assert`-pinned);
the derivation itself is in `architecture.md` under *Memory domains*. `SIM_SCGC6` and `PORTD_PCRn`
stay privileged permanently and are the escalation surfaces: the first could ungate any
peripheral, the second could re-mux SPI onto or off arbitrary pins.

The shipped service deviates from the design brief in two places, and the CODE is the contract:
the granted window is **`0x40`**, not `0x100` (it still covers `MCR..SR` and the `PUSHR`/`POPR`
pair the transfer path uses, and it is pow2 and 32-aligned so the same grant encodes on
PMSA/PMP too), and CS is **`CS_GPIO` on PTC4**, not hardware `PCS0` -- a DSPI HW-PCS `CONT`
window clocks a trailing dummy byte on release, so it suits only per-frame-CS-tolerant devices
(see *Chip-select policy* above). The transfer path is polled on `SR.RXCTR` rather than blocking
on IRQ 26; the IRQ row above is the silicon fact, not a claim that this service arms it.

## Neutrality

Nothing in the wire names a FIFO depth, a CTAR, a PCS count, or a shift unit: those are all
engine-internal. The abstraction leaks are `cs_index` and `cs_policy`, and both are REFUSED
rather than interpreted loosely. `k64dspi` drives one hardwired GPIO CS (`PTC4`) and `xmcssc`
one fixed `SELO0`, so each accepts `cs_index == 0` only and answers `-KOS_ENOTSUP` otherwise; a
driver with more than one CS line reads and bounds it. `cs_policy` is the real leak: the two
engines accept DISJOINT subsets of it (`KOS_BUS_CS_HW` on the XMC, `KOS_BUS_CS_GPIO` on the
K64F, each refusing the other with `-KOS_ENOTSUP`), so a consumer moving between them changes
that one field. Recorded rather than papered over: see `../design-driver-era-scope.md` (the
neutrality matrix) for how the same contract lands on DSPI / USIC / PL022 / C6.

## Cross-references

- The call/reply transport + its error codes: `ipc-call-reply.md`.
- The per-instance bring-up config (base/window/prio/cs as DATA, distinct from the
  per-device `kos_bus_cfg`): `architecture.md` (service list) + `system/include/kickos/sys/service.h`.
- The GPIO-CS direct-MMIO reasoning: `../book/the-fast-path-is-the-capability-gpio-direct-mmio.md`.
