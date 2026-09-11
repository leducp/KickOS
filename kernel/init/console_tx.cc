// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Buffered console TX ring (see console_tx.h), drained by a TX interrupt where the backend
// has one and in the producer's own context where it has none.
//
// THE UNIT OF ATOMICITY IS A LINE, and console_tx_insert_line holds it: one IrqLock spans the
// whole copy, or the line is refused. console_tx_write promises no such thing, its IrqLock
// spanning ONE ring chunk, so a write wider than the free space can be interleaved at a chunk
// boundary by a concurrent producer.

#include <kickos/irq_route.h>
#include <kickos/console_tx.h>

#include <kickos/config/limits.h>
#include <kickos/diag.h>
#include <kickos/instance.h>
#include <kickos/irq.h>
#include <kickos/irqlock.h>
#include <kickos/arch/arch.h>

#include <kickos/sys/atomic.h>

namespace kickos
{
    void kpanic(char const* msg) __attribute__((noreturn));
}

namespace
{
    using kickos::Atomic;
    using kickos::Order;

    // TWO INDEPENDENT BUFFERS SHARE ONE RING, and only the ordinary one sizes it. A fault
    // record wider than an ordinary line would be refused, and the reporter would take the
    // locked writer on the descent the red-zone gate measures.
    static_assert(KDIAG_FAULT_LINE_MAX <= KICKOS_DIAG_LINE_MAX,
                  "the fault reporter's line is wider than the one the console ring is sized "
                  "from, so the ring cannot take it whole");

    // Only head/tail are shared between the thread producer and the drain ISR. Every
    // other field is set once at init and read-only after.
    struct ConsoleTxRing
    {
        console_tx_backend const* backend = nullptr;
        char* buf = nullptr;
        uint32_t size = 0; // power of two; usable capacity = size - 1
        uint32_t mask = 0;
        Atomic<uint32_t, Order::RELAXED> head = 0; // bytes queued; written by a producer
        Atomic<uint32_t, Order::RELAXED> tail = 0; // bytes drained; written by the drain OR a producer
        int irq_line = -1;          // TX IRQ line (from the backend); console_tx_deinit detaches it
        bool armed = false;
        // Set across the copy in console_tx_insert_line. IrqLock masks interrupts, so nothing
        // ASYNCHRONOUS can land in there, but a synchronous CPU fault (illegal instruction,
        // MPU, bus) is not gated by the mask, and its reporter writes to this same console. A
        // nested insert would build its line at the head this one has not published yet and
        // then have its publication overwritten by this one's. It is refused instead.
        bool inserting = false;
        // Set by whichever producer is draining in its own context, on a backend with no TX
        // interrupt. The INSERT is serialised by IrqLock; the DRAIN must not be, or the
        // masked window would be a transmission again, so it needs an exclusion of its own.
        // A producer that finds a drainer running queues its line and returns: the running
        // drainer will carry it, because it re-reads head every pass.
        bool draining = false;

        // Indices stay in [0, size); power-of-two size makes (head - tail) & mask the
        // used count (unsigned wrap reduces mod size). One slot reserved so head==tail
        // is unambiguously empty.
        uint32_t used() const { return (head - tail) & mask; }
        uint32_t space() const { return size - 1u - used(); }
    };

    constinit kickos::InstanceLocal<ConsoleTxRing> g_tx_all;

    ConsoleTxRing& tx()
    {
        return g_tx_all.get();
    }

    // Every synchronous poll is bounded so a dead TX channel cannot hang panic, fault or
    // boot. Matches the chips' own TX_POLL_TIMEOUT.
    constexpr uint32_t DRAIN_POLL_CAP = KICKOS_POLL_SPIN_MAX;

    bool wait_slot()
    {
        ConsoleTxRing& r = tx();
        for (uint32_t i = 0; i < DRAIN_POLL_CAP; i++)
        {
            if (r.backend->slot_free() != 0)
            {
                return true;
            }
        }
        return false;
    }

