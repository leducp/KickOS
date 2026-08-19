<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->

# One generic driver service, N chips: the descriptor ruling

> **Status: LANDED.** Implemented in full across seven steps; the m4.8.1 commits cite it. This is
> the ruling M4.8.1's own conclusion ("a service is a transport over a class") forces once you
> count the instances rather than the classes. Written against `a1220233` on
> `M4.8.1-driver-class`. **Four corrections are folded in below and are not rewritten away**: leg L7
> as first ruled was unsatisfiable; the instance count at that tree was TWELVE, not nine, and it
> grows with every new driver (`grep -rln 'drv::valid' system user`); leg **L8 applied
> HANDOVER-only reasoning to RETAIN** and refused two legitimate shapes (section 3.3.1); and the
> validator accepted thirteen defective shapes an adversarial pass proved by compilation, closed as
> two new legs plus new arms on L3, L4 and L8 and a rewritten class-side check (section 3.3.2).
> **THE CODE LISTINGS BELOW ARE THE M4.8.1 SHAPE AND ARE NOT THE LIVE CONTRACT.** M4.8.3 put every
> driver thread in a task, so `spawn_one` gained the task handle and stopped passing a per-thread
> memory grant at all; M4.8.4 then DELETED the per-thread memory flag these listings show, because
> its only reader was an OR-reduction into the group's grant. Read
> `user/include/kickos/sys/driver_service.h` for the field set and
> `docs/design-task-layer.md` open question 7 for the ruling. The listings stay as they are: they
> record what M4.8.1 decided.
> See `design-m4-driver-model.md` for the numbered rules this builds on.

## 0. What is actually duplicated

Not "five UART services". **One service per (class x chip)**, and both axes multiply:

| class | instances today | substrate |
| --- | --- | --- |
| UART | 5 on silicon (`c6uart`, `lx6uart`, `k64uartirq`, `xmcuartirq`, `rxsci`) plus 1 in the sim | `sys/uart_service.h`, 426 lines |
| SPI | 2 (`k64dspi`, `xmcssc`) | `sys/spi_service.h`, 233 lines |
| USB CDC | 1 (`rpusb`) | `sys/usb_cdc_service.h`, 780 lines |
| I2C, CAN | 0, and each new one multiplies across every chip | none yet |

Measured: 255 / 255 / 262 / 267 / 332 lines for the UART services, 106 / 138 for SPI, 521 for
`rpusb`, 271 for the sim list. `c6uart.cc` and `lx6uart.cc` share 223 of 255 lines verbatim:
they are the same program with two identifiers swapped. SPI is small because it runs one thread
per bus, not because it is different in kind.

A UART-shaped abstraction moves the duplication one level up and multiplies again on the next
chip. So the ruling has to be **generic over the class**, with a per-chip **descriptor** for
the chip specifics.

## 1. The ruling: two seams, and only one of them is new

The single most useful thing this pass established is that the duplicated code is not one
thing. It splits cleanly, and the split is what makes the design small:

```
   per-chip TU (kickos_c6uart, kickos_rxsci, kickos_k64dspi, ...)
   +-----------------------------------------------------------+
   |  constexpr Descriptor   <- the chip's SHAPE, plain data    |
   |  constexpr UartParams   <- the chip's class-side knobs     |
   |  2 tiny thread-entry / block-init wrappers                 |
   |  extern "C" <name>_<kind>_start -> drv::bring_up(...)      |
   +-----------------------------------------------------------+
        |                                        |
        | SEAM A (new, class-agnostic)           | SEAM B (exists, per class)
        v                                        v
   sys/driver_service.h                     sys/uart_service.h
   the BRING-UP CHOREOGRAPHY:               the THREAD BODIES:
   alloc + self-grant, endpoint,            irq_loop, serve_loop,
   publish, claim N lines, spawn N          console_write_all, tx_write
   threads with per-thread grants and       (+ sys/spi_service.h,
   cap roles, the readiness barrier,        sys/usb_cdc_service.h)
   the unwind, the handover tail                  |
        |                                        v
        | names NO class symbol             user/include/kickos/driver/uart.h
        |                                   the five-call CLASS contract
        v                                        |
   the class appears ONLY as a                   v
   function pointer to a thread entry       uart_c6.cc / uart_sci.cc / ...
```

**Seam B already exists.** `uart_service.h` already holds the loops, and each per-chip service
calls into them. It is templated on the backend type today (`irq_loop<Uart>`) and stays that
way, for one reason only, given in section 3.4.

**Seam A is the new thing, and it needs no template.** The bring-up never calls a class
function. It calls `kos_ram_alloc`, `kos_mem_self_grant`, `kos_endpoint_create`,
`kos_console_publish`, `kos_irq_claim`, `kos_thread_spawn`, `kos_handle_close`, `kos_sleep_ns`
and `kos_send_timed`. Not one class symbol. So it is **plain data plus function pointers**, and
the descriptor is a POD aggregate.

That is the answer to "how is the class plugged in": **it is not plugged into the bring-up at
all.** It is plugged into the thread body, which is where the tree already puts it. Three
consequences fall out for free, and each of them was a hard constraint:

- **No template parameter for the class.** House style says templates own multi-type
  containers; a bring-up generic over a class it never calls would be cleverness. The new
  header contains zero templates (section 3.5 shows how the one plausible need, the offset of
  the readiness latch, is answered by a `constexpr uint16_t` in the class substrate instead).
- **REGDIR-private headers stay private.** The bring-up TU includes no chip header. The line
  numbers, the window base and the class config all arrive as descriptor data authored in the
  per-chip TU, which is the only TU with REGDIR on its include path.
- **The per-target SPI class-symbol rename is untouched.** `kickos_k64dspi` renames its four
  `kos_spi_*` symbols privately (`k64dspi/CMakeLists.txt:26-30`). Those symbols are called from
  `spi_service.h` (inline, in the per-chip TU) and from the per-chip thread body, both of which
  still compile inside `kickos_k64dspi`. The generic bring-up carries no rename because it has
  nothing to rename. Section 7 makes this concrete.

**Judgment call:** seams A and B are separate headers rather than one. Reason: seam A must not
name a class, and a single header holding both would give every SPI service a transitive
include of the UART substrate.

## 2. What the descriptor must be able to SAY

Collected from the instances that break a naive two-thread descriptor. Each row is a
descriptor field or a validator leg, not a special case.

| the fact | where it shows | expressed as |
| --- | --- | --- |
| Two lines with different roles and different triggers | `rxsci.cc:225,232` | a `lines[]` array with a per-line `trigger` |
| Per-thread rights on the same line (WAIT here, SIGNAL there) | `rxsci.cc:241,259,295` | a per-thread `caps[]` of `{resource, rights}` |
| Cap ROLES are per-thread: the same two child indices mean different objects | `rxsci.cc:49-53` vs `uart_service.h:45-48` | `caps[]` index IS the child index; roles are per-thread by construction |
| A thread with NO ring grant and NO thread arg | `rxsci.cc:260,263` | a per-thread memory flag beside `arg`. **RETIRED IN M4.8.4**: under a task the block is the GROUP's region, so the flag could not deliver the opt-out it read as. It equalled `arg == KOS_DRV_ARG_BLOCK` in every descriptor and is deleted; the ruling is `docs/design-task-layer.md` open question 7 |
| Which side of the readiness barrier a thread is spawned on | `rxsci.cc:242,260,278,296` | `barrier_after`, a count |
| A relayed line must be EDGE | `rxsci.cc:103-105` | validator leg L5, at compile time |
| The thread arg is the window base as a VALUE | `driver_bringup.h:52`, `k64dspi.cc:50` | `arg = KOS_DRV_ARG_WINDOW` |
| No ring block at all | `driver_bringup.h:54` | `block_size = 0` |
| Root KEEPS the endpoint, there is no tail | `k64dspi.cc:103`, `xmcssc.cc:135` | `ep_posture = RETAIN` |
| Zero IRQ lines | `k64dspi.cc:52` | `line_count = 0` |
| No window grant at all | `service_list_uart.cc:198` | per-thread `window_grant = false` |
| No console publish, kind is not CONSOLE | `service_list_uart.cc:142,263` | `ep_posture = RETAIN`, `svc_kind` |
| A 2048-byte block of a different type | `usb_cdc_service.h:88-106` | `block_size` + `ready_offset` |
| Prime the source before the first wait | 4 UARTs do, `rxsci.cc:95-100` does not | `UartParams::prime`, seam B not seam A |
| A cfg naming another window is refused | present in 4, absent in 2 | `expected_base`, and leg L9 |
| A cfg naming no rate means 115200 here | `k64uartirq.cc:153-160` | the per-chip `block_init`, not the descriptor |

The last two rows are deliberate relocations away from the descriptor, argued in section 3.6.

## 3. The types

### 3.1 `user/include/kickos/sys/driver_service.h`, the descriptor

