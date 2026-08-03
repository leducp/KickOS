// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Shared buffered-UART choreography, templated over a concrete per-chip Uart class. No
// MMIO and no chip knowledge live here: only the shared ring block's layout, the wire-ABI
// framing over <kickos/sys/uart.h>, and the two loops the driver's two threads run.
//
// The Uart class supplies the implicit interface (design-m4.6-irq-driver.md section 7.3):
//     uint32_t configure(uint32_t baud, uint8_t data_bits, uint8_t parity, uint8_t stop);
//     void     service_irq();  // one pass: read status once, drain TX, fill RX, clear
//     bool     tx_idle() const;
//     void     tx_irq_enable();   // arm the peripheral TX-empty source
// configure/service_irq touch registers, so they may be called ONLY from the IRQ thread
// (below).
//
// TWO THREADS: one thread cannot block on both kos_recv (client requests) and
// kos_irq_wait (device events), because KickOS has no receive-from-either primitive. So
// the driver is a service thread parked in recv and an IRQ thread parked in irq_wait,
// sharing one ring block.
//
// The IRQ thread owns EVERY peripheral register plus TX `tail` and RX `head`; the service
// thread owns TX `head` and RX `tail`. The window is granted to the IRQ thread alone and a
// DEV window has exactly one holder, so a service thread that tried to touch the device
// would fail at SPAWN (section 3.3). One writer per index is also what keeps both rings
// strict SPSC with no lock, which <byte_ring.h> requires and cannot check.
//
// The service thread holds no lock, so it must touch neither `tail` nor the device: it
// rings kos_irq_notify and the IRQ thread does both steps. RULE T1 (section 9.3): the
// first byte of a burst is pushed by the same pass that enables the TX interrupt, NEVER by
// a later interrupt. On a transition-triggered part (XMC TBIEN, RX SCI TIE) enabling the
// source on an idle channel raises nothing, so waiting for an interrupt to send the first
// byte deadlocks.

#ifndef KICKOS_SYS_UART_SERVICE_H
#define KICKOS_SYS_UART_SERVICE_H

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/sys/byte_ring.h>
#include <kickos/sys/bytes.h> // mem_copy
#include <kickos/sys/errno.h>
#include <kickos/sys/uart.h>

#include <stdint.h>
#include <stddef.h>

// The board's CRLF posture, set by the build. An out-of-tree consumer reaching this header
// through find_package(KickOS) gets no add_compile_definitions, so an undefined knob must
// mean "raw" rather than a compile error.
#ifndef KICKOS_CONSOLE_CRLF
#define KICKOS_CONSOLE_CRLF 0
#endif

