<!-- SPDX-License-Identifier: CECILL-C -->
# The SPI / I2C bus-service contract

The exact wire contract a client and an unprivileged bus driver exchange 1:1 over a
`kos_call` endpoint (`ipc-call-reply.md` is the transport). Code source of truth:
`user/include/kickos/sys/bus.h` (the wire ABI), `user/include/kickos/driver/spi_client.h`
+ `user/lib/spi_client/spi_client.cc` (the neutral client wrapper),
`system/driver/mk64f/k64dspi/k64dspi.cc` + `system/driver/xmc4800/xmcssc/xmcssc.cc` (the two
reference SPI services). If a page and the code disagree, the page is the bug.

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
        uint8_t  device;          // device slot on this bus (CS index / I2C address slot)
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

Enums (`bus.h`): `KOS_BUS_SEG_RD = 1<<0` / `KOS_BUS_SEG_STOP = 1<<1`;
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
> current `system/include/kickos/sys/errno.h` taxonomy has no `KOS_EIO`. Because no I2C
> driver body exists yet, nothing defines it. The code that lands the first I2C service
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

## The `KOS_BUS_OP_CONFIG` op

Applied per device slot at bring-up (not per transfer). The driver folds `{hz, mode,
word_bits}` into its controller registers (DSPI CTAR, USIC BRG/FDR + SCTR, ...), re-derives
dividers from `KOS_SYS_PERIPH_CLOCK_HZ` (base-keyed, never a baked-in clock), rounds the
bit clock DOWN, and replies the ACHIEVED hz truthfully. Wire shape of the reply: a
`kos_bus_rsp` header (`status = 0`, `len = 4`) immediately followed by a 4-byte `uint32_t`
achieved-hz value. Both reference services emit exactly this.

## Service-side discipline (invariant)

**The reply cap is consumed on EVERY loop path.** A malformed request (bad `proto` / `op`
/ `nseg`, segment lengths exceeding the message, a reply that would not fit the wire, a
`region_cap != -1`) gets an immediate `kos_reply` carrying `status = -KOS_EINVAL` (or
`-KOS_ENOSYS` for the region path). A leaked reply cap parks the client forever (until the
driver dies). The service loop is: `kos_recv` a request with a `kos_recv_info`; if
`reply_cap < 0` it is a plain send (not this protocol) -- ignore; otherwise run the class
transaction under the MMIO grant and `kos_reply` a `kos_bus_rsp`.

## The shared serve loop (`<kickos/sys/spi_service.h>`)

A concrete SPI service supplies only its silicon; the recv / parse / reply choreography is
shared and templated over the chip's `Bus` class. `kickos::spi::serve_loop<Bus>(bus)` blocks on
the delegated WAIT recv cap (`KOS_SPAWN_DELEGATED_CAP0`), drops plain sends (no reply cap), and
hands every call to `serve_one`, which parses the `kos_bus_req` / `seg` / `cfg` framing, enforces
the inline budget, and ALWAYS consumes the reply cap (the invariant above) -- returning when the
endpoint dies (`n < 0` -> `EPIPE`) so the driver thread can exit and let root respawn. The `Bus`
supplies exactly the two members the template calls (the implicit interface):

    uint32_t configure(uint32_t hz, uint8_t mode, uint8_t word_bits, uint8_t cs_policy); // -> achieved hz
    void     transfer(unsigned char* buf, size_t len);                                   // full-duplex, in place

`Bus` is defined in an anonymous namespace, so each instantiation is TU-local (internal linkage,
no COMDAT). `k64dspi` and `xmcssc` are the two reference services; a new bus driver writes only
its `Bus` and calls `serve_loop`.

## The spawn helper (`<kickos/sys/driver_bringup.h>`)

`kickos::driver::spawn_unprivileged(entry, win_base, win_size, name, prio, ep, fail_tag)` is the
shared tail of every unprivileged-driver bring-up: it spawns the driver thread unprivileged with
its granted MMIO window (passed as BOTH the entry arg value and the grant) and a WAIT-only recv
cap on the service endpoint at child cap index 1, printing `fail_tag` and closing the endpoint on
a spawn failure. The privileged, per-class bring-up (pinmux, clock, register init) stays in the
caller; only the identical 14-arg `kos::thread::spawn` shape is factored here.

## The client wrapper (`<kickos/driver/spi_client.h>`)

Chip-neutral -- no chip register, no CS knowledge, no MMIO; the same object links against
any SPI service. A client holds a `SIGNAL`-bearing cap on the service endpoint (so it must
be a spawned pool thread -- see `ipc-call-reply.md`).

    long spi_transfer(int ep, void const* tx, void* rx, size_t len);       // 1 segment
    long spi_transact(int ep, void const* wr, size_t wlen,                 // write then read,
                      void* rd, size_t rlen);                              //   one CS bracket
    int  spi_config(int ep, struct kos_bus_cfg const* cfg, uint32_t* achieved_hz);

`spi_transfer`/`spi_transact` return rx bytes (`>= 0`) or a negative `-KOS_E*` (a
`kos_call` failure OR the service `status`). `spi_config` returns 0 or a negative code and
writes the driver's rounded-down bit clock to `*achieved_hz` when non-NULL.

## Neutrality

Nothing in the wire names a FIFO depth, a CTAR, a PCS count, or a shift unit -- those are
all class-internal. The one deliberate abstraction leak is `cs_index` (a small integer
naming a controller CS line or a driver pin slot), bounded per driver and rejected with
`status = -KOS_EINVAL` out of range. See `../design-driver-era-scope.md` (the neutrality
matrix) for how the same contract lands on DSPI / USIC / PL022 / C6.

## Cross-references

- The call/reply transport + its error codes: `ipc-call-reply.md`.
- The per-instance bring-up config (base/window/prio/cs as DATA, distinct from the
  per-device `kos_bus_cfg`): `architecture.md` (service list) + `system/include/kickos/sys/service.h`.
- The GPIO-CS direct-MMIO reasoning: `../book/the-fast-path-is-the-capability-gpio-direct-mmio.md`.
