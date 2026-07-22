<!-- SPDX-License-Identifier: CECILL-C -->
# Design: RP2040 AMP cross-core IPC

Scope: the concrete cross-core IPC that `docs/design-multicore.md` assumes but
KickOS does NOT have. That doc's verdict is AMP-first on RP2040 (two core-private
`Kernel` instances) then SMP-BKL on RP2350. AMP needs a channel between the two
cores; the only sync primitives today -- `Semaphore` / `Mutex` (kernel/sync/sync.cc)
-- are INTRA-core: they park/wake on the local run queue and guard with `IrqLock`
== PRIMASK (kernel/include/kickos/irqlock.h), which masks only the issuing core and
is invisible to the peer. This designs the transport, the API/syscall seam, the MPU
sharing, the boot handshake, and the arch abstraction. No build/runtime code here;
snippets are illustrative. Grounds every claim in live code (file:line) and the
RP2040 datasheet (RP-008371-DS) by section.

## Invariant-first: what the IPC must guarantee

IPC-1  No shared mutable state crosses cores WITHOUT explicit ordering.
  M0+ has no LDREX/STREX and the SIO/IOPORT "does not support atomic accesses at
  the bus level" (DS 2.1.2). A naturally-aligned 32-bit load/store IS single-copy
  atomic on the AMBA fabric, but nothing orders two of them across cores except a
  barrier. Every cross-core producer publishes its data store BEFORE its index
  store, separated by `DMB`; every consumer orders its index load BEFORE its data
  load with `DMB`. Armv6-M HAS `DMB`/`DSB`/`ISB` (the arch's only barrier/hint
  ops) even though it lacks the exclusives -- the switch path already uses `dsb`/
  `isb` (arch/arm/armv6m/switch.S:146-147), so the toolchain emits them.

IPC-2  Exactly one writer per index. A single-producer/single-consumer (SPSC)
  ring has a producer-owned head and a consumer-owned tail. Each index word is
  WRITTEN by one core and only READ by the other -- so there is never a lost-
  update race, which is the sole reason a ring would otherwise need CAS. This is
  what lets AMP dodge the M0+ no-atomics wall for the fast path (design-multicore.md
  "AMP dissolves its hard problem").

IPC-3  A blocked `recv` parks on ITS OWN core's run queue and is woken by ITS OWN
  core's ISR. The wakeup crosses cores as an INTERRUPT (the doorbell), never as a
  cross-core scheduler poke. `sched::wake` (kernel/sched/sched.cc:131) and the
  whole reschedule path stay strictly core-local -- unchanged.

