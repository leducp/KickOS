<!--
SPDX-License-Identifier: CECILL-C
Copyright (c) 2026 Philippe Leduc
-->

# M4.6.2 design -- a USB CDC-ACM console on picopi, pizero2350 and teensy41

> **Status: ACTIVE** -- continued as M4.9.1; the code shipped.

M4.6.2 gives the three boards whose console needs an external USB-serial adapter a console
over the USB device controller each part already carries. It consumes M4.6.1's substrate --
the line capability, spawn-time delegation, reclaim on driver death, the SPSC byte ring and
the two-thread driver shape -- **unchanged**. Nothing here proposes a new kernel object, a new
syscall, or a new service kind.

`TODO.md` ("M4.6.2 -- USB CDC console") carries the motivation and five requirement bullets and
flags two claims about the controllers as unverified. Both are now verified against the local
datasheets; sections 1 and 2 are that verification. Where a bullet turned out to be wrong or
incomplete the correction is stated in place, marked **CORRECTION**, rather than collected in a
list -- there are seven of them and each belongs next to the argument it changes.

---

## 0. The four verdicts that decide the cost

| Question | Verdict | Where |
|---|---|---|
| RP2040 vs RP2350 USB block | **SAME IP, RP2350 a documented superset.** One backend, two boards, one mandatory delta and four cosmetic ones | sec.1 |
| i.MX RT1062 controller | **EHCI-derived device mode, queue heads plus transfer descriptors. A different programming model, confirmed.** +1 backend | sec.2 |
| Can anything be gated with no board | **Effectively nothing.** No emulator in this project models a USB device, and the class layer's only host-testable surface is descriptor construction | sec.8 |
| Does the two-thread shape meet the enumeration deadline | **Yes, by three orders of magnitude, and that is NOT why M4.6.2 follows M4.6.1** | sec.4.4 |

The last row is the most load-bearing correction in this document, because it removes the reason
`TODO.md` gives for the sequencing and replaces it with the real ones.

---

## 1. VERDICT -- the RP2040 and RP2350 USB device blocks are the same IP

Both datasheets are present in the local reference set. The RP2040 datasheet is
`RP-008371-DS-1-rp2040-datasheet.pdf` (build 2025-02-20), chapter **4.1 "USB"**. The RP2350
datasheet is `RP-008373-DS-2-rp2350-datasheet.pdf` (build 2025-07-29, `d126e9e-clean`), section
**12.7 "USB"**. Neither is quoted at length below; every row is a register-level comparison
made independently in each document, so the verdict does not rest on the vendor's own summary
sentence alone.

The vendor's summary sentence does exist, and it is unusually direct. RP2350 **12.7.2 "Changes
from RP2040"**: all changes are a superset of the RP2040 features, existing RP2040 USB software
continues to work with **one** exception -- `MAIN_CTRL.PHY_ISO` must be cleared at startup and
after a power-down event -- and `LINESTATE_TUNING` should be left at its reset value. RP2350
**12.7.2.1** adds that RP2350 fixes all RP2040 USB errata.

### 1.1 The independent comparison

| Item | RP2040 (DS 4.1) | RP2350 (DS 12.7) | Same |
|---|---|---|---|
| DPRAM base | `0x50100000` (DS 2.2.2) | `0x50100000` (DS 2.2.5) | yes |
| DPRAM size | 4096 B | 4096 B | yes |
| Register base | `0x50110000` (DS 4.1.4) | `0x50110000` (DS 12.7.3.7.2 as controller `+0x10000`) | yes |
| Setup-packet buffer | DPRAM `0x00`-`0x07`, 8 B | DPRAM `0x00`-`0x07`, 8 B | yes |
| Endpoint-control regs | DPRAM `0x08`-`0x7f`, 4 B each, EP1..EP15 IN/OUT interleaved, **no EP0 entry** | identical, no EP0 entry | yes |
| Buffer-control regs | DPRAM `0x80`-`0xff`, 4 B each, EP0..EP15, **EP0 present** | identical | yes |
| EP0 buffers / free space | `0x100`, `0x140`; data buffers from `0x180` | `0x100`, `0x140`; data buffers from `0x180` | yes |
| Endpoint-control bits | 31 enable, 30 double-buffered, 29 int-per-buffer, 28 int-per-2, 27:26 type, 17 int-on-stall, 16 int-on-NAK, 15:6 buffer address | identical | yes |
| Buffer-control bits | 31/30/29 buf1 full/last/PID, 28:27 iso offset, 26 buf1 available, 25:16 buf1 length, 15/14/13 buf0 full/last/PID, 12 reset, 11 stall, 10 buf0 available, 9:0 buf0 length | identical | yes |
| Register offsets | `ADDR_ENDP` 0x00, `MAIN_CTRL` 0x40, `SOF_WR` 0x44, `SOF_RD` 0x48, `SIE_CTRL` 0x4c, `SIE_STATUS` 0x50, `INT_EP_CTRL` 0x54, `BUFF_STATUS` 0x58, `BUFF_CPU_SHOULD_HANDLE` 0x5c, `EP_ABORT` 0x60, `EP_ABORT_DONE` 0x64, `EP_STALL_ARM` 0x68, `NAK_POLL` 0x6c, `EP_STATUS_STALL_NAK` 0x70, `USB_MUXING` 0x74, `USB_PWR` 0x78, PHY 0x7c/0x80/0x84, `INTR` 0x8c, `INTE` 0x90, `INTF` 0x94, `INTS` 0x98 | identical, all of them | yes |
| `SIE_STATUS` bits | 31 data-seq, 30 ack-rec, 29 stall-rec, 28 nak-rec, 27 rx-timeout, 26 rx-overflow, 25 bit-stuff, 24 crc, 19 bus-reset, 18 trans-complete, 17 setup-rec, 16 connected, 11 resume, 10 vbus-overcurr, 9:8 speed, 4 suspended, 3:2 line-state, 0 vbus-detected | identical positions, plus 23 endpoint-error and 12 rx-short-packet documented | superset |
| `INTE`/`INTS` bits | 0..19, the map in sec.4.1 below | 0..19 identical, plus 20 rx-short-packet, 21 endpoint-error, 22 watchdog-fired, 23 epx-stopped-on-nak | superset |
| `BUFF_STATUS` map | bit `2n` = EPn IN, bit `2n+1` = EPn OUT, write-1-to-clear | identical | yes |
| Speed | Full Speed 12 Mb/s device (integrated USB 1.1 PHY) | Full Speed 12 Mb/s device | yes |
| Endpoint count | up to 32 (EP0..EP15, both directions) | up to 32 (EP0..EP15, both directions) | yes |
| SETUP handling | hardware always accepts, auto-ACKs, lands at DPRAM 0, raises `SIE_STATUS` bit 17; `EP_STALL_ARM` auto-cleared on SETUP | identical | yes |
| Access rules | DPRAM takes 8/16/32-bit accesses and has **no set/clear aliases**; controller writes status back as a **16-bit** half-word write | identical | yes |
| Ordering rule | write buffer info, delay >= 1 `clk_usb` cycle, set `AVAILABLE`/`STALL` **last** | identical | yes |
| USB IRQ | **5** | **14** | no |

The undocumented gap at register offset `0x88` in the RP2040 register list is `LINESTATE_TUNING`
on the RP2350. That is corroborating evidence for one silicon block rather than a coincidence.

### 1.2 The deltas a shared backend must carry

Small enough that a per-chip constants header plus one init hook covers all of them.

| Delta | RP2040 | RP2350 | Consequence |
|---|---|---|---|
| `MAIN_CTRL.PHY_ISO` (bit 2) | absent | resets **1**; must be cleared at startup and after power-down | the one mandatory per-chip init step (DS 12.7.2) |
| `SIE_CTRL.PULLDOWN_EN` reset | 0 | **1** | init writes absolute values, never a read-modify-write from reset |
| `USB_MUXING.TO_PHY` reset | 0 | **1** | same |
| `LINESTATE_TUNING` (0x88) | undocumented | do not write; leave at reset | one "hands off" comment |
| USB IRQ number | 5 | 14 | a per-chip constant, `KICKOS_MAX_IRQ` is 32 and 52, so both fit |
| Errata | E2, E5, E15, E16 | all four fixed; **E12** in their place | sec.1.3 |
| Access control | none | `ACCESSCTRL.USBCTRL` defaults to Secure-only; SAU is bypassed (peripherals are IDAU-Exempt) so **the MPU is the only gate** | no action -- KickOS boots Secure with no TrustZone -- but it is the sentence that says the grant model works |

`RESETS.RESET` holds the block in reset out of power-on on both parts; the block must be taken
out of reset before DPRAM is readable. Instruction fetch from DPRAM bus-errors unconditionally
on RP2350, which is a free guarantee rather than a constraint.

**Decision: ONE backend, `rp2xxx`-shaped, with a per-chip constants header and one per-chip
init hook.** This is the `stm32f411` shape `TODO.md` predicted, and the prediction is now
verified rather than assumed.

Rejected alternative: **two backends, one per chip.** Rejected because the delta list above is
the entire difference and four of the six rows are handled by writing absolute values, which is
already the house rule for a reclaim body (`arch/include/kickos/arch/arch.h`). Two backends
would duplicate roughly a thousand lines of endpoint and control-transfer logic to express six
constants, and would let the two copies drift -- the failure the fleet's shared-backend pattern
exists to prevent.

### 1.3 Errata, and one that changes a design choice