    // Caller MUST have the TX IRQ disabled. On a stuck channel this DROPS the undrained
    // bytes rather than hang.
    void drain_sync()
    {
        ConsoleTxRing& r = tx();
        uint32_t const head = r.head;
        uint32_t tail = r.tail;
        while (tail != head)
        {
            if (not wait_slot())
            {
                r.tail = head;
                return;
            }
            r.backend->push(static_cast<uint8_t>(r.buf[tail]));
            // Publish AFTER each byte, never once at the end. A synchronous CPU fault
            // (illegal instruction, MPU, bus) is not gated by the interrupt mask, so it can
            // land mid-loop, and its handler flushes again; a stale tail would make that
            // flush re-push bytes already sent, doubling output before the panic banner.
            tail = (tail + 1u) & r.mask;
            r.tail = tail;
        }
    }

    // Runs at the CALLER's interrupt level with no IrqLock held, so the drain ISR can run.
    // False means nothing drained in the whole window, so the ISR cannot run: a stuck
    // channel, or a caller that reached console_tx_write with interrupts already masked.
    bool wait_space()
    {
        ConsoleTxRing& r = tx();
        for (uint32_t i = 0; i < DRAIN_POLL_CAP; i++)
        {
            if (r.space() != 0)
            {
                return true;
            }
        }
        return false;
    }

    // Caller MUST hold IrqLock. Copies as much of [buf, buf+n) as the ring will take and
    // returns that count; 0 means full. Never caches head across a call, so a producer
    // that ran during a lock gap is picked up.
    uint32_t enqueue_locked(char const* buf, size_t n)
    {
        ConsoleTxRing& r = tx();
        uint32_t chunk = r.space();
        if (n < chunk)
        {
            chunk = static_cast<uint32_t>(n);
        }
        if (chunk == 0)
        {
            return 0;
        }
        bool const was_empty = (r.used() == 0);
        uint32_t idx = r.head;
        for (uint32_t i = 0; i < chunk; i++)
        {
            r.buf[idx] = buf[i];
            idx = (idx + 1u) & r.mask;
        }
        KICKOS_CONSOLE_TX_BARRIER();
        r.head = idx;
        r.backend->irq_enable();
        // With a transition-triggered TX interrupt, enabling the IRQ on an idle channel
        // raises nothing: only this byte's completion event starts the drain ISR.
        //   RX SCI TXI: REQUIRED. RX72M HW manual Rev.1.20 section 42.12.2(1) p.2308, a
        //               TXI request is not generated "by setting the SCR.TIE bit to 1 while
        //               the setting of the SCR.TE bit is 1". Same page, Note 2: gate a burst
        //               at the ICU and NEVER by toggling TIE, because clearing TIE discards
        //               an internally retained request.
        //   XMC TBIEN:  REQUIRED. The USIC event is edge-per-word (RM V1.3 18.2.2.4
        //               p.18-18), so an idle channel produces no event at all.
        //   K64F TDRE:  harmless immediate send, level-asserted while the buffer is empty
        //               (RM Rev.4 52.3.5; S1 resets to 0xC0 untransmitted).
        //   PL011 FEN=0: the priming runs there on the analogy above, not on a citation.
        uint32_t const tail = r.tail;
        if (was_empty and idx != tail and r.backend->slot_free() != 0)
        {
            r.backend->push(static_cast<uint8_t>(r.buf[tail]));
            r.tail = (tail + 1u) & r.mask;
        }
        return chunk;
    }

    // Runs with interrupts UNMASKED: a synchronous write is the long operation this file
    // keeps out of a masked span. The re-read is console_chip_writable, which stays true
    // through a handover: this caller can be the in-flight writer a publish is draining, and
    // refusing it truncates the message it is finishing.
    void write_unbuffered(char const* buf, size_t n)
    {
        if (console_chip_writable() == 0)
        {
            return;
        }
        console_chip_writer_enter();
        arch_console_write_sync(buf, n);
        console_chip_writer_leave();
    }

