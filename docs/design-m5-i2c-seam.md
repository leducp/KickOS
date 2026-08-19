<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# Design: the I2C seam, judged against three controllers and nine parts

> **Status: DESIGNED AND PAPER-CHECKED BEFORE ANY ENGINE IS WRITTEN.** The hardware facts below are
> read from the reference manuals and datasheets on this box and cited by section. The three target
> controllers are `mk64f` (NXP I2C), `xmc4800` (I2C as a USIC protocol mode) and `rx72m` (RIICa):
> two ISAs, three unrelated designs. Nine parts across nine vendors are judged against the seam,
> each from its own datasheet. The first real consumer is the FXOS8700CQ accelerometer/magnetometer
> on the FRDM-K64F, which is what stops the API being judged only by its own author. Read section 4
> before implementing anything that reacts to a stalled bus.

## 1. The seam already exists; only the drivers are missing

`docs/design-m5-driver-set.md` section 4 carries the survey: `user/include/kickos/sys/bus.h` was
authored for both protocols and already specifies I2C in detail, the proto discriminator,
`KOS_BUS_SEG_RD` for per-segment direction, `KOS_BUS_SEG_STOP` whose ABSENCE is repeated START,
10-bit addressing, and the request and reply payload rules. What is owed is a class header beside `spi.h`, an engine
per chip, the proxy, and a service.

**The segment list is the right abstraction, and the strongest evidence is that it is ISOMORPHIC to
one of the three controllers' native form.** The XMC4800 drives I2C by writing an opcode plus a byte
into a queue, where the opcode is START / repeated-START / send / receive-and-ACK /
receive-and-NACK / STOP (RM 18.5.4, Table 18-13). A `{direction, length, stop}` segment list is that
stream with the run-lengths folded up.

## 2. One transaction, one IPC call, and all three controllers can express it

**Can each controller express a repeated-START register read, meaning write the register pointer,
repeated START, read N bytes, as ONE indivisible transaction with no return to the client mid-way?**

**Yes on all three.** This is the question that could have forced a different shape, because KickOS
drivers are unprivileged threads reached by one IPC call per transaction, and a transaction needing
a client round trip mid-way would be a design problem rather than a coding one. It does not arise:
the driver thread owns the controller across the whole segment list, exactly as
`spi::serve_loop` already runs a list before it replies.

What differs is only how hard the driver works, and it differs by an order of magnitude.

| | `mk64f` | `rx72m` | `xmc4800` |
|---|---|---|---|
| where the transaction LIVES | nowhere: a sequence of control-register edges | two command bits plus a 1-byte buffer with a shadow | **a value**: up to 64 queued opcode words |
| buffer depth | 1 | 2 (double buffer) | 64 (shared across the module) |
| START / rSTART / STOP | register-bit edges | `ST` / `RS` / `SP` command bits | three opcodes in the data stream |
| direction ownership | software (`C1.TX`) | hardware, peeled from the address bit | per-opcode |
| driver wakeups for a 6-byte read | roughly ten | a few, plus a mandatory CPU tail | **one** |

On the XMC the whole transaction can be preloaded and software can be absent while it runs. On the
`mk64f` every byte raises the single interrupt flag. That spread is a fact about the chips, not
about the abstraction, and the abstraction survives it.

## 3. What the seam MUST NOT promise

Five refusals. The first four exist because at least one controller cannot honour the promise
without giving up its own strengths; the fifth exists because faking it is unsafe on a real part.
Nine parts across nine vendors were judged against them and none broke one.

- **No "decide the ACK after seeing the byte" hook.** The XMC fixes ACK-versus-NACK at ENQUEUE time,
  because it is part of the opcode. `mk64f` (`SMB[FACK]`) and `rx72m` (`ICMR3.RDRFS`) both offer the
  late decision natively. An SMBus-PEC-shaped API would therefore be free on two chips and would
  cost the third its entire advantage, silently. Do not offer it. The refusal also stands on its own
  merits rather than on the absence of a counterexample: validating a packet error check is a
  comparison made AFTER the transaction, never a wire ACK whose value depends on a byte just
  received, and the one packet-error-checking part in the sample reports the feature unsupported
  anyway.