```cpp
#ifndef KICKOS_SYS_DRIVER_SERVICE_H
#define KICKOS_SYS_DRIVER_SERVICE_H

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/cap_index.h> // KOS_CAP_STDOUT: the handover probe rides cap 0
#include <kickos/sys/service.h>

#include <stdint.h>

namespace kickos
{
namespace driver
{

enum
{
    KOS_DRV_LINES_MAX = 2,
    KOS_DRV_THREADS_MAX = 3,
    KOS_DRV_CAPS_MAX = 2
};

// What a per-thread cap entry NAMES. A thread's caps[] index is also its child cap index
// offset from KOS_SPAWN_DELEGATED_CAP0, so caps[0] lands at index 1 and caps[1] at index 2.
// This is why the same two indices can mean different objects in different threads.
enum
{
    KOS_DRV_RES_EP = 0,
    KOS_DRV_RES_LINE0 = 1,
    KOS_DRV_RES_LINE1 = 2
};

struct Cap
{
    uint8_t resource; // KOS_DRV_RES_EP, or KOS_DRV_RES_LINE0 + i
    uint8_t rights;   // a kos_cap_rights subset
};

struct Line
{
    int32_t number;  // the chip vector; REGDIR-private, which is why a descriptor is
                     // authored in the per-chip TU and never here
    uint8_t trigger; // KOS_IRQ_EDGE or KOS_IRQ_LEVEL
};

enum kos_drv_arg
{
    KOS_DRV_ARG_NONE = 0,
    KOS_DRV_ARG_BLOCK = 1, // the granted ring block pointer
    KOS_DRV_ARG_WINDOW = 2 // cfg->mmio_base as a VALUE, never dereferenced as memory
};

struct Thread
{
    void (*entry)(void*);
    char const* name; // null takes cfg->name
    int8_t prio_delta;
    uint8_t arg;        // enum kos_drv_arg
    bool mem_grant;     // a spawn-time grant of the whole ring block
    bool window_grant;  // cfg->mmio_base + cfg->mmio_window; at most one thread, because a
                        // DEV window has exactly one holder
    uint8_t cap_count;
    struct Cap caps[KOS_DRV_CAPS_MAX];
};

enum kos_drv_ep
{
    // kos_console_publish, then the handover tail. From the publish on, the console is
    // USER_OWNED and a kernel-console write is DROPPED until closing E takes recv_holders
    // to 0, which is why every diagnostic below closes before it prints.
    KOS_DRV_EP_HANDOVER = 0,
    // No publish; root keeps a full-rights cap for the app to narrow per client. Root
    // therefore holds a WAIT-bearing cap forever, so recv_holders never reaches 0 and the
    // last-receiver-gone EPIPE wake never fires: NO failure path in a driver thread under
    // this posture may exit(), it must panic (xmcssc.cc:52-57).
    KOS_DRV_EP_RETAIN = 1
};

// No readiness latch in the block, so no barrier.
constexpr uint16_t KOS_DRV_READY_NONE = 0xFFFFu;

struct Descriptor
{
    char const* tag;         // "[c6uart] ", prefixed to every diagnostic this bring-up prints
    uintptr_t expected_base; // 0 = no guard; safe because a granted window base is never 0
                             // and the one instance whose base IS 0 grants no window
    uint32_t block_size;     // 0 = no ring block, no arena allocation, no self-grant
    uint16_t ready_offset;   // byte offset of a `volatile uint32_t` latch inside the block
    uint8_t ep_posture;      // enum kos_drv_ep
    uint8_t svc_kind;        // enum kos_svc_kind; a cfg of another kind is refused
    uint8_t line_count;
    uint8_t thread_count;
    uint8_t barrier_after;   // threads spawned BEFORE the readiness poll
    struct Line lines[KOS_DRV_LINES_MAX];
    struct Thread threads[KOS_DRV_THREADS_MAX];
    // Lays out the block and fills the class config from the cfg. The ONLY place a class's
    // own vocabulary appears in a bring-up. Null iff block_size == 0.
    int (*block_init)(void* blk, struct kos_service_cfg const* cfg);
};
```

Every field is a literal type, so a descriptor is `constexpr` and section 3.3's validator runs
in a `static_assert`.

### 3.2 The thread set, and the tail

```cpp
struct ThreadSet
{
    kos::thread::Handle t[KOS_DRV_THREADS_MAX];
    uint8_t n = 0;

    void add(kos::thread::Handle const& h)
    {
        t[n] = h;
        n++;
    }

    // Swept unconditionally and in reverse spawn order: kos_thread_kill is COOPERATIVE and
    // honoured only in kos_irq_wait, so a refusal on one handle says nothing about the next,
    // and stopping at the first would leave a live thread holding a line or the window.
    void cancel_all() const
    {
        for (uint8_t i = n; i > 0; i--)
        {
            (void)t[i - 1].kill();
        }
    }
};

constexpr uint32_t KOS_DRV_HANDOVER_PROBE_US = 1000000;

// The LAST two steps of a console handover, in this order because either order wrong fails
// silently. Closes the caller's own WAIT-bearing cap on E, then PROVES a driver is serving
// before any console client runs. Returns 0, or the probe's negative rc.
//
// -KOS_EPIPE means the SERVICE thread died. The console comes back only once the register
// window is free, and the window is held by a thread that is still alive, so EVERY remaining
// peer is cancelled before the tag is printed. `peers` is the whole set the bring-up spawned:
// a single handle cannot express a driver whose third thread holds two lines.
//
// Any other refusal, -KOS_ETIMEDOUT above all, leaves the service thread ALIVE and still the
// sole receiver: recv_holders never reaches 0, the console is NOT reclaimed, and nothing here
// recovers. The code is returned unchanged and no tag is printed.
int console_handover_finish(kos_cap_t ep, char const* tag, ThreadSet const& peers);
```

`peers` is passed by const reference and is a plain array of handles on root's stack: 3 handles
is 24 bytes on a 32-bit target. Section 6 says what a Task later does to it.

### 3.3 The validator, and why it is the point