    void console_tx_isr_trampoline(void*) { console_tx_isr(); }
}

extern "C"
{

void console_tx_init(console_tx_backend const* be, char* storage, uint32_t size, int irq_line)
{
    ConsoleTxRing& r = tx();
    r.backend = be;
    r.buf = storage;
    r.size = size;
    r.mask = size - 1u;
    r.head = 0;
    r.tail = 0;
    r.irq_line = irq_line; // set BEFORE armed: deinit must never see armed with a stale line
    r.armed = true;
}

int console_tx_armed(void) { return static_cast<int>(tx().armed); }

static void drain_in_producer(void);

// One line, indivisibly, or nothing (nonzero on success). NEVER WAITS UNDER THE LOCK, which
// makes it safe from ISR and fault context. A line that does not fit is refused WHOLE and the
// caller owes it NO fallback.
//
// A LINE THE RING CANNOT TAKE DOES NOT GO OUT. The kernel console is a DEBUG facility, so
// losing a line under console pressure is the honest outcome; what is not acceptable is a
// SPLIT line, and any direct write while the ring holds bytes produces one: the drain and
// the direct writer are two writers at one device and interleave mid-line.
//
// The one direct write left is the UNARMED ring, before console_tx_init has run. No ring means
// no drain means no second writer, and early-boot output predates the ring. That case is
// decided under the same lock as everything else, so it cannot race an arm.
//
// CRLF is expanded during the copy. A caller-side cooked buffer would stand on the fault
// reporter's descent, which the trap red-zone gate measures (kickos/diag.h).
int console_tx_insert_line(char const* buf, size_t n, int crlf)
{
    ConsoleTxRing& r = tx();
    if (n == 0)
    {
        return 1;
    }

    bool unbuffered = false;
    {
        kickos::IrqLock lock;
        if (not r.armed)
        {
            unbuffered = true;
        }
        else
        {
            if (r.inserting)
            {
                return 0;
            }
            uint32_t needed = static_cast<uint32_t>(n);
            if (crlf != 0)
            {
                for (size_t i = 0; i < n; i++)
                {
                    if (buf[i] == '\n')
                    {
                        needed++;
                    }
                }
            }
            if (needed > r.space())
            {
                return 0;
            }

            bool const was_empty = (r.used() == 0);
            uint32_t idx = r.head;
            r.inserting = true;
            for (size_t i = 0; i < n; i++)
            {
                if (crlf != 0 and buf[i] == '\n')
                {
                    r.buf[idx] = '\r';
                    idx = (idx + 1u) & r.mask;
                }
                r.buf[idx] = buf[i];
                idx = (idx + 1u) & r.mask;
            }
            KICKOS_CONSOLE_TX_BARRIER();
            r.head = idx;
            r.inserting = false;
            r.backend->irq_enable();

            // A transition-triggered TX interrupt raises nothing on an idle channel, so the
            // first byte is pushed here. Citations in enqueue_locked.
            //
            // NOT WHILE A PRODUCER DRAIN OWNS A BYTE. That drain takes its byte under this
            // same lock and pushes it with the lock open, so between the two the ring reads
            // EMPTY while a byte is still going to the device. Priming on that reading puts a
            // second writer on the wire and splits the line already in flight, which is the
            // defect the whole drop rule exists to remove.
            uint32_t const tail = r.tail;
            if (was_empty and not r.draining and idx != tail and r.backend->slot_free() != 0)
            {
                r.backend->push(static_cast<uint8_t>(r.buf[tail]));
                r.tail = (tail + 1u) & r.mask;
            }
        }
    }

    if (unbuffered)
    {
        console_write_line_sync(buf, n);
        return 1;
    }
    drain_in_producer();
    return 1;
}

static void drain_in_producer(void)
{
    ConsoleTxRing& r = tx();
    {
        kickos::IrqLock lock;
        if (not r.armed or r.irq_line >= 0 or r.draining)
        {
            return;
        }
        r.draining = true;
    }
    while (true)
    {
        uint8_t b = 0;
        {
            // THE BYTE IS TAKEN, NOT BORROWED. tail advances here, under the lock, BEFORE the
            // device write, so this byte belongs to this drain alone. Advancing after the
            // push instead lets a flush landing in the open window send the same byte and
            // this path send it again: an external audit reproduced the duplicate as
            // ABCDEFGHB. Validating the sampled tail after the write cannot fix that, because
            // the second send has already happened by the time the check runs.
            kickos::IrqLock lock;
            if (r.tail == r.head)
            {
                r.draining = false;
                return;
            }
            b = static_cast<uint8_t>(r.buf[r.tail]);
            r.tail = (r.tail + 1u) & r.mask;
        }
        // Bounded, like every other poll on this path: a wedged device must not hang a
        // producer that was only trying to print.
        if (not wait_slot())
        {
            kickos::IrqLock lock;
            uint32_t const h = r.head;
            r.tail = h; // discard rather than spin forever; the bytes are already lost
            r.draining = false;
            return;
        }
        r.backend->push(b);
    }
}

void console_tx_write(char const* buf, size_t n)
{
    ConsoleTxRing& r = tx();
    // Zero length would skip the chunk loop and fall into the synchronous fallback below,
    // draining a whole queued ring inside one masked span.
    if (n == 0)
    {
        return;
    }
    if (not r.armed)
    {
        write_unbuffered(buf, n);
        return;
    }

    // The wait between chunks runs UNMASKED, so the masked window is one ring copy and not
    // one transmission: an unprivileged kos_kconsole_write cannot hold interrupts off for
    // the line time of its own output.
    size_t off = 0;
    while (off < n)
    {
        uint32_t queued = 0;
        bool armed_now = false;
        {
            kickos::IrqLock lock;
            armed_now = r.armed;
            if (armed_now)
            {
                queued = enqueue_locked(buf + off, n - off);
            }
        }
        // console_tx_deinit can land in the gap between two chunks. A byte queued after it
        // detaches the handler is never drained and flush_sync cannot recover it, and
        // enqueue_locked's irq_enable would undo its irq_disable and leave a latched pend
        // the driver takes as spurious. So `armed` is re-read under the SAME lock as the
        // enqueue. The remainder is the tail of a message already on the wire, so it must
        // go out rather than drop: HANDING_OFF keeps the UART writable until this returns.
        if (not armed_now)
        {
            write_unbuffered(buf + off, n - off);
            return;
        }
        off += queued;
        if (off == n)
        {
            return;
        }
        if (not wait_space())
        {
            break;
        }
    }

    // Only reachable when nothing drained for a whole DRAIN_POLL_CAP window, so the ISR is
    // not running. drain_sync runs first to keep the bytes already queued ahead of the
    // remainder, including any a concurrent producer added.
    {
        kickos::IrqLock lock;
        if (r.armed)
        {
            r.backend->irq_disable();
            drain_sync();
            for (size_t i = off; i < n; i++)
            {
                if (not wait_slot())
                {
                    return; // stuck TX: give up rather than hang
                }
                r.backend->push(static_cast<uint8_t>(buf[i]));
            }
            return;
        }
    }
    write_unbuffered(buf + off, n - off);
}

// console_tx_deinit detaches the handler and NVIC-masks the TX line under the same IrqLock
// that enters HANDING_OFF, strictly before kos_console_publish flips to USER_OWNED, so this
// ISR has already stopped by the time a driver owns the console. That ordering is what stands
// in for the chip-writer bracket every other device poke takes.
void console_tx_isr(void)
{
    // ONE WRITER OF `tail` AT A TIME. At one kernel core the producers' own IrqLock masks
    // this line across their read-and-write of tail; above one core that mask reaches only
    // the core that took it and an insert primes the channel from another, so the drain
    // takes the same lock. The wait is safe from here: IrqLock masks before it acquires and
    // releases before it unmasks, so no core holds it with this line deliverable, and klock's
    // `owed` covers the one span where a core holds it at depth zero (kickos/klock.h).
#if KICKOS_KERNEL_CORES > 1
    kickos::IrqLock lock;
#endif
    ConsoleTxRing& r = tx();
    uint32_t const head = r.head;
    uint32_t tail = r.tail;
    while (tail != head and r.backend->slot_free() != 0)
    {
        r.backend->push(static_cast<uint8_t>(r.buf[tail]));
        // Publish per byte: a synchronous fault mid-drain flushes again, and a stale tail
        // would re-push already-sent bytes.
        tail = (tail + 1u) & r.mask;
        r.tail = tail;
    }
    if (tail == head)
    {
        r.backend->irq_disable();
    }
}

void console_tx_flush_sync(void)
{
    ConsoleTxRing& r = tx();
    if (not r.armed)
    {
        return;
    }
    // Under IrqLock so the TX-IRQ disable and the [tail, head) snapshot are atomic against
    // the drain ISR and any thread producer: a producer racing between the disable and
    // drain_sync's head read could re-enable the IRQ or extend head mid-drain. Nests under
    // kpanic_enter's own mask.
    kickos::IrqLock lock;
    r.backend->irq_disable();
    drain_sync();
}

// Call once, after irq_init(). The TX line's priority must land in the IrqLock-maskable
// band. No-op on sim and polled-only chips.
void console_buffer_init(void)
{
    char* buf = nullptr;
    uint32_t size = 0;
    int line = -1;
    console_tx_backend const* be = arch_console_tx_backend(&buf, &size, &line);
    if (be == nullptr or buf == nullptr or size == 0)
    {
        return;
    }
    // A backend with no TX interrupt still arms the ring; drain_in_producer carries it.
    if (line < 0)
    {
        console_tx_init(be, buf, size, line);
        return;
    }
    // A dropped attach would leave the ring armed but never drained: output fills it, falls
    // back to the bounded sync path, and looks like it works while every buffered write
    // stalls. A misconfigured TX line at boot is a port bug, so panic.
    if (not kickos::irq_attach(line, console_tx_isr_trampoline, nullptr))
    {
        kickos::kpanic(kickos::diag::kConsoleAttach);
    }
    // Arm the ring BEFORE the line can fire: the latch-and-coalesce contract redelivers any
    // pend latched on this line before boot the instant ISER is set, and console_tx_isr on a
    // zero-init ring would deref a NULL backend.
    console_tx_init(be, buf, size, line);
    kickos::irq_line_op(line, kickos::LineOp::CLEAR);
    kickos::irq_line_op(line, kickos::LineOp::UNMASK);
}

// Relinquish the buffered TX path so a userspace driver can take the UART. One IrqLock
// makes the four steps atomic against the drain ISR. console_tx_write holds the lock one
// chunk at a time, so it re-reads `armed` under the same lock as each enqueue. The disarmed
// guard also covers polled-only chips (mps2/virt/nrf51 never arm) and a re-publish. The
// caller holds the state at HANDING_OFF across this, never USER_OWNED, so the flush here and
// a synchronous fault mid-deinit both act on a kernel-owned, kernel-inited UART.
void console_tx_deinit(void)
{
    ConsoleTxRing& r = tx();
    if (not r.armed)
    {
        return;
    }
    kickos::IrqLock lock;
    console_tx_flush_sync();
    r.backend->irq_disable();
    if (r.irq_line >= 0)
    {
        kickos::irq_detach(r.irq_line);
    }
    r.armed = false;
}

} // extern "C"