- **No hardware timeout, anywhere.** The XMC4800 I2C chapter has **no timeout mechanism at all**;
  `rx72m`'s `ICFER.TMOE` and `mk64f`'s SMBus `SLT` both default OFF. See section 4: this is the
  sharp one. Nor is the refusal in tension with the SMBus standard, whose bus timeout is discharged
  BY THE SLAVE. The PMBus fan controller in the sample terminates and resets its own bus state
  machine at 25 to 35 ms and asks nothing of the master's hardware, so "no backend arms a controller
  watchdog" and "the standard mandates a timeout" are both true at once. A master-side hardware
  timeout could only fire EARLIER and abort a legitimate sequence.
- **No address-NACK versus data-NACK distinction.** **None of the three provides it**; each has one
  NACK bit and the position in the sequence is the only discriminator. This is where they converge,
  so the taxonomy can be uniform, and it must be documented as a SOFTWARE-DERIVED fact rather than
  a status bit, or someone will look for the bit. Section 9.1 states the mechanism the derivation
  depends on and the exact case where it runs out.
- **No uniform bus-busy check.** `mk64f` has `S[BUSY]` and `rx72m` has `ICCR2.BBSY`; **the XMC has
  no such bit**.
- **No device-determined transfer length.** The length is the caller's, decided before the
  transaction starts, and there is no short transfer. Faking a device-determined length by
  over-reading is unsafe on real silicon: the PMBus fan controller answers an over-read with fault
  bits and then stops responding to its own address until an alert-response cycle runs.

The class header records the first four, because three of them are ABSENCES and a comment is the
only place an absence can be written down. The fifth needs no separate note there: the transfer
length is a caller-supplied argument and a return below it is an error, so the refusal is enforced
by the shape of the call.

Clock configuration also refuses to unify: `mk64f` is a 64-entry LOOKUP TABLE times a multiplier
(RM Table 51-2) with a documented divider tolerance at low index; `rx72m` is a formula carrying
explicit rise and fall time terms; the XMC is a five-stage divider chain plus a symbol length fixed
at 10 or 25 time quanta by a protocol bit, with a hard floor on the peripheral clock. `hz` meaning
"the driver rounds DOWN and reports what it achieved" stays honest for all three, but the XMC can
outright REFUSE a rate its peripheral clock cannot reach, and that refusal must be expressible.

## 4. Held SCL has TWO causes

- **Our driver was late.** The controller stretches because a byte was not consumed or an ACK not
  written.
- **The slave is stretching on purpose.** On the SDP600 this is the NORMAL, documented,
  every-measurement data path: its datasheet calls it Hold Master, and the sensor pulls SCL down
  on receipt of a read header and holds it until the conversion finishes. The SHT3x does the same in
  its clock-stretching command variants.

**From the bus the two are indistinguishable**: SCL held low is SCL held low, and no controller has
a bit naming who holds it. So a deadline alone cannot tell a wedged driver from a healthy sensor,
and a design that treats every expiry as a fault will report a working part as broken.

**From INSIDE the controller they are distinguishable on two of our three, and NOT on the one
section 2 praises.** Our-fault leaves a status flag ASSERTED AND UNSERVICED, the byte unconsumed or
the ACK unwritten, which `mk64f` shows through its pending interrupt flag and `rx72m` through its
automatic low-hold with a transfer flag still set. A stretching slave leaves the flag CLEAR and the
transfer merely incomplete. **The XMC4800 cannot make this distinction, and the reason is exactly
its advantage**: a preloaded opcode queue that has stalled looks identical whether a slave is
stretching or software failed to refill it. The chip that needs a deadline least is the chip on
which a deadline says least.

### 4.1 The deadline is per DEVICE, and it lives in `kos_bus_cfg`

There are two deadlines at two levels, and only one of them can cross the wire.

**At the class level the deadline is a mandatory argument of the transfer call**, enforced in
software, rejected when zero and capped by a class-level ceiling. A driver that links an engine
directly sizes it for the device it is driving, which is the case where the caller knows the part.