| Erratum | Substance | What M4.6.2 does |
|---|---|---|
| RP2040-E16 / RP2350-E12 | status signals cross `clk_usb` -> `clk_sys` unsynchronised and can be **lost** when `clk_sys <= clk_usb`. Workaround: run `clk_sys` at least 10 % faster than `clk_usb` while the block is in use | satisfied by construction: `picopi` runs 125 MHz and `pizero2350` 150 MHz against 48 MHz. **But see sec.7.2** -- the degraded clock path is not |
| RP2040-E2 | with `EP_ABORT` set, the device NAKs forever on all endpoints. Workaround: do not use `EP_ABORT` | **the panic path must not abort the driver's in-flight buffer** (sec.5.2) |
| RP2040-E5 | the device needs 800 us of idle J-state after a bus reset before reaching CONNECTED; behind a hub transaction translator it never sees that idle and never enumerates. Hardware-fixed in RP2040B2 | needs the bench Pico's **stepping identified** before this is either dismissed or worked around. Open question 3 |
| RP2040-E15 | at full speed, **bulk IN buffers larger than 50 bytes** can hang the device controller against a VL805 host (Raspberry Pi 4 / 400 downstream ports); recovery needs a controller reset | **decided: cap the CDC bulk endpoints at `wMaxPacketSize` 32**, which puts every buffer under the threshold and costs nothing a console can measure. Rejected alternative: the vendor's timing workaround, which withholds bulk IN buffers during the last 200 us of each frame -- that needs the SOF interrupt armed permanently and gives up about 20 % of bulk IN bandwidth to protect a console that needs neither |

---

## 2. VERDICT -- the i.MX RT1062 is EHCI-derived, and a different programming model

`IMXRT1060RM_rev3_annotations.pdf` is present. Chapter **42 "Universal Serial Bus Controller
(USB)"**, with the PHY split into chapter **43**. One caution about this copy: it carries
hand-written annotations that are not NXP text, so nothing below rests on a marginal note.

**The word "ChipIdea" appears nowhere in chapters 42 or 43.** `TODO.md` calls the controller
"ChipIdea/EHCI-style"; the first half is not the manual's vocabulary and should not be cited as
though it were. The manual's own establishing sentences are RM 42.4.1 ("an instantiation of an
EHCI-compatible core", supporting high, full and low speed) and RM 42.5.4.3 ("an Embedded USB
Host Controller as defined by the EHCI specification"), with EHCI deviations enumerated in RM
42.5.4. Device mode is explicitly **not** EHCI: it uses device queue heads and device transfer
descriptors, and RM 42.5.6.4.3 says outright that the device-mode operational model does not use
the host-side structure. So the accurate statement is **EHCI-derived, with a device mode of its
own design** -- which is stronger support for `TODO.md`'s conclusion than the label it used.

Two OTG cores. OTG1 registers at `0x402E0000`, OTG2 at `0x402E0200` (RM 42.7, `+512d * i`), the
non-core `USBNC` control at `0x402E0800`, the whole peripheral window `0x402E0000`-`0x402E3FFF`.
RM 9.9.1 says the boot ROM supports **USB OTG1** for boot and no other port, which is the only
evidence in the manual for which core reaches a connector; the RM never mentions Teensy.
Device mode is high and full speed, no low speed (RM 42.4.1.2), and the on-chip UTMI PHY is
mandatory -- RM 42.2.2.1 warns that selecting any other `PORTSC_PTS` interface type produces
unpredictable behaviour and may hang the system.

### 2.1 The minimum device-mode data structures

| Structure | Size | Alignment | Count | Cite |
|---|---|---|---|---|
| Endpoint queue head (dQH) | 48 B | **64 B** each | one per endpoint per direction; even index RX/OUT, odd index TX/IN; 8 endpoints so **16 heads** | RM 42.5.5.1, 42.5.6.5 |
| dQH **list** base | 16 * 64 = 1024 B | **2048 B** | one, programmed into `ENDPTLISTADDR` (offset `0x158`) | RM 42.5.5 note; the register makes bits 10:0 read-as-zero |
| Device transfer descriptor (dTD) | 32 B block, 7 hardware DWords | **32 B** | as many as queued | RM 42.5.5.2, 42.5.6.6.2 |

The RM contradicts itself once here: RM 42.7.25 also calls the list-base alignment 64 bytes while
the same register discards bits 10:0. The enforceable requirement is **2048**, and that is the
number to design to.

A hard global rule, RM 42.5.5: **no interface data structure reachable by the device controller
may span a 4 KiB page boundary.**

dQH layout: DWord 0 capabilities (`Mult` 31:30, `zlt` 29, max packet length 26:16 up to 1024,
`ios` 15), DWord 1 current-dTD pointer (hardware-owned), DWords 2..8 the **transfer overlay**
(next-dTD pointer with the terminate bit, token with total-bytes / `ioc` / status, and five
buffer-page pointers), DWord 9 free for software link pointers, DWords 10..11 the **8-byte setup
buffer**. Only the RX queue head receives setup data (RM 42.5.5.1.4). Software must not touch
the overlay or a live dTD until the transfer expires.

dTD layout: next-link pointer with bit 0 the terminate bit, a token carrying total-bytes (up to
five 4 KiB pages, 16 KiB recommended), `IOC` bit 15 and an 8-bit status field whose bits are
**Active 7, Halted 6, data-buffer-error 5, transaction-error 3**, then five 4 KiB page pointers
with a 12-bit current offset in the first.

### 2.2 The prime/complete handshake

Every per-endpoint bitmap register uses the same split: **OUT endpoint n is bit `n`, IN endpoint
n is bit `16 + n`.** That holds for `ENDPTPRIME` (`0x1B0`), `ENDPTFLUSH` (`0x1B4`), `ENDPTSTAT`
(`0x1B8`, read-only), `ENDPTCOMPLETE` (`0x1BC`) and `ENDPTNAK` / `ENDPTNAKEN`
(`0x178` / `0x17C`).

Prime: setting the endpoint's `ENDPTPRIME` bit makes the controller load the dTD the queue head
points at; **until an endpoint is primed it NAKs every request from the host** (RM 42.7.35). The
register is write-to-set, so there is no read-modify-write hazard. Completion of the prime is
`ENDPTPRIME` bit == 0 **and** `ENDPTSTAT` bit == 1; prime clear with status not set means the
prime **failed**, and the RM names the only causes -- a malformed dQH or dTD, or a SETUP arriving
mid-prime (RM 42.5.6.4.2.2). Both registers are momentarily touched by hardware during automatic
re-priming, so neither may be read as "my own prime is still pending".

Adding a descriptor to an **already primed** endpoint is not a matter of not writing `ENDPTPRIME`
-- `TODO.md` does not claim otherwise, but a reader might assume it. The sanctioned procedure is
the `USBCMD.ATDTW` (add-dTD tripwire, bit 14) loop of RM 42.5.6.6.3: append, re-read the prime
bit, and if it is clear use ATDTW as a semaphore against the hardware's own hazard window.

Complete: `ENDPTCOMPLETE` is write-1-to-clear, and **several dTDs may retire under one
notification**, so the handler must walk its own list and retire every descriptor whose Active
bit is clear rather than assuming one (RM 42.5.6.6.4).

`ENDPTFLUSH` carries a warning worth carrying into the design: its wait loop "may take a large
amount of time depending on the USB bus activity" and the RM says explicitly it is not desirable
inside an interrupt service routine (RM 42.5.6.6.5). That rules it out of the panic path too.

### 2.3 SETUP, and the tripwire

`ENDPTSETUPSTAT` (`0x1AC`) sets one bit per endpoint that received a setup transaction. With
setup lockout disabled (`USBMODE.SLOM`, bit 3) the prescribed sequence is RM 42.5.6.4.2.1: clear
the `ENDPTSETUPSTAT` bit, set `USBCMD.SUTW` (bit 13), copy the 8 setup bytes out of the queue
head into a local buffer, re-read `SUTW`, and **if hardware cleared it, redo the copy** -- a new
setup arrived mid-read. Then clear `SUTW` and process the local copy. The RM calls the tripwire
the preferred behaviour because ignoring repeated setup packets under long interrupt latency is
a compliance issue.

RM 42.5.6.5.2 gives the same handshake with the copy **before** the acknowledge. Only the
42.5.6.4.2.1 ordering is self-consistent with the retry loop -- you cannot detect a new setup via
`SUTW` unless the status bit was already cleared -- so that is the one to implement, and the
discrepancy is recorded here so it is not rediscovered as a bug.

### 2.4 Verdict

**A different programming model, not a variant of the RP block.** No DPRAM, no per-endpoint
buffer-control word, no software-owned data PID; instead a bus-master controller walking
software-built descriptor lists in system RAM, with a prime/complete bitmap and a mandatory
tripwire for both SETUP extraction and descriptor append. `TODO.md`'s conclusion -- "+1 backend
for `teensy41`, not +1 stack" -- is confirmed. Sections 3.2 and 4.2 give the two places where
this backend costs more than a second copy of the same shape.

---

## 3. What is shared and what is per-backend

Same split `docs/design-m4.6-irq-driver.md` section 1 uses.

