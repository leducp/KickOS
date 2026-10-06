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

        // Once disarmed, `buf` holds kernel records bound for the published console's driver,
        // over indices of their own: a producer drain still unwinding reads [tail, head), which
        // the disarm left empty. [held_tail, held_open) is the committed records, each a 16-bit
        // length and its bytes, held_taken bytes of the first taken already. [held_open,
        // held_head) is the open records' entries, interleaved: each record a marker, then its
        // lines.
        uint32_t held_head = 0;
        uint32_t held_tail = 0;
        uint32_t held_taken = 0;
        uint32_t held_open = 0;
        bool held_changing = false;

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
// losing an ordinary line under console pressure is the honest outcome; what is not acceptable
// is a SPLIT line, and any direct write while the ring holds bytes produces one: the drain and
// the direct writer are two writers at one device and interleave mid-line. A fault record's
// line makes room instead (console_tx_insert_record_line).
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
        return 0;
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
        return static_cast<int>(n);
    }
    drain_in_producer();
    return static_cast<int>(n);
}

// A FAULT RECORD'S LINE IS NOT LOST TO A FULL RING. Under the mask, the oldest queued bytes go
// out through the polled writer, in ring order, until the line fits, and the line then queues
// as any other: the ISR and every producer are held off for the span, so the wire has one
// writer, and the span is at most the line's own expanded length of wire time. A producer drain
// holding a byte it took is the one writer the mask does not stop, so that ring is not touched.
int console_tx_insert_record_line(char const* buf, size_t n, int crlf)
{
    ConsoleTxRing& r = tx();
    kickos::IrqLock lock;
    if (not r.armed or r.inserting or r.draining)
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
    if (needed > r.size - 1u)
    {
        return 0;
    }
    while (r.space() < needed)
    {
        uint32_t const tail = r.tail;
        uint32_t run = needed - r.space();
        if (run > r.size - tail)
        {
            run = r.size - tail;
        }
        arch_console_write_sync(r.buf + tail, run);
        r.tail = (tail + run) & r.mask;
    }
    return console_tx_insert_line(buf, n, crlf);
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

#if KICKOS_BENCH
uint32_t console_tx_used(void)
{
    ConsoleTxRing& r = tx();
    if (not r.armed)
    {
        return 0;
    }
    return r.used();
}

void console_tx_wait_progress(void)
{
    ConsoleTxRing& r = tx();
    if (not r.armed)
    {
        return;
    }
    // A backend with no TX interrupt is drained by its own producer, so the ring is empty by
    // the time an insert returns and nothing will happen here however long it spins.
    if (r.irq_line < 0)
    {
        return;
    }
    uint32_t const before = r.used();
    if (before == 0)
    {
        return;
    }
    // NO LOCK AND NO MASK. Waiting is the whole point, and the drain ISR is what has to run:
    // a wait that masked would stall the thing it waits for. Bounded like every other poll
    // in this file, so a wedged channel costs one window and not a hang.
    for (uint32_t i = 0; i < DRAIN_POLL_CAP; i++)
    {
        if (r.used() < before)
        {
            return;
        }
    }
}
#endif

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

static constexpr uint32_t HELD_HEADER = 2u;
static constexpr uint32_t HELD_LINE = 2u;
static constexpr uint32_t HELD_MARK = 6u;
// In a marker's id byte: the record dropped a line, so it drops every later one.
static constexpr uint8_t HELD_FULL = 0x80u;
static constexpr uint32_t HELD_ID_COUNT = 128u;

#ifdef KICKOS_HELD_STEP
// A host test's hook, run at every store a change of the open area makes.
extern "C" void kickos_held_step(void);
#define HELD_STEP() kickos_held_step()
#else
#define HELD_STEP() \
    do              \
    {               \
    } while (false)
#endif

// The store is being rearranged on this core: a panic landing inside the change leaves it unread.
// Another core's change the panic excludes by taking the lock.
static void held_change(ConsoleTxRing& r, bool changing)
{
    kickos::fence_release();
    r.held_changing = changing;
    kickos::fence_release();
}

static uint32_t held_len(ConsoleTxRing const& r, uint32_t at)
{
    return static_cast<uint32_t>(static_cast<uint8_t>(r.buf[at]))
           | (static_cast<uint32_t>(static_cast<uint8_t>(r.buf[at + 1u])) << 8);
}

static bool held_is_mark(ConsoleTxRing const& r, uint32_t at)
{
    return r.buf[at + 1u] == 0;
}

// The size of the open entry at `at`, or 0 where it does not fit below `end`.
static uint32_t held_fit(ConsoleTxRing const& r, uint32_t at, uint32_t end)
{
    if (at >= end or end - at < HELD_LINE)
    {
        return 0;
    }
    uint32_t size = HELD_LINE + static_cast<uint8_t>(r.buf[at + 1u]);
    if (held_is_mark(r, at))
    {
        size = HELD_MARK;
    }
    if (size > end - at)
    {
        return 0;
    }
    return size;
}

static bool held_record_fits(ConsoleTxRing const& r, uint32_t at, uint32_t end)
{
    return at < end and end - at >= HELD_HEADER and held_len(r, at) <= end - at - HELD_HEADER;
}

static uint8_t held_id(ConsoleTxRing const& r, uint32_t at)
{
    return static_cast<uint8_t>(static_cast<uint8_t>(r.buf[at]) & ~HELD_FULL);
}

static uint32_t held_key_at(ConsoleTxRing const& r, uint32_t at)
{
    uint32_t key = 0;
    for (uint32_t i = 0; i < 4u; i++)
    {
        key = key | (static_cast<uint32_t>(static_cast<uint8_t>(r.buf[at + 2u + i])) << (8u * i));
    }
    return key;
}

static uint32_t held_find(ConsoleTxRing const& r, uint32_t key)
{
    uint32_t at = r.held_open;
    for (uint32_t size = held_fit(r, at, r.held_head); size != 0;
         size = held_fit(r, at, r.held_head))
    {
        if (held_is_mark(r, at) and held_key_at(r, at) == key)
        {
            return at;
        }
        at = at + size;
    }
    return r.held_head;
}

static bool held_id_used(ConsoleTxRing const& r, uint8_t id)
{
    uint32_t at = r.held_open;
    for (uint32_t size = held_fit(r, at, r.held_head); size != 0;
         size = held_fit(r, at, r.held_head))
    {
        if (held_is_mark(r, at) and held_id(r, at) == id)
        {
            return true;
        }
        at = at + size;
    }
    return false;
}

static uint32_t held_record_bytes(ConsoleTxRing const& r, uint8_t id)
{
    uint32_t bytes = 0;
    uint32_t at = r.held_open;
    for (uint32_t size = held_fit(r, at, r.held_head); size != 0;
         size = held_fit(r, at, r.held_head))
    {
        if (not held_is_mark(r, at) and held_id(r, at) == id)
        {
            bytes = bytes + size - HELD_LINE;
        }
        at = at + size;
    }
    return bytes;
}

static void held_move_down(char* b, uint32_t dst, uint32_t from, uint32_t to)
{
    while (from != to)
    {
        b[dst] = b[from];
        HELD_STEP();
        dst++;
        from++;
    }
}

static void held_reverse(char* b, uint32_t lo, uint32_t hi)
{
    while (lo + 1u < hi)
    {
        hi--;
        char const c = b[lo];
        b[lo] = b[hi];
        HELD_STEP();
        b[hi] = c;
        HELD_STEP();
        lo++;
    }
}

// Steps past every record the reader has taken whole, and moves the open entries down to the
// start once no committed record is left in front of them.
static void held_settle(ConsoleTxRing& r)
{
    while (r.held_tail != r.held_open)
    {
        if (not held_record_fits(r, r.held_tail, r.held_open))
        {
            r.held_tail = r.held_open;
            r.held_taken = 0;
            HELD_STEP();
            break;
        }
        uint32_t const len = held_len(r, r.held_tail);
        if (r.held_taken != len)
        {
            break;
        }
        r.held_tail = r.held_tail + HELD_HEADER + len;
        r.held_taken = 0;
        HELD_STEP();
    }
    if (r.held_tail == r.held_open and r.held_tail != 0)
    {
        held_move_down(r.buf, 0, r.held_open, r.held_head);
        r.held_head = r.held_head - r.held_open;
        HELD_STEP();
        r.held_open = 0;
        HELD_STEP();
        r.held_tail = 0;
        HELD_STEP();
    }
}

static void held_drop(ConsoleTxRing& r, uint8_t id)
{
    uint32_t dst = r.held_open;
    uint32_t at = r.held_open;
    for (uint32_t size = held_fit(r, at, r.held_head); size != 0;
         size = held_fit(r, at, r.held_head))
    {
        if (held_id(r, at) != id)
        {
            held_move_down(r.buf, dst, at, at + size);
            dst = dst + size;
        }
        at = at + size;
    }
    r.held_head = dst;
    HELD_STEP();
}

// [from, from + n) to the device, the lock held.
static void held_write(ConsoleTxRing const& r, uint32_t from, uint32_t n)
{
    if (n != 0)
    {
        console_write_line_sync(r.buf + from, n);
    }
}

int console_held_append(uint32_t key, char const* buf, uint32_t n, uint32_t max)
{
    ConsoleTxRing& r = tx();
    if (r.armed or r.buf == nullptr)
    {
        return 0;
    }
    uint32_t const mark = held_find(r, key);
    if (mark == r.held_head)
    {
        uint8_t id = 0;
        while (id != HELD_ID_COUNT and held_id_used(r, id))
        {
            id++;
        }
        if (id == HELD_ID_COUNT or r.size - r.held_head < HELD_MARK)
        {
            return 0;
        }
        r.buf[mark] = static_cast<char>(id);
        r.buf[mark + 1u] = 0;
        for (uint32_t i = 0; i < 4u; i++)
        {
            r.buf[mark + 2u + i] = static_cast<char>((key >> (8u * i)) & 0xFFu);
        }
        r.held_head = r.held_head + HELD_MARK;
    }
    if ((static_cast<uint8_t>(r.buf[mark]) & HELD_FULL) != 0 or n == 0)
    {
        return 1;
    }
    uint8_t const id = held_id(r, mark);
    if (n > 0xFFu or held_record_bytes(r, id) + n > max or r.size - r.held_head < HELD_LINE + n)
    {
        r.buf[mark] = static_cast<char>(static_cast<uint8_t>(r.buf[mark]) | HELD_FULL);
        return 1;
    }
    r.buf[r.held_head] = static_cast<char>(id);
    r.buf[r.held_head + 1u] = static_cast<char>(n);
    for (uint32_t i = 0; i < n; i++)
    {
        r.buf[r.held_head + HELD_LINE + i] = buf[i];
    }
    r.held_head = r.held_head + HELD_LINE + n;
    return 1;
}

int console_held_commit(uint32_t key)
{
    ConsoleTxRing& r = tx();
    uint32_t const mark = held_find(r, key);
    if (mark == r.held_head)
    {
        return 0;
    }
    held_change(r, true);
    uint8_t const id = held_id(r, mark);
    // The record's entries gathered at the front of the open area, in their order, by rotating
    // each one down over the entries of other records it passes.
    uint32_t gathered = r.held_open;
    uint32_t at = r.held_open;
    for (uint32_t size = held_fit(r, at, r.held_head); size != 0;
         size = held_fit(r, at, r.held_head))
    {
        uint32_t const end = at + size;
        if (held_id(r, at) == id)
        {
            held_reverse(r.buf, gathered, at);
            held_reverse(r.buf, at, end);
            held_reverse(r.buf, gathered, end);
            gathered = gathered + size;
        }
        at = end;
    }
    // Then their headers dropped behind one length. The marker comes first and is wider than
    // that length, so every copy lands below the entry it reads.
    uint32_t dst = r.held_open + HELD_HEADER;
    at = r.held_open;
    for (uint32_t size = held_fit(r, at, gathered); size != 0; size = held_fit(r, at, gathered))
    {
        if (not held_is_mark(r, at))
        {
            held_move_down(r.buf, dst, at + HELD_LINE, at + size);
            dst = dst + size - HELD_LINE;
        }
        at = at + size;
    }
    uint32_t const len = dst - r.held_open - HELD_HEADER;
    if (len == 0)
    {
        dst = r.held_open;
    }
    held_move_down(r.buf, dst, gathered, r.held_head);
    r.held_head = r.held_head - (gathered - dst);
    HELD_STEP();
    if (len != 0)
    {
        r.buf[r.held_open] = static_cast<char>(len & 0xFFu);
        r.buf[r.held_open + 1u] = static_cast<char>(len >> 8);
        HELD_STEP();
        r.held_open = dst;
        HELD_STEP();
    }
    held_settle(r);
    held_change(r, false);
    return 1;
}

void console_held_abandon(uint32_t key)
{
    ConsoleTxRing& r = tx();
    uint32_t const mark = held_find(r, key);
    if (mark == r.held_head)
    {
        return;
    }
    held_change(r, true);
    held_drop(r, held_id(r, mark));
    held_settle(r);
    held_change(r, false);
}

void console_held_write_sync(void)
{
    ConsoleTxRing const& r = tx();
    if (r.buf == nullptr or r.held_changing)
    {
        return;
    }
    uint32_t const tail = r.held_tail;
    uint32_t const open = r.held_open;
    uint32_t const head = r.held_head;
    if (head > r.size or open > head or tail > open)
    {
        return;
    }
    // Committed records, the first from the line after the one its reader was cut in.
    uint32_t at = tail;
    while (held_record_fits(r, at, open))
    {
        uint32_t const len = held_len(r, at);
        uint32_t const end = at + HELD_HEADER + len;
        uint32_t from = at + HELD_HEADER;
        if (at == tail and r.held_taken != 0)
        {
            if (r.held_taken > len)
            {
                return;
            }
            from = from + r.held_taken;
            if (r.buf[from - 1u] != '\n')
            {
                while (from != end and r.buf[from] != '\n')
                {
                    from++;
                }
                if (from != end)
                {
                    from++;
                }
            }
        }
        held_write(r, from, end - from);
        at = end;
    }
    // Each open record's lines in their order, records in the order they opened.
    uint32_t mark = open;
    for (uint32_t size = held_fit(r, mark, head); size != 0; size = held_fit(r, mark, head))
    {
        if (held_is_mark(r, mark))
        {
            uint8_t const id = held_id(r, mark);
            at = mark + size;
            for (uint32_t line = held_fit(r, at, head); line != 0; line = held_fit(r, at, head))
            {
                if (not held_is_mark(r, at) and held_id(r, at) == id)
                {
                    held_write(r, at + HELD_LINE, line - HELD_LINE);
                }
                at = at + line;
            }
        }
        mark = mark + size;
    }
}

void console_held_clear(void)
{
    ConsoleTxRing& r = tx();
    if (r.held_changing)
    {
        return;
    }
    r.held_head = 0;
    r.held_open = 0;
    r.held_tail = 0;
    r.held_taken = 0;
}

uint32_t console_held_ready(void)
{
    ConsoleTxRing const& r = tx();
    if (not held_record_fits(r, r.held_tail, r.held_open)
        or r.held_taken >= held_len(r, r.held_tail))
    {
        return 0;
    }
    return held_len(r, r.held_tail) - r.held_taken;
}

char const* console_held_data(void)
{
    ConsoleTxRing const& r = tx();
    return r.buf + r.held_tail + HELD_HEADER + r.held_taken;
}

void console_held_take(uint32_t n)
{
    ConsoleTxRing& r = tx();
    r.held_taken = r.held_taken + n;
    held_change(r, true);
    held_settle(r);
    held_change(r, false);
}

} // extern "C"
