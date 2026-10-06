// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Buffered UART service with shared rings and the uart.h protocol.
// Only the IRQ thread may call kos_uart_* or access registers. It owns TX
// tail and RX head; the service thread owns TX head and RX tail. Each ring
// index must have one writer to satisfy byte_ring.h's SPSC contract.

#ifndef KICKOS_SYS_UART_SERVICE_H
#define KICKOS_SYS_UART_SERVICE_H

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/driver/uart.h>
#include <kickos/sys/atomic.h>
#include <kickos/sys/byte_ring.h>
#include <kickos/sys/bytes.h> // mem_copy
#include <kickos/sys/console_ring.h>
#include <kickos/sys/console_service.h>
#include <kickos/sys/driver_geometry.h> // KICKOS_UART_BLOCK_SIZE (generated)
#include <kickos/sys/driver_service.h>
#include <kickos/sys/errno.h>
#include <kickos/sys/uart.h>

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

namespace kickos::uart
{

// Child cap indices the two threads read. NOTIFY and EP share an index, and LINE shares one
// with the doorbell, because they are different threads' cap tables.
enum
{
    // Service thread: the request endpoint (WAIT) and the badged notification copy it rings
    // (SIGNAL).
    KOS_UART_CAP_EP = console::KOS_CONSOLE_CAP_EP,
    KOS_UART_CAP_DOORBELL = console::KOS_CONSOLE_CAP_DOORBELL,
    // IRQ thread: the notification it binds and waits on (WAIT), and the line it acks (WAIT).
    KOS_UART_CAP_NOTIFY = KOS_SPAWN_DELEGATED_CAP0,
    KOS_UART_CAP_LINE = KOS_SPAWN_DELEGATED_CAP0 + 1
};

// EVERY bit of the object. It is this driver's own, nothing else raises into it, and the
// pass below services whatever the device has: a mask naming particular bits would be a
// second statement of which lines the descriptor claims and could only drift from it.
constexpr uint32_t KOS_UART_ACCEPT = 0xFFFFFFFFu;

// The shared block, in ONE power-of-two naturally-aligned allocation: the RAM arm of
// grant_region_admissible requires that of every caller, privileged included.
enum
{
    KOS_UART_TX_SIZE = 512,
    KOS_UART_RX_SIZE = 256,
    KOS_UART_BLOCK_SIZE = KICKOS_UART_BLOCK_SIZE
};

static_assert((KOS_UART_BLOCK_SIZE & (KOS_UART_BLOCK_SIZE - 1)) == 0,
              "the UART shared block is one power-of-two grant");

struct Shared
{
    struct kos_byte_ring tx;
    struct kos_byte_ring rx;
    struct kos_uart_stats stats;
    // Set by the IRQ thread once its own bring-up has run. A bring-up MUST spin on this,
    // bounded, BEFORE it spawns the service thread, and fail loud on the timeout: once the
    // service thread holds a WAIT cap, recv_holders never reaches 0, nothing reclaims the
    // console, and a diagnostic goes to an endpoint nobody is draining.
    Atomic<uint32_t, Order::RELAXED> ready;
    // Write policy for the unframed console arm, from kos_uart_flags. The service thread is
    // its only writer. Zero, so BLOCKING: a UART drains whether or not anything is
    // listening. A transport whose consumer may never exist must seat KOS_UART_F_NONBLOCK.
    Atomic<uint32_t, Order::RELAXED> mode;
    // The flush handshake: the service thread writes flush_req, the IRQ thread flush_ack.
    // RELAXED suffices because a request is raised only once the ring is empty, and only the
    // IRQ thread empties it.
    Atomic<uint32_t, Order::RELAXED> flush_req;
    Atomic<uint32_t, Order::RELAXED> flush_ack;
    uint8_t tx_buf[KOS_UART_TX_SIZE];
    uint8_t rx_buf[KOS_UART_RX_SIZE];
};

static_assert(sizeof(struct Shared) <= KOS_UART_BLOCK_SIZE,
              "the UART shared block must fit its power-of-two grant");

// kos_byte_ring_init REFUSES a non-power-of-two or sub-2 size and leaves the ring reporting
// empty-and-full forever, which a blocking (unbounded) console write would spin on.
static_assert(KOS_UART_TX_SIZE >= 2 and (KOS_UART_TX_SIZE & (KOS_UART_TX_SIZE - 1)) == 0,
              "the TX ring size must be a power of two >= 2 or it never accepts a byte");

// Lay out the block. Not thread-safe: call it before either thread exists.
void shared_init(Shared* s);

// The whole granted block: the shared rings plus the class config the IRQ thread opens the
// device with. Every thread reaches it through its thread ARG.
struct Ctx
{
    struct Shared sh;
    struct kos_uart_config ucfg;
};

static_assert(sizeof(Ctx) <= KOS_UART_BLOCK_SIZE,
              "the UART driver context must fit the shared grant");

// The offset a generic bring-up polls the readiness latch through, it being unable to name
// Ctx. Never write the offset as a literal in a descriptor: the latch must be the atomic
// this expression locates.
constexpr uint16_t KOS_UART_READY_OFFSET =
    static_cast<uint16_t>(offsetof(Ctx, sh) + offsetof(Shared, ready));

// Lay out the block and fill the class config from the instance at `fallback_baud`; 0 there
// means "keep the divisor the boot console left", not "0 baud".
int ctx_init(Ctx* ctx, struct kos_driver_instance const* in, uint32_t fallback_baud);

// ---------------------------------------------------------------------------------
// Staging segment for one service pass, used first for RX and then for TX. Any size is
// correct: the pass is re-entered on the next wake.
constexpr uint32_t KOS_UART_IRQ_SEG = 64;

// One service pass: fill RX, then drain TX. A wake is not proof of a hardware event
// (kos_notify is a pure raise), so both transfers must tolerate an idle device and both
// rings a zero-length move.
//
// THIS DECLARATION MUST STAY AHEAD OF THE TEMPLATE BELOW: without it a call with a
// `struct kos_uart*` deduces the template and asks for a service_irq() that does not exist.
void irq_pass(struct kos_uart* dev, Shared* sh);

// The arm for a backend that carries a service_irq() method instead of the class API: the
// sim loopback in system/driver/sim/simuart/simuart.cc.
template <typename Uart>
inline void irq_pass(Uart* dev, Shared*)
{
    dev->service_irq();
}

// Give the device back quiet. FLUSH BEFORE CLOSE: on a part whose stop is a mode-disable
// rather than a drain, closing truncates a frame still shifting. Declared ahead of the
// template for the same reason as irq_pass.
void dev_shutdown(struct kos_uart* dev);

// Pairs with the irq_pass arm above: such a backend has no close, so its device is left as
// its own driver set it.
template <typename Uart>
inline void dev_shutdown(Uart*)
{
}

// Drain the device's transmit path: kos_uart_flush, as strong as its backend states. Declared
// ahead of the template for the same reason as irq_pass.
int32_t dev_flush(struct kos_uart* dev);

// Pairs with the irq_pass arm above: such a backend's pass hands its bytes to the host
// synchronously, so nothing is left in flight once the ring is empty.
template <typename Uart>
inline int32_t dev_flush(Uart*)
{
    return 0;
}

// The IRQ thread's half of the flush handshake, run after each pass. `*seen` is the last
// request this thread attempted, so a device whose flush fails costs one attempt per
// request rather than one per wake. Returns whether a request was attempted.
template <typename Uart>
bool flush_answer(Uart& dev, Shared* sh, uint32_t* seen)
{
    uint32_t const req = sh->flush_req;
    if (req == *seen or kos_byte_ring_used(&sh->tx) != 0u)
    {
        return false;
    }
    *seen = req;
    if (dev_flush(&dev) == 0)
    {
        sh->flush_ack = req;
    }
    return true;
}

// ---------------------------------------------------------------------------------
// The IRQ thread. It alone touches the device.
template <typename Uart>
void irq_loop(Uart& dev, Shared* sh)
{
    // Bind before signaling readiness and waiting: a raise before the bind is LATCHED in the
    // object and delivered to whoever binds next, so nothing is lost either way.
    int bound = kos_notify_bind(KOS_UART_CAP_NOTIFY);
    if (bound != 0)
    {
        dev_shutdown(&dev);
        driver::trap();
    }
    sh->ready = 1;
    // Seeded from the ack: a request raised before this thread's first wait is still pending.
    uint32_t flush_seen = sh->flush_ack;
    int waited = 0;
    while (true)
    {
        // The FIRST wait is also what arms the line: a claim leaves it masked so no window
        // exists in which it is armed and unowned. One wait covers the line and the
        // doorbell, which is why this class needs no relay thread.
        waited = kos_notify_wait(KOS_UART_CAP_NOTIFY, KOS_UART_ACCEPT, KOS_TIMEOUT_NONE, nullptr);
        if (waited != 0)
        {
            break; // the cap went away: the line is gone, so this thread has no work
        }
        kos_counter_increment(&sh->stats.irq_wakes, 1u);
        // "Found nothing" is judged from what THIS thread owns: tx_bytes moves under the
        // service thread, so its value says nothing about whether this wake found work.
        uint32_t const rx_before = kos_counter_load(&sh->stats.rx_bytes);
        bool const tx_had_work = (kos_byte_ring_used(&sh->tx) != 0u);
        irq_pass(&dev, sh);
        bool const flushed = flush_answer(dev, sh, &flush_seen);
        if (kos_counter_load(&sh->stats.rx_bytes) == rx_before and not tx_had_work
            and not flushed)
        {
            // A doorbell with an empty ring, or a stray raise.
            kos_counter_increment(&sh->stats.irq_spurious, 1u);
        }
    }
    dev_shutdown(&dev);
    driver::trap();
}

// Per-byte cap on the first-light poll: a channel that never reports room costs a delay,
// not the IRQ thread.
constexpr uint32_t KOS_UART_TX_SPIN_MAX = 1000000u;

// Direct-to-device diagnostic, not stdio and not the ring, and legal only before the
// line's first irq_wait.
void win_puts(struct kos_uart* dev, char const* s);

// `prime` is NOT derivable from the trigger mode: xmcuartirq is EDGE and primes, rxsci is
// EDGE and must not. Priming a source whose only raise is a transfer taken with the source
// already armed waits on a transition that has already happened; not priming a source that
// needs it loses the first wake.
struct UartParams
{
    char const* announce;
    bool prime;
};

// NEVER exits on an open failure: the service thread would stay the endpoint's receiver and
// keep accepting stdout into a ring nothing drains. The trap ends the whole task.
template <typename Uart>
void irq_thread(Ctx* ctx, UartParams const& p)
{
    Uart dev;
    int32_t const opened = kos_uart_open(&dev, &ctx->ucfg);
    if (opened < 0)
    {
        driver::trap();
    }
    // The marker must precede the first pass: only a pend latched before the line's FIRST
    // irq_wait is discarded.
    win_puts(&dev, p.announce);
    if (p.prime)
    {
        irq_pass(&dev, &ctx->sh);
    }
    irq_loop(dev, &ctx->sh); // parks in irq_wait; never returns
}

// ---------------------------------------------------------------------------------
// The service thread's flush: the ring, then the device's transmit path through the IRQ
// thread, each stage bounded by the console::flush budget. Returns 0 once both drained,
// -KOS_EBUSY when either budget expired first, the IRQ thread being dead included.
int32_t flush(Shared* sh);

// The request side of the console is <kickos/sys/console_service.h>. This is what a UART
// changes about it: a UART drains whether or not anything is listening, so no mode bit is
// required and a blocking write is honourable; bytes leave the ring into the device's own
// FIFO and shift register, which only the IRQ thread can drain; and no TX loss is counted
// outside the ring.
struct Transport
{
    static constexpr uint32_t MODE_REQUIRED = 0u;
    static int32_t flush(Shared* sh) { return uart::flush(sh); }
    static uint32_t tx_lost(Shared const*) { return 0u; }
};

// The service thread. Replies out of ring state and never touches the device.
//
// `mode` is null for a service with no unframed console arm, which is what makes
// KOS_UART_SET_MODE refuse there instead of storing a mode nothing reads.
size_t serve_one(Shared* sh, Atomic<uint32_t, Order::RELAXED>* mode, uint8_t* buf, size_t n);

// Recv/dispatch loop with no console arm. Returns only when the endpoint dies, which is the
// respawn signal.
void serve_loop(Shared* sh);

int32_t console_serve_loop(Shared* sh);

// The console service thread entry: its ARG is the Ctx the bring-up granted.
void console_thread(void* arg);

// ---------------------------------------------------------------------------------
// The class-side half of the descriptor check. The generic validator cannot know that the
// loops above read KOS_UART_CAP_EP == 1, KOS_UART_CAP_DOORBELL == 2 and KOS_UART_CAP_LINE ==
// 1, so a descriptor that grants the right caps in the wrong ORDER passes valid() and stalls
// silently. Nothing else in the tree checks this.
//
// valid() only RANGE-checks ready_offset, so a literal there can land on a field already
// non-zero when the poll first reads it, which turns the barrier into a silent no-op.
constexpr bool desc_ok(driver::Descriptor const& d)
{
    return driver::ring_doorbell_shape_ok(d, KOS_UART_READY_OFFSET,
                                          static_cast<uint32_t>(KOS_UART_BLOCK_SIZE));
}

}

#endif
