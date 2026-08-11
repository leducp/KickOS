// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Buffered-UART service over the raw UART class: the shared ring block, the wire ABI of
// <kickos/sys/uart.h>, and the two loops the driver's two threads run.
//
// Every kos_uart_* call touches registers, so all of them may be made ONLY from the IRQ
// thread. That thread owns every register plus TX `tail` and RX `head`; the service thread
// owns TX `head` and RX `tail`. One writer per index is what keeps both rings SPSC with no
// lock, which <byte_ring.h> requires and cannot check.

#ifndef KICKOS_SYS_UART_SERVICE_H
#define KICKOS_SYS_UART_SERVICE_H

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/driver/uart.h>
#include <kickos/sys/byte_ring.h>
#include <kickos/sys/bytes.h> // mem_copy
#include <kickos/sys/driver_service.h>
#include <kickos/sys/errno.h>
#include <kickos/sys/uart.h>

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

// The board's CRLF posture, set by the build. An out-of-tree consumer reaching this header
// through find_package(KickOS) gets no -D, so an undefined knob must mean "raw" rather than
// a compile error.
#ifndef KICKOS_CONSOLE_CRLF
#define KICKOS_CONSOLE_CRLF 0
#endif

namespace kickos
{
namespace uart
{

// Child cap indices the two threads read. LINE and EP share an index because they are
// different threads' cap tables.
enum
{
    // Service thread: the request endpoint (WAIT) and the line cap it rings (SIGNAL).
    KOS_UART_CAP_EP = KOS_SPAWN_DELEGATED_CAP0,
    KOS_UART_CAP_DOORBELL = KOS_SPAWN_DELEGATED_CAP0 + 1,
    // IRQ thread: the line cap it waits on (WAIT).
    KOS_UART_CAP_LINE = KOS_SPAWN_DELEGATED_CAP0
};

// The shared block, in ONE power-of-two naturally-aligned allocation: the RAM arm of
// grant_region_admissible requires that of every caller, privileged included. Header plus
// payload must fit one power of two, which the static_assert below enforces.
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
    // Set by the IRQ thread once its own bring-up has run. A bring-up MUST spin on this,
    // bounded, BEFORE it spawns the service thread, and fail loud on the timeout: once the
    // service thread holds a WAIT cap, recv_holders never reaches 0, nothing reclaims the
    // console, and a diagnostic goes to an endpoint nobody is draining.
    volatile uint32_t ready;
    unsigned char tx_buf[KOS_UART_TX_SIZE];
    unsigned char rx_buf[KOS_UART_RX_SIZE];
};

static_assert(sizeof(struct Shared) <= KOS_UART_BLOCK_SIZE,
              "the UART shared block must fit one 1 KiB power-of-two grant");

// Lay out the block. Not thread-safe: the bring-up calls it before either thread exists.
inline void shared_init(Shared* s)
{
    mem_zero(s, sizeof(*s));
    kos_byte_ring_init(&s->tx, s->tx_buf, KOS_UART_TX_SIZE);
    kos_byte_ring_init(&s->rx, s->rx_buf, KOS_UART_RX_SIZE);
}

// The whole granted block: the shared rings plus the class config the IRQ thread opens
// the device with. Every thread reaches it through its thread ARG.
struct Ctx
{
    struct Shared sh;
    struct kos_uart_config ucfg;
};

static_assert(sizeof(Ctx) <= KOS_UART_BLOCK_SIZE,
              "the UART driver context must fit the 1 KiB shared grant");

// The offset a generic bring-up polls the readiness latch through, it being unable to name
// Ctx. Never write the offset as a literal in a descriptor: the latch must be the volatile
// uint32_t this expression locates.
constexpr uint16_t KOS_UART_READY_OFFSET =
    static_cast<uint16_t>(offsetof(Ctx, sh) + offsetof(Shared, ready));

// Lay out the block and fill the class config from the service cfg. `fallback_baud` is what
// a cfg naming no rate asks for, and 0 there means "keep the divisor the boot console left"
// rather than "0 baud".
inline int ctx_init(Ctx* ctx, struct kos_service_cfg const* cfg, uint32_t fallback_baud)
{
    shared_init(&ctx->sh);
    uint32_t baud = cfg->hz;
    if (baud == 0u)
    {
        baud = fallback_baud;
    }
    ctx->ucfg.base = cfg->mmio_base;
    ctx->ucfg.stats = &ctx->sh.stats;
    ctx->ucfg.baud = baud;
    ctx->ucfg.data_bits = 8;
    ctx->ucfg.parity = KOS_UART_PARITY_NONE;
    ctx->ucfg.stop_bits = 1;
    ctx->ucfg.rsv = 0;
    return 0;
}

// ---------------------------------------------------------------------------------
// Staging segment for one service pass, used first for RX and then for TX. Any size is
// correct: the pass is re-entered on the next wake.
constexpr uint32_t KOS_UART_IRQ_SEG = 64;

// One service pass: fill RX, then drain TX.
//
// A wake is not proof of a hardware event, kos_irq_notify being a pure post, so both
// transfers must tolerate an idle device and both rings a zero-length move.
inline void irq_pass(struct kos_uart* dev, Shared* sh)
{
    unsigned char seg[KOS_UART_IRQ_SEG];

    // rx_dropped here and rx_bytes inside kos_uart_read are written only from the IRQ
    // thread, which is what keeps them single-writer.
    uint32_t const got = kos_uart_read(dev, seg, KOS_UART_IRQ_SEG);
    uint32_t const kept = kos_byte_ring_push(&sh->rx, seg, got);
    sh->stats.rx_dropped += got - kept;

    // THE LOOP IS WHAT MAKES THE DISARM CORRECT. kos_uart_write disarms the TX source on a
    // call it accepted whole, so a segmented drain that stopped after a full segment would
    // leave the device disarmed with the ring still loaded and nothing to wake it. The
    // have == 0 case still calls write, and that call is the disarm.
    //
    // Peek then drop, never pop then write: the device may take fewer bytes than it was
    // offered and a popped byte it refused has nowhere to go back to.
    while (true)
    {
        uint32_t const have = kos_byte_ring_peek(&sh->tx, seg, KOS_UART_IRQ_SEG);
        uint32_t const took = kos_uart_write(dev, seg, have);
        kos_byte_ring_drop(&sh->tx, took);
        if (took < have)
        {
            break;
        }
        if (have < KOS_UART_IRQ_SEG)
        {
            break;
        }
    }
}

// TRANSITIONAL: two backends still carry a service_irq() method instead of the class API,
// rx72m/rxsci (receive arrives on a different vector than transmit) and the sim loopback in
// system/init/sim/service_list_uart.cc.
template <typename Uart>
inline void irq_pass(Uart* dev, Shared*)
{
    dev->service_irq();
}

// Give the device back quiet. FLUSH BEFORE CLOSE: close stops the channel, and on a part
// whose stop is a mode-disable rather than a drain it truncates a frame still shifting.
inline void dev_shutdown(struct kos_uart* dev)
{
    (void)kos_uart_flush(dev);
    (void)kos_uart_close(dev);
}

// TRANSITIONAL, with the irq_pass overload above: such a backend has no close, so its
// device is left as its own driver set it.
template <typename Uart>
inline void dev_shutdown(Uart*)
{
}

// ---------------------------------------------------------------------------------
// The IRQ thread. It alone touches the device.
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
        // "Found nothing" is judged from what THIS thread owns: reading tx_bytes would
        // race the service thread, which owns it.
        uint32_t const rx_before = sh->stats.rx_bytes;
        bool const tx_had_work = (kos_byte_ring_used(&sh->tx) != 0u);
        irq_pass(&dev, sh);
        if (sh->stats.rx_bytes == rx_before and not tx_had_work)
        {
            sh->stats.irq_spurious++; // a doorbell with an empty ring, or a stray raise
        }
    }
    dev_shutdown(&dev);
    exit(0);
}