| Layer | Item | Status |
|---|---|---|
| **Shared class layer, one implementation** | device / configuration / interface / endpoint descriptor tables, and the string descriptors | M4.6.2, reused by any later USB class |
| | the standard control-request state machine (`GET_DESCRIPTOR`, `SET_ADDRESS`, `SET_CONFIGURATION`, `GET_STATUS`, `CLEAR_FEATURE`, `SET_FEATURE`) | M4.6.2 |
| | the CDC class requests: `SET_LINE_CODING`, `GET_LINE_CODING`, `SET_CONTROL_LINE_STATE` | M4.6.2 |
| | the enumeration state machine: default -> addressed -> configured, and the bus-reset return to default | M4.6.2 |
| | TX and RX policy over the two rings, and the drop-versus-block ruling of sec.4.5 | M4.6.2 |
| **Shared M4.6.1 substrate, reused unchanged** | `user/include/kickos/sys/byte_ring.h` (`kos_byte_ring`) | landed |
| | the two-thread shape and the doorbell (`user/include/kickos/sys/uart_service.h` (`irq_loop`, `serve_loop`)) | landed |
| | `user/include/kickos/sys/driver_service.h` (`spawn_one`, `console_handover_finish`) | landed |
| | the line capability, spawn-time delegation, reclaim on death | landed |
| **Per-backend controller half** | endpoint setup and teardown; the "queue this buffer" and "a buffer completed" primitives | 2 backends |
| | the interrupt-status demux, and the bus-reset / suspend / resume handling | 2 backends |
| | the SETUP-extraction protocol -- trivial on the RP block, a tripwire loop on the RT1062 | 2 backends |
| | the panic-path single-buffer transmit (sec.5) | 2 backends |
| **Privileged bring-up, per chip** | clock tree: `PLL_USB` and `clk_usb` on the RP parts (sec.7.1), `CCM_ANALOG_PLL_USB1` + `USBPHY1` + `CCGR6` on the RT1062 | 2 chips, root/kernel side |
| | taking the block out of reset, and `MAIN_CTRL.PHY_ISO` on the RP2350 | 2 chips |

### 3.1 Where the seam sits, exactly

**The seam is "queue one buffer for transmission, and tell me when a queued buffer came back",
plus the same pair inbound, plus "here are the 8 setup bytes".** The class layer never learns
what a DPRAM buffer-control word or a dTD is; the controller layer never learns what a
descriptor or a line coding is.

That is deliberately the same shape as the `Uart` concept M4.6.1 already ships
(`user/include/kickos/sys/uart_service.h` states the implicit interface in its header comment),
and it is a C++ *concept* satisfied by an anonymous-namespace type per board, not a virtual
interface -- which keeps it inside the template-discipline rule and costs no indirect call.
Proposed shape, so the reviewer is ruling on something concrete:

```c++
// The UsbDevice concept. Owns one device controller and the memory the controller reads.
// EVERY method touches the granted register window, so all of them may be called ONLY from
// the IRQ thread (design-m4.6-irq-driver.md section 7.2 and section 3.3's asymmetric grant).
struct UsbDevice
{
    void     bring_up();                                  // block out of reset, EP0 armed
    void     service_irq();                               // one pass: demux, complete, refill
    bool     setup_pending(unsigned char out[8]);          // the 8 SETUP bytes, protocol-safe
    void     ep_open(uint8_t ep, uint8_t type, uint16_t max_packet);
    uint32_t ep_queue_in(uint8_t ep, unsigned char const* buf, uint32_t len);
    void     ep_stall(uint8_t ep, bool on);
    void     set_address(uint8_t addr);
    bool     configured() const;
};
```

**No new arch seam is needed, and that is a finding rather than an omission.** Every candidate
was checked. The register window reaches the driver through the existing spawn MMIO grant. The
IRQ reaches it through the existing line capability. The panic-path transmit is a body of the
**existing** `arch_console_reclaim` seam plus the existing polled writer, not a new entry point
(sec.5.4). The clock tree is chip init, which is already per-chip code. The one genuine gap is
cache maintenance on the RT1062, and sec.4.3 argues that the right answer is not a seam.

---

## 4. How it rides M4.6.1's substrate

### 4.1 The IRQ lines

| Board | Chip | Lines needed | Number | Trigger | Cite |
|---|---|---|---|---|---|
| `picopi` | RP2040 | **one** | 5 (`USBCTRL_IRQ`) | level: `INTS` is a pure OR of status registers cleared at their source | DS 2.3.2, 4.1.4 |
| `pizero2350` | RP2350 | **one** | 14 (`USBCTRL_IRQ`) | same | DS 3.2, 12.7.5 |
| `teensy41` | i.MX RT1062 | **one** | 113 (`USB_OTG1`; OTG2 is 112) | level: `USBSTS` bits are write-1-to-clear | RM Table 4-2, 42.4.4.1 |

One line per controller on all three, and the RM confirms it in words for the RT1062: each core
uses one dedicated vector. So the shared-line default of `docs/design-m4.6-irq-driver.md`
section 7.7 applies unchanged and no board needs a second IRQ thread.

**All three claim `KOS_IRQ_LEVEL`, not `KOS_IRQ_EDGE`.** On the RP block only `INTR` bit 3 is
cleared by writing the interrupt register; every other bit is cleared at its source in
`SIE_STATUS` or `BUFF_STATUS`, and `INTS` bit 4 stays asserted until **all** `BUFF_STATUS` bits
are clear. That is the level shape RULE L1 and the level rearm exist for: the driver clears at
the peripheral, the kernel masks at the controller. The RT1062 is the same shape with `USBSTS`.

The RT1062 has one extra wrinkle the RM states plainly (RM 42.4.4.2): the USBNC wake-up interrupt
is generated outside the core but **shares the core's vector**. It is only armed if `WIE` is set,
which suspend/resume power management would do and this milestone does not, so the driver's
demux may treat the vector as core-only -- recorded because a later power feature makes it wrong.

**One hazard the RT1062 boot ROM creates**, RM 9.5 note 2: the ROM enables the USB1 interrupt in
serial-downloader mode and disables it again before jumping to a user application, **but it
cannot do so if a debugger interrupted its execution**, in which case the application's startup
code must disable it. On `teensy41` the flash path *is* the ROM's USB path, so this is reachable
on a bench where a debugger is attached. An `irq_claim` of line 113 must therefore not assume the
line starts masked at the NVIC. It does not need to: `irq_claim` already leaves the line masked
and `arch_irq_clear_pending` runs before the first arm
(`docs/design-m4.6-irq-driver.md` section 2.5). The substrate covers this for free; the point of
recording it is that a *chip-init* USB probe outside that path would not be covered.

### 4.2 The two-thread shape, and where enumeration goes

The mapping is clean, and it is the reason the substrate fits without modification:

```
     client --kos_call--> [ service thread ]   parks in kos_recv(ep)
                              | push TX ring (owns head)
                              | pop  RX ring (owns tail)
                              | kos_irq_notify(doorbell)
                              v
                        [ shared block: 2 rings + stats + controller memory ]
                              ^
                              | pop  TX ring (owns tail)  -> queue a bulk IN buffer
                              | push RX ring (owns head)  <- a bulk OUT buffer completed
     host   --IRQ------>  [ IRQ thread ]       parks in kos_irq_wait(line)
                              | owns EVERY register AND the whole control endpoint
```

**Enumeration lives entirely in the IRQ thread**, and it must, for a reason that is structural
rather than stylistic: control transfers touch the register window and (on the RP parts) DPRAM,
and a DEV window has exactly ONE holder, so the service thread cannot reach them
(`docs/design-m4.6-irq-driver.md` section 3.3, the asymmetric-grant correction). The service
thread stays exactly what it is today -- a ring pusher that replies out of ring state and rings a
doorbell. **The whole USB protocol is invisible to it.**

The doorbell means here exactly what it means for the UART: the service thread has accepted bytes
into the TX ring and cannot touch the device, so it posts the binding and the IRQ thread does
both steps. And the reason `user/include/kickos/sys/uart_service.h` (`serve_one`) rings on
**every** accepted push rather than on an observed idle-to-busy transition transfers verbatim:
the lost-wakeup race it avoids is a property of the two-writer split, not of the device. RULE T1
transfers too, in a sharper form -- on both controllers an endpoint that is not primed simply
NAKs, so a driver that waits for an interrupt before queueing its first buffer waits forever with
the host politely retrying.

### 4.3 The shared block, and the M7 coherency problem

On the RP parts the shared block holds what the UART's does: two rings, the stats, the ready
latch. The controller's own memory is DPRAM, inside the granted MMIO window. So the RP backend's
shared block is the UART's block with bigger rings -- **one power-of-two, naturally-aligned
`kos_ram_alloc` block**, per the RAM arm of the grant predicate.

On the RT1062 the controller's memory is **system RAM the controller reads as a bus master**, and
that changes two things.

First, alignment. Every RM rule and the KickOS grant predicate are satisfied simultaneously by
**one 4 KiB naturally-aligned block with the dQH list at offset 0**: 4096-aligned implies
2048-aligned so `ENDPTLISTADDR` accepts it, 32-byte-aligned dTDs inside a 4 KiB block can never
span a 4 KiB page, and a power-of-two naturally-aligned block is what the grant predicate wants
anyway. That is a pleasant coincidence and it should be asserted, not relied on silently.

Second, and this is the real cost: **the L1 D-cache.** `KICKOS_IMXRT_DCACHE` defaults **ON**
(`arch/CMakeLists.txt`), `arch/arm/chip/imxrt1062/chip_imxrt1062.cc` (`arch_init`) enables it,
and its own comment says the coherency obligation arrives with M4-era DMA. **A ChipIdea-class
device controller is that DMA master.** `arch/arm/armv7m/cache.cc` provides only
`kickos_armv7m_icache_enable` and `kickos_armv7m_dcache_enable`, the latter invalidating the
whole cache by set/way once at boot; **there is no clean-or-invalidate-by-address anywhere in
the tree.** Worse, the maintenance registers live in the PPB, so an **unprivileged driver cannot
perform its own cache maintenance at all** -- and a syscall per queued buffer is exactly the
per-toggle-syscall shape `docs/design-driver-era-scope.md` section 3.5 already rejected for GPIO.

