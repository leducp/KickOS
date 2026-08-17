// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The ring side of a published console: the TX ring, the doorbell, the CRLF posture and the
// flush protocol. No register and no device class. Every buffered console service layer
// binds to THIS, so a second copy of any rule below is a bug.
//
// A caller owns TX `head`, the driver's IRQ thread owns TX `tail`. One writer per index is
// what keeps the ring SPSC with no lock, which <byte_ring.h> requires and cannot check.

#ifndef KICKOS_SYS_CONSOLE_RING_H
#define KICKOS_SYS_CONSOLE_RING_H

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/sys/atomic.h>
#include <kickos/sys/byte_ring.h>
#include <kickos/sys/uart.h>

#include <stdint.h>

// The board's CRLF posture, set by the build. An out-of-tree consumer reaching this header
// through find_package(KickOS) gets no -D, so an undefined knob must mean "raw" rather than
// a compile error.
#ifndef KICKOS_CONSOLE_CRLF
#define KICKOS_CONSOLE_CRLF 0
#endif

namespace kickos::console
{

// The line cap the service thread rings to wake the consumer. Every buffered console
// driver's two-thread spawn puts it at this index.
enum
{
    KOS_CONSOLE_CAP_DOORBELL = KOS_SPAWN_DELEGATED_CAP0 + 1
};

// The write-policy bits a service stores per endpoint. They ARE the wire flags of
// <kickos/sys/uart.h>, stored verbatim, so there is one definition of each bit.
enum
{
    KOS_CONSOLE_MODE_MASK = KOS_UART_F_NONBLOCK
};

// Apply a KOS_UART_SET_MODE request. Returns 0, or the negative KOS_E* to reply with.
//
// `mode` is null for a service with NO unframed console arm: such a service must refuse,
// because a mode it stored would never be read and a caller would believe byte loss was
// enabled when the writes still block.
//
// `required` names mode bits the transport cannot clear. A ring whose consumer may never
// exist cannot honour a blocking write, that being unbounded, so it requires
// KOS_UART_F_NONBLOCK and REFUSES a request to clear it. A caller asking for back-pressure
// it cannot have is told so, instead of being given a hang; the framed KOS_UART_WRITE op is
// where such a caller gets an exact per-call count and can retry.
//
// An unknown bit is refused rather than masked away, so a flag this build does not know
// cannot read back as accepted. Nothing is stored on any refusal.
int32_t mode_apply(Atomic<uint32_t, Order::RELAXED>* mode, uint32_t flags, uint32_t required);

// Build the 36-byte KOS_UART_STATS payload out of the LIVE counters, field by field: the
// other thread keeps writing them, so no two fields need belong to the same instant.
//
// `tx_lost` is added into tx_dropped, for a service that counts part of its TX loss in a
// field of its own. A service that does not passes 0.
void stats_pack(uint8_t* wire, struct kos_uart_stats const* live, uint32_t tx_lost);

// The client side of the same reply.
void stats_unpack(struct kos_uart_stats* dst, uint8_t const* wire);

// ---------------------------------------------------------------------------------
// Queue bytes for transmit and ring the doorbell. Returns the bytes ACCEPTED, short of n on
// a full ring; the retry policy for the remainder belongs to the caller. EVERY producer path
// must go through here rather than open-coding push + notify.
//
// Rung on EVERY call, including one that accepted nothing: with the ring full the parked
// consumer has no other wake source, and gating on an idle -> busy edge loses the wake the
// other way (the consumer can drain, disarm and park between the test and the push).
uint32_t tx_write(struct kos_byte_ring* tx, struct kos_uart_stats* stats,
                         uint8_t const* p, uint32_t n);

constexpr uint64_t KOS_CONSOLE_FLUSH_SLEEP_NS = 100000u; // ~1 byte time at 115200 baud
// Bounds flush() ONLY. A write does not get a budget: see push_all.
constexpr uint32_t KOS_CONSOLE_FLUSH_MAX = 2000u;        // ~200 ms total

// ---------------------------------------------------------------------------------
// Block until nothing is left to send, or the budget runs out. Returns what is STILL
// outstanding, so 0 means drained.
//
// BOUNDED, unlike push_all, and the difference is the return value. This reports what it
// could not drain, so a caller CAN tell "drained" from "gave up with N left"; that is the
// property whose absence makes a capped write incoherent. It also sits on the ORDERED
// shutdown path, where waiting forever costs the ability to halt at all, and losing a tail
// is the lesser failure.
//
// `inflight`, when non-null, counts bytes the consumer took OUT of the ring and handed to
// the device without seeing them complete. On such a device an empty ring is not an empty
// channel, and what it hides is the tail of the stream. Pass nullptr where no such state
// exists.
uint32_t flush(struct kos_byte_ring* tx, Atomic<uint32_t, Order::RELAXED> const* inflight);

// ---------------------------------------------------------------------------------
// Queue ALL of a raw console write. Always returns n: it does not give up.
//
// UNBOUNDED, and that is the contract rather than an oversight. A capped "blocking" write
// stalls its caller AND still loses the tail, and the caller cannot tell which happened, so
// it offers neither guarantee. A caller that must not wait sets KOS_UART_F_NONBLOCK and
// takes the short count instead.
//
// TERMINATION IS THE CONSUMER, not a counter, so this is only safe where a consumer is
// guaranteed to exist. On a transport whose consumer may never appear the service seats
// KOS_UART_F_NONBLOCK as the endpoint's initial mode; see each shared_init.
//
// The retry has to live on this side: a plain send has no reply, so the sender can neither
// be told about a short accept nor retry it, and the stream would be spliced mid-token.
// Staying out of kos_recv until the ring has taken everything also blocks the NEXT send on
// the endpoint, which is the console path's only backpressure.
uint32_t push_all(struct kos_byte_ring* tx, struct kos_uart_stats* stats,
                         uint8_t const* p, uint32_t n);

// One segment of a console write under the caller's chosen policy. Blocking is the default
// because a console that silently truncates is worse than one that paces its producer.
uint32_t push_some(struct kos_byte_ring* tx, struct kos_uart_stats* stats,
                          uint8_t const* p, uint32_t n, uint32_t mode);

// Staging window for the CRLF expansion. Any size is correct because the loop chunks.
constexpr uint32_t KOS_CONSOLE_COOK_CHUNK = 64;

// Expand '\n' to "\r\n" from `in` into `out`, stopping at whichever runs out first. Returns
// the cooked length written; `*taken` receives the INPUT bytes it represents.
//
// A '\n' is never split from its '\r': the loop stops before an input byte whose expansion
// would not fit whole, so `*taken` is always a clean resume point.
uint32_t cook_crlf(uint8_t const* in, uint32_t in_n, uint8_t* out,
                          uint32_t out_cap, uint32_t* taken);

// The CONSOLE write: push p under `mode`, cooking '\n' to "\r\n" where the board asks for
// it. Returns the INPUT bytes accepted, so a caller's `n - took` stays input-byte
// accounting.
//
// The CRLF posture does NOT depend on the mode: a non-blocking write loses bytes, it does
// not change what the wire looks like.
//
// The console arm cooks; a KOS_UART_WRITE op goes through tx_write and stays
// byte-transparent, because a client sending binary over the wire ABI must get its bytes
// back.
//
// EVERY '\n' is expanded, so an app writing "\r\n" gets "\r\r\n". On a short accept the
// input-to-cooked mapping is reported at the last CHUNK boundary, so tx_dropped can
// over-count by up to one chunk.
uint32_t write_console(struct kos_byte_ring* tx, struct kos_uart_stats* stats,
                             uint8_t const* p, uint32_t n, uint32_t mode);

}

#endif