namespace kickos
{
namespace uart
{

// Child cap indices the two threads read. A driver NAMES them; the bring-up chooses them.
enum
{
    // Service thread: the request endpoint (WAIT) and the line cap it rings (SIGNAL).
    KOS_UART_CAP_EP = KOS_SPAWN_DELEGATED_CAP0,
    KOS_UART_CAP_DOORBELL = KOS_SPAWN_DELEGATED_CAP0 + 1,
    // IRQ thread: the line cap it waits on (WAIT).
    KOS_UART_CAP_LINE = KOS_SPAWN_DELEGATED_CAP0
};

// The shared block, in ONE power-of-two naturally-aligned allocation because the RAM arm
// of grant_region_admissible requires it of every caller, privileged included
// (design-m4-driver-model.md rule 7). Both threads see the same object at the same
// address: it is passed as the thread ARG.
//
// TX 512 matches the kernel ring's CONSOLE_TX_SIZE (itself chosen above kprintf's 256 B
// buffer), RX 256. Header + payload must fit one power of two, which the static_assert
// below enforces.
enum
{
    KOS_UART_TX_SIZE = 512,
    KOS_UART_RX_SIZE = 256,
    KOS_UART_BLOCK_SIZE = 1024
};

struct Shared
{
    struct kos_byte_ring tx;
    struct kos_byte_ring rx;
    struct kos_uart_stats stats;
    // Set by the IRQ thread once its own bring-up has run. Plain volatile: a one-way latch
    // written once by one thread, so it needs no barrier pair.
    //
    // BINDING ON THE BRING-UP, not a hint: a bring-up MUST spin on this, bounded, BEFORE
    // it spawns the service thread, and must fail loud on the timeout. No request may be
    // served against a device that is not yet configured, and the timeout is reportable
    // only while root is still the sole receiver: once the service thread holds a WAIT cap,
    // recv_holders never reaches 0, nothing reclaims the console, and the diagnostic is
    // dropped into a published endpoint nobody is draining.
    volatile uint32_t ready;
    unsigned char tx_buf[KOS_UART_TX_SIZE];
    unsigned char rx_buf[KOS_UART_RX_SIZE];
};

static_assert(sizeof(struct Shared) <= KOS_UART_BLOCK_SIZE,
              "the UART shared block must fit one 1 KiB power-of-two grant");

// Lay out the block. Called by the BRING-UP, before either thread exists, so it races
// nothing.
inline void shared_init(Shared* s)
{
    mem_zero(s, sizeof(*s));
    kos_byte_ring_init(&s->tx, s->tx_buf, KOS_UART_TX_SIZE);
    kos_byte_ring_init(&s->rx, s->rx_buf, KOS_UART_RX_SIZE);
}

// ---------------------------------------------------------------------------------
// The IRQ thread. Owns every register; parks in irq_wait and services one pass per wake.
// A wake is NOT proof of a hardware event: kos_irq_notify is a pure post, so
// service_irq() must be idempotent about finding nothing asserted (section 2.6).
template <typename Uart>
void irq_loop(Uart& dev, Shared* sh)
{
    sh->ready = 1;
    while (true)
    {
        // The FIRST wait is also what arms the line: a claim leaves it masked so no window
        // exists in which it is armed and unowned (INVARIANT H1).
        if (kos_irq_wait(KOS_UART_CAP_LINE) != 0)
        {
            break; // the cap went away: the line is gone, so this thread has no work
        }
        sh->stats.irq_wakes++;
        // "Found nothing" is judged from what THIS thread owns: rx_bytes is written only
        // here, plus the TX ring's emptiness on entry. Reading tx_bytes would race the
        // service thread, which owns it.
        uint32_t const rx_before = sh->stats.rx_bytes;
        bool const tx_had_work = (kos_byte_ring_used(&sh->tx) != 0u);
        dev.service_irq();
        if (sh->stats.rx_bytes == rx_before and not tx_had_work)
        {
            sh->stats.irq_spurious++; // a doorbell with an empty ring, or a stray raise
        }
    }
    kos_exit(0);
}

// ---------------------------------------------------------------------------------
// Queue bytes for transmit and ring the doorbell. Returns the bytes ACCEPTED, which is
// less than n on a full ring; the retry policy for the remainder belongs to the caller.
// EVERY producer path must go through here rather than open-coding push + notify.
//
// Rung on EVERY call, including one that accepted nothing: with the ring full the parked IRQ
// thread has no other wake source, so gating on an accepted push strands the channel, and
// gating on an idle -> busy edge loses the wake the other way (the IRQ thread can drain,
// disarm and park between the test and the push). The producer owns `head`, the IRQ thread
// `tail`; neither may act on the other's index.
//
// PRECONDITION: KOS_UART_CAP_DOORBELL is the line's SIGNAL cap, which the two-thread spawn
// provides. Any other caller has the notify refused on the cap TYPE check rather than
// ringing a stranger's object.
inline uint32_t tx_write(Shared* sh, unsigned char const* p, uint32_t n)
{
    uint32_t const took = kos_byte_ring_push(&sh->tx, p, n);
    sh->stats.tx_bytes += took;
    (void)kos_irq_notify(KOS_UART_CAP_DOORBELL);
    return took;
}

// ---------------------------------------------------------------------------------
// Block until the TX ring is empty, or the budget runs out. Returns the bytes STILL
// queued, so 0 means drained.
//
// A ZERO-LENGTH plain send on a console endpoint means FLUSH. It is the only drain
// request root can make: kos_call is refused to a non-pool caller (-KOS_EPERM), so a
// reply-bearing op is unreachable from the thread that is about to shut the system down,
// and the length is the only field a plain send carries.
//
// Rings the doorbell on every pass, for the reason tx_write does: with the consumer parked
// in kos_irq_wait the caller is the only remaining wake source, and that pass is also what
// re-arms a TX source that went quiet.
//
// 512 B at 115200 baud is ~44 ms, so the budget sits well above a full ring and a dead
// channel gives up instead of stranding the shutdown.
constexpr uint64_t KOS_UART_FLUSH_SLEEP_NS = 100000u; // ~1 byte time at 115200 baud
constexpr uint32_t KOS_UART_FLUSH_MAX = 2000u;        // ~200 ms total

inline uint32_t console_flush(Shared* sh)
{
    uint32_t left = kos_byte_ring_used(&sh->tx);
    for (uint32_t i = 0; i < KOS_UART_FLUSH_MAX and left != 0u; i++)
    {
        (void)kos_irq_notify(KOS_UART_CAP_DOORBELL);
        kos_sleep_ns(KOS_UART_FLUSH_SLEEP_NS);
        left = kos_byte_ring_used(&sh->tx);
    }
    return left;
}

// ---------------------------------------------------------------------------------
// Queue ALL of a raw console write, waiting for room instead of dropping the tail.
// Returns the bytes accepted, short of n only when the budget expired on a channel that
// is not draining.
//
// A plain send has no reply, so the SENDER can neither be told about a short accept nor
// retry it; the retry has to live on this side or the stream is spliced mid-token (a line
// prefix followed by the prefix of a later line). Root's tap/_write loop advances by what
// the RECEIVER took, which is the whole datagram, not what the ring accepted.
//
// It is also the console path's only backpressure. A plain send with a thread already
// parked in kos_recv completes in the sender's own context, so a producer at any priority
// fills a 512 B ring at memory speed while the wire moves ~11.5 kB/s. Staying out of
// kos_recv until the ring has taken everything blocks the NEXT send on the endpoint, which
// paces the producer to the wire.
//
// Same budget and same per-pass doorbell as console_flush.
inline uint32_t push_all(Shared* sh, unsigned char const* p, uint32_t n)
{
    uint32_t off = tx_write(sh, p, n);
    for (uint32_t i = 0; i < KOS_UART_FLUSH_MAX and off < n; i++)
    {
        kos_sleep_ns(KOS_UART_FLUSH_SLEEP_NS);
        off += tx_write(sh, p + off, n - off);
    }
    return off;
}

// Staging window for the CRLF expansion. Any size is correct because the loop chunks, so
// this only trades service-thread stack against calls to the pump.
constexpr uint32_t KOS_UART_COOK_CHUNK = 64;

// Expand '\n' to "\r\n" from `in` into `out`, stopping at whichever runs out first.
// Returns the cooked length written; `*taken` receives the INPUT bytes it represents.
//
// ALWAYS COMPILED, even where the board does not cook and the only caller is #if-gated
// out, so the `console_crlf` selftest can exercise it on every board.
//
// A '\n' is never split from its '\r': the loop stops before an input byte whose expansion
// would not fit whole, so `*taken` is always a clean resume point.
inline uint32_t cook_crlf(unsigned char const* in, uint32_t in_n, unsigned char* out,
                          uint32_t out_cap, uint32_t* taken)
{
    uint32_t c = 0;
    uint32_t t = 0;
    while (t < in_n)
    {
        unsigned char const b = in[t];
        uint32_t need = 1;
        if (b == '\n')
        {
            need = 2;
        }
        if (c + need > out_cap)
        {
            break;
        }
        if (b == '\n')
        {
            out[c] = '\r';
            c++;
        }
        out[c] = b;
        c++;
        t++;
    }
    *taken = t;
    return c;
}

// The CONSOLE write: push all of p, cooking '\n' to "\r\n" where the board asks for it.
// Returns the INPUT bytes accepted, so a caller's `n - took` stays input-byte accounting.
//
// WHO OWNS THE COOK: the console abstraction cooks, the UART transport does not. This
// function is the console arm and it cooks; the KOS_SVC_UART WRITE op goes through
// tx_write and stays byte-transparent, because a client sending binary over the wire ABI
// must get its bytes back. kconsole_write expands for the kernel-owned route off the same
// KICKOS_CONSOLE_CRLF knob (kernel/init/console.cc), so publishing a driver must not
// change the SHAPE of the output.
//
// EVERY '\n' is expanded, with no look-back at the previous byte, so an app writing
// "\r\n" gets "\r\r\n", which is a no-op on the wire. Cross-datagram look-back state
// would buy nothing but a way for the two routes to diverge again.
//
// On a short accept the input-to-cooked mapping is reported at the last CHUNK boundary,
// so tx_dropped can over-count by up to one chunk.
inline uint32_t console_write_all(Shared* sh, unsigned char const* p, uint32_t n)
{
#if KICKOS_CONSOLE_CRLF
    // n == 0 must reach the pump in BOTH postures: it is one doorbell, and the doorbell
    // count must not depend on the CRLF posture.
    if (n == 0u)
    {
        return push_all(sh, p, 0u);
    }
    unsigned char cooked[KOS_UART_COOK_CHUNK];
    uint32_t in_done = 0;
    while (in_done < n)
    {
        uint32_t take = 0;
        uint32_t const c =
            cook_crlf(p + in_done, n - in_done, cooked, KOS_UART_COOK_CHUNK, &take);
        if (push_all(sh, cooked, c) < c)
        {
            return in_done;
        }
        in_done += take;
    }
    return n;
#else
    return push_all(sh, p, n);
#endif
}

// The service thread. Parks in recv, replies out of ring state, never touches the device.
inline void reply_status(int reply_cap, int32_t status, uint16_t len)
{
    struct kos_uart_rsp rsp;
    rsp.status = status;
    rsp.len = len;
    rsp.rsv = 0;
    (void)kos_reply(reply_cap, &rsp, sizeof(rsp));
}

inline void serve_one(Shared* sh, unsigned char const* msg, size_t n, int reply_cap)
{
    if (reply_cap < 0)
    {
        return; // a plain send, not a call: nothing to reply to, and nothing to do
    }
    if (n < sizeof(struct kos_uart_req))
    {
        reply_status(reply_cap, -KOS_EINVAL, 0);
        return;
    }
    struct kos_uart_req req;
    mem_copy(&req, msg, sizeof(req));
    unsigned char const* payload = msg + sizeof(req);
    size_t const payload_len = n - sizeof(req);

    switch (req.op)
    {
    case KOS_UART_WRITE:
    {
        if (req.len > payload_len)
        {
            reply_status(reply_cap, -KOS_EINVAL, 0); // framing claims more than it carried
            return;
        }
        uint32_t const took = tx_write(sh, payload, req.len);
        // A short accept is NOT an error: the client sees `len < req.len` and retries. A
        // userspace driver must never mask interrupts, so retry is the only backpressure
        // available here.
        reply_status(reply_cap, 0, static_cast<uint16_t>(took));
        return;
    }
    case KOS_UART_READ:
    {
        if ((req.flags & KOS_UART_F_BLOCK) != 0)
        {
            // Refused rather than silently returning 0, so "unsupported" cannot be
            // mistaken for "no data".
            reply_status(reply_cap, -KOS_ENOSYS, 0);
            return;
        }
        unsigned char out[KOS_EP_MSG_MAX];
        uint32_t want = req.len;
        if (want > KOS_EP_MSG_MAX - sizeof(struct kos_uart_rsp))
        {
            want = KOS_EP_MSG_MAX - sizeof(struct kos_uart_rsp);
        }
        struct kos_uart_rsp rsp;
        rsp.status = 0;
        rsp.rsv = 0;
        uint32_t const got = kos_byte_ring_pop(&sh->rx, out + sizeof(rsp), want);
        rsp.len = static_cast<uint16_t>(got);
        mem_copy(out, &rsp, sizeof(rsp));
        (void)kos_reply(reply_cap, out, sizeof(rsp) + got);
        return;
    }
    case KOS_UART_STATS:
    {
        unsigned char out[sizeof(struct kos_uart_rsp) + sizeof(struct kos_uart_stats)];
        struct kos_uart_rsp rsp;
        rsp.status = 0;
        rsp.len = static_cast<uint16_t>(sizeof(struct kos_uart_stats));
        rsp.rsv = 0;
        mem_copy(out, &rsp, sizeof(rsp));
        mem_copy(out + sizeof(rsp), &sh->stats, sizeof(sh->stats));
        (void)kos_reply(reply_cap, out, sizeof(out));
        return;
    }
    case KOS_UART_CONFIGURE:
    {
        // The device belongs to the IRQ thread, so the service thread cannot program it.
        // The baud divisor is unwritable while TE/RE are set on every in-tree chip, so
        // this is refused rather than queued: a queued CONFIGURE would either reprogram
        // mid-frame or lie.
        reply_status(reply_cap, -KOS_ENOSYS, 0);
        return;
    }
    default:
    {
        reply_status(reply_cap, -KOS_EINVAL, 0);
        return;
    }
    }
}

// Recv/dispatch loop. Returns only when the endpoint dies, which is the respawn signal.
inline void serve_loop(Shared* sh)
{
    unsigned char msg[KOS_EP_MSG_MAX];
    while (true)
    {
        struct kos_recv_info info;
        long const n = kos_recv(KOS_UART_CAP_EP, msg, sizeof(msg), &info);
        if (n < 0)
        {
            break; // endpoint dead (EPIPE) or a bad cap: let the bring-up respawn us
        }
        serve_one(sh, msg, static_cast<size_t>(n), info.reply_cap);
    }
}

} // namespace uart
} // namespace kickos

#endif