Three ways out, and the ranking is not close:

| Option | Verdict |
|---|---|
| Build `teensy41` with `KICKOS_IMXRT_DCACHE=OFF` for the USB console posture | **CHOSEN for stage 1, and the cost is now MEASURED at about 6x on IPC** (`m493dcon` vs `m493dcoff`, teensy41 `bench`: 61,124 ns/round-trip with the D-cache on, 363,005 ns with it off, 32 B payload; the ratio runs 5.3x at 8 B to 6.3x at 256 B). The mechanism is XIP: the I-cache still covers instruction fetch, but literal pools and `.rodata` are DATA reads, and with the D-cache off they go to external QSPI flash, which the boot config block drives single-pad at 30 MHz. With the cache on the figure is flat across payloads; with it off it climbs, so memory is the bottleneck. This makes S7 a priority rather than a nicety |
| Mark the descriptor block non-cacheable through the MPU | **CHOSEN as the stage-3 answer.** `arch/arm/chip/imxrt1062/chip_imxrt1062.cc` already runs a fixed-region pass (`kickos_arm_mpu_fixed_init`) that marks unbacked external bands as Device for the ERR011573 anti-speculation fix, so the mechanism exists. What it needs is for a *dynamically allocated* grant to carry a non-cacheable attribute, which the grant encoder cannot express today. That is a real, bounded piece of work and it must not be smuggled into stage 1 |
| A new `arch` cache-maintenance seam plus a syscall | **REJECTED.** Per-transfer privileged calls on the hot path, a new seam on every arch to serve one chip, and the rejected GPIO shape |

**CORRECTION to `TODO.md`:** the section costs `teensy41` at "+1 backend". The backend is indeed
one backend, but the coherency obligation is a *second* item and it is not USB-specific -- it is
the first time anything in KickOS lets a bus master read memory the core has cached. Whatever
M4.6.2 decides here is the precedent DMA inherits.

### 4.4 The enumeration deadline -- and why it is not the reason for the sequencing

**No USB specification document is in the local reference set** -- it holds no USB 2.0 spec and
no USB-CDC class document. The spec numbers below are therefore stated
from the USB 2.0 specification as known and are **not verified against a local copy**, which is
a weaker footing than every other number in this document and is flagged as such. Getting the
USB 2.0 spec and the CDC/PSTN class documents into the reference set is a prerequisite for the
class layer, and it is listed in sec.9 as stage 0.

The deadlines that bound a device, USB 2.0 chapter 9:

| Deadline | Value |
|---|---|
| Request with no data stage: status stage complete | 50 ms |
| Request with a data stage: first data packet | 500 ms |
| Subsequent data packets | 500 ms each |
| Status stage after the data stage | 50 ms |
| `SET_ADDRESS`: the new address must be live | 2 ms after the status stage completes |
| Reset / resume recovery before the host accesses the device | 10 ms |
| Whole standard request | 5 s |

The tightest is **2 ms**, for `SET_ADDRESS`. Everything else is 50 ms or more.

**Now the half that IS locally verified, and it is the half that carries the argument.** On both
controllers the hardware absorbs all the sub-millisecond timing:

- The RP block **always accepts a SETUP packet in hardware and ACKs it automatically**, into a
  dedicated 8-byte DPRAM region, then raises `SIE_STATUS` bit 17 (DS 4.1.2.8.1 / 12.7.3.8.1).
  Software is not in the handshake loop. Every other token gets a NAK until software makes a
  buffer available (DS 4.1.2.8.2).
- The RT1062 NAKs every request on an endpoint that is not primed (RM 42.7.35), and its control
  endpoint answers a SETUP token with an ACK in the not-primed state as well (RM Table 42-71).
- Neither part offers, or needs, any hardware deadline assistance -- and neither imposes one.
  There is no control-transfer timer in either block.

So the deadline that actually reaches software is **milliseconds**, against a scheduler whose IRQ
thread runs strictly above its service thread and above every client. A wake, a status read and a
buffer queue is microseconds of work. The two-thread shape meets this **by roughly three orders
of magnitude**, and the RT1062's 2 ms `SET_ADDRESS` window even has hardware help --
`DEVICEADDR.USBADRA` stages the address write so it lands after the endpoint-0 IN is ACKed
(RM 42.7.23), removing the only case where software timing could plausibly matter.

**CORRECTION to `TODO.md`, and it is the most important one here.** The section says the
dependency on M4.6.1 "is not a preference: enumeration is interrupt-driven with a deadline on
answering `SETUP`". The deadline is real but it is not tight, and it is not what makes M4.6.1 a
prerequisite. The dependency is nonetheless correct, for three reasons that are:

1. **Reclaim on driver death.** A console driver that dies must leave a console behind. Before
   M4.6.1 nothing released a line on death and nothing flipped the console's ownership state, so
   a dead USB driver left the line armed, its binding slot burned and the board silent. That is
   hole 2 of `docs/design-m4.6-irq-driver.md` section 0 and it is closed.
2. **The handover ordering.** `user/include/kickos/sys/driver_service.h`
   (`console_handover_finish`) exists so a bring-up failure is *reportable*. USB enumeration is
   the longest and most failure-prone bring-up in the fleet, so it is the case that most needs a
   failure path that is not a dark board.
3. **Two threads over one shared ring block with one line delegated at two different rights.**
   That is the whole of M4.6.1's second half, and USB needs it verbatim.

The corrected sentence is therefore: **M4.6.2 follows M4.6.1 because a USB console fails in more
ways than a UART does, and M4.6.1 is what makes a failure reportable.** State it that way and the
sequencing survives the deadline claim being wrong.

### 4.5 The handover probe proves the driver, not the link -- and what follows

This is the sharpest structural difference between a USB console and every landed one, and
nothing in `TODO.md` anticipates it.

`console_handover_finish` closes the caller's own WAIT cap and then proves the driver is serving
with a zero-length rendezvous on capability 0. On a UART that is very nearly proof the console
works: a configured UART transmits whether or not anything is listening. **On USB it proves
strictly less.** The driver can be parked in `kos_recv`, fully correct, while the device is
un-enumerated because no host is attached -- and it will stay that way indefinitely. Bytes enter
the TX ring and no IN token ever comes to take them out.

`docs/reference/console.md` ("The publisher's obligations") describes the existing dark window as
one the rendezvous absorbs. **On USB the dark window is unbounded and host-controlled.** Three
consequences, and they are policy decisions the driver must make rather than discover:

1. **The console must never block on the link.** A full TX ring returns a short accept, which is
   already what `serve_one` does, and the client retries or gives up. What must be added is that
   a client which gives up **counts a drop and moves on**. If the first `printf` of `main` could
   block until a host attaches, an un-cabled board would hang at boot -- strictly worse than
   today, where it merely needs an adapter.
2. **The probe must not be strengthened into a link check.** A bring-up that waited for
   enumeration would make boot depend on a cable. Recorded as a rejected alternative because it
   is the obvious "fix".
3. **`ready` in the shared block gains a second meaning to keep separate.** The UART's `ready`
   latch means the IRQ thread has clocked the device
   (`user/include/kickos/sys/uart_service.h` (`Shared`)). USB needs a *distinct*, non-latching
   `configured` flag, because it goes back to false on every bus reset and on unplug. Conflating
   them would make a re-enumeration look like a driver restart.

**Decision: the USB console is a drop-on-full sink from boot's point of view, and enumeration
state is observable but never blocking.** The tail loss this produces is the same accounting the
UART already has, and it is counted in the same `kos_uart_stats`-shaped counters.

---

## 5. The panic path

### 5.1 The claim, verified

`TODO.md` sketches it: write into the bulk IN endpoint's buffer, mark it available with a length,
poll until the controller returns it, and **no device interrupt is needed because the host issues
the IN tokens**. **That claim is CORRECT on both families**, and both datasheets say so
explicitly rather than by inference:

- RP block: software sets length, FULL and PID in the buffer-control word, then `AVAILABLE`; the
  controller sends on the next IN token, clears FULL and sets the endpoint's `BUFF_STATUS` bit,
  which is write-1-to-clear (DS 4.1.2.7.4, 4.1.2.8.2). Polling either FULL or `BUFF_STATUS`
  works with `INTE` all zero.
- RT1062: RM 42.7.20 states outright that `USBSTS` shows interrupt sources **even when disabled
  in `USBINTR`, allowing polling of interrupt events by software**, and RM 42.5.6.6.4 offers the
  poll alternative in as many words -- "alternately, the DCD can poll the endpoint complete
  register". Success is the dTD's **Active clear, Halted clear, transaction-error clear,
  data-buffer-error clear**, with total-bytes reaching zero for a transmit (RM 42.5.6.4.1).

### 5.2 The three constraints `TODO.md` names, each discharged