IPC-4  The shared ring SRAM is the ONLY memory writable by both cores. Everything
  else (each core's kernel data, arena, stacks) stays MPU-private per core
  (design-multicore.md INV-4). The blast radius of a wild write is bounded to the
  shared window.

IPC-5  Per-direction SPSC needs NO lock. A structure written by both cores (an
  MPMC mailbox, or one console ring shared by both) needs the SIO hardware
  spinlock -- the ONLY cross-core mutex on M0+ (DS 2.3.1.3). The design AVOIDS
  such structures on the fast path; where one is unavoidable it is named.

## 1. Transport -- SPSC ring in shared SRAM + DMB + FIFO doorbell

Two rings per channel, one per direction, in the shared IPC region (section 3).
Fixed-size slots (copy transport; zero-copy variant below). Power-of-two capacity
so the index wrap is a mask, not a divide.

```c
// kickos/ipc_ring.h -- header sketch (traditional guard, spelled operators)
#ifndef KICKOS_IPC_RING_H
#define KICKOS_IPC_RING_H
#include <stdint.h>
#include <stddef.h>

#define KICKOS_IPC_SLOT   32u   // bytes per message slot
#define KICKOS_IPC_SLOTS  8u    // power of two

struct IpcRing            // lives in the shared IPC region; both cores map it
{
    volatile uint32_t head;                 // producer writes, consumer reads
    volatile uint32_t tail;                 // consumer writes, producer reads
    uint8_t slot[KICKOS_IPC_SLOTS][KICKOS_IPC_SLOT];
};
#endif
```

Producer (core A -> core B ring). One writer, so no lock; DMB orders the payload
store ahead of the head publish (IPC-1):

```c
bool ipc_ring_send(struct IpcRing* r, void const* msg, size_t n)
{
    uint32_t const h = r->head;
    uint32_t const t = r->tail;             // read peer's index (plain load)
    if (h - t >= KICKOS_IPC_SLOTS)          // unsigned wrap-safe fullness
    {
        return false;                       // full: caller retries / drops
    }
    memcpy((void*)r->slot[h & (KICKOS_IPC_SLOTS - 1)], msg, n);
    arch_dmb();                             // payload visible BEFORE head bump
    r->head = h + 1;                        // single 32-bit publish (atomic store)
    return true;
}
```

Consumer (core B drains its inbound ring). DMB orders the head load ahead of the
payload load (IPC-1):

```c
bool ipc_ring_recv(struct IpcRing* r, void* out, size_t n)
{
    uint32_t const t = r->tail;
    if (r->head == t)                       // empty
    {
        return false;
    }
    arch_dmb();                             // see head BEFORE reading the slot
    memcpy(out, (void const*)r->slot[t & (KICKOS_IPC_SLOTS - 1)], n);
    arch_dmb();                             // slot consumed BEFORE tail bump
    r->tail = t + 1;                        // release the slot to the producer
    return true;
}
```

Why no spinlock on M0+ (IPC-2): `head` is written only by the producer, `tail`
only by the consumer. Neither side does a read-modify-write on a word the OTHER
side writes -- `h + 1` reads a producer-private value. A 32-bit aligned store is
single-copy atomic on the bus, so the reader never sees a torn index. The only
hazard is REORDERING (payload vs index), closed by `DMB`. No CAS, no exclusives,
no SIO spinlock. This is exactly the class of structure M0+ CAN do.

The doorbell: after `ipc_ring_send` publishes, the producer rings the peer by
writing ONE word into the SIO TX FIFO (DS 2.3.1.4: 32-bit x 8-deep per direction),
which raises `SIO_IRQ_PROCn` on the peer (IRQ 15 on core 0, IRQ 16 on core 1). The
FIFO is the DOORBELL, not the transport: 8 words is far too small for payloads and
draining it would race the ring. The word carries a small TAG (which channel /
"work pending"), never the message. Coalescing: if the FIFO is full (FIFO_ST.WOF /
RDY==0) the producer DROPS the doorbell write -- a doorbell already sits queued and
the peer's ISR drains ALL non-empty rings regardless, so a lost doorbell never
loses a message (level-of-work semantics, not edge). The peer ISR reads/drains its
RX FIFO (clearing the doorbell + the ROE/WOF sticky bits via FIFO_ST), then scans
the rings.

Where a SIO spinlock IS required (IPC-5): a single MPMC mailbox both cores push
to, or ONE console ring shared by threads on both cores. On M0+ the head bump then
becomes a contended RMW with no CAS to protect it -- it must be bracketed by a SIO
hardware spinlock (DS 2.3.1.3: read == attempt-claim, write == release, core 0 wins
ties). The design keeps the fast path SPSC to avoid this; the console (section 4)
stays SPSC per direction for the same reason.

## 2. API / seam -- a channel primitive alongside Semaphore/Mutex

Endpoint objects. A `Channel` is a per-direction pairing of one `IpcRing` (in the
shared region) with one `Semaphore` (in the RECEIVER's core-private kernel, its
run queue). The ring is the cross-core data; the semaphore is the intra-core park/
wake (IPC-3), reusing the EXISTING ISR-safe `sem_post` (kernel/sync/sync.cc:81, the
header documents "safe from thread or ISR context").

```c
struct Channel                 // one direction A->B
{
    struct IpcRing* ring;      // shared IPC region (both cores mapped)
    Semaphore       rx_ready;  // in B's kernel; B parks here, ISR posts it
};
```

Ownership: the `IpcRing` is owned by neither kernel -- it lives in the shared
region and is only ever touched through `ipc_ring_send`/`recv`. The `rx_ready`
semaphore is owned by the receiving core's `Kernel` (allocated from its
`sems` slotpool, kernel/include/kickos/instance.h:64) exactly like any other
semaphore; it is only ever posted from that same core's SIO ISR. So no kernel
object is shared -- only the ring bytes are (IPC-4).