// Per-byte cap on the first-light poll: a channel that never reports room costs a delay,
// not the IRQ thread.
constexpr uint32_t KOS_UART_TX_SPIN_MAX = 1000000u;

// Direct-to-device diagnostic, not stdio and not the ring, and legal only before the
// line's first irq_wait.
inline void win_puts(struct kos_uart* dev, char const* s)
{
    for (; *s != '\0'; s++)
    {
        unsigned char const b = static_cast<unsigned char>(*s);
        for (uint32_t i = 0; i < KOS_UART_TX_SPIN_MAX; i++)
        {
            if (kos_uart_write(dev, &b, 1u) == 1u)
            {
                break;
            }
        }
    }
}

// `prime` is NOT derivable from the trigger mode: xmcuartirq is EDGE and primes, rxsci is
// EDGE and must not. Priming a source whose only raise is a transfer taken with the source
// already armed waits on a transition that has already happened; not priming a source that
// needs it loses the first wake.
struct UartParams
{
    char const* open_fail; // the kos_panic tag
    char const* announce;
    bool prime;
};

// NEVER exits on an open failure: once root has closed its own cap the service thread is the
// endpoint's sole receiver and would keep accepting stdout into a ring nothing drains, so the
// panic path is what reclaims the console (D6).
template <typename Uart>
void irq_thread(Ctx* ctx, UartParams const& p)
{
    Uart dev;
    if (kos_uart_open(&dev, &ctx->ucfg) < 0)
    {
        kos_panic(p.open_fail);
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
// Queue bytes for transmit and ring the doorbell. Returns the bytes ACCEPTED, short of n on
// a full ring; the retry policy for the remainder belongs to the caller. EVERY producer path
// must go through here rather than open-coding push + notify, and KOS_UART_CAP_DOORBELL must
// be the line's SIGNAL cap.
//
// Rung on EVERY call, including one that accepted nothing: with the ring full the parked IRQ
// thread has no other wake source, and gating on an idle -> busy edge loses the wake the
// other way (the IRQ thread can drain, disarm and park between the test and the push).
inline uint32_t tx_write(Shared* sh, unsigned char const* p, uint32_t n)
{
    uint32_t const took = kos_byte_ring_push(&sh->tx, p, n);
    sh->stats.tx_bytes += took;
    (void)kos_irq_notify(KOS_UART_CAP_DOORBELL);
    return took;
}

// ---------------------------------------------------------------------------------
// Block until the TX ring is empty, or the budget runs out. Returns the bytes STILL queued,
// so 0 means drained.
//
// A ZERO-LENGTH plain send on a console endpoint means FLUSH: length is the only field a
// plain send carries.
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
// Queue ALL of a raw console write, waiting for room instead of dropping the tail. Returns
// the bytes accepted, short of n only when the budget expired on a channel that is not
// draining.
//
// The retry has to live on this side: a plain send has no reply, so the sender can neither
// be told about a short accept nor retry it, and the stream would be spliced mid-token.
// Staying out of kos_recv until the ring has taken everything also blocks the NEXT send on
// the endpoint, which is the console path's only backpressure.
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

// Staging window for the CRLF expansion. Any size is correct because the loop chunks.
constexpr uint32_t KOS_UART_COOK_CHUNK = 64;

// Expand '\n' to "\r\n" from `in` into `out`, stopping at whichever runs out first. Returns
// the cooked length written; `*taken` receives the INPUT bytes it represents.
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
// The console arm cooks; the KOS_SVC_UART WRITE op goes through tx_write and stays
// byte-transparent: a client sending binary over the wire ABI must get its bytes back.
//
// EVERY '\n' is expanded, so an app writing "\r\n" gets "\r\r\n". On a short accept the
// input-to-cooked mapping is reported at the last CHUNK boundary, so tx_dropped can
// over-count by up to one chunk.
inline uint32_t console_write_all(Shared* sh, unsigned char const* p, uint32_t n)
{
#if KICKOS_CONSOLE_CRLF
    // n == 0 must reach the pump in BOTH postures: the doorbell count must not depend on
    // the CRLF posture.
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

// The service thread. Replies out of ring state and never touches the device.
inline void reply_status(kos_cap_t reply_cap, int32_t status, uint16_t len)
{
    struct kos_uart_rsp rsp;
    rsp.status = status;
    rsp.len = len;
    rsp.rsv = 0;
    (void)kos_reply(reply_cap, &rsp, sizeof(rsp));
}

inline void serve_one(Shared* sh, unsigned char const* msg, size_t n, kos_cap_t reply_cap)
{
    if (reply_cap == KOS_CAP_NONE)
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
        // A short accept is NOT an error: the client sees `len < req.len` and retries,
        // which is the only backpressure a driver that must never mask interrupts has.
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
        // The device belongs to the IRQ thread, and the baud divisor is unwritable while
        // TE/RE are set on every in-tree chip. Queueing a CONFIGURE would reprogram
        // mid-frame or lie, so it is refused.
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

// The same loop with the CONSOLE arm: this endpoint carries TWO protocols, a kos_call
// being a kos_uart_req frame and a plain send raw console bytes (kos_console_publish
// routes libc stdout to cap 0 unframed), so the recv must be info-bearing to tell them
// apart.
inline void console_serve_loop(Shared* sh)
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
        if (info.reply_cap != KOS_CAP_NONE)
        {
            serve_one(sh, msg, static_cast<size_t>(n), info.reply_cap);
            continue;
        }
        if (n == 0)
        {
            (void)console_flush(sh); // zero-length plain send == flush
            continue;
        }
        // A plain send cannot report a short accept, so a producer that outruns the wire
        // would have its tail silently spliced away. Staying out of kos_recv until the ring
        // took it all is what paces it.
        uint32_t const took = console_write_all(sh, msg, static_cast<uint32_t>(n));
        sh->stats.tx_dropped += static_cast<uint32_t>(n) - took;
    }
}

inline void console_thread(void* arg)
{
    console_serve_loop(&static_cast<Ctx*>(arg)->sh);
    exit(0);
}

// ---------------------------------------------------------------------------------
// The class-side half of the descriptor check. The generic validator cannot know that the
// loops above read KOS_UART_CAP_EP == 1, KOS_UART_CAP_DOORBELL == 2 and KOS_UART_CAP_LINE ==
// 1, so a descriptor that grants the right caps in the wrong ORDER passes valid() and stalls
// silently. Nothing else in the tree checks this.
//
// valid() only RANGE-checks ready_offset, so a literal 0 there points at the ring head, which
// is non-zero after shared_init: wait_ready returns true on its first read and THE BARRIER
// SILENTLY BECOMES A NO-OP.
constexpr bool desc_ok(driver::Descriptor const& d)
{
    return driver::ring_doorbell_shape_ok(d, KOS_UART_READY_OFFSET,
                                          static_cast<uint32_t>(KOS_UART_BLOCK_SIZE));
}

} // namespace uart
} // namespace kickos

#endif