**The spin must be bounded.** Correct, and required by both manuals' silence rather than their
text: the RP datasheet documents no timeout on an unclaimed buffer, and the RT1062 RM specifies
no bulk IN timeout at all (its only related statement is that an unserviced ISO dTD "will stay
primed indefinitely"). With no host there are no IN tokens and the buffer never returns.
`KICKOS_POLL_SPIN_MAX` is the precedent and it is the right one -- a bring-up wait must be
bounded, and `user/include/kickos/sys/driver_service.h` bounds both of its own that way
(`KOS_DRV_READY_WAIT_MAX` on the readiness latch, `KOS_DRV_HANDOVER_PROBE_US` on the probe).

**A fault before the host finishes configuring has no console.** Correct, and sharper than
`TODO.md` puts it: the window is not bring-up-shaped, it is *host*-shaped, so on an un-cabled
board it is the entire life of the system. This is the strongest argument in the document for
keeping a pin UART or RTT as the early path, and sec.6 turns it into a decision.

**A suspended device needs resume signalling.** Correct, and it is one bit on the RP parts:
`SIE_STATUS` bit 4 reports SUSPENDED and `SIE_CTRL` bit 12 (`RESUME`, self-clearing) drives
remote wakeup, with the hardware handling the 1-to-15 ms drive. On the RT1062 it is the reverse
of RM 42.4.3.1's suspend sequence -- clear `USBPHYx_CTRL.CLKGATE`, clear the `USBPHYx_PWD` bits,
clear `PORTSC1.PHCD` -- which is more work but still register writes.

**The constraint `TODO.md` misses, and it is the one that will bite:** remote wakeup only works
if the host **enabled** it with `SET_FEATURE(DEVICE_REMOTE_WAKEUP)`. A host that did not is
entitled not to be woken, so a fault on a suspended device with wakeup disabled has no console.
Narrow, honest, and it must be written down rather than debugged.

### 5.3 The two constraints neither document names

**PID sequencing.** On the RP block the data PID is **software-owned** (buffer-control bit 13);
the controller does not toggle it. The driver's next-PID state lives in the driver's own RAM,
which the panic path cannot read and must not trust. The panic writer must therefore read the
buffer-control word back, take the PID bit it finds and invert it. That is an inference from the
register definition, not a quoted sentence, so it is on the silicon list. Consequence to accept
up front: **at the sequence boundary the panic path may lose or duplicate exactly one packet** --
the USB analogue of the roughly 8 bytes `xmc4800-relax` reproducibly clips out of `TBUF0`, and
the same kind of bounded, characterised tail loss.

**The panic path must not abort, and on the RP2040 must not even try.** RP2040-E2 says the device
NAKs forever on **all** endpoints once `EP_ABORT` is set, and the workaround is to never use it.
On the RT1062, `ENDPTFLUSH`'s wait loop is explicitly unsuitable for an ISR (RM 42.5.6.6.5). So
on both parts the panic writer **overwrites** the driver's buffer rather than reclaiming it. That
is acceptable on a terminal path and it is the reason the RP2040 erratum, which looks like a
driver concern, is really a panic-path constraint.

### 5.4 The layout must become an ABI between the chip backend and the driver

This is the part that needs designing rather than discovering, and `TODO.md` does not reach it.

`docs/design-m4.6-irq-driver.md` section 4.3 rules that the kernel does not know the driver's
ring layout, deliberately. **The panic path inverts that requirement**: to write into "the bulk
IN endpoint's buffer" the kernel must know which endpoint number and, on the RP parts, which
DPRAM offset. The driver chose both.

**Decision: the CDC console's bulk IN endpoint number and its buffer placement are per-chip
constants that the driver is required to use, not values the driver picks.** The chip backend
owns them; the driver reads them. Proposed:

```c++
// arch/arm/chip/rp2040 and rp2350, shared constants header. The panic writer and the
// driver MUST agree, so neither may choose: a panic cannot ask a dead driver where it put
// its buffer. EP2 IN, buffer at DPRAM 0x180, the first free slot above EP0's two.
enum { KICKOS_RP_USB_CONSOLE_EP_IN = 2 };
enum { KICKOS_RP_USB_CONSOLE_BUF_OFF = 0x180 };
enum { KICKOS_RP_USB_CONSOLE_MAX_PACKET = 32 }; // under RP2040-E15's 50-byte threshold
```

On the RT1062 the same problem has a cleaner answer, because the descriptors are in RAM: the
panic path carries **its own** static dTD and a small static buffer in kernel `.bss`, links it
into the console endpoint's queue head and primes. It clobbers the queue head's overlay, which is
fine on a terminal path, and it needs to know only the endpoint number. It also gets cache
maintenance for free that the driver cannot have: the panic path runs privileged, so it may clean
its own descriptor and buffer before priming -- **which is the one place the D-cache problem of
sec.4.3 solves itself.**

Rejected alternative: **a new arch seam, `arch_console_panic_write`.** Rejected because
`arch_console_reclaim` plus the existing polled writer already IS this seam. The reclaim body's
contract is "drive every writable register in the window to a known ready value, with idempotent
absolute stores" (`arch/include/kickos/arch/arch.h`; `docs/reference/porting.md`), and for USB
"ready" means "EP0 and the console IN endpoint re-armed from kernel-owned state". The polled
writer then queues and spins. Adding a seam would duplicate a contract that already fits.

### 5.5 The three reclaim bodies M4.6.2 owes

**None of the three boards has an `arch_console_reclaim` body today.** Only
`arch/arm/chip/xmc4800/usic_uart.cc` and `arch/arm/chip/mk64f/chip_mk64f.cc` define one; every
other chip links `arch/common/arch_console_reclaim_default.cc`, a no-op.
`docs/reference/porting.md` makes a real body a **precondition** for a board turning on console
handover, not a follow-up. So M4.6.2 owes three, and sec.6.2 makes them cheaper than they look.

---

## 6. The kernel console is a different device -- the structural surprise

### 6.1 What publishing actually does

On every landed handover the userspace driver takes the **same** UART the kernel was using.
`kernel/syscall/syscall.cc` (`case KOS_SYS_CONSOLE_PUBLISH`) is written for that: it calls
`console_tx_deinit()` whenever the kernel still owns the console, then flips
`kernel/init/console.cc` (`enum class ConsoleState`) to `USER_OWNED`, after which
`console_emit`'s chip path **drops**.

On these three boards the USB console is a **different peripheral** from the kernel console
(`picopi` UART0 on GP0, `pizero2350` UART1 on GP4/GP5, `teensy41` LPUART6 -- all recorded in
`docs/reference/boards.md`). Publishing a USB console therefore blinds a pin UART the driver
never touches, and does so on the exact boards where that UART is the only thing that works
before enumeration. Combined with sec.5.2, an un-cabled `picopi` that published a USB console
would boot into total silence.

### 6.2 Decision: driver death returns these boards to `KERNEL_OWNED`, not `RECLAIMED`

`docs/design-m4.6-irq-driver.md` section 4.4 considered returning to `KERNEL_OWNED` and
**rejected** it, for a good reason: a dead driver may have left the UART in an arbitrary state,
and re-arming the kernel ring would need a per-chip "restore the ring's assumptions" step that
does not exist.

**That reason does not apply when the devices are disjoint.** The dead USB driver cannot have
touched the pin UART -- it never held a window covering it, and the domain model enforces that
rather than trusting it. The kernel's ring is still armed, its line still attached, its
assumptions still true. So the correct post-death state for a disjoint-device console is
`KERNEL_OWNED`, and the section-4.4 ruling stands unchanged for the same-device case it was
written about.

This is worth the delta because it pays for itself three times:

- **A dead USB console falls back to the pin UART**, which is the best available outcome and
  strictly better than `RECLAIMED` on a device the panic path may not be able to use.
- **The three `arch_console_reclaim` bodies of sec.5.5 get smaller.** Their job is no longer "put
  a UART back" -- the UART never left. It is only "re-arm the USB console endpoint from
  kernel-owned state so the panic banner can be queued", and on a board with no host attached the
  body may legitimately do nothing at all and let the pin-UART path carry the banner.
- **It removes the un-cabled-silence failure mode entirely.**

The kernel delta is small and it is a *publish-time* property, not a new state: the publisher
must be able to say "the device I am handing over is not the kernel console device", so that
`console_tx_deinit` is skipped and the death hook restores `KERNEL_OWNED`. Whether that rides a
flag on the publish, a field in `kos_service_cfg` (`system/include/kickos/sys/service.h`), or a
second service kind is **open question 1** -- it is a real ABI choice and the reviewer should
make it rather than inherit it.

**CORRECTION to `TODO.md`:** the bullet "It is a service, not a port" says the handover machinery
is transport-agnostic and "nothing in `system/init/` should need to learn about USB". The second
half is right -- nothing in `system/init/` learns about USB. The first half is not quite: the
machinery is transport-agnostic but it is **not device-disjointness-agnostic**, and that is the
one genuinely new thing a USB console asks of the kernel. It asks for nothing USB-specific.

### 6.3 What does NOT need to change

`KOS_SVC_CONSOLE` covers this: a console publishes as a console and no new service kind is
needed, exactly as `TODO.md` says. The two-thread bring-up in
`system/init/sim/service_list_uart.cc`
(`sim_uart_start`) is the structural template -- allocate the block, self-grant it, create the
endpoint, claim the line, spawn the IRQ thread with `{window, block, line(WAIT)}`, spawn the
service thread with `{block, endpoint(WAIT), line(SIGNAL)}`, close root's line cap -- and a USB
console differs from it only in taking the MMIO window and ending with
`console_handover_finish`.

### 6.4 The MMIO window, per board

One window per driver, because a spawn grants one and a DEV window has one holder.

| Board | Window | Encodable because |
|---|---|---|
| `picopi` | `0x50100000` + **128 KiB**, covering DPRAM and the register block in one power-of-two region | `0x50100000` is 128 KiB-aligned, and the whole span is the USB block's own AHB slot -- the next peripheral is far above it |
| `pizero2350` | `0x50100000` .. the end of the register block, byte-granular | PMSAv8 is base-plus-limit at a 32-byte granule (`arch/arm/common/arch_arm_pmsav8.cc`), so no power-of-two run-up is paid |
| `teensy41` | `0x402E0000` + **512 B**, the OTG1 core registers alone | 512 is a power of two, `0x402E0000` is 512-aligned, and it deliberately excludes OTG2 (`0x402E0200`) and USBNC (`0x402E0800`) |

The `teensy41` row is the isolation principle applied: the PHY (`0x400D9000`), the PLL
(`0x400D8010`) and the clock gate (`0x400FC080`) stay **privileged bring-up**, which is where
clocks belong anyway, and the driver gets the narrowest unit that is its own. The RP rows cannot
be narrowed the same way because DPRAM is not optional -- it is where the endpoint state lives.

---

## 7. Reboot, and the idle path

### 7.1 Reboot-to-bootloader takes the USB device away on all three

Confirmed, and the evidence is uneven in a way worth stating.

- **`pizero2350` is silicon-witnessed.** `docs/reference/boards.md` ("Terminal dead-ends and
  BOOTSEL handover") records the board re-enumerating as `2e8a:000f Raspberry Pi RP2350 Boot` on
  a fresh USB device number, four ordered exits and three fault exits off one BOOTSEL press. The
  RP2350 datasheet confirms the descriptor: idVendor `0x2e8a`, idProduct `0x000f`, both
  OTP-overridable (DS 5.6.1).
- **`picopi` is not witnessed.** `arch/arm/chip/rp2040/chip_rp2040.cc` (`arch_reboot`) calls the
  bootrom's `_reset_to_usb_boot`, which by construction re-enumerates as the bootrom's own
  device, but `docs/reference/boards.md` names `pizero2350` as the *first* witness for
  `kos_reboot` on any chip and no RP2040 run is recorded.
- **`teensy41` is neither witnessed nor vendor-documented.**
  `arch/arm/chip/imxrt1062/chip_imxrt1062.cc` (`arch_reboot`) issues `bkpt #251` and its own
  comment says BENCH-UNWITNESSED and not vendor-documented, evidenced only from PJRC's
  `_reboot_Teensyduino_` and a third-party port. `TODO.md` states this as settled fact
  ("`imxrt1062`'s `bkpt #251` has the MKL02 present HalfKay"); the code is more honest and the
  code is right. **CORRECTION.**

One more thing `TODO.md` does not say: **`arch_reboot` is compiled only under
`KICKOS_ENABLE_SELFTEST` on all three chips**, and `KICKOS_SHUTDOWN_TO_BOOTLOADER` requires that
option (`CMakeLists.txt`). So "reboot takes the console away" is a property of selftest-class
images, not of every image, which slightly narrows the interaction.

The flip side on `teensy41` is real and `TODO.md` states it correctly: the RM confirms the boot
ROM's serial downloader uses **USB OTG1 and no other port** (RM 9.9.1), so a USB console and the
flashing path share one connector by design. Two practical consequences: the host's CDC node
disappears at the handover, so a capture script must expect the device to vanish rather than go
quiet; and the ROM's own USB use is why the interrupt hazard of sec.4.1 exists at all.

**A CDC console does not restore `picotool` recovery, and this bounds the motivation.**
`docs/reference/boards.md` records the `pizero2350` bench cost that `TODO.md` cites as the
concrete case: the board left the USB bus mid-session because KickOS has no USB device stack, and
recovery took a physical BOOTSEL press. A CDC console keeps the port enumerated, which is worth
having -- but `picotool` speaks PICOBOOT, not CDC, so **a wedged image still needs a physical
press.** Host-triggerable recovery needs a vendor reset interface alongside CDC, plus a route
from it to `arch_reboot` (which is selftest-only). That is a genuinely attractive follow-up and
it is out of scope here, in sec.11.

### 7.2 The idle path -- answered, and the answer is better than feared

`TODO.md` asks whether the tickless idle path keeps the USB controller clocked, "since the answer
could constrain `arch_idle_wait` there". **Answered from the datasheets: it does, and
`arch_idle_wait` needs no constraint at all today.**

Both RP parts enter a chip-level SLEEP automatically when both processors are in WFI or WFE and
the DMA is idle (RP2040 DS 2.11.2; RP2350 DS 6.5.1). In SLEEP the top-level clock gates are
masked by `SLEEP_ENx` instead of `WAKE_ENx`, and **every bit in both sets resets to 1**:

| Chip | Register | USB bits | Reset |
|---|---|---|---|
| RP2040 | `SLEEP_EN1` at `CLOCKS` + `0xac` | bit 11 `CLK_USB_USBCTRL`, bit 10 `CLK_SYS_USBCTRL` | 1, 1 |
| RP2350 | `SLEEP_EN1` at `CLOCKS` + `0xb8` | bit 27 `CLK_USB`, bit 26 `CLK_SYS_USBCTRL` | 1, 1 |

Different bit positions, same conclusion. The RP2040 datasheet is explicit that by default all
clocks are enabled and the programmer only needs these registers for a low-power design, and adds
that the PLLs are unaffected by SLEEP. Neither `arch/arm/chip/rp2040/chip_rp2040.cc` nor
`arch/arm/chip/rp2350/chip_rp2350.cc` writes `WAKE_EN` or `SLEEP_EN` at all. So the ARM default
`arch_idle_wait` (`arch/arm/common/arch_idle_wait_default.cc`, a bare `wfi`) is **already safe
for USB on both parts**, and the constraint lands somewhere else entirely:

> **RULE U1.** Any future low-power work that clears `SLEEP_EN1`'s USB bits, or that enters
> DORMANT, kills a USB console. DORMANT stops all clocks and oscillators, its only documented
> wake sources are a GPIO event and the timer, and the RP2040 datasheet warns that PLLs left
> running into DORMANT generate out-of-control clocks. The prohibition belongs to the power
> feature, not to `arch_idle_wait`.

On the RT1062 the answer is a genuine "it depends, and you control it". RM 14.7.27 gives
`CCM_CCGR6` field `CG0` for the `usboh3` clocks, with encoding `01` = on in RUN but **off in WAIT
and STOP** and `11` = on in all modes except STOP. A WFI only starts a low-power sequence if
`CLPCR[LPM]` selects one. **Decision: set `CCGR6[CG0] = 11` in the `teensy41` USB bring-up**, so
the block survives WAIT regardless of what a later power feature does to `LPM`. Cheap, explicit,
and it makes the poll-only panic path of sec.5 sound rather than lucky.

The RP2040 debug-bus symptom that `TODO.md` alludes to -- the board sleeping when both cores idle
and the SWD DAP then failing to power up -- is a **different gate**. That is the debug clock, not
`clk_usb`, and the two do not interact. (The build knob named for it in the local operational
notes does not exist anywhere in this tree; see the report accompanying this document.)

### 7.3 One clock finding that is not about idle at all

**Neither RP chip configures `PLL_USB` or `clk_usb` today.** `clocks_init()` in both
`arch/arm/chip/rp2040/chip_rp2040.cc` and `arch/arm/chip/rp2350/chip_rp2350.cc` brings up only
`PLL_SYS`. So the RP backend is a **clock-tree addition** as well as a peripheral driver:
`PLL_USB` up, `CLK_USB_CTRL` onto it at exactly 48 MHz, and note that `clk_usb` has no glitchless
mux (RP2040 DS 2.15.3.2), so it must be disabled while its source is switched.

And the existing **degraded path must refuse USB.** Both chips already fall back to
`ROSC_NOMINAL_HZ` (6.5 MHz, `arch/arm/chip/rp2040/regs/xosc.h`,
`arch/arm/chip/rp2350/regs/clocks.h`) when the crystal does not come up. A full-speed device
cannot be sourced from the ring oscillator at all, and 6.5 MHz also violates the
`clk_sys` > 1.1 * `clk_usb` workaround for RP2040-E16 / RP2350-E12. So:

> **RULE U2.** USB bring-up is refused, loudly and at bring-up time, when the crystal did not
> come up. The condition is already computed and already recorded in `SystemCoreClock`; what is
> missing is a service that reads it and declines. A silently non-enumerating device on a board
> whose crystal failed is the worst of both outcomes.

This is a case the existing code already reaches, which is what makes the rule worth writing
rather than hypothetical.

---

## 8. Validation -- and the headline is that almost nothing is gatable in-env

**Stated first because the tables below must not read as coverage: there is no USB device model
in any emulator this project uses, so essentially nothing about a USB console can be gated in CI
or on the host.** M4.6.1's second half could at least put a real two-thread driver over a
loopback device on the sim (`system/init/sim/service_list_uart.cc`, gated by
`tests/integration/check_sim_uartloop.sh`) because its "device" was host `fd` 1 and the doorbell was the only
thing that could move a byte. **USB has no such trick available.** A USB device is defined by
what a *host* does to it -- tokens, resets, a nine-stage enumeration -- and neither the sim nor
QEMU on any target in this fleet presents one. Writing a fake device controller whose "host" is a
loop in the same image would gate the fake, not the design; that is the shape M4.5.8 was cleaned
up for and it must not come back here.

So the honest coverage claim is: **one small host-testable layer, one bench-testable layer that
needs a real host on the other end, and everything that matters on silicon.**

### 8.1 What CAN be gated with no board

| Case | What it asserts | Why it is real coverage |
|---|---|---|
| `usb_descriptors` | the built descriptor tables parse: total lengths self-consistent, every `bLength` correct, the configuration's `wTotalLength` equal to the sum of what follows it, endpoint addresses matching the interface's, `bNumEndpoints` matching the count actually emitted | descriptor construction is pure data with no device in it, and a wrong `wTotalLength` is a classic enumeration failure that costs a bench session to find |
| `usb_setup_decode` | the control-request decoder maps a table of byte-exact `SETUP` packets (including the CDC ones) to the right handler, and refuses malformed ones | pure function of 8 bytes; this is where "test the grammar, not the socket" applies |
| `usb_enum_sm` | the enumeration state machine, driven by a scripted event list, reaches configured and returns to default on a bus reset | the state machine is the class layer's only stateful host-independent part |

Three cases, all of them in the class layer, none of them touching a controller. That is the
whole of the hardware-free gate and it should be described as such rather than counted.

### 8.2 What needs a real host on the other end

Everything else about enumeration, and it is not a CI activity: attach and enumerate, the host's
CDC node appearing, `SET_LINE_CODING` and `SET_CONTROL_LINE_STATE` round trips, bulk IN carrying
console output, bulk OUT carrying input, unplug and replug re-enumerating, and a driver killed
mid-session giving the console back to the pin UART (sec.6.2). A Linux host with the pin UART
*also* wired is the rig, because the pin UART is the only channel that can report a USB failure.

### 8.3 What only silicon settles

- The PID read-and-invert inference of sec.5.3 -- whether a panic write after a completed IN
  lands as a fresh packet or is dropped as a retransmission.
- Whether the panic path's bounded spin actually completes against a real host, and how many
  bytes the tail loses.
- RP2040-E5: whether the bench Pico's stepping needs the 800 us J-state workaround.
- RP2040-E15 against a Raspberry Pi 4 host, if that host is ever used. The 32-byte packet
  decision of sec.1.3 is intended to make this unreachable, and that intent is itself a silicon
  claim.
- Every clock assertion of sec.7.2 and 7.3, including RULE U2's refusal path, which needs a
  deliberately broken crystal to witness and probably never will be.
- The `teensy41` D-cache posture: whether `KICKOS_IMXRT_DCACHE=OFF` is the only thing standing
  between a working and a corrupt descriptor list.

### 8.4 What no gate in this plan covers, stated so the tables are not read as complete

- **The panic flush on a USB console.** The kernel-ring case is already witnessed on
  `pizero2350` and provably dead on the host (`STATE.md`, *Open blockers*); this inherits the
  same in-env hole with a worse device.
- **The un-cabled boot path.** Nothing can assert "the board still boots and still prints with no
  USB host attached" except attaching nothing, which no gate does by construction.
- **`teensy41` has no CI gate of any kind** and `picopi` is build-only
  (`docs/reference/boards.md`), so two of the three boards start from no automated coverage at
  all.

---

## 9. Staging

Each stage builds and passes the whole suite on its own. Two stages must not land alone and their
rows say so.

| Stage | Content | Note |
|---|---|---|
| **S0** | **Get the USB 2.0 specification and the CDC/PSTN class documents into the local reference set.** No code | **Blocks S2.** Every number in sec.4.4 is currently unverifiable, and a descriptor table built from memory is the shape section 8.1 of `docs/design-m4.6-irq-driver.md` calls a blocker rather than a bug |
| **S1** | the RP clock-tree half: `PLL_USB` and `clk_usb` at 48 MHz on both RP chips, the block out of reset, `MAIN_CTRL.PHY_ISO` cleared on the RP2350, `CCGR6[CG0] = 11` plus PHY and PLL bring-up on the RT1062, and RULE U2's refusal. Buildable and separately meaningful: a probe app can read back the lock and the selected source with no USB traffic | lands alone safely |
| **S2** | the shared class layer with no controller behind it: descriptors, the control-request decoder, the enumeration state machine, the CDC requests. Gated by the three cases of sec.8.1 | **Must not land alone** in the sense that matters: it is untestable against reality until S3, so its gate must be honest about being a data gate. Depends on S0 |
| **S3** | the RP controller backend plus the two-thread bring-up on `pizero2350`. First board because it is the only one of the three with a witnessed reboot path, a witnessed unprivileged root and PMSAv8 base-plus-limit windows, so its window grant needs no power-of-two reasoning | the first stage that can enumerate |
| **S4** | the console-disjointness delta of sec.6.2 and the three `arch_console_reclaim` bodies, plus the panic-path constants of sec.5.4 | **Must not land after S3 in a separate release.** S3 without it publishes a console that blinds the pin UART and has no reclaim body -- the exact combination `docs/reference/porting.md` forbids. If they cannot be one commit they must be one merge |
| **S5** | `picopi`: the same backend behind the per-chip delta list, plus the 128 KiB window and the RP2040-E5 stepping question | armv6m, so it is also the fleet's cheapest check that the window encodes on PMSAv6 |
| **S6** | `teensy41`: the RT1062 backend, the 4 KiB descriptor block, `KICKOS_IMXRT_DCACHE=OFF` as the stage posture, the setup tripwire and the ATDTW append loop | the largest single piece of new register work in the milestone |
| **S7** | the D-cache follow-up: a non-cacheable attribute on the block's ALLOCATION, so `teensy41` gets its D-cache back. **Built; UNWITNESSED on silicon** | separable, and the precedent DMA inherits |

`pizero2350` before `picopi` inverts the obvious ordering, deliberately: the RP2350 has zero USB
errata of its own beyond the clock-ratio one that its 150 MHz already satisfies, while the RP2040
carries four including one that may need a silicon-stepping investigation. Bring the block up
where nothing is in the way, then port it to the part with the errata.

---

## 10. Open questions the reviewer must rule on

1. **How does the publisher say "this is not the kernel console device"?** Sec.6.2 needs it and
   there are three shapes: a flag argument on the publish syscall, a field in
   `kos_service_cfg` (`system/include/kickos/sys/service.h`) that the bring-up reads, or a
   distinct service kind. The recommendation is the `kos_service_cfg` field, because the
   knowledge is per-board configuration data and that struct exists precisely to carry
   per-instance config as data rather than as literals in a driver TU -- but it changes a
   size-asserted layout, so it is the reviewer's call.
2. **Is a USB console allowed to be a board's ONLY console?** Sec.5.2 and sec.6.2 assume not, and
   every decision in this document leans on the pin UART still working. If a board is ever
   expected to ship with no pin console at all, the un-cabled-silence case stops being
   acceptable and the early-boot path needs a different answer. Answering "no, never" now is
   cheap; discovering it later is not.
3. **Which RP2040 stepping is on the bench?** RP2040-E5 is hardware-fixed in B2 and needs an
   awkward internal-debug workaround on B0/B1. This is a five-minute `picotool` question that
   changes whether S5 carries a workaround at all, and it should be answered before S5 is
   planned rather than during it.
4. **Does the console driver get an interrupt endpoint, or only the two bulk endpoints?**
   `TODO.md` specifies "two bulk endpoints plus one interrupt endpoint", which is the canonical
   CDC-ACM shape -- the interrupt IN endpoint carries `SERIAL_STATE` notifications. Nothing in a
   console needs them, and every endpoint costs a DPRAM buffer and a descriptor. Hosts are
   generally tolerant of a CDC-ACM function that never sends a notification, but they are
   **not** reliably tolerant of a descriptor set that omits the endpoint. Recommendation: declare
   it, allocate its buffer, never queue anything on it. That keeps the descriptor set canonical
   at the cost of 32 bytes of DPRAM, and it is worth an explicit yes rather than silence.
5. **Does the RT1062 use OTG1 exclusively, and is that written down anywhere checkable?** RM
   9.9.1 says the boot ROM supports OTG1 only, which is strong circumstantial evidence for which
   core reaches the connector, but the RM is chip-level and never mentions the board. If the
   answer rests on PJRC schematics rather than on the RM, that provenance belongs in
   `docs/reference/boards.md` and not in a design record.
6. **Is `KICKOS_IMXRT_DCACHE=OFF` acceptable as a shipped posture for a board with a USB
   console, or is S7 a hard prerequisite for S6 landing?** Sec.4.3 stages it as a follow-up; a
   reviewer who considers a D-cache-off M7 unacceptable is choosing to make S6 much larger, and
   should say so before S6 starts.

---

## 11. Deliberately out of scope

Each is deferred by an argument above, not by omission.

- **USB host mode.** Both RP parts and both RT1062 cores support it (sec.1.1, sec.2). Nothing in
  M4.6 needs it and it is a larger subsystem than the device side.
- **High-speed device mode on the RT1062.** The part supports it (sec.2); a console does not
  benefit, and full speed keeps the two backends' packet sizing identical. Revisit with a class
  that moves data.
- **Suspend and resume as power management.** Sec.7.2: the panic path's resume signalling is in
  scope because a fault must be reportable; entering suspend to save power is not, and RULE U1
  says why -- it would need the USBNC wake-up path of sec.4.1 and it would put a clock decision
  in front of a console.
- **A vendor reset interface for host-triggered BOOTSEL recovery.** Sec.7.1: attractive, because
  it is the only thing that actually fixes the bench cost `TODO.md` cites, but it needs a route
  from a USB request to `arch_reboot`, which is compiled only under `KICKOS_ENABLE_SELFTEST`.
- **A blocking read on the USB console.** Sec.4.5 and the same missing primitive
  `docs/design-m4.6-irq-driver.md` section 7.5 names -- receive-from-either-of-two-sources, an M6
  kernel object. Unchanged by USB and must not be smuggled in here either.
- **Any USB class other than CDC-ACM.** The seam of sec.3.1 is drawn so a later class reuses the
  controller backends, and that is the whole of the provision made for it.
- **The `EP_ABORT` and `ENDPTFLUSH` paths.** Sec.5.3: forbidden on the RP2040 by erratum and
  unsuitable for a terminal path on the RT1062. A driver that needs to cancel a transfer, rather
  than overwrite it, is a later requirement with a later argument.
- **`arch_console_reclaim` fleet-wide.** Sec.5.5 adds three bodies because three boards need
  them; the rest of the fleet is still M4.7 work, unchanged from
  `docs/design-m4.6-irq-driver.md` section 12.
- **DORMANT and any tickless deepening on the RP parts.** RULE U1.

Two items are **owned elsewhere and must not be re-derived here**: the `teensy41` reboot path's
missing witness (`docs/reference/boards.md` is where a witness would land, and
`arch/arm/chip/imxrt1062/chip_imxrt1062.cc` already carries the honest comment), and the
silicon consumer still owed by M4.6.1's second half on `xmc4800-relax` (`STATE.md`) -- M4.6.2
must not start before that closes, because it is the only thing that proves the substrate this
document builds on against real hardware.

## S7 built, and not witnessed: where a non-cacheable attribute belongs

The stage table listed S7 as "a non-cacheable attribute on a dynamic grant". **One word of that
sentence was wrong: it belongs on the ALLOCATION, not on the grant.** The design below is what was
built; the record of what the code does, and of what it has NOT proved, is at the end of the section.

**The attribute: Normal, Outer-and-Inner Non-cacheable, Shareable. NOT Device.** Device forbids
speculation and makes any unaligned access UNPREDICTABLE, including a multi-word load or store that
spans a Normal/Device boundary. That rules it out here, because the shared 4 KiB block is not only
descriptor lists: the USB driver puts its console byte rings and its breadcrumb words in the SAME
block, and walks the rings with `memcpy`. Coherency needs only "not cached"; Device would additionally
make the driver's own ring copies undefined. The existing barrier before the doorbell is still
required either way.

**Why the allocation and not the grant.** The block is created in three steps -- reserve arena, grant
it to root's own domain, then hand it to the task -- and root writes the block through that first
mapping to initialise it. Both grants are hard-coded read-write, which is CACHEABLE. So an attribute
riding on the individual grant would leave the bring-up path itself creating dirty lines for the
block, later evicted over descriptors the controller wrote. It would also permit two mappings of one
block to DISAGREE, which is the exact corruption being fixed. Attaching it to the allocation means no
cacheable mapping of the block ever exists -- and therefore **no cache-maintenance primitive is ever
needed**, which is what section 4.3 was trying to avoid inventing.

**The seam must be THREE-valued, and this is what a naive version gets wrong.** Not "honour or
refuse": a chip with NO data cache in the path trivially satisfies a non-cacheable request and must
ACCEPT it, or every armv6m and cacheless armv7m board breaks the day the flag appears. So: can
program it, honour; has no cache, accept as already satisfied; has a cache and cannot express it,
REFUSE. That is a per-CHIP property rather than a per-arch one. Refusal must happen at ADMISSION,
beside the existing region-encodable check, because every commit backend today fails CLOSED AND
SILENT -- a region that cannot be encoded is simply not programmed, with no path reporting upward.
Silently ignoring a non-cacheable request on a bus-mastering peripheral is a data-corruption bug.

**Cost: zero bytes.** The region descriptor's attribute word has only its low bits defined, so a new
bit moves no structure size and perturbs no layout assert.

**One audit point for whoever lands it**, because a new bit changes two exact-equality tests: the
domain layer dedups regions by comparing the attribute word against a literal read-write pair, and
one backend detects region changes by exact equality. A non-cacheable region correctly stops deduping
with a cacheable one, which is the desired behaviour, but both sites want eyes rather than assumption.

### What landed, and the one thing the design did not say

The shape is the one above. `ARCH_MPU_NOCACHE` is bit 4 of the region attribute word; PMSAv7 encodes
it `TEX=0b001, C=0, B=0, S=1` and PMSAv8 takes a third MAIR slot at `0x44`. The user-facing flag is
`KOS_MEM_NOCACHE`, and it rides `Descriptor::block_flags`, which the generic bring-up passes to BOTH
`kos_mem_self_grant` and `kos_task_create` from one place. The three-valued seam is
`arch_mpu_nocache_support()`, answered per chip and per enforcement posture; admission refuses only
`ARCH_MPU_NOCACHE_REFUSED`, in `grant_region_admissible`, and that arm of the predicate is the ONE
part of the grant module still live at `KICKOS_HAVE_MPU=0` -- an ignored non-cacheable request
corrupts data rather than merely weakening isolation, so it cannot be stubbed out with the rest.

**The design missed one thing, and it is the whole reason the bring-up path stays clean.**
`KOS_SYS_MEM_SELF_GRANT` short-circuits on "already reachable", and root is PRIVILEGED, so that test
passed unconditionally and added no descriptor at all. Root therefore reached the block through the
PRIVDEFENA background map, which is CACHEABLE -- exactly the first mapping the design set out to
eliminate. The short-circuit now asks the typed question when the chip programs the attribute: is
there a region the caller already carries that describes this range with this memory type. Privileged
reach is no answer to it, because the background map carries the chip's default type. Where the chip
answers `INHERENT` the plain question is kept, or the call would spend one of eight region slots to
say nothing.

**Where the three-valued seam is answered, and where the design's phrasing is too strong.** The rule
is right: can program it, honour; has no cache, accept; has a cache and cannot express it, refuse.
Reading that as "PMP, the RX MPU and SYSMPU refuse" is NOT right, because none of the chips on those
backends has a data cache over its arena -- a K64F is a Cortex-M4, an RX72M and an ESP32-C6 reach
internal SRAM uncached -- and refusing there would break a cacheless board for nothing. Each of those
backends therefore answers `INHERENT`, and the standing obligation is recorded here rather than in
the code: a future chip on one of those backends that DOES cache its arena must change the answer to
`REFUSED`, because its region descriptor carries no memory type at all and a commit backend drops
what it cannot encode in silence.
Two ARM chips override the PMSA default rather than inherit it. `mk64f` for exactly that reason:
SYSMPU is access permissions only, so the PMSA answer would have been a lie about the mechanism even
where it lands on the right outcome. And `imxrt1062`, the one ARM part in tree with a data cache,
because the fallback's answer would otherwise depend on a fact invisible from `arch/arm/common` --
that `arch_init` enables the cache only under `KICKOS_HAVE_MPU`, so a build that programs no region
has no cache in the path either. Moving that enable out of its guard would silently make the seam
lie, so the chip states its own answer where the enable lives.

The consequence is that **no configuration in the tree reaches the `REFUSED` arm today**. It is
covered by construction, not by a board: the self-test pairs `KOS_GRANT_OP_NOCACHE_SUPPORT` against
`KOS_GRANT_OP_RAM_NOCACHE` and asserts the verdict follows the tri-state, so a chip that ever answers
`REFUSED` is checked the moment it exists.

**Why no cache-maintenance primitive is still needed, with the cache ON.** The block is never
written through a cacheable mapping: `arch_ram_alloc` only moves a bump pointer and touches no byte
of it, the D-cache enable invalidates the whole cache once at boot before any thread runs, and the
bring-up's FIRST write to the block happens after the non-cacheable self-grant is committed. The M7
may still speculatively pull a line covering the block while the default map is in force, but
speculation produces only CLEAN lines and a clean line is never written back, so nothing can land on
top of what the controller writes. That argument is what the whole design rests on, and it is the
first thing to re-check if the falsification list below ever fires.

### The owed witness

Nothing here has run on silicon. No `teensy41` is on this bench, so the whole of S7 is a build-time
and host-time claim.

**The measurement that would settle it.** Flash a `teensy41` `_usbcdc` image configured with
`-DKICKOS_IMXRT_DCACHE=ON`, enumerate it on a host, and run the IPC bench across the CDC console.
Against the two captures already tagged in the bench record -- `m493dcon` (D-cache ON, no USB
console: 61,124 ns per call/reply round-trip, 32 B payload) and `m493dcoff` (D-cache OFF, the posture
S6 shipped: 363,005 ns) -- the fix is confirmed only if the round-trip lands at roughly `m493dcon`,
about 61 us, while the console stays up. The point of S7 is buying back the 6x, so a figure near
363 us means the cache is not actually being used and the fix bought nothing even if the console
works.

**What would FALSIFY it, stated before the run:**

- Round-trip near 363 us with the console alive. The image is not getting the cache back; either the
  fixed anti-speculation rows or the block's own descriptor is covering more than intended.
- Enumeration fails, or the CDC endpoint stalls or drops bytes, with the cache ON but succeeds with
  it OFF. That is coherency, not USB: the controller is reading a descriptor list the core left in a
  dirty line, which means a mapping of the block is still cacheable somewhere.
- A MemManage fault in the driver's first block write. The block is being described by no descriptor
  at all rather than by a non-cacheable one.
- The round-trip is fast AND the console works, but only until the ring wraps. The `Shared` rings are
  walked with `memcpy` and a Normal region tolerates the unaligned accesses that produces; a fault or
  corruption that appears only on a wrap would say the region was encoded as Device rather than as
  Normal non-cacheable, which is the specific mistake the attribute choice above exists to avoid.

Until one of those runs, `arch/CMakeLists.txt` names the combination UNWITNESSED at configure time
and the default for a USB-console image stays `KICKOS_IMXRT_DCACHE=OFF`.