**The wire has nowhere to put it.** `kos_bus_req` carries proto, op, device, nseg, a region cap and
an offset, and `kos_bus_cfg` carries hz, addr, mode, word bits and chip-select policy. Neither
carries a deadline, so a proxy cannot forward its caller's argument and a service's deadline is the
service's own policy.

**Across the wire the bound is therefore PER-DEVICE, not per-transaction**, and that is the right
answer rather than a concession. Stretch behaviour is a property of the DEVICE; `kos_bus_cfg` is
already the per-device profile the service re-applies before every transfer; a per-call override
would let N clients express one physical fact N different ways and disagree, which is the second
truth the tiebreaker forbids. A per-call deadline is also a client-chosen grant of shared-bus time,
which inverts the isolation principle.

The field is a 16-bit hold bound spent out of `kos_bus_cfg`'s reserved bytes. **Not in
microseconds**: one part in the sample has a 50 ms typical warm-up with no stated maximum, so a
microsecond field overflows the very case it exists for; 100-microsecond ticks or milliseconds are
the honest units. The struct stays its asserted size, SPI callers leave it zero and every SPI engine
ignores it exactly as it already ignores the chip-select index. A CONFIG for I2C with the field
unset is REFUSED rather than defaulted, so the authority is total and there is no sentinel meaning
"guess".