Twelve legs (**CORRECTED from ten**; L11 and L12 are section 3.3.2's), each a one-liner over a
`constexpr` descriptor. Four of them turn a class of silent defect the tree currently carries into
a compile error.

```cpp
constexpr bool valid(Descriptor const& d);
```

| leg | rule | what it catches |
| --- | --- | --- |
| L1 | `thread_count <= KOS_DRV_THREADS_MAX`, `line_count <= KOS_DRV_LINES_MAX` | array overrun in a descriptor. **CORRECTED:** the `thread_count >= 1` arm it also carried is redundant, implied by L6, and is gone; the upper bounds are what keep L2..L12 in range and stay |
| L2 | every thread: `cap_count <= KOS_DRV_CAPS_MAX`, every `caps[i].resource < 1 + line_count`, every `caps[i].rights != 0` | a cap naming a line the descriptor does not claim |
| L3 | at most one thread has `window_grant`; **`arg == ARG_WINDOW` implies `window_grant`** | a DEV window has exactly one holder; a second spawn is refused `-KOS_EBUSY` at runtime today. The second arm is new: `spawn_one` hands `cfg->mmio_base` to an `ARG_WINDOW` thread whether or not the window was granted, so one without it faults on its first register touch |
| L4 | `arg == ARG_BLOCK` implies `block_size != 0`; **`block_size != 0` implies some thread takes `ARG_BLOCK`**; **`block_size == 0` IFF `block_init == nullptr`** | a thread handed a block nobody allocated, and a block granted to the whole group that nothing reads. **RESTATED IN M4.8.4**: the two arms about the per-thread memory flag went with the flag, and the converse arm replaced them -- the block lands as a region on every member, so a descriptor carrying one no thread takes is the widest ask it can make and nothing else would catch it |
| **L5** | a thread holding `WAIT` on a line and having no `window_grant` implies that line is `KOS_IRQ_EDGE` | **the relay rule.** Only the window holder can clear a peripheral flag, so a thread that waits without a window cannot serve a LEVEL source and would rearm into a still-asserted line and spin (`rxsci.cc:103-105`) |
| L6 | exactly one thread holds `{EP, WAIT}` | two receivers, or none |
| **L7** | `ep_posture == HANDOVER` implies `thread_count == 1` or `ready_offset != KOS_DRV_READY_NONE` | a console handover with no readiness latch has no reportable window, UNLESS there is only one thread |
| **L8** | POSTURELESS: `ready_offset != NONE` implies `block_size != 0`, `ready_offset + 4 <= block_size`, **`ready_offset % 4 == 0`**, and `1 <= barrier_after <= thread_count`. HANDOVER-ONLY: `barrier_after < thread_count` and the `{EP, WAIT}` holder's index `>= barrier_after` | **the barrier rule.** Under HANDOVER the poll must sit STRICTLY between the spawns, because once the ep holder exists `recv_holders` never reaches 0, nothing reclaims the console, and the timeout diagnostic goes to an endpoint nobody drains (`uart_service.h:66-69`). **CORRECTED:** the last two arms were applied to RETAIN as well and refused two legitimate shapes -- section 3.3.1. The alignment arm is new: an unaligned `volatile uint32_t` load is tolerated on ARMv7-M and FAULTS on RX and Xtensa |
| **L9** | `line_count > 0` and some thread has `window_grant` implies `expected_base != 0` | **the base-pin rule.** A driver that claims a vector BY NUMBER is hard-wired to one peripheral instance, so a cfg naming another window would grant one block and interrupt on another. Exempts the sim (no window) and `k64dspi` (no line, and `spi_dspi.cc` is genuinely base-parameterised across DSPI0/1/2) |
| L10 | `tag != nullptr`, every `threads[i].entry != nullptr` | a half-authored descriptor |
| **L11** | `ep_posture == HANDOVER` implies `svc_kind == KOS_SVC_CONSOLE` | NEW. `bring_up` calls `kos_console_publish` under HANDOVER, so any other kind under that posture publishes a BUS endpoint as the board's console and routes every stdout writer at it. Holds for all eight HANDOVER descriptors |
| **L12** | every claimed line has EXACTLY ONE `WAIT` holder | NEW, **the line-role rule.** A claimed line comes back MASKED and only its waiter's first `irq_wait` arms it, so a line nobody waits on stays masked forever and every event on it is lost silently. Section 3.3.3 is why this catches a swapped relay without any layer knowing which line is transmit |

L5, L8, L9 and L12 are the design's real dividend. Each is a rule the tree states in prose, in
several files, and violates in at least one place.

**L7 CARRIED A BUG, and implementation is what found it.** As first ruled it was simply
`HANDOVER` implies `ready_offset != NONE`, with no thread-count exemption, and that is **jointly
unsatisfiable with L8** at one thread: L8 requires `1 <= barrier_after < thread_count`, which no
one-thread descriptor can meet. So the version above is a repair, not the original. It matters
because THREE shipped drivers are one-thread `HANDOVER` consoles -- `k64uart`, `xmcuart` and the
sim's `simcon` -- and none of them could be expressed at all. The exemption is sound rather than a
carve-out: a one-thread driver's only thread IS the endpoint's receiver, so no point exists before
it at which a readiness timeout could be reported, and the handover probe is the stronger witness
anyway because it proves the thread is SERVING where a latch proves only that it started. The
repair is narrow, and that was checked: `rpusb` at two threads still fails L7 without a ready
offset.

**The root cause of that error is the count below.** This document was written saying nine
instances; the three one-thread console drivers were missing from the inventory, which is also why
section 8 claims `driver_bringup.h` has one remaining user when it in fact had four. **Every bare
instance count in this file is a snapshot and none of them is authoritative**: the live list is
`grep -rln 'drv::valid' system user`, and it has kept growing since (nine, then twelve, fourteen at
the time of this edit). Read any "nine" below as "nine of the set I had counted".

#### 3.3.1 L8 CARRIED THE SAME BUG, and an adversarial pass is what found it

L7's repair above fixed one leg and left its sibling. **L8's arms `barrier_after < thread_count`
and `ep_holder(d) >= barrier_after` encode HANDOVER-only reasoning and were applied to RETAIN
unconditionally.** L8's own justification is the console reclaim: once the endpoint's receiver
exists, `recv_holders` never reaches 0 and nothing reclaims the console. **Under RETAIN root keeps
a WAIT-bearing cap forever, so `recv_holders` never reaches 0 in the first place and there is no
console to reclaim.** The premise is absent, and with it the rule.

Two shapes that should be expressible were refused, both proved by compilation:

- **A one-thread RETAIN service with a readiness latch.** `barrier_after >= 1` and
  `barrier_after < thread_count` cannot both hold at `thread_count == 1`, so a one-thread bus that
  wants "prove the bus opened before I hand the app the endpoint" was inexpressible. `k64dspi` and
  `xmcssc` dodge it only by happening to want no block.
- **A RETAIN receiver at index 0.** "Spawn the server first, then a helper, latch between" was
  refused, because under RETAIN the `ep_holder >= barrier_after` arm makes index 0 unreachable.

Both arms are now gated on `ep_posture == KOS_DRV_EP_HANDOVER`. **The duality is the point and is
preserved:** the SAME `ep_holder >= barrier_after` arm is the SOLE rejecter of a receiver spawned
before the barrier under HANDOVER, and is over-strict under RETAIN. Compile-proved both ways -- the
four HANDOVER shapes L8 must keep refusing (one-thread with a latch, `barrier_after` at 0, above
every barrier position, receiver at index 0) still fail on the new tree.

**Relaxing a leg is not free, and this one was not.** Gating `barrier_after < thread_count` alone
would have made the shape it admits a LIE: `bring_up`'s spawn loop ran `i < thread_count`, so
`barrier_after == thread_count` matched no iteration and the poll never ran -- the barrier silently
becoming a no-op, which is the exact failure L8 exists to prevent. So the loop now walks
`thread_count + 1` barrier positions and polls after the last spawn, which is the only readiness
window a one-thread service has. Witnessed by a new `drv_bringup` arm whose discriminating half is
the TIMEOUT: the rc alone cannot tell "polled after the spawn" from "never polled", but the sleep
token can only appear after `spawn50`.

The same relaxation opened one door on the SPI side -- a RETAIN descriptor could now name a block
the SPI substrate never lays out -- and `spi::desc_ok` closes it (section 3.3.2).

#### 3.3.2 Thirteen false acceptances, and where each one belongs

An adversarial pass compiled thirteen defective descriptors that both checks accepted. The split
is the same one section 1 draws: class-agnostic ARITHMETIC belongs in `valid()`, class KNOWLEDGE in
`desc_ok`.

Closed in `valid()`, as new legs L11 and L12 and as new arms on L3, L4 and L8 (see the table
above): `arg == ARG_WINDOW` without the window grant; `arg == ARG_BLOCK` without the memory grant;
a misaligned `ready_offset`; `HANDOVER` on a non-console kind; a `block_init` for a zero-size
block; and the swapped relay of section 3.3.3.

Closed class-side. **The class substrate DEFINES every constant involved and then never compared
the descriptor against it** -- both headers say in prose "Never write the offset as a literal in a
descriptor" and neither enforced it:

- `ready_offset` must EQUAL `KOS_UART_READY_OFFSET` / `KOS_USB_READY_OFFSET`. This is the sharp
  one. `valid()` only RANGE-checks the field, so a literal `0` points at the ring head, which is
  non-zero after `shared_init`: `wait_ready` then returns true on its first read and **the barrier
  silently becomes a no-op**, the very failure L8 exists to prevent, reintroduced through the one
  field L8 only bounds.
- `block_size` must EQUAL `KOS_UART_BLOCK_SIZE` / `KOS_USB_BLOCK_SIZE`. A 512 against an 880-byte
  `uart::Ctx` passed.
- The service thread must NOT hold the window grant. Every class call touches a register and the
  service thread makes none. L5 caught this only when the line happened to be LEVEL, purely as an
  accident of its scope, so on `xmcuartirq`, `rxsci` and `simuart` (all EDGE) nothing caught it.
- A non-service thread's `caps[1]` must be a non-EP SIGNAL cap. `spi::desc_ok` already pinned every
  cap position and every rights bit, and **`spi`'s formulation is the one the other two should have
  had**: `drv::edge_relay_thread` hard-codes `kos_irq_notify(KOS_SPAWN_DELEGATED_CAP0 + 1)`, so a
  relay's `caps[1]` is exactly as much a compile-time fact as the service thread's doorbell.

**Judgment call, and it falls between the two layers.** `uart::desc_ok` and `usb::desc_ok` were
byte-identical bar one comment word, which is precisely how one missing arm became two separate
findings. They are now one predicate,
`driver::ring_doorbell_shape_ok(d, ready_offset, block_size)`, which each substrate calls with its
own two constants. The SHAPE (a ring plus a doorbell, one receiver on `caps[0]` ringing `caps[1]`,
every other thread parked on its own `caps[0]`) is shared by two of the three classes; the
CONSTANTS stay at the call site. It lives in `driver_service.h` but is deliberately NOT a leg of
`valid()` and says so in its own comment, because `spi_service.h` does not follow it. Naming it a
convention rather than a rule is the honest placement; folding it into `valid()` would make the
class-agnostic header assert a convention one of its clients breaks.

**JU-5, a cross-layer contradiction, resolved by NOT raising `KOS_DRV_CAPS_MAX`.**
`spi::desc_ok` demands `cap_count == 1 + line_count`, which is 3 for a two-line bus, while
`KOS_DRV_CAPS_MAX == 2` and L2 refuses `cap_count > 2`: a two-line SPI bus was unsatisfiable and
an author met it as two contradictory errors. Raising the cap to 3 is free in `struct Thread`'s
existing padding, and it is still the WRONG fix, because it would convert a compile error into a
silent stall: `kos_spi_bus_config` carries ONE `irq` cap and `KOS_SPI_CAP_LINE` is the only line
index the substrate names, so the second line would be claimed and never waited on. The limit is a
CLASS limit, so `spi::desc_ok` now states it directly (`line_count <= 1`) and the author gets one
error naming the real reason instead of an arithmetic pair that contradicts itself. `spi::desc_ok`
also pins `block_size == 0` and `ready_offset == KOS_DRV_READY_NONE`, which is the SPI form of the
same "the substrate defines the constant" rule and closes the door section 3.3.1's relaxation
opened.

#### 3.3.3 Why L12 catches a swapped relay without knowing which line is which

The worst of the thirteen: swap `rxsci`'s two relay line caps and it waits on TX and posts RX. Two
threads then wait on TXI, RXI is never waited on so it stays masked forever, and **every received
byte is silently lost, with no diagnostic.** Which line is transmit and which is receive is
per-chip knowledge neither layer holds, and the obvious answer -- a `role` byte on `struct Line` --
was **rejected**: it adds a field, needs a per-chip value on every descriptor in the tree, and encodes a
fact the swap does not actually require.

L12 is purely structural and needs no new field. The swap breaks the count on BOTH lines at once:
line 0 gains a second waiter, line 1 loses its only one. Two independent rejections, from one
arithmetic leg over data the descriptor already carries.

**What L12 does NOT catch, stated plainly.** Permuting the `lines[]` array itself -- authoring
`{{RXI, EDGE}, {TXI, EDGE}}` and leaving every cap where it is -- preserves one waiter per line and
passes. That permutation is not the same defect: the IRQ thread's pass reads and writes regardless
of which vector woke it, and the doorbell rides whichever line the IRQ thread waits on, so it is
arguably an equivalent shape rather than a broken one. A check that appeared to cover it would be
the weaker check the brief warned against.

**A second, class-side check.** The generic validator cannot know that `uart_service.h`'s
thread bodies read `KOS_UART_CAP_EP == 1` and `KOS_UART_CAP_DOORBELL == 2`. So each class
substrate contributes its own `constexpr` predicate over the same descriptor:

```cpp
// in sys/uart_service.h -- the two constants are the class's own, and the shape is the one
// usb_cdc_service.h shares (section 3.3.2)
constexpr bool desc_ok(driver::Descriptor const& d)
{
    return driver::ring_doorbell_shape_ok(d, KOS_UART_READY_OFFSET, KOS_UART_BLOCK_SIZE);
}
```

Both are asserted per instance:

```cpp
static_assert(drv::valid(k_desc), "not a well-formed driver shape");
static_assert(uart::desc_ok(k_desc), "cap positions do not match KOS_UART_CAP_*");
```

Nothing checks the second fact today, and getting it wrong is a silent stall.

**The `static_assert` is not the only enforcement any more.** `valid()` also runs as the first
statement of `bring_up`, for the reason R1.1 in section 8.1 gives.

### 3.4 The bring-up

**THE CODE BELOW (AND THE `ThreadSet` STRUCT IN 3.2) IS STALE AS OF M4.8.3 AND IS KEPT ONLY AS
THE REASONING.** Step 9.4 deleted `ThreadSet`: `bring_up` creates one task per driver and spawns
every thread into it, and `unwind` and `console_handover_finish` take a `kos_task_t` in place of
the set -- `unwind`'s `peers.cancel_all()` becomes one `kos_task_kill`. Section 5.1 has the full
correction. What survives below is the choreography -- what `bring_up` does in what order, what
`unwind` closes before it cancels -- not the `ThreadSet` type it was written against.

```cpp
int bring_up(Descriptor const& d, struct kos_service_cfg const* cfg, kos_cap_t* out_ep);
```

`out_ep` receives the retained endpoint under `KOS_DRV_EP_RETAIN` and must be null under
`HANDOVER`. This pairing is the one thing `valid()` cannot check, because `out_ep` is a runtime
pointer; it is refused at the top of `bring_up` with a tag.

The body, with the failure paths elided to their shape:

```cpp
inline int bring_up(Descriptor const& d, struct kos_service_cfg const* cfg, kos_cap_t* out_ep)
{
    // R1.1's belt, first: every index below is bounded by a leg, and d.tag is L10's own
    // subject so it cannot be trusted here.
    if (not valid(d)) { ... }
    if (cfg == nullptr or cfg->kind != d.svc_kind)
    {
        return fail(d.tag, "ERROR: bad or wrong-kind service cfg\n");
    }
    if (d.expected_base != 0u and cfg->mmio_base != d.expected_base)
    {
        return fail(d.tag, "ERROR: cfg mmio_base is not this driver's block\n");
    }

    void* blk = nullptr;
    if (d.block_size != 0u)
    {
        blk = kos_ram_alloc(d.block_size); // ONE power-of-two, naturally aligned: the RAM arm
                                           // of the grant predicate demands it of root too
        ...
        // Reach it before writing it: kos_ram_alloc grants nothing, and under enforcement
        // root's own region set does not cover the arena. `block_flags` (kos_mem_flags)
        // rides BOTH this grant and the task grant below, so a non-cacheable block has no
        // cacheable mapping even during bring-up (docs/design-m4.6.2-usb-cdc.md, S7).
        if (kos_mem_self_grant(blk, d.block_size, d.block_flags) != 0) { ... }
        if (d.block_init(blk, cfg) != 0) { ... }
    }

    kos_cap_t ep = KOS_CAP_NONE;
    if (kos_endpoint_create(&ep) != 0) { ... }

    // PUBLISH BEFORE CLAIM: irq_claim refuses a line while any handler but the default is
    // attached, and only the publish detaches the kernel's own ring from that vector.
    if (d.ep_posture == KOS_DRV_EP_HANDOVER)
    {
        if (kos_console_publish(ep) != 0) { ... }
    }

    kos_cap_t line[KOS_DRV_LINES_MAX];
    uint8_t claimed = 0;
    for (uint8_t i = 0; i < d.line_count; i++)
    {
        // Claimed HERE because minting needs KOS_AUTH_IRQ and every driver thread runs at
        // authority 0. A line comes back MASKED: the waiting thread's first irq_wait arms it.
        if (kos_irq_claim(d.lines[i].number, d.lines[i].trigger, &line[i]) != 0)
        {
            unwind(d, line, claimed, ep, ThreadSet{});
            return fail(d.tag, "ERROR: irq_claim failed\n");
        }
        claimed++;
    }

    ThreadSet peers;
    // thread_count + 1 barrier positions, not thread_count: section 3.3.1 is why the last one
    // has to exist.
    for (uint8_t i = 0; i <= d.thread_count; i++)
    {
        if (d.ready_offset != KOS_DRV_READY_NONE and i == d.barrier_after)
        {
            if (not wait_ready(blk, d.ready_offset))
            {
                unwind(d, line, claimed, ep, peers);
                return fail(d.tag, "ERROR: a driver thread never reached its loop\n");
            }
        }
        if (i == d.thread_count) { break; }
        kos::thread::Handle const h = spawn_one(d, d.threads[i], cfg, blk, ep, line);
        if (not h.valid())
        {
            unwind(d, line, claimed, ep, peers);
            return fail(d.tag, "ERROR: driver thread spawn failed\n");
        }
        peers.add(h);
    }

    // With the driver threads the only holders, a line returns to the pool when they die.
    for (uint8_t i = 0; i < claimed; i++)
    {
        kos_handle_close(line[i]);
    }

    if (d.ep_posture == KOS_DRV_EP_RETAIN)
    {
        *out_ep = ep;
        return 0;
    }
    return console_handover_finish(ep, d.tag, peers);
}
```

`unwind` is the whole point of writing this once. Its order is `rpusb`'s, which is the only one
in the tree that gets it right on every path:

```cpp
inline void unwind(Descriptor const& d, kos_cap_t const* line, uint8_t claimed, kos_cap_t ep,
                   ThreadSet const& peers)
{
    for (uint8_t i = 0; i < claimed; i++)
    {
        kos_handle_close(line[i]);
    }
    // CLOSE BEFORE CANCELLING AND BEFORE PRINTING. Closing takes the endpoint's last receiver
    // holder to 0, which notes the console dead and reclaims it, so the tag the caller prints
    // next reaches the wire; and the note must already be set when a cancelled thread's exit
    // runs the reclaim.
    kos_handle_close(ep);
    peers.cancel_all();
}
```

`spawn_one` resolves the descriptor's resource ids against the caps this bring-up minted and
does nothing else:

```cpp
inline kos::thread::Handle spawn_one(Descriptor const& d, Thread const& t,
                                     struct kos_service_cfg const* cfg, void* blk,
                                     kos_cap_t ep, kos_cap_t const* line)
{
    kos_cap_grant grants[KOS_DRV_CAPS_MAX];
    for (uint8_t i = 0; i < t.cap_count; i++)
    {
        if (t.caps[i].resource == KOS_DRV_RES_EP)
        {
            grants[i].source_cap = ep;
        }
        else
        {
            grants[i].source_cap = line[t.caps[i].resource - KOS_DRV_RES_LINE0];
        }
        grants[i].rights_mask = t.caps[i].rights;
    }

    void* arg = nullptr;
    if (t.arg == KOS_DRV_ARG_BLOCK)
    {
        arg = blk;
    }
    else if (t.arg == KOS_DRV_ARG_WINDOW)
    {
        arg = reinterpret_cast<void*>(cfg->mmio_base);
    }

    void* mem = nullptr;
    uint32_t mem_size = 0;
    if (t.mem_grant)
    {
        mem = blk;
        mem_size = d.block_size;
    }

    void* win = nullptr;
    uint32_t win_size = 0;
    if (t.window_grant)
    {
        win = reinterpret_cast<void*>(cfg->mmio_base);
        win_size = cfg->mmio_window;
    }

    char const* name = t.name;
    if (name == nullptr)
    {
        name = cfg->name;
    }

    // Stack is always the kernel default: zero per-chip stack variation exists across the
    // nine instances. A descriptor field would need to point at a real KOS_STACK_DEFINE
    // buffer, which is expressible, and is left out until one instance needs it.
    return kos::thread::spawn(t.entry, arg, name,
                              static_cast<uint8_t>(cfg->prio + t.prio_delta),
                              KOS_POLICY_FIFO, /*quantum_ns=*/0, /*privileged=*/false,
                              mem, mem_size, /*stack=*/nullptr, /*stack_size=*/0,
                              win, win_size, grants, t.cap_count);
}
```

One generic thread body belongs here too, because it names no class:

```cpp
// A pure cap-to-cap edge converter: wait on one line, post another. PRECONDITION, and leg L5
// is what enforces it: the waited line must be EDGE. This thread holds no window, so it cannot
// clear a peripheral flag, and on a LEVEL source it would rearm into a still-asserted line.
inline void edge_relay_thread(void*)
{
    while (true)
    {
        if (kos_irq_wait(KOS_SPAWN_DELEGATED_CAP0) != 0)
        {
            break; // the cap went away: no line left to relay
        }
        (void)kos_irq_notify(KOS_SPAWN_DELEGATED_CAP0 + 1);
    }
    exit(0);
}
```

`rxsci`'s third thread stops being a `rxsci` fact and becomes a reusable shape, which is the
right outcome: "wait here, notify there" names no peripheral.

### 3.5 The readiness latch, without a template

The generic bring-up must poll a `volatile uint32_t` inside a block whose type it cannot name.
The obvious answer is a template helper; the better answer is that **the class substrate already
knows the offset and can state it as a constant**:

```cpp
// in sys/uart_service.h, beside Shared
struct Ctx
{
    struct Shared sh;
    struct kos_uart_config ucfg;
};
static_assert(sizeof(Ctx) <= KOS_UART_BLOCK_SIZE,
              "the UART driver context must fit the 1 KiB shared grant");

constexpr uint16_t KOS_UART_READY_OFFSET =
    static_cast<uint16_t>(offsetof(Ctx, sh) + offsetof(Shared, ready));
```

`usb_cdc_service.h` gets `KOS_USB_READY_OFFSET` the same way. No descriptor ever contains a
hand-written byte offset, and the new header stays template-free. The poll itself:

```cpp
// The latch MUST be a `volatile uint32_t` at this offset, which is why the offset comes from a
// class-substrate constant and never from a descriptor literal.
constexpr uint32_t KOS_DRV_READY_WAIT_NS = 1000000u; // 1 ms
constexpr uint32_t KOS_DRV_READY_WAIT_MAX = 1000u;   // ~1 s total

inline bool wait_ready(void const* blk, uint16_t off)
{
    volatile uint32_t const* const flag = reinterpret_cast<volatile uint32_t const*>(
        static_cast<unsigned char const*>(blk) + off);
    // Sleeping, not spinning: the IRQ thread may sit below root's priority.
    for (uint32_t i = 0; i < KOS_DRV_READY_WAIT_MAX; i++)
    {
        if (*flag != 0u)
        {
            return true;
        }
        kos_sleep_ns(KOS_DRV_READY_WAIT_NS);
    }
    return *flag != 0u;
}
```

The bound unifies at 1000 steps. Today `c6uart` and `rpusb` use 1000 and `rxsci` and the sim
use 500; the change only lengthens a failure path, never a success one.

`Ctx` moving into the substrate deletes the same struct plus the same `static_assert` from five
per-chip files.

### 3.6 What deliberately does NOT go in the descriptor

Two fields the evidence inventory proposed, relocated. Both are judgment calls.

**`prime_before_first_wait` goes in a class-side params struct, not the descriptor.** It is not
a bring-up fact: it is read inside the IRQ thread body, between `kos_uart_open` and
`irq_loop`. Putting it in seam A would make the class-agnostic descriptor carry a UART concept.
It is genuinely not derivable from the trigger mode (`xmcuartirq` is EDGE and DOES prime), so it
gets its own field, in `uart_service.h`:

```cpp
struct UartParams
{
    char const* open_fail; // the kos_panic tag; open failure must never exit()
    char const* announce;  // direct-to-device marker, legal only before the first irq_wait
    bool prime;            // call irq_pass once before the first wait
};

// Absorbs win_puts (identical in three files) and the open/panic/announce/prime/loop sequence
// (identical in five). NEVER exits on an open failure: once root has closed its own cap the
// service thread is the endpoint's sole receiver and would keep accepting stdout into a ring
// nothing drains, so the panic path is what reclaims the console.
template <typename Uart>
void irq_thread(Ctx* ctx, UartParams const& p);
```

**`default_baud` goes in the per-chip `block_init`, not the descriptor.** Only `k64uartirq`
substitutes 115200 for `hz == 0`. `block_init` is already the one hook whose job is filling the
class config from the cfg, and the driver-model ruling's rule 4 keeps class vocabulary out of
service vocabulary in both directions. It costs one `if` in an eight-line function.

## 4. Every instance, expressed

`c6uart.cc` in full, as the archetype. This is the whole file:

```cpp
// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-C6 UART0 buffered userspace UART driver.
//
// The grouped UART0 line cannot be claimed while the kernel's own TX ring holds it, so the
// publish MUST precede the claim: that ordering is KOS_DRV_EP_HANDOVER's, not this file's.

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/driver/uart.h>
#include <kickos/sys/driver_service.h>
#include <kickos/sys/service.h>
#include <kickos/sys/uart_service.h>

#include "irq.h"
#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace drv = kickos::driver;
namespace uart = kickos::uart;
namespace mmap = kickos::esp32c6::mmap;
namespace c6irq = kickos::esp32c6::irq;

namespace
{
    constexpr uart::UartParams k_uart = {
        .open_fail = "[c6uart] UART0 open refused: source clock, divisor or frame",
        .announce = "[c6uart] device up (IRQ TX/RX)\n",
        .prime = true
    };

    // No kos_periph_enable: the PMP grant carries the window, PCR leaves UART0's bus clock
    // ungated out of reset, and arch_init's HP_APM REE0 permit already covers the block.
    void irq_entry(void* arg)
    {
        uart::irq_thread<struct kos_uart>(static_cast<uart::Ctx*>(arg), k_uart);
    }

    int block_init(void* blk, struct kos_service_cfg const* cfg)
    {
        // hz travels as the REQUESTED baud; 0 keeps the divisor the ROM left.
        return uart::ctx_init(static_cast<uart::Ctx*>(blk), cfg, /*fallback_baud=*/0u);
    }

    constexpr drv::Descriptor k_desc = {
        .tag = "[c6uart] ",
        .expected_base = mmap::UART0_BASE,
        .block_size = uart::KOS_UART_BLOCK_SIZE,
        .ready_offset = uart::KOS_UART_READY_OFFSET,
        .ep_posture = drv::KOS_DRV_EP_HANDOVER,
        .svc_kind = KOS_SVC_CONSOLE,
        .line_count = 1,
        .thread_count = 2,
        .barrier_after = 1,
        // LEVEL: the UART source stays asserted until the driver clears the latch.
        .lines = { { c6irq::UART0_TX_LINE, KOS_IRQ_LEVEL } },
        .threads = {
            { .entry = irq_entry, .name = "c6uartirq", .prio_delta = 1,
              .arg = drv::KOS_DRV_ARG_BLOCK, .mem_grant = true, .window_grant = true,
              .cap_count = 1,
              .caps = { { drv::KOS_DRV_RES_LINE0, KOS_CAP_WAIT } } },
            { .entry = uart::console_thread, .name = nullptr, .prio_delta = 0,
              .arg = drv::KOS_DRV_ARG_BLOCK, .mem_grant = true, .window_grant = false,
              .cap_count = 2,
              .caps = { { drv::KOS_DRV_RES_EP, KOS_CAP_WAIT },
                        { drv::KOS_DRV_RES_LINE0, KOS_CAP_SIGNAL } } }
        },
        .block_init = block_init
    };

    static_assert(drv::valid(k_desc), "the c6uart descriptor is not a well-formed driver shape");
    static_assert(uart::desc_ok(k_desc), "the c6uart cap positions do not match KOS_UART_CAP_*");
}

extern "C"
{

int c6uart_console_start(struct kos_service_cfg const* cfg)
{
    return drv::bring_up(k_desc, cfg, nullptr);
}

}
```

**The four two-thread UARTs** are that file with four things changed: the tag, the
`expected_base`, the line number and trigger, and the `UartParams`. `lx6uart` differs from
`c6uart` in nothing else. `k64uartirq` additionally passes `/*fallback_baud=*/115200u` and gains
an `expected_base` it does not have today. `xmcuartirq` uses `KOS_IRQ_EDGE` and also gains an
`expected_base`.

**`rxsci`, three threads and two lines.** All four of the things a two-thread descriptor cannot
say, said:

```cpp
    constexpr uart::UartParams k_uart = {
        .open_fail = "[rxsci] SCI6 open refused: source clock, divisor or frame",
        .announce = "[rxsci] device up (IRQ TX/RX)\n",
        // NO prime, unlike the LEVEL-lined backends: TXI's only raise is a transfer taken with
        // the source already armed, so a pass that stopped with the ring loaded would wait on
        // a transition that has already happened.
        .prime = false
    };

    constexpr drv::Descriptor k_desc = {
        .tag = "[rxsci] ",
        .expected_base = mmap::SCI6,
        .block_size = uart::KOS_UART_BLOCK_SIZE,
        .ready_offset = uart::KOS_UART_READY_OFFSET,
        .ep_posture = drv::KOS_DRV_EP_HANDOVER,
        .svc_kind = KOS_SVC_CONSOLE,
        .line_count = 2,
        .thread_count = 3,
        .barrier_after = 2,
        // Dedicated SCI6 vectors with their own INTB slot, both EDGE: a raise taken while the
        // line is masked latches and redelivers on the rearm.
        .lines = { { SCI6_TXI_LINE, KOS_IRQ_EDGE }, { SCI6_RXI_LINE, KOS_IRQ_EDGE } },
        .threads = {
            { .entry = irq_entry, .name = "rxsciirq", .prio_delta = 1,
              .arg = drv::KOS_DRV_ARG_BLOCK, .mem_grant = true, .window_grant = true,
              .cap_count = 1,
              .caps = { { drv::KOS_DRV_RES_LINE0, KOS_CAP_WAIT } } },
            { .entry = drv::edge_relay_thread, .name = "rxscirx", .prio_delta = 1,
              .arg = drv::KOS_DRV_ARG_NONE, .mem_grant = false, .window_grant = false,
              .cap_count = 2,
              .caps = { { drv::KOS_DRV_RES_LINE1, KOS_CAP_WAIT },
                        { drv::KOS_DRV_RES_LINE0, KOS_CAP_SIGNAL } } },
            { .entry = uart::console_thread, .name = nullptr, .prio_delta = 0,
              .arg = drv::KOS_DRV_ARG_BLOCK, .mem_grant = true, .window_grant = false,
              .cap_count = 2,
              .caps = { { drv::KOS_DRV_RES_EP, KOS_CAP_WAIT },
                        { drv::KOS_DRV_RES_LINE0, KOS_CAP_SIGNAL } } }
        },
        .block_init = block_init
    };
```

Read the four claims off it:

1. **Two lines, different roles.** `lines[0]` (TXI 87) is WAIT for the IRQ thread and SIGNAL for
   two others; `lines[1]` (RXI 86) is WAIT for the relay only. Per-line trigger, per-thread
   rights mask, both present.
2. **The relay carries no grant and no arg.** `arg = ARG_NONE`, and at M4.8.1 a per-thread memory
   flag saying the same. **That second half did not survive the task layer** and the flag is gone as
   of M4.8.4: the block is the group's region and every member sees it, the relay included. Leg L3
   still keeps the window where it belongs, and the window is the grant that matters here.
3. **A third private cap-role set.** The relay's `caps[0]`/`caps[1]` are child indices 1 and 2,
   the same two indices the service thread uses, naming different objects. Because `caps[]` is
   per thread, this is not a special case; it is the ordinary reading of the field.
4. **Which side of the barrier.** `barrier_after = 2`, so the IRQ thread and the relay are
   spawned before the readiness poll and the service thread after. The relay may be there
   because it holds no WAIT cap on the endpoint, so it does not defeat the "root is still the
   sole receiver" argument, and leg L8 is exactly that argument made structural.
5. **The LEVEL refusal.** `edge_relay_thread` holds WAIT on `lines[1]` and has no window, so
   leg L5 requires `lines[1].trigger == KOS_IRQ_EDGE`. Change it to LEVEL and the file does not
   compile. That was a comment.

**Both SPI shapes.** `k64dspi.cc`, whose five differences are five field values:

```cpp
    constexpr drv::Descriptor k_desc = {
        .tag = "[k64dspi] ",
        // NO base guard: spi_dspi.cc is genuinely base-parameterised across DSPI0/1/2, and no
        // vector is claimed by number, so there is nothing to pin the cfg against.
        .expected_base = 0,
        .block_size = 0,     // no Shared, no ring, no doorbell, no readiness flag
        .ready_offset = drv::KOS_DRV_READY_NONE,
        .ep_posture = drv::KOS_DRV_EP_RETAIN,
        .svc_kind = KOS_SVC_SPI,
        .line_count = 0,     // the DSPI pump polls its FIFOs
        .thread_count = 1,
        .barrier_after = 1,
        .lines = {},
        .threads = {
            { .entry = k64dspi_service, .name = nullptr, .prio_delta = 0,
              .arg = drv::KOS_DRV_ARG_WINDOW, .mem_grant = false, .window_grant = true,
              .cap_count = 1,
              .caps = { { drv::KOS_DRV_RES_EP, KOS_CAP_WAIT } } }
        },
        .block_init = nullptr
    };
```

with `k64dspi_spi_start` becoming `return drv::bring_up(k_desc, cfg, &g_spi0_ep);`. Per-device
state stays on the driver thread's stack, in `spi_service.h`'s `serve_loop`, because the
descriptor never mentions it. `xmcssc.cc` is the same with `line_count = 1`,
`.lines = { { USIC0_SR1_IRQ, KOS_IRQ_EDGE } }`, `caps = { {EP, WAIT}, {LINE0, WAIT} }`, and an
`expected_base` of the U0C1 base that leg L9 now requires and the file does not have today.

Note what does NOT appear: `cap_count` is data in the descriptor, so
`spawn_unprivileged`'s `if (irq_cap != KOS_CAP_NONE) { cap_count = 2; }` branch disappears
rather than being generalised.

**The sim loopback.** The instance that kills any descriptor demanding a window, a publish, or
a `struct kos_uart` backend:

```cpp
        .tag = "[simuart] ",
        .expected_base = 0,
        .block_size = uart::KOS_UART_BLOCK_SIZE,
        .ready_offset = uart::KOS_UART_READY_OFFSET,
        .ep_posture = drv::KOS_DRV_EP_RETAIN,   // no kos_console_publish, no handover tail
        .svc_kind = KOS_SVC_UART,               // not KOS_SVC_CONSOLE
        .line_count = 1,
        .thread_count = 2,
        .barrier_after = 1,
        .lines = { { SIMUART_LINE, KOS_IRQ_EDGE } },
        .threads = {
            { .entry = sim_irq_entry, .name = "uartirq", .prio_delta = 1,
              .arg = drv::KOS_DRV_ARG_BLOCK, .mem_grant = true,
              .window_grant = false,           // mmio_base is 0 and there is no window
              ... },
            ...
        },
```

`window_grant = false` is what exempts it from leg L9, and it is honest: the sim IRQ thread
takes no window. Its backend keeps `service_irq()` and reaches the substrate through
`uart_service.h`'s transitional template overload, which is the only thing that overload is
still for on this tree (its comment naming `rxsci` is stale: `arch/rx/chip/rx72m` implements the
full five-call class now).

**One correction the sim's own comment invites.** `service_list_uart.cc:207-211` justifies its
barrier partly on reportability. Under `RETAIN` root keeps a WAIT-bearing cap forever, so
`recv_holders` never reaches 0 and the console is never darkened in the first place: that half
does not apply. The other half does, and is enough: no request may be served against a device
that is not yet configured. Leg L8 keeps the barrier where it is for both postures, on the
weaker premise.

**`rpusb`.** The descriptor holds it, and it is in scope. `block_size = 2048`,
`ready_offset = usb::KOS_USB_READY_OFFSET`, `HANDOVER`, `line_count = 1` LEVEL,
`expected_base = reg::DPRAM_BASE`, two threads, `barrier_after = 1`. Identical choreography, a
different block size and a different `Shared` type, which is exactly what the two fields are
for.

Three `rpusb` facts that look like blockers and are not, because the descriptor never touches
the write path:

- **It is a DROP-ON-FULL sink and must never block on the link** (`rpusb.cc:24-26`), where the
  UART substrate's `push_all` retries for ~200 ms (`uart_service.h:204-205`). Sharing that path
  would hang an un-cabled board. Nothing here shares it: the write policy lives in
  `usb_cdc_service.h`'s own `console_serve_loop`, one per class, and the bring-up routes no
  bytes.
- **Its console is a DIFFERENT PERIPHERAL from the one it drives** (`rpusb.cc:429-432`), which
  falsifies the premise every UART tail rests on. The correct behaviour, falling back to
  KERNEL_OWNED on driver death rather than reclaiming, is not implemented on any tree. The
  descriptor does not need a flag for it today, and adding one now would be a knob with one
  value; when that feature lands it is a third `ep_posture`, which is where it belongs.
- **There is no `driver/usb.h`**, so the USB "class" is a C++ template concept
  (`usb_cdc_service.h`'s `Cdc<UsbDev>`) rather than a set of `kos_usb_*` C symbols. That means
  `check_class_backend.sh` derives no USB symbols and the class-versus-service split is
  unenforced for USB. Orthogonal to this ruling, and unchanged by it.

**Summary table.** The instance list is `grep -rln 'drv::valid' system user`; read that first,
because the table below is a snapshot and the set grows with every new driver. It was **CORRECTED
from nine to twelve** when the three one-thread `HANDOVER` consoles turned up missing, and has since
grown to FOURTEEN with `f4uartirq` and `rt1062usb`. One descriptor per instance, no exceptions:

| instance | thr | lines | block | barrier | ep | base guard | window |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `c6uart` | 2 | 1 LEVEL | 1024 | after 1 | HANDOVER | UART0 | thread 0 |
| `lx6uart` | 2 | 1 LEVEL | 1024 | after 1 | HANDOVER | UART0 | thread 0 |
| `k64uartirq` | 2 | 1 LEVEL | 1024 | after 1 | HANDOVER | UART0 (new) | thread 0 |
| `xmcuartirq` | 2 | 1 EDGE | 1024 | after 1 | HANDOVER | U0C0 (new) | thread 0 |
| `rxsci` | 3 | 2 EDGE | 1024 | after 2 | HANDOVER | SCI6 | thread 0 |
| `simuart` | 2 | 1 EDGE | 1024 | after 1 | RETAIN | none | none |
| `k64dspi` | 1 | 0 | none | none | RETAIN | none | thread 0 |
| `xmcssc` | 1 | 1 EDGE | none | none | RETAIN | U0C1 (new) | thread 0 |
| `rpusb` | 2 | 1 LEVEL | 2048 | after 1 | HANDOVER | DPRAM | thread 0 |
| `k64uart` | 1 | 0 | none | none | HANDOVER | none | thread 0 |
| `xmcuart` | 1 | 0 | none | none | HANDOVER | none | thread 0 |
| `simcon` | 1 | 0 | none | none | HANDOVER | none | none |
| `f4uartirq` | 2 | 1 LEVEL | 1024 | after 1 | HANDOVER | USART2 | thread 0 |
| `rt1062usb` | 2 | 1 LEVEL | 4096 | after 1 | HANDOVER | USB1 | thread 0 |

## 5. The tail, and the Task

`console_handover_finish` takes one optional `Handle` today, and its doc makes a
default-constructed handle mean "single-thread driver" (`driver_bringup.h:96,107-108`). Three
multi-thread services therefore declare themselves single-thread by omission:
`c6uart.cc:251`, `lx6uart.cc:251`, `rxsci.cc:328`. For `rxsci` **there is no correct call**:
passing `irqt` fixes the reported failure, because `irqt` holds the register window and that is
what blocks the console reclaim, but it still leaks `relayt`, which holds WAIT on RXI6 and
SIGNAL on TXI6 after root has dropped both caps (`rxsci.cc:326-327`), so both lines stay out of
the pool for the rest of the run.

**The replacement is `ThreadSet const&` (section 3.2), built by the bring-up from the
descriptor's thread list.** Omission stops being expressible: the set is whatever was spawned.
`rxsci` gets three entries because its descriptor has three threads. This is the strongest
evidence in the tree that topology must be a first-class list, and the generic service fixes it
once instead of three times.

### 5.1 Where this sits relative to the Task ruling

`design-task-layer.md` rules that a `Task` type should exist (a set of threads), that the
address space attaches to `Domain` rather than to `Task`, that a thread naming no task gets an
implicit task of one, and that it landed **after** the host unit-test layer. Its section 1.1 is
this exact defect, found independently.

**Explicitly: the `ThreadSet` is not the Task in embryo. It is the hand-rolled emulation the
Task later absorbs, completed.** The distinction matters and it is what keeps this design from
pre-empting that ruling. As 3.4's callout above already flags, M4.8.3's step 9.4 absorbed it
WHOLE, at the one call site this design had already collapsed it to -- confirming this section's
prediction rather than contradicting it. The contrast still reads correctly against the shape it
replaced:

- `ThreadSet` is a userspace array of handles on root's stack. No kernel object stands behind
  it, no syscall takes it, and no thread's fate is coupled to another's. The spike's section 6
  (task-scoped death) and its section 9.5 (the group kill) are untouched and unanticipated.
- Every spawn still names no task, so under the spike's 5.3 default each driver thread remains
  an implicit one-thread task, bit for bit. This change alters no spawn call site's meaning.
- The spike's 5.2 refusal is **preserved structurally rather than by discipline**: a task owns
  the window's lifetime, the thread that asked owns its access. Leg L3 permits exactly one
  `window_grant`, so nothing here widens a WINDOW to fix a lifetime problem. **The memory half of
  this bullet is false from M4.8.3 on**: the block became the group's region, so the relay does get
  it. The service thread still gets no window, and a descriptor that changed that still fails to
  compile.
- **What the Task later deletes:** `ThreadSet`, `cancel_all`, the tail's `peers` parameter and
  the `Handle::kill()` sites. Under a Task, `bring_up` creates one task, spawns the descriptor's
  N threads into it, and the tail names the task instead of the set. The spike's 9.4 ("the six
  multi-thread drivers become one task each") reduces to changing one function, because the
  membership declaration already exists as data.
- **What the Task does not delete:** the descriptor itself. The line list, the per-thread cap
  and rights vectors, the barrier position, the `ep_posture` and the base guard are grant and
  choreography facts a task object does not answer, and would still be needed to create the
  task in the first place.

So the descriptor's thread list is the **input** a future task creation reads. That is a
narrower claim than "the Task in embryo", and it is the one this design can make without
touching the kernel.

## 6. Where the code lives

**Created, one file.**

| file | lines | contents |
| --- | --- | --- |
| `user/include/kickos/sys/driver_service.h` | ~330 | `Cap`, `Line`, `Thread`, `Descriptor`, `ThreadSet`, `valid()`, `bring_up()`, `spawn_one()`, `unwind()`, `wait_ready()`, `edge_relay_thread()`, `console_handover_finish()` |

**Dies, one file.** The old driver bring-up header, 131 lines, and it is GONE as of step 7 -- so it
is named here in prose rather than cited, because a path that no longer resolves is a doc bug.
Its spawn helper is absorbed by `spawn_one`; its handover tail becomes
`console_handover_finish` with a `ThreadSet` in place of the single thread handle; its probe-timeout
constant moves; and its two generic driver cap indices become `KOS_SPI_CAP_EP` and
`KOS_SPI_CAP_LINE` in `spi_service.h`, mirroring what `uart_service.h` already does for UART.
Nothing is left.

**It had FOUR users, not one**, which section 0's count of nine instances is what hid: besides
`rpusb`, the three one-thread console drivers `k64uart`, `xmcuart` and the sim's `simcon`. All four
are converted.

**Grows, to absorb what nine files each held a copy of.**

| file | lines | change |
| --- | --- | --- |
| `sys/uart_service.h` | 426 -> ~510 | gains `Ctx` (from 5 files), `KOS_UART_READY_OFFSET`, `ctx_init`, `UartParams`, `irq_thread` (absorbing `win_puts` from 3 files and the open/announce/prime/loop sequence from 5), `console_thread`, `console_serve_loop` (the console arm 5 service threads open-code), `desc_ok` |
| `sys/spi_service.h` | 233 -> ~250 | gains the cap-role enum and `desc_ok` |
| `sys/usb_cdc_service.h` | 780 -> ~790 | gains `KOS_USB_READY_OFFSET` and `desc_ok` |

**Shrinks to a descriptor.** File names are unchanged: the rename is subsumed by this change,
and the class backends (`uart_c6.cc`, `uart_lx6.cc`, `uart_k64.cc`, `uart_usic.cc`,
`uart_sci.cc`, `spi_dspi.cc`, `spi_usic.cc`) keep their names, which are already right.

| file | now | after | what remains |
| --- | --- | --- | --- |
| `esp32c6/c6uart/c6uart.cc` | 255 | ~70 | descriptor, `UartParams`, 2 wrappers, `start` |
| `esp32/lx6uart/lx6uart.cc` | 255 | ~70 | as above |
| `mk64f/k64uartirq/k64uartirq.cc` | 262 | ~72 | plus the 115200 fallback |
| `xmc4800/xmcuartirq/xmcuartirq.cc` | 267 | ~72 | as above |
| `rx72m/rxsci/rxsci.cc` | 332 | ~90 | 3 threads, 2 lines |
| `mk64f/k64dspi/k64dspi.cc` | 106 | ~60 | `take_endpoint`, the bus thread body, descriptor |
| `xmc4800/xmcssc/xmcssc.cc` | 138 | ~65 | as above |
| `rp2xxx/rpusb/rpusb.cc` | 521 | ~420 | the ~100-line bring-up collapses; the USB device layer stays |
| `system/init/sim/service_list_uart.cc` | 271 | ~120 | `LoopUart` stays; the bring-up, the cfg and the list |

**Unchanged.** Every class backend, `driver/uart.h`, `driver/spi.h`,
`tests/static/check_class_backend.sh`, `cmake/kickos.cmake`, and every driver `CMakeLists.txt` except
`rxsci`'s (see risk R5).

**Totals.** Per-chip service code 2307 -> ~1039. Substrate 1570 -> ~1880. Net about **-1000
lines**, but the number that matters is that the bring-up choreography goes from **eight copies
to one**, and the next chip adds ~70 lines instead of ~260.

## 7. The build shape

**Header-only, no new target, and the reason is size.** `bring_up` is `inline` in
`driver_service.h` and compiles into each per-chip driver target. Three things follow:

- **REGDIR stays private.** The descriptor is authored in the per-chip TU, which already has
  REGDIR on its include path via `kickos_add_driver(... REGDIR ...)`. The generic header
  includes no chip header, so no chip register header ever reaches a shared TU. This is the
  whole reason the design is data-driven rather than a shared library with chip hooks.
- **The SPI class-symbol rename is untouched, and this is the sharpest constraint.** The rename
  is `target_compile_definitions(kickos_k64dspi PRIVATE kos_spi_bus_open=k64dspi_bus_open ...)`,
  per target. A single generic library shared across chips could not carry one rename set.
  It does not have to: the generic bring-up calls no `kos_spi_*` symbol at all. Every class
  call still compiles inside the per-chip target, from `spi_service.h` (inline) and from the
  thread body, both under that target's own definitions. **A generic SPI service therefore
  remains one target per chip**, which it must be anyway for REGDIR and for the line numbers,
  and the rename does not move.
- **The gates are unaffected.** `check_class_backend.sh` derives its symbol set from
  `user/include/kickos/driver/*.h`, so a future `i2c.h` or `usb.h` extends it automatically and
  the new header, living in `sys/`, contributes nothing. Its leg 3 flags an object defining a
  public `kos_*` symbol; `bring_up` is `kickos::driver::bring_up`, C++-mangled, so leg 3 stays
  green. `kickos_service_libs_closure` BFS-walks `LINK_LIBRARIES` and this change adds no edge,
  so the rescan archive group is unchanged.

**The ABI is unchanged.** One `extern "C" int <name>_<kind>_start(struct kos_service_cfg const*)`
per instance, declared by hand in the provider TU. The symbol name IS the ABI and every
`start()` keeps its name; each body becomes one `return drv::bring_up(...)`.

**Size, measured not assumed.** Nine inline copies exist only in principle: a board links one
console service and at most one bus service, so a real image carries one or two copies of
~200 lines of code, against the ~130 lines of hand-written bring-up each service carries today.
Size-neutral to slightly better, and that must be measured on the smallest board that links a
service before the last conversion lands. **Fallback if it is not:** one
`kickos_driver_service` STATIC target carrying `bring_up`, `spawn_one`, `unwind` and
`wait_ready` as non-inline functions taking the descriptor by const reference. It still calls no
class symbol, so the rename answer above is unchanged, and the only cost is one new edge in the
closure walk.

## 8. Order of work, and what falsifies the seam

The instruction is to prove it on UART first, because five instances make a wrong seam show up
immediately. But `rxsci` and SPI are variation points the descriptor must express, not
exceptions to route around: **if it cannot hold both, it is the wrong abstraction and the work
stops.**

**Step 1: seam B only.** Move `Ctx`, `win_puts`, the open/announce/prime/loop sequence and the
console arm of the service thread into `uart_service.h`, and convert the five UART services'
**thread bodies** while leaving every bring-up byte-for-byte alone. Removes ~200 duplicated
lines, changes no choreography, is independently fleet-gateable, and is a clean revert point.
Why first: it shrinks step 2's diff to the bring-up alone, which is the part that needs the
witness.

**Step 2: the descriptor, on the sim first.** `driver_service.h`, `valid()`, `bring_up()`, and
`service_list_uart.cc` as the first conversion. The sim is the only host-runnable instance,
which is exactly the leverage the M4.8.2 host unit-test layer depends on, and it exercises the
`RETAIN` / no-window / no-publish / non-CONSOLE arms in one go. If `bring_up` has a bug, it
fails on a host in seconds instead of on a bench.

**Step 3: `c6uart` and `lx6uart`.** The 223-of-255-verbatim pair. Tripwire: after conversion
their two files must differ in **exactly four values** (tag, `expected_base`, line number,
`UartParams`). Any fifth difference is the seam leaking.

**Step 4: `rxsci`. The first falsification test.** Three threads, two lines with different
triggers and roles, a thread with no grant and no arg, a third private cap-role set, a
pre-barrier spawn, and a LEVEL refusal. If any of those needs a descriptor field with exactly
one user, or a branch inside `bring_up`, **stop**: the descriptor is a UART abstraction wearing
a different name.

**Step 5: both SPI. The second falsification test.** Arg-as-value, zero block, zero lines on
one and one line on the other, `RETAIN`, no barrier, no tail. Same tripwire.

**Step 6: `k64uartirq` and `xmcuartirq`.** Held until here because they gain an `expected_base`
they do not have today, so their diff carries a behaviour change and wants its own witness.

**Step 7: `rpusb`.** Last. Its bring-up is the same shape, but it is the only instance whose
console is a disjoint peripheral and whose correct tail behaviour is unimplemented, so it should
convert against a seam that six other instances have already witnessed.

**The tripwire, stated once.** A descriptor field whose value is the same in every instance but
one is a special case in disguise. Two are legitimate today and both are named: `arg` and
`UartParams::prime` (four true, one false, and it lives in seam B where the class already varies).
A third appearing during steps 4 to 6 is the signal to stop.

**CORRECTED count for `arg`.** This section said "seven `BLOCK`, two `WINDOW`", which predates the
nine-to-twelve correction of section 3.3. Re-derive it with
`grep -o 'KOS_DRV_ARG_[A-Z]*' <each descriptor file>`; over the fourteen instances that stands today
it is **nine `BLOCK`** (`c6uart`, `lx6uart`, `k64uartirq`, `xmcuartirq`, `rxsci`, `simuart`, `rpusb`,
`f4uartirq`, `rt1062usb`), **four `WINDOW`** (`k64dspi`, `k64uart`, `xmcssc`, `xmcuart`) and **one
`NONE`** (`simcon`);
`rxsci`'s relay thread is `NONE` too, which is why the field varies WITHIN an instance as well as
across them. Three distinct values makes it more clearly not a special case, not less.

### 8.1 Risks

- **R1. The `ready_offset` reinterpret_cast is the one type-unsafe thing in the design.**
  Mitigated by section 3.5: the offset is a `constexpr` in the class substrate computed with
  `offsetof`, never a literal in a descriptor, and leg L8 bounds it inside the block. A wrong
  offset would poll a byte that is never written and time out loudly rather than corrupt.
- **R2. `valid()` needs constexpr-evaluable line numbers.** Every line number in the tree today
  is a `constexpr` chip constant, so every `static_assert` resolves. An instance whose vector is
  runtime data keeps a runtime `valid()` call at the top of `bring_up`, which should exist as a
  belt anyway.
- **R1.1. The belt R2 called for did NOT exist, and now does.** `valid()` was enforced only by a
  `static_assert` each driver writes BY HAND, while `bring_up` indexes
  `kos_cap_t line[KOS_DRV_LINES_MAX]` by `d.line_count` and `ThreadSet::t[KOS_DRV_THREADS_MAX]` by
  the spawn count. A driver that omitted the assert, or a descriptor that stopped being `constexpr`
  (exactly R2's case), got an out-of-bounds WRITE on ROOT's stack. It is now the first statement of
  `bring_up`, returning the same `fail()` as every other bring-up error, and the `drv_bringup` arm
  that witnesses it observed the defect directly: with the belt neutralised, a `thread_count` of 4
  produces `spawn50 spawn51 spawn52 spawn53` into a 3-slot `ThreadSet`.
  **"A constexpr branch costs nothing" is FALSE as measured**, so the number is recorded here
  rather than the claim: GCC 15.3 at `-Os` inlines `valid()` into `bring_up` but does not
  constant-propagate the descriptor through it. Per image linking a bring-up: **+352 B on
  frdmk64f** (`k64console` 66048 -> 66400), **0 B on rx72m, esp32c6-wroom, esp32-wroom, microbit and
  picopi**, and 77 of 88 images unchanged. On xmc4800-relax three images (`blink`, `hello_c`,
  `libc_exit`) grow **+32768 B**, which is NOT code: their `.text` sat within 352 B of `0x08008000`
  and `.appdata` carries `ALIGN(32768)`, so its LMA steps to `0x08010000` and `objcopy -O binary`
  zero-fills the gap. The driver-bearing xmc image, `consoledemo`, is unchanged.
- **R3. Flash cost of nine inline copies.** Addressed in section 7, with a measurement
  obligation and a named fallback.
- **R4. The legs encode today's choreography.** A future class needing two barriers, two
  windows, or two receivers falsifies L3, L6 or L8. That is what a ruling is for: the leg is a
  compile error naming the assumption, at the descriptor that broke it, rather than a silent
  misbehaviour on a bench.
- **R5. `rxsci`'s bench trace hooks sit inside the seam.** `RXSCI_TRACE` and `RXSCI_LED_TRACE`
  (`rxsci.cc:312-323`) run between the last spawn and the tail, which `bring_up` now owns.
  **Judgment call: delete both, and the `foreach` in `rxsci/CMakeLists.txt:24-28` that plumbs
  them.** Reason: they are localizers for a bug that is closed, and the alternative is a
  `post_spawn` hook in the descriptor with exactly one user, which is the tripwire above.
- **R6. Two witnesses, not one.** Steps 1 and 2 are separately gateable, which means two fleet
  passes rather than one. Accepted: the alternative is one diff that changes both the thread
  bodies and the choreography of five silicon services at once.

## 9. What this fixes for free

Not parameterised. Absorbed, because there is one bring-up left to be right.

1. **The three omitted handles.** `c6uart.cc:251`, `lx6uart.cc:251` and `rxsci.cc:328` pass no
   thread handle and so declare themselves single-thread, leaking a live IRQ thread and its
   register window on a failed handover, on the exact path that reports it. The tail now takes
   the set the bring-up already built; omission is not expressible.
2. **`rxsci`'s unfixable case.** No argument to today's tail is correct, because `irqt` fixes
   the reported failure and `relayt` still leaks two lines. Three threads in, three handles
   cancelled.
3. **`c6uart`'s print-then-close order.** `c6uart.cc:189-190` prints then closes the endpoint on
   an `irq_claim` failure, where all four others close first. The console is USER_OWNED from the
   publish on, so its tag is dropped. `unwind` has one order and it is the correct one.
4. **The three missing `.kill()` sets.** `c6uart`, `lx6uart` and `rxsci` cancel nothing on the
   readiness timeout or on a service-spawn failure, where `k64uartirq`, `xmcuartirq` and `rpusb`
   cancel `irqt` on both. One unwind path sweeps everything spawned so far, in reverse order,
   unconditionally.
5. **The two missing `expected_base` guards, and a third.** `k64uartirq` and `xmcuartirq` have
   no guard where `c6uart`, `lx6uart`, `rxsci` and `rpusb` do, which the inventory reads as
   omissions rather than decisions. Leg L9 makes the omission a compile error, and applying the
   leg found a third: **`xmcssc` claims USIC0_SR1 by number and pins no base either.**
6. **The barrier placement.** "Strictly between the two spawns, and that placement is
   load-bearing" is a comment in six files and an invariant in none. Leg L8.
7. **The relay's LEVEL precondition.** A comment in `rxsci.cc:104-105`. Leg L5, and it now
   guards every future relay rather than the one that documented it.
8. **The unified readiness bound.** Four different pairs of `READY_WAIT_*` constants across the
   instances collapse to one, and a `500` becomes `1000`, which lengthens only a failure path.

## 10. What this ruling deliberately does not do

- **It does not change a class contract.** `driver/uart.h`'s five calls and `driver/spi.h`'s
  four are stable and untouched, as are all seven backends. The 1:1 rule
  (`design-m4-driver-model.md`) is unaffected: the service is still a transport over the class,
  and this only stops writing that transport nine times.
- **It does not rename a service file.** The rename is subsumed by this change.
- **It does not couple thread fates, add a syscall, or name a task.** Section 5.1.
- **It does not touch the write path.** Every retry, drop and CRLF policy stays in its class
  substrate, one per class, which is what keeps `rpusb`'s drop-on-full sink and the UART's
  ~200 ms `push_all` from ever meeting.
- **It does not answer the disjoint-console fallback.** `rpusb`'s console is a different
  peripheral from the one it drives, and falling back to KERNEL_OWNED on driver death is
  unimplemented. When it lands it is a third `ep_posture`.
- **It does not retire `uart_service.h`'s transitional template.** `irq_loop<Uart>` exists only
  because the sim loopback carries `service_irq()` instead of the five-call class. Once the
  M4.8.2 host unit-test layer gives the sim a real class backend, the overload at
  `uart_service.h:128-136` and the template both go, and `irq_loop(struct kos_uart&, Shared*)`
  becomes a plain function. That is the one place where a template survives this design, and it
  survives for a reason that has an expiry date.