Blocking recv (runs on the receiver's core, thread context):

```c
void chan_recv(struct Channel* c, void* out, size_t n)
{
    while (not ipc_ring_recv(c->ring, out, n))
    {
        sem_wait(&c->rx_ready);   // parks on THIS core's run queue (sync.cc:58)
    }
}
```

Non-blocking send (runs on the sender's core):

```c
bool chan_send(struct Channel* c, void const* msg, size_t n, unsigned peer)
{
    if (not ipc_ring_send(c->ring, msg, n))
    {
        return false;             // ring full
    }
    arch_ipc_notify(peer, c->tag);  // ring the doorbell (section 5)
    return true;
}
```

The wakeup path (peer's SIO ISR, drains + posts -- trigger #4, interrupt-exit
switch, exactly as an `irq_sem_post` binding does today, syscall.cc:67):

```c
void sio_proc_isr(void)          // SIO_IRQ_PROCn handler on the receiving core
{
    arch_ipc_drain();            // read/clear the RX FIFO doorbell (+ ROE/WOF)
    for (each inbound Channel c on this core)
    {
        if (c->ring->head != c->ring->tail)
        {
            sem_post(&c->rx_ready);   // ISR-safe wake of the parked receiver
        }
    }
}
```

Note the `sem_post` reads only the ring's two indices to decide whether to post --
a plain load of a peer-written word, no lock. A spurious post (ring drained by the
woken thread before it re-checks) is harmless: `chan_recv` loops on `ipc_ring_recv`
and simply parks again.

Syscall surface. A channel is exposed the way semaphores are (syscall.cc): small
integer handles into a per-`Kernel` slot pool, no pointers cross the user boundary.
New numbers: `KOS_SYS_chan_open` (bind a direction + slot geometry, returns a
handle), `KOS_SYS_chan_send`, `KOS_SYS_chan_recv`. `send`/`recv` bound the user
buffer with the existing `user_readable_ok` / `user_range_ok` confused-deputy floor
(syscall.cc:93-145) -- the kernel copies into/out of the shared ring privileged, so
the user buffer must lie in a region the caller is granted, identical to
`kconsole_write`. The ring slot bytes are NEVER handed to userspace as a pointer;
`recv` copies out into the user buffer (copy transport), so a user thread cannot
retain a dangling pointer into the shared window.

Zero-copy vs copy tradeoff:
  - Copy (RECOMMENDED first): fixed-size slots, `memcpy` in/out. No lifetime
    question, bounded by slot size, no shared allocator. Right for control messages
    and the ping-pong bring-up.
  - Zero-copy: the ring carries a handle into a shared buffer POOL (also in the
    shared region); producer allocates, fills, publishes the handle; consumer reads
    in place then returns the buffer. Buys large payloads without a copy, but needs
    a shared pool allocator -- which is MPMC and on M0+ must take the SIO spinlock
    (IPC-5) -- and a buffer-ownership protocol. Defer to when a bulk workload
    justifies it; on RP2350 native atomics make the pool cheap (section 5).

## 3. MPU -- the shared IPC region (the one genuinely-new isolation decision)

Each core has its own MPU and programs it on every switch (`arch_mpu_apply` stashes the set,
`kickos_arch_mpu_commit`/`kickos_arm_mpu_program` program it from the switch epilogue,
arch/arm/common/arch_arm_common.cc, shared v6-M/v7-M PMSA; RP2040's M0+ carries
the optional 8-region PMSAv6 and rp2040.ld already has `KICKOS_HAVE_MPU`
enforcement blocks). Today no region is reachable from two cores.

New: a FIXED shared-IPC region carved from SRAM, at ONE absolute address both
cores' linker scripts agree on, pow2-sized and pow2-aligned so PMSA can encode it
(the same constraint the app window already meets via `arch_ram_region_size`):

```ld
/* rp2040.ld -- static SRAM partition for AMP (264 KiB @ 0x2000_0000):
   core0 arena | core1 arena | shared IPC window (pow2, base aligned to size). */
.shared_ipc (NOLOAD) : ALIGN(4K)
{
    __kickos_ipc_start = .;
    . = . + 4K;                 /* holds both rings + channel headers */
    __kickos_ipc_end = .;
} > RAM
```

Both cores add this window to their endpoint threads'/domains' MPU region set as a
normal R|W data region (`ARCH_MPU_R | ARCH_MPU_W`, `mpu_rasr` encodes it XN
data, arch_arm_common.cc:168). It is the ONLY region present in BOTH cores' grants.

Isolation implication (IPC-4): every other region -- each core's kernel .data/.bss,
its arena, its thread stacks -- appears in ONLY that core's MPU program and is
unreachable from the peer. A wild write or a faulting thread on core 1 cannot
corrupt core 0's kernel or heap; the shared window is the whole cross-core attack
surface, and it holds only rings (indices + slots), no pointers into either private
space. This PRESERVES the per-core-MPU isolation win the AMP verdict rests on
(design-multicore.md INV-4) -- AMP adds exactly one hole, and it is bounded, aligned,
and enumerable. On a non-MPU build the window is still needed as a fixed address
both cores share; it just is not enforced.

Granularity caveat: PMSAv6 needs pow2 size + natural alignment, so the window
snaps to a pow2 (4 KiB here). The rings must fit; the partition sizes are a
bench-silicon item (section end). The window must NOT overlap either arena --
`__kickos_ram_end` on each core stops below `__kickos_ipc_start`.

## 4. Boot / lifecycle

Core-1 launch (DS 2.8.2). After reset core 1 sleeps (WFE, SLEEPDEEP) in the bootrom
`wait_for_vector`. Core 0 boots exactly as today (Reset_Handler -> arch_init ->
kmain, chip_rp2040.cc:456), owns the console/UART/peripherals, then hands core 1 the
six-word sequence `{0, 0, 1, VTOR, SP, PC}` over the TX FIFO -- each word echoed
back by core 1 before advancing, a `0` preceded by draining core 1's replies and a
`__sev()`. `PC` points at a `kmain_secondary` entry; `VTOR`/`SP` are core 1's own
vector table + stack in ITS arena.

FIFO reclaim -- the load-bearing handoff. The bootrom used the FIFO for
`wait_for_vector`; once core 1 has left the bootrom and is running KickOS code, the
FIFO is FREE and KickOS takes it over as the IPC doorbell. BOTH cores must drain any
residual handshake/echo words and clear FIFO_ST.ROE/WOF BEFORE enabling
`SIO_IRQ_PROCn`, else a stale word fires a spurious doorbell (harmless -- the ISR
just finds empty rings -- but drain anyway for a clean start). Then each core sets
its NVIC to enable its `SIO_IRQ_PROCn` line (per-core NVIC already exists on each
M0+).

Ring init ordering. Core 0 (the primary) zeroes both rings' head/tail in the shared
region and `DMB`s BEFORE it starts the launch handshake. The handshake itself is the
happens-before edge: core 1 only begins executing after core 0's ring writes are
posted, so `kmain_secondary` sees valid empty rings and must NOT re-init them.

Console ownership (INV-6). Core 0 owns UART0 (single physical producer). Core 1 has
no UART: its `kconsole_write` posts log records into a shared console ring (SPSC
core1 -> core0, its own direction) and rings the doorbell; core 0's SIO ISR drains
that ring and feeds its existing `console_tx` backend (chip_rp2040.cc:359). Threads
on core 1 serialize into that ring under core 1's `IrqLock` (intra-core), so the
cross-core edge stays SPSC -- no SIO spinlock (IPC-5). A single ring written by BOTH
cores would be the MPMC case that needs the spinlock; the per-direction split avoids
it.

Chip-SLEEP / SWD interaction (bench item). The RP2040 enters a chip-wide SLEEP that
gates the debug bus (SWD detaches until a power-cycle) only when BOTH cores are idle
in WFI/WFE. An AMP build where both cores idle in WFI thus drops SWD; if debug access
during a run matters, one core's idle policy can busy-spin instead. Confirm on silicon.

MCU exit hazard (pre-existing). On ARM, a non-last thread that EXITs panics
(deferred PendSV masked by exit_current's IrqLock) -- MCU workloads use daemon
workers. The recv worker on each core must LOOP forever, never return/exit.

## 5. Fit with the existing seam

`kernel()` per-CPU. The selector (instance.h:89-96) already compiles to a per-
instance pointer under `KICKOS_MULTI_INSTANCE` (a host-TLS pointer, built for the
KickCAT multi-slave sim). AMP is the SAME shape keyed on hardware core id instead:

```c
inline Kernel& kernel()          // AMP build
{
    return detail::g_instances[arch_cpu_id()];   // SIO CPUID: 0 or 1
}
```

`arch_cpu_id()` reads the SIO CPUID register (DS 2.3.1.1, SIO base 0xd0000000 +
0x000: core 0 reads 0, core 1 reads 1) -- a single-cycle IOPORT load. The per-core
instances are two static `Kernel` objects; each core touches only its own, so
`IrqLock` == PRIMASK stays a correct mutual exclusion (no shared run queue). This is
the identical refactor `docs/design-multicore.md` Phase 0/1 names; IPC does not add
to it beyond the CPUID accessor.

Minimal new arch surface -- a doorbell abstraction (so RP2350 SIO v2 doorbells /
any mailbox back the SAME channel API):

```c
unsigned arch_cpu_id(void);                       // SIO CPUID / mhartid
void     arch_ipc_notify(unsigned peer, uint32_t tag);  // ring the peer's doorbell
void     arch_ipc_drain(void);                    // ack/clear this core's doorbell
void     arch_dmb(void);                           // Armv6-M DMB (cross-core order)
```

The ring transport + `Channel` + the recv-parks/ISR-posts machinery are PORTABLE
kernel code (shared SRAM + DMB + the existing Semaphore). Only `arch_ipc_notify`/
`arch_ipc_drain` are chip-specific:
  - RP2040: notify == write tag to SIO TX FIFO (raises peer `SIO_IRQ_PROCn`);
    drain == read/clear RX FIFO + FIFO_ST sticky bits.
  - RP2350: notify == `DOORBELL_OUT_SET` (raises peer `SIO_IRQ_BELL`, int 26);
    drain == read/clear `DOORBELL_IN`. The FIFO is freed for other use.

## Recommended minimal first-cut -- two-core ping-pong + thread wake

Build the smallest thing that exercises the WHOLE substrate: bring up core 1, send
a message each way, and wake a parked thread on each core.

1. Per-CPU `kernel()` keyed on SIO CPUID; two static `Kernel` instances. Add
   `arch_cpu_id` + `arch_dmb`.
2. rp2040.ld: static SRAM partition -- core0 arena, core1 arena, one 4 KiB
   `.shared_ipc` window holding two `IpcRing`s (A->B, B->A), 8 slots x 32 B.
3. Core 0: boot as today; zero both rings + DMB; launch core 1 via the FIFO
   handshake ({0,0,1,VTOR,SP,PC}); drain + reclaim the FIFO; install/enable
   `SIO_IRQ_PROC0`.
4. Core 1 `kmain_secondary`: init its `Kernel`; drain + reclaim its FIFO end;
   install/enable `SIO_IRQ_PROC1`; spawn a DAEMON worker that `chan_recv`s and
   echoes back.
5. `Channel` per direction = { ring*, `rx_ready` Semaphore in the receiver's
   kernel }. `chan_recv` parks via `sem_wait`; the SIO ISR drains the FIFO and
   `sem_post`s each non-empty ring's `rx_ready` (interrupt-exit switch to the woken
   worker).
6. Ping-pong: core 0 `chan_send`("ping") -> doorbell -> core 1 ISR wakes worker ->
   worker `chan_send`("pong") -> doorbell -> core 0 ISR wakes its waiter. One
   message + one thread wake cross each way. That single loop validates: core-1
   bring-up, FIFO reclaim, ring + DMB visibility, the doorbell IRQ, and the
   cross-core `sem_post` wake -- the entire AMP IPC substrate.

API sketch (kernel-facing): `chan_open(dir, slots) -> handle`,
`chan_send(handle, buf, n) -> bool`, `chan_recv(handle, buf, n)` (blocking).
Userspace mirrors these as `KOS_SYS_chan_*` with the `user_readable_ok` buffer
check.

## Needs bench silicon

  - DMB sufficiency for cross-core ring visibility on the real RP2040 fabric (M0+
    store buffer + SIO ordering) -- the IPC-1 memory-model claim is the one that
    must be proven on hardware, not asserted.
  - FIFO reclaim after the bootrom handshake: that no residual echo/`__sev` words
    remain, and `SIO_IRQ_PROCn` latency once reclaimed.
  - Chip-SLEEP with both cores live: the SLEEP condition (which gates the debug bus)
    now depends on both cores' idle policy, not just core 0's.
  - Static partition sizes: per-core arena vs the shared window (and slot count /
    size) against real app + console-ring footprints.
  - PMSAv6 grant of `.shared_ipc` on BOTH cores under an MPU-enabled build (natural
    alignment, no arena overlap).

## Generalizes to RP2350 vs RP2040-specific

Generalizes (portable, reused unchanged on RP2350 SIO v2): the SPSC ring + DMB
transport, the `Channel` = ring + Semaphore pairing, recv-parks / ISR-`sem_post`
wake, the shared-IPC linker region + per-core MPU grant, and the `kernel()`-per-CPU
seam. All of it is chip-neutral above `arch_ipc_notify`/`arch_ipc_drain`.

RP2040-specific: the doorbell rides the 8 x 32-bit FIFO + `SIO_IRQ_PROCn`, and
because M0+ has NO atomics the SIO hardware spinlock is the ONLY cross-core mutex --
so any MPMC structure (shared pool, single shared mailbox) is spinlock-bound, and
zero-copy pools are costly. Core-1 launch is the six-word FIFO handshake.

RP2350 (SIO v2, when the port lands): swap the FIFO doorbell for native Doorbells
(`DOORBELL_OUT_SET` -> peer `SIO_IRQ_BELL` int 26) -- accumulative "ring once/answer
once", and it frees the FIFO. Native atomics (M33 LDREX/STREX, Hazard3 A-extension)
mean an MPMC shared pool or a single shared console ring can use a real atomic ring
instead of the SIO spinlock, making the zero-copy pool cheap. The Secure/Non-secure
SIO banks (separate FIFOs/doorbells/spinlocks under a TrustZone M33 build) add a
bank-selection concern absent on RP2040. The `arch_ipc_notify`/`arch_ipc_drain`
seam absorbs exactly this doorbell swap; the channel API above does not change.