**That spends the last free field.** `kos_bus_cfg` is fixed at twelve bytes by a static assert and
its entire slack is those two reserved bytes. Three further per-device facts are already known that
would want one: a bus-free minimum between STOP and START (one part specifies 1.4 ms, a thousand
times the bus specification's), a clock-rate FLOOR (section 11), and a multiplexer leg index. The
next per-device fact forces the struct to grow, and that is a decision to make deliberately rather
than discover.

### 4.2 What the deadline is, and what it is NOT

**It is a LIVENESS bound. It is not a DoS bound.** A slot legitimately configured for a 20 ms
stretch lets its client stop the bus for 20 ms per transaction, and a wedged driver on that same
slot gets the same 20 ms. Sizing the deadline for the worst device on the bus is what makes it
useless as a latency guarantee for everyone else.

**What actually bounds bus-wide latency is WHICH DEVICES SHARE A BUS**, which is board
documentation, not ABI. Two rules follow, and they are worth more than any field:

- **Prefer a non-stretching device mode wherever the part offers one.** The SHT3x is the proof: its
  polled single-shot commands replace a 15 ms bus hold with roughly 630 microseconds plus a
  client-side retry, a twenty-five-fold improvement in worst-case occupancy for free.
- **Do not co-locate an unavoidably-stretching part with a latency-sensitive peer.** The SDP600
  offers no polled alternative at all, and its datasheet gives no maximum stretch, only typicals.
  **Any deadline chosen for it is a human's guess with margin over a typical**, and must be
  documented as such rather than presented as engineering.

**The seam CANNOT enforce the first rule.** A non-stretching command and a stretching one differ
only in two opaque payload bytes inside a write segment; the bus driver cannot see which went past
without parsing per-device command semantics, which is precisely the layering the seam exists to
avoid and would not generalise beyond one part anyway. **The refusal belongs in the class or
consumer layer, never in the wire.** So the seam must still own a deadline sized for the possibility
that a client asked for a stretch.

Expiry semantics SPLIT along the two causes above: expiry with our own flag pending is our fault and
the peripheral should be reset; expiry with the transfer outstanding may be a healthy slave and the
only safe action is to report it. On the XMC that distinction is unavailable, so every expiry there
is a guess, and no uniform rule can be written across the three.

### 4.3 What the driver thread DOES for those milliseconds

The I2C driver thread's priority and preemptibility are part of its contract, and so is what the
thread does while a slave stretches: **busy-waiting at driver priority for 15 ms is not
acceptable**, and parking on the completion interrupt is the answer only if the park path can be
taken without releasing the controller mid-transaction.

## 5. Two isolation facts that are NOT driver details

- **On the XMC the isolation unit is the USIC MODULE, not the channel.** The FIFO RAM is
  module-global and shared between the two channels, and the registers that carve it up live INSIDE
  each channel's own register window, so a misprogrammed channel can overlap its sibling's FIFO
  storage, and no MPU region can see it. Service requests and the clock gate are module-scoped too.
  Granting one channel of a USIC module is therefore NOT isolation from the other channel, and a
  board that wants two independent USIC-based buses must spend two modules.
- **`rx72m` has no bus-master memory protection.** The CPU MPU is CPU-only. A DTC or DMAC channel
  aimed at a sibling controller's data register is not stopped by the grant, which matters
  precisely because DTC is the performance answer on that chip. Any per-channel grant scheme there
  must also control who may program the transfer engines.

By contrast the register windows themselves are all clean: 12 bytes of registers alone in a 4 KiB
slot on `mk64f` with a byte-granular protection unit, a 32-byte stride against a 16-byte granule on
`rx72m`, and a naturally aligned 512-byte channel window that is an exact power-of-two fit on the
XMC.

## 6. What is NOT known, and must not be guessed

- **The FXOS8700CQ register map has a source on this box** and the sensor's numbers must be READ
  FROM IT rather than written from memory: the `WHO_AM_I` offset, its expected value and the sample
  burst layout. The board facts are manual-backed from the FRDM-K64F user guide: I2C0, SCL and SDA
  on the PTE24/PTE25 pair, 7-bit address `0x1D` as strapped here, and the two interrupt lines. The
  ACCESS SHAPE depends on none of those numbers; it is settled by section 2.
- **On `mk64f`, the timing window in which a repeated START may legally be requested is never
  defined.** The manual says only that it works when the part is the current master, and lists a
  mistimed repeat among the causes of arbitration loss, without saying what mistimed means. Whether
  an arbitrarily delayed repeated START, one issued after the register-address byte has been
  acknowledged by a driver that was preempted in between, is safe **is not answerable from the
  manual**. This is the highest-value unknown on the page: it is the chip hosting the first real
  consumer, and it interacts directly with section 4. **Settle it on silicon, not by reasoning.**
- One open-drain detail on `mk64f` that is easy to miss: selecting the I2C pin function does not by
  itself enable open drain; the port control register bit must be set.

## 7. Order of work

1. The class header beside `spi.h`, mirroring its four-symbol shape and its rename discipline.
2. **`rx72m` first, not `mk64f`.** It is on the bench, its register window is the cleanest, and its
   transaction model sits between the other two, so an abstraction that fits it is unlikely to be
   accidentally shaped by either extreme. `xmc4800` second, also on the bench, and it is the one
   that will prove whether the segment list really is expressible as an opcode queue.
3. `mk64f` third, with the FXOS8700CQ, once that board is attached. It is the binding constraint on
   any latency promise: least buffering, least useful DMA, and the one timing question the manual
   does not answer.

## 8. What the first consumer actually needs, and where it touches the SEAM

Read from the FXOS8700CQ datasheet Rev 8.0. Cited facts, with the gaps kept separate because a
confident wrong register number costs a silicon session.

The bring-up is small: `WHO_AM_I` at `0x0D` reads **`0xC7`** on production silicon (`0xC4` marks a
preproduction part, so gate on `0xC7` and treat `0xC4` as a board to ask about). Almost every
configuration register is **writable only in standby**, standby-versus-active being one bit of
`CTRL_REG1`, so the driver's shape is: read the identity, drop to standby, configure, then set
active in the same write that sets the rate. Accelerometer samples are a **six-byte burst from
`0x01`**, three axes of 14-bit **left-justified** two's complement, MSB byte first, and, unlike the
magnetometer registers, they carry **no MSB-first latch requirement**, so a partial read is merely
useless rather than corrupting.

**Two things here are seam-level, not sensor-level.**

- **The repeated START is REQUIRED, not merely convenient.** The datasheet documents the register
  read only in its repeated-START form, and whether a STOP between the pointer write and the read is
  tolerated is **not stated either way**, so it must not be assumed. This is reinforced by a
  separate documented behaviour: **the auto-increment pointer resets to `0x00` on an I2C STOP.** A
  seam that could not hold a transaction across the direction change would therefore not merely be
  slower here, it would read the wrong registers. Section 2's answer is what makes this consumer
  expressible at all.
- **A NACK is a LEGITIMATE outcome of one transaction.** Issuing the software reset makes the part
  reset immediately and **not acknowledge the byte that commanded it**, which the datasheet says
  outright. The seam reports a NACK as an error, and correctly so; what this shows is that a
  consumer may need to ISSUE a transaction whose NACK is expected. That does NOT justify a
  per-segment "NACK is fine" flag: adding one would weaken the taxonomy for every other caller to
  serve a single register write. The driver ignores the error on that one transaction, and says why.

Three ordinary facts that constrain the driver rather than the seam: the part is characterised to
**400 kHz** and no higher rate is guaranteed; it **does not use clock stretching**, so it is not the
peer that will trigger section 4's hazard; and a reset needs a **1 ms quiet period** before the bus
is touched again.

**Gaps carried forward, deliberately unfilled.** Whether STOP-then-START is tolerated in place of a
repeated START; what the auto-increment pointer does past the end of the mapped range; what actually
happens electrically if a standby-only register is written while active; and, the one worth chasing,
**the errata document the datasheet itself points at for the reset mechanism is not on this box**,
so reset handling must not be finalised from the datasheet alone.

## 9. What nine parts demand of the seam

The design above answers to one consumer and three controllers. It is also judged against nine
parts across nine vendors, each from its own datasheet and each read to break the design rather than
to confirm it.

**Segment expressibility holds on shape for every one of them.** No device comes close to
`KOS_BUS_SEG_MAX`: a BMP180 cycle is five transactions of at most two segments, an SDP600
measurement is a single two-segment transaction, and an SHT3x periodic fetch is two segments. The
one part that does not fit exceeds a SIZE rather than the shape, and section 9.5 says where it
lands.

### 9.1 A NACK carries its POSITION, and refusal 3 rests on that

The no-address-versus-data-NACK refusal holds because a NACK surfaces as a negative status with
`len` set to the bytes actually transferred BEFORE it, so position, and therefore meaning, is
recoverable from the segment list the caller submitted. That rule lives in the bus service
reference and in the class header's `xferred` out-parameter, and **it is not optional**: an
implementation that returns zero length on every error silently destroys the discrimination the
refusal depends on, and nothing would catch it.

**The recovery works only while the count is greater than zero.** An address-phase NACK on the
first segment transfers nothing, so the reply is a negative status with a zero count, and that is
byte-identical for "not ready", "device absent", "device asleep" and, on a bus with a multiplexer,
"wrong channel selected". So the scope is exact: position is recoverable only when bytes preceded
the NACK; a zero-count NACK is definitionally ambiguous, and only a class that has previously seen
the device ACK can tell absent from busy. The refusal itself is unaffected, since no controller
offers the bit and inventing one in software would be a second truth.

That ambiguous case is not a corner. **A NACK is the EXPECTED reply of most transactions in a
polled loop**, and one of the three parts that make it so produces exactly the zero-count kind: the
SHT3x makes an ADDRESS NACK the documented signal for "measurement not ready". Section 8's
unacknowledged reset is a second expected NACK, and the SDP600 makes a data NACK the documented
signal for "command not recognised". That is three parts across three vendors. A taxonomy whose common case is an error invites log floods and tempts someone
to add the per-segment "NACK is fine" flag section 8 refuses. **The class layer is where "this NACK
is expected" is expressed.**

### 9.2 Two holes that need no stretching, no late driver and no deadline

- **A bus-wide reset is reachable through the seam.** The SHT3x supports the I2C general call:
  writing one byte to address `0x00` resets EVERY general-call-capable device on the bus, and its
  datasheet says so plainly. Any client able to configure a device slot with that address gets that
  primitive. **The service must refuse the reserved I2C address ranges at CONFIG time** unless a
  board explicitly grants them. A few lines, no wire bytes, and it closes a denial of service that the
  deadline cannot reach because there is nothing slow about it.
- **A part can lock the bus before any transaction exists.** The BMP180's datasheet warns that
  powering its interface rail before its core rail leaves its pins undefined and can lock the bus.
  No transaction, so no deadline; this is a board-bring-up ordering constraint, and the only place it
  can live is board documentation.

### 9.3 Device state is unowned across transactions

Section 2's property, one transaction, one IPC call, no cross-call state held, is a statement about
the DRIVER, and every part confirms it: no device needs the bus withheld between a conversion start
and its result read, and none needs a repeated START to span the wait.

**It is not a statement about the DEVICE.** Parts carry state across transactions that the seam
neither owns nor protects. The BMP180's result register does not record whose conversion produced
it, so two clients interleaving a temperature and a pressure read silently swap answers. The SDP600
stores its command internally, so the obvious optimisation of dropping the command segment on
repeat reads is exactly what exposes a client to another client's command. SHT3x periodic mode is
entered by one transaction and left by another, so a client that dies leaves the part measuring
forever, which its own datasheet warns can self-heat and corrupt the measurand **for every other
client**; it also mandates a 1 ms gap between commands that no single client can enforce when a slot
has two.

With four device slots, no owner field, and no stated restriction on who may configure one, the
position is that **a device slot has exactly one client BY BOARD POLICY**, and that this is a
convention rather than a mechanism. Say it, or give the slot an owner.

**Two mutually-untrusting clients interleaving on one bus is not reachable today**: the bus service
reference already forbids it, one client reaches a bus service, the endpoint capability lacks the
transfer right, and bring-up hands it out once. The gap is still real by three other routes, and the
third is the one that matters:

- A multiplexer channel left selected when a transfer times out or a client dies simply STAYS
  selected, with no owner and no reset-on-release, and nothing observes it.
- Two threads inside ONE client interleave at exactly transaction granularity.
- Badged endpoints are on the roadmap, so the day multi-client support lands, a multiplexer converts
  a new feature into a silent wrong-data bug. **A hazard that arrives with a planned feature is a
  dependency, not a risk.**

### 9.4 The recovery primitive a real part prescribes exists on one of our three controllers

The SHT3x documents its own interface reset as toggling SCL nine or more times with SDA held high.
`rx72m` can do exactly that, one pulse at a time, self-clearing, with arbitration-loss detection
disabled while it runs. **`mk64f` and `xmc4800` cannot.** On those two the only route is muxing the
pins back to GPIO and bit-banging the pulses, **which requires pin-mux authority the I2C driver's
grant does not include**. That is a grant-shape question, and it must be settled before three
engines exist rather than discovered by the first wedged bus.

### 9.5 One part exceeds the inline budget, and that is what the region path is for

A PMBus fan controller's fault-log command returns 255 bytes in one block, which with its command
byte exceeds the 212-byte inline budget by about twenty percent. It cannot be split: the part offers
no offset, only "execute it fifteen times", each execution returning a whole log.

**This is the first concrete consumer for the region-capability path the wire ABI already reserves
and marks deferred**, and that is the right answer. Raising the endpoint message maximum instead
would cost every endpoint in the system for one diagnostic command.

## 10. The dividing line, stated once so it stops being decided case by case

Every part poses the same question in a different costume, and it has one answer:

> **Anything observable ONLY ON THE WIRE belongs to the seam. Anything meaningful ONLY TO THE DEVICE
> belongs to the client.**

Client, because only the device gives them meaning: byte order within a multi-byte value (three
conventions across three vendors in this sample alone), register semantics, page geometry, which
opaque command byte selects a stretching measurement, whether a NACK is expected here, and every
irreversible lock hiding inside an ordinary-looking write.

Seam, because nothing above it can observe them: START, repeated START and STOP; addressing and
direction; the ACK bit; the clock rate; the deadline; arbitration; and bus recovery.

**The rule catches BOTH failure modes, which is why it is worth stating rather than deciding each
case.** Parsing payload to enforce a device policy is the seam reaching up: it is why the SHT3x's
non-stretching command set cannot be mandated here (section 4.2), why page-wrap protection cannot
live here (section 11), and why granting a device slot on the M24M02E-F grants three irreversible
operations the seam cannot see. **Discarding a NACK's POSITION is the same rule broken the other
way**: the position is observable only by the controller, no client can recover it, so dropping it
is not staying out of the client's business, it is losing something only the seam ever had.

The rule is also what keeps the four-symbol byte-stream shape paying: three byte orders, two word
widths and nine register conventions pass through it without the wire learning any of them.

One consequence worth stating because it looks like an exception and is not: `kos_bus_mode`'s
LSB-first bit is a SPI concept. SPI can genuinely shift either way, and I2C mandates MSB-first on
the wire, so it names nothing on this bus. It is a DECOY, sitting in a shared enum looking applicable
to a device with 16-bit registers. The engine refuses it, along with every other mode bit that is not
10-bit addressing, rather than ignoring it. That refusal is the rule applied, not an extra check.

## 11. Where the EEPROM and the light sensor push hardest

**Two structural findings.**

- **One part needs FIVE I2C addresses and a bus service tracks FOUR.** The M24M02E-F's 2 Mbit array
  carries its top two address bits INSIDE the device select byte, so it answers at four consecutive
  addresses, and its identification page and configuration registers answer at a fifth under a
  different device type identifier. The array alone consumes every slot, leaving nothing for the ID
  page and nothing for any other device on the bus. **`device` stops being a device identity and
  becomes an address bit.** The slots are also NOT independent: a write on one makes all five NACK
  for up to 4 ms, and writing the address-configuration register renames the part out from under the
  whole table. This is open, and it is the sharpest open question on the page.
- **A legal-looking read CORRUPTS a nonvolatile page.** The read-lock-status instruction answers
  entirely in the ACK bit of one data byte, and terminating it with a plain STOP right after that
  byte's acknowledgement is exactly the condition that TRIGGERS the internal write cycle, committing
  the probe byte into the identification page. The safe terminator is a repeated START followed by a
  STOP, which the segment list CAN express only as a trailing **zero-length write segment**. So
  zero-length segments are load-bearing for CORRECTNESS, not merely for polling efficiency.

**Two contract rules follow, and they belong in the reference rather than in the wire.**

- **Zero-length legality is stated, not inferred.** A zero-length WRITE segment is the address-only
  presence probe; a zero-length READ segment is refused, there being no way to address a device for
  reading and then clock nothing. This has to be written down because the SPI precedent points the
  other way and refuses zero length outright, so anyone writing the service from the reference plus
  the SPI template would otherwise have a coin-flip chance of refusing the idiom the EEPROM
  requires.
- **The reply length is read against the SIGN of the status.** The wire header describes it as the
  read bytes that follow; the NACK rule describes it as the bytes transferred before the failure.
  For a WRITE-ONLY transaction those disagree outright, and the EEPROM hits that on ordinary traffic
  rather than in a corner. A non-negative status means `len` is rx bytes and they follow; a negative
  status means no payload follows at all and `len` is the transferred count.

**One open question.** A clock-rate FLOOR has no home. The VEML7700 specifies a minimum bus
frequency, and "the driver rounds down and reports what it achieved" is honest but for the first
time not sufficient: rounding down is normally a performance question and here it is a correctness
one. There is no per-device field left to put a floor in (section 4.1).

**And one vindication.** The VEML7700's LSB-first word order is the third byte-order convention in
the sample. Had the I2C device config kept a word-width or endianness notion, and the shared config
struct shows the temptation existed, the wire would now be arbitrating three conventions and would
be wrong for two of them. Section 10 is why it does not.

## 12. The boundary of what the seam is FOR

The single most useful sentence to come out of nine parts:

> **The seam's unit of atomicity is ONE TRANSACTION TO ONE ADDRESS, and that is not a limitation of
> the wire framing. It is the boundary of what the seam is FOR.**

Every failure found that is not a size problem is the same request wearing a different costume: a
device asking for atomicity ACROSS that boundary. A PCA954x multiplexer needs two addresses across
two transactions; a PMBus group command needs several addresses inside one; a page selector, a
sticky register pointer, a stored command, a hidden log cursor and an 8 ms reset window all need two
transactions welded together. Section 9.3 states the slot's single client as a convention rather
than a mechanism. For a sensor the convention's violation is untidy. For a secure element it is a
silent cryptographic failure, and for a multiplexer it is correct-looking data from the wrong
physical device. **Give the slot an owner.**

### 12.1 Two parts to REFUSE, and one rule to adopt

**Refuse the secure element behind this seam, and write the refusal down.** It needs a session that
is atomic against other clients, a bounded maximum gap BETWEEN transactions enforced by scheduling
rather than by a config field, a transfer length decided from the data, and an exclusive owner for
the whole session. Those are the properties of a device service, not of a bus transaction. Its own
service owning its own bus costs one service and zero ABI, against a wire change that would tax
every other part and that one controller cannot perform at all. Note also that the document on this
box for it is a SUMMARY datasheet carrying no protocol whatsoever: every protocol-shaped fact about
it came from a vendor library, which is an implementation and not a specification.

**Refuse a group command**, meaning several addresses inside one transaction, for the same reason.

**Adopt one class-layer rule, which generalises past every part that suggested it:** *a device whose
register pointer or command persists across transactions must have it rewritten on every access.*
The saved segment is never worth the silent-wrong-data exposure. On the ADS111x ADC that motivated
it the extra cost is about fifty microseconds against a conversion of one to a hundred and
twenty-five milliseconds.

## 13. What nine parts were really probing: stream, or a set of devices

The findings in sections 9 to 12 are not nine defects. They are ONE model boundary, found nine times
from nine directions.

There are two ways to build a bus service, and the tree has built the first:

- **The service as a STREAM.** It is a transaction pipe. A client hands it a segment list, it
  arbitrates access to the controller, and device identity is nothing more than a slot's configured
  address and profile. Its unit is ONE TRANSACTION TO ONE ADDRESS, exactly as section 12 states.
- **The service as a SET OF DEVICES ON A BUS.** It owns the TOPOLOGY. It knows what is attached, it
  hands a client a capability to A DEVICE rather than to the bus, and ownership, sequencing and
  reachability are its to enforce.

**Every open finding on this page is the second model asking to exist.** A multiplexer is topology,
and a stream has no topology. A slot owner is device identity with a lifetime, and a stream has only
an address. A secure element's session is sequencing across transactions, and a stream's boundary is
one transaction. A page selector, a sticky pointer, a stored command and a hidden log cursor are all
per-device state with a rightful owner, and a stream owns nothing between calls. Even the exhausted
reserve in section 4.1 is the same pressure in miniature: per-device facts accumulating in a struct
that was sized for a stream's idea of a device.

**This is not a defect in the stream model.** A stream is the right shape for what it does, it is
what nine parts out of nine can be driven through, and its refusals are what keep it honest. The
error would be to grow it a field at a time until it is a bad version of the second model. Section
4.1's insistence that the next field be a deliberate decision, section 12.1's two refusals, and the
argument against a per-segment device selector are all the same refusal: **do not pay a wire cost on
every caller to half-serve a model the wire is not.**

### 13.1 The distinction that resolves the secure element

**The secure element belongs to the I2C DRIVER. It does not belong behind the I2C SERVICE.** Those
are different layers and the difference is the whole point:

- The **class and engine** are a LIBRARY. A driver links them, owns its controller, and drives a
  device with ordinary code. A session is one function; the watchdog is bounded by that function's
  own scheduling; a length decided from the data is three lines, because the driver never left the
  transaction.
- The **bus service** is an IPC-mediated ARBITER over a shared controller. Its boundary is a
  transaction, by construction, and every property the secure element needs crosses it.

So the part is not refused by KickOS. It is refused by ONE LAYER, and it is served by the layer
below with no ABI change anywhere. The same reading applies to the multiplexer: topology owned by
whoever owns the controller is ordinary code, and topology expressed through a transaction pipe is
the thing that has no answer.

### 13.2 Where the second model stands

It has been considered and not developed. Nothing on this page argues for building it now: the
driver era's remaining work does not need it, and the stream serves every part checked. What this
survey adds is **the evidence for when it WOULD be needed, gathered before it is needed**: the
moment badged endpoints make a bus reachable by mutually-untrusting clients, every convention
section 9.3 relies on becomes a mechanism that must exist, and a multiplexer turns that transition
into silent wrong data rather than an error.

**That is what nine paper checks buy.** Not nine fixes: a mapped boundary, a documented set of
refusals at it, and a named condition for revisiting it.
