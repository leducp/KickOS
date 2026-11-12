// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Minimal in-kernel debug console: write-only, routed to the arch console bottom edge
// (sim: host stdout). Reserved for panic, early boot and fault reporting.

#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/arch/arch.h>
#if KICKOS_KERNEL_CORES > 1
#include <kickos/arch/atomic_swap.h>
#endif
#include <kickos/cap.h>
#include <kickos/console_tx.h>
#include <kickos/grant.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kruntime.h>
#include <kickos/sync.h>
#include <kickos/task.h>
#include <kickos/sys/abi.h>
#include <kickos/sys/atomic.h>
#include <kickos/sys/errno.h>

#include <stdarg.h>

// Set by the build from KICKOS_CONSOLE. The chip-only default keeps a standalone
// compile printing.
#ifndef KICKOS_CONSOLE_CHIP
#define KICKOS_CONSOLE_CHIP 1
#endif
#ifndef KICKOS_CONSOLE_RTT
#define KICKOS_CONSOLE_RTT 0
#endif
// Lowers '\n' to CR+LF on the chip UART only. Off by default so a standalone/sim
// compile stays raw.
#ifndef KICKOS_CONSOLE_CRLF
#define KICKOS_CONSOLE_CRLF 0
#endif

#if KICKOS_CONSOLE_RTT
#include <kickos/rtt.h>
#endif

#include <kickos/arch/console_retry.h>

// Deliberately NO fallback #define: 0 means the reporter stays on the stack it was called
// from, which is right on ARCH_SIM and wrong everywhere else, so a build that lost the
// generated board config must fail rather than resolve the quiet answer.
#ifndef KICKOS_PANIC_STACK_SIZE
#error "KICKOS_PANIC_STACK_SIZE is missing; the generated board config carries it"
#endif

namespace
{
    using kickos::Atomic;
    using kickos::Order;

    // console_emit's answer in the dark window, kept inside kconsole_write_user, so it is outside
    // the -KOS_E* range.
    constexpr int CONSOLE_DARK = -1000;

    // Who owns the UART TX register: in USER_OWNED the kernel touches the device on no path.
    // No path restores KERNEL_OWNED, only a publish leaves RECLAIMED, and nothing leaves
    // PANICKED.
    enum class ConsoleState : uint8_t
    {
        KERNEL_OWNED, // boot default; buffered ring or polled
        HANDING_OFF,  // a publish is in progress: NEW kernel writers are refused, the UART is
                      // still the kernel's, and a writer already inside the bracket finishes
                      // on it
        USER_OWNED,   // a userspace driver owns the UART; kernel chip path DROPS
        RECLAIMED,    // the kernel took the UART back at a driver's death; polled-only
        PANICKED      // a panic took it back for good; polled-only, its writers uncounted
    };
    constinit Atomic<ConsoleState, Order::RELAXED> g_console_state = ConsoleState::KERNEL_OWNED;

    // A publish has begun: the device is the driver's, or will be once the drain ends.
    bool published(ConsoleState state)
    {
        return state == ConsoleState::USER_OWNED or state == ConsoleState::HANDING_OFF;
    }

    // The registers arch_console_reclaim writes, size 0 where a chip names none.
    struct ConsoleWindow
    {
        uintptr_t base = 0;
        size_t size = 0;
    };

    ConsoleWindow console_window(void)
    {
        ConsoleWindow w;
        arch_console_reclaim_window(&w.base, &w.size);
        return w;
    }

    // Set by the cap layer when the published console's task ends, and sticky until the reclaim
    // goes through: a refused one, and a hand-off still under way, both leave it for a later
    // step to act on. Only a new publish retires it (console_handover_begin).
    constinit Atomic<bool, Order::RELAXED> g_console_driver_died = false;

    // In-flight kernel chip writers. A console driver's create enters HANDING_OFF first and then
    // parks until this is zero, so a writer that raced past a stale device-owning read is off
    // the device before the userspace driver touches it. That argument holds ONLY because
    // nothing increments after that flip, which is what chip_writer_enter enforces.
    constinit Atomic<int, Order::RELAXED> g_chip_writers = 0;

    // The thread handing the console over, from its publish's first half to its last step.
    constinit kickos::Thread* g_publisher = nullptr;

    // An ISR's bracket closes before any switch; a thread's can be ended by a slay, whose exit
    // has to leave it (kconsole_slain). Caller holds IrqLock.
    void chip_writer_mark(bool inside)
    {
        kickos::Thread* const t = kickos::sched::current();
        if (t != nullptr and arch_in_isr() == 0)
        {
            t->console_writing = inside;
        }
    }

    // The printing thread's TCB address, truncated: TCBs sit in one pool far narrower than
    // 4 GiB, so two live threads never share a key.
    uint32_t held_key(void)
    {
        return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(kickos::sched::current()));
    }

    constexpr uint8_t WRITE_RECORD = 1u;  // a line of a fault record
    constexpr uint8_t WRITE_OFFERED = 2u; // offered again where the chip took part of it
    constexpr uint8_t WRITE_USER = 4u;    // the current thread's own write
    constexpr uint8_t WRITE_AGAIN = 8u;   // a record line offered again, RTT already carrying it

    // A record line's wait for ring room ends once this long passes with no byte leaving the
    // ring: the channel is wedged, and the dying thread drops the line rather than stay alive.
    constexpr uint32_t RECORD_STALL_US = 50000u;

    // How often a writer a polled line refused offers again: that writer wakes nobody.
    constexpr uint32_t LINE_RETRY_US = 1000u;

#if KICKOS_CONSOLE_CHIP
    // Who speaks on a console written outside the ring: the context inside a polled or claimed
    // write, and on an own-image AMP node the thread whose last write left its line open under a
    // live grant. A line is one context's until it ends, so no other context writes inside it,
    // a fault record excepted: from its first line to its end it goes ahead of every thread's
    // line, which resumes after it. Under IrqLock.
    struct LineState
    {
        void const* writer = nullptr;
        bool writer_wakes = false; // the writer's end wakes the writers it refused
#if KICKOS_AMP_CONSOLE_CLAIM
        bool writer_user = false; // a syscall's write, whose bytes give way to a record
        kickos::Thread const* open = nullptr;
#endif
        bool waiting = false;
        // The record whose line is on the wire, and the threads inside a record.
        kickos::Thread const* record = nullptr;
        uint16_t records = 0;
        // A byte of a writer that gives way to a record is on the wire.
        bool byte = false;
    };
    constinit kickos::InstanceLocal<LineState> g_line_all;

    LineState& line(void)
    {
        return g_line_all.get();
    }
    // The key of every interrupt context, which never holds a line past its own write.
    constinit char const g_line_isr = 0;

    // noinline: tests/static/trap_redzone_roots.txt cuts the panic class at this node.
    __attribute__((noinline)) void line_wake(kickos::Held held)
    {
        if (line().waiting)
        {
            line().waiting = false;
            kickos::console_dark_wake(held);
        }
    }

    // The key the calling context writes under.
    void const* line_key(void)
    {
        kickos::Thread const* const t = kickos::sched::current();
        if (t != nullptr and arch_in_isr() == 0)
        {
            return t;
        }
        return &g_line_isr;
    }

    // Whether `key` is a thread inside its own fault record.
    bool in_record(void const* key)
    {
        return key != &g_line_isr and static_cast<kickos::Thread const*>(key)->console_record != 0;
    }

    // Whether the line's writer is one whose bytes wait while a record speaks.
    bool writer_gives_way(void)
    {
#if KICKOS_AMP_CONSOLE_CLAIM
        return line().writer_user;
#else
        return line().writer != &g_line_isr;
#endif
    }

    // Whether a context other than `key` speaks now. An ISR is kept off by any write in
    // progress, its own interrupted one included. A record waits out an interrupt's line,
    // another record's and, on a claimed UART, a kernel line. Caller holds IrqLock.
    bool line_taken(void const* key, kickos::Held held)
    {
        if (in_record(key))
        {
            return (line().writer != nullptr and not writer_gives_way())
                   or (line().record != nullptr and line().record != key);
        }
        if (line().records != 0)
        {
            return true;
        }
        if (line().writer != nullptr and (key == &g_line_isr or line().writer != key))
        {
            return true;
        }
#if KICKOS_AMP_CONSOLE_CLAIM
        if (line().open == nullptr or line().open == key)
        {
            return false;
        }
        if (arch_console_claim_left() != 0)
        {
            return true;
        }
        line().open = nullptr;
        line_wake(held);
#else
        (void)held;
#endif
        return false;
    }

    // The thread whose line refused a record, or null where it is an interrupt context, which
    // another core may be running. Caller holds IrqLock.
    kickos::Thread const* line_speaker(void)
    {
        if (line().record != nullptr)
        {
            return line().record;
        }
        if (line().writer == &g_line_isr)
        {
            return nullptr;
        }
        return static_cast<kickos::Thread const*>(line().writer);
    }

    // One byte of a writer that gives way to a record: waits, interrupts open, while a record
    // speaks, then runs `put` masked and answers it.
    template <class Put>
    auto give_way(Put put) -> decltype(put())
    {
        while (true)
        {
            arch_irq_state_t const irq = arch_irq_save();
            bool go = false;
            {
                kickos::IrqLock lock;
                go = line().records == 0;
                line().byte = go;
            }
            if (go)
            {
                auto const stored = put();
                {
                    kickos::IrqLock lock;
                    line().byte = false;
                }
                arch_irq_restore(irq);
                return stored;
            }
            arch_irq_restore(irq);
        }
    }

    // A record's line starts once no byte of a writer giving way to it is on the wire.
    void record_line_open(void)
    {
        while (true)
        {
            kickos::IrqLock lock;
            if (not line().byte)
            {
                return;
            }
        }
    }

#if KICKOS_AMP_CONSOLE_CLAIM
    // A refused write answers 0: a kernel line is lost to it, and a user or record writer
    // waits (console_claim_wait). Each wire byte is its own masked span. noinline: inlined, its
    // frame joins console_emit's under the exit red zone check_trap_redzone.sh measures.
    __attribute__((noinline)) int line_write(char const* buf, size_t n, bool* cr_pending)
    {
        void const* const key = line_key();
        bool const record = in_record(key);
        bool const user = not record and key != &g_line_isr and cr_pending != nullptr;
        {
            kickos::IrqLock lock;
            if (line_taken(key, lock))
            {
                return 0;
            }
            if (record)
            {
                line().record = static_cast<kickos::Thread const*>(key);
            }
            else
            {
                line().writer = key;
                line().writer_wakes = true;
                line().writer_user = user;
            }
        }
        int took = 0;
        if (user)
        {
            while (static_cast<size_t>(took) < n)
            {
                int const one = give_way(
                    [&]() { return arch_console_write_retry(buf + took, 1, cr_pending); });
                if (one <= 0)
                {
                    break;
                }
                took = took + one;
            }
        }
        else
        {
            if (record)
            {
                record_line_open();
            }
            took = arch_console_write_retry(buf, n, cr_pending);
        }
        kickos::IrqLock lock;
        if (record)
        {
            line().record = nullptr;
            line_wake(lock);
            return took;
        }
        line().writer = nullptr;
        if (key != &g_line_isr and arch_console_claim_left() != 0)
        {
            line().open = static_cast<kickos::Thread const*>(key);
        }
        else if (line().open == key)
        {
            line().open = nullptr;
        }
        // A writer parked on this write offers again: on a line still open it parks again for
        // that line's grant.
        line_wake(lock);
        return took;
    }
#endif
#endif

#if KICKOS_CONSOLE_CHIP
    enum class ChipEntry : uint8_t
    {
        KERNEL_OWNED,
        RECLAIMED,
        PANICKED, // no publish can follow, so the writer is not counted
        SERVED,   // the writer's own stdout send would be taken now
        OWED,     // the console is dark: the writer waits it out
        DROPPED
    };

    // The state read and the increment are ONE masked operation, or publish's drain is
    // blind to a writer that read a device-owning state just before the flip: the drain
    // sees 0, the driver starts, and the woken writer then writes a UART it no longer
    // owns. Refuses HANDING_OFF as well as USER_OWNED: a publish that admitted new writers
    // could drain forever. The answer IS the decisive read: a caller must not re-read the
    // state. A record line is held under the same read, so the reclaim, which flips the state
    // and writes the open records under this lock, sends each line to exactly one of the
    // device and the driver.
    ChipEntry chip_writer_enter(char const* buf, size_t n, uint8_t flags)
    {
        kickos::IrqLock lock;
        ConsoleState const state = g_console_state;
        if (state == ConsoleState::PANICKED)
        {
            return ChipEntry::PANICKED;
        }
        if (published(state))
        {
            if ((flags & WRITE_RECORD) != 0)
            {
                (void)console_held_append(held_key(), buf, static_cast<uint32_t>(n),
                                          KDIAG_FAULT_RECORD_MAX);
            }
            if ((flags & WRITE_USER) != 0)
            {
                if (kickos::cap_console_serves(kickos::sched::current()))
                {
                    return ChipEntry::SERVED;
                }
                if (g_console_driver_died)
                {
                    return ChipEntry::OWED;
                }
            }
            return ChipEntry::DROPPED;
        }
        g_chip_writers = g_chip_writers + 1;
        chip_writer_mark(true);
        if (state == ConsoleState::RECLAIMED)
        {
            return ChipEntry::RECLAIMED;
        }
        return ChipEntry::KERNEL_OWNED;
    }
#endif
}

extern "C" int console_owner_is_kernel(void)
{
    return static_cast<int>(g_console_state == ConsoleState::KERNEL_OWNED);
}

// A publish already handing the console over, or a panic, refuses a new one. Caller holds
// IrqLock across this and console_handover_begin.
extern "C" int console_handover_refused(void)
{
    ConsoleState const state = g_console_state;
    return static_cast<int>(state == ConsoleState::HANDING_OFF or state == ConsoleState::PANICKED);
}

// Publish's first half. Leaves the UART kernel-owned so a writer already inside the bracket
// can finish on it. The caller MUST drain console_chip_writers to zero before
// console_owner_set_user (console_publish_drain), else that writer lands on the driver's UART,
// and the drain ends only because of the refusal installed here. A note standing here names
// the console this publish replaces; one taken from here on names the task this publish serves.
extern "C" void console_handover_begin(void)
{
    kickos::IrqLock lock;
    KICKOS_ASSERT(console_handover_refused() == 0);
    g_console_driver_died = false;
    g_console_state = ConsoleState::HANDING_OFF;
    g_publisher = kickos::sched::current();
    console_tx_deinit();
}

// Publish's LAST step. A writer still counted here is one the drain was meant to wait for,
// and it would finish its message on the driver's UART. A panic during the drain keeps the
// device. A death noted during the hand-off is acted on here, as console_on_driver_death would
// have under USER_OWNED.
extern "C" void console_owner_set_user(void)
{
    KICKOS_ASSERT(console_chip_writers() == 0);
    kickos::IrqLock lock;
    g_publisher = nullptr;
    if (g_console_state != ConsoleState::HANDING_OFF)
    {
        return;
    }
    g_console_state = ConsoleState::USER_OWNED;
    if (g_console_driver_died)
    {
        console_on_driver_death(lock);
        return;
    }
    kickos::console_dark_wake(lock);
}

// Every access to the chip-writer count, mutators and reader alike, MUST run under IrqLock:
// console_emit can run in ISR/fault context, so an unlocked read-modify-write tears against
// a thread producer's and an unlocked reader can observe the intermediate. The last writer wakes
// a publisher draining, whatever a panic made of the state meanwhile.
extern "C" void console_chip_writer_leave(void)
{
    kickos::IrqLock lock;
    chip_writer_mark(false);
    g_chip_writers = g_chip_writers - 1;
    if (g_chip_writers == 0 and g_publisher != nullptr)
    {
        kickos::console_dark_wake(lock);
    }
}

extern "C" void console_note_driver_death(void)
{
    g_console_driver_died = true;
}

// The kernel has the device back: what no driver took goes out polled, the open records'
// lines so far included, and those records' later lines then reach the device directly. The
// lock keeps another core's change of the store out. always_inline: a frame of its own deepens
// the exit chains the trap red-zone gate measures.
__attribute__((always_inline)) static inline void console_held_flush_sync(void)
{
    kickos::IrqLock lock;
    console_held_write_sync();
    console_held_clear();
}

// A chip's reclaim reprograms the device and destroys in-flight TX, and arch_console_write_sync
// hands bytes to a FIFO rather than the wire, so the flush comes first. It stays out of the chip
// body: arch.h scopes arch_console_reclaim to straight-line absolute stores, since it may run in
// a partial nested-fault state. The held records go out after, on the device the reclaim reset.
// Once per reclaim: a second run could truncate the byte in the shift register.
static void console_flush_then_reclaim(void)
{
    arch_console_flush_sync();
    arch_console_reclaim();
    console_held_flush_sync();
}

extern "C" int console_dark(void)
{
    return static_cast<int>(g_console_driver_died and published(g_console_state));
}

void console_on_driver_death(kickos::Held held)
{
    if (not g_console_driver_died or g_console_state == ConsoleState::HANDING_OFF)
    {
        return;
    }
    // The note fires when the task ENDS, and its members are only slain then: the thread
    // holding the registers may still be running. Reclaiming on the note alone would
    // reprogram the UART under it. So the precondition is asked of the DEVICE: nobody may
    // still hold the window arch_console_reclaim is about to write. A slain member is still a
    // holder until its own exit releases its windows, so the note stays set across the
    // refusal and the LAST holder's exit_current reclaims.
    ConsoleWindow const w = console_window();
    if (w.size != 0 and not kickos::dev_window_free(w.base, w.size))
    {
        return;
    }
    g_console_driver_died = false;
    // Any other state means a panic reclaimed first. RECLAIMED is stored before the body so a
    // fault inside that body cannot recurse.
    if (g_console_state != ConsoleState::USER_OWNED)
    {
        return;
    }
    g_console_state = ConsoleState::RECLAIMED;
    console_flush_then_reclaim();
    kickos::console_dark_wake(held);
}

extern "C" void console_tx_room_freed(void)
{
    kickos::IrqLock lock;
    kickos::console_dark_wake(lock);
}

// Whether a thread of `t`'s own task holds the console's registers. A window has one holder.
static bool console_window_own(kickos::Thread const* t)
{
    ConsoleWindow const w = console_window();
    kickos::IrqLock lock;
    return w.size != 0 and not kickos::dev_window_free(w.base, w.size)
           and not kickos::dev_window_held_outside(w.base, w.size, t->task);
}

// A stale zero here makes the handover's drain give the UART to a userspace driver
// while a kernel writer is still on the device, hence the lock.
extern "C" int console_chip_writers(void)
{
    kickos::IrqLock lock;
    return g_chip_writers;
}

// CR+LF goes out as its own segment: a cooked buffer here would stand on a descent the trap
// red-zone gate measures (kickos/diag.h). The first stalled segment ends the line, so a wedged
// channel costs one stall per line.
__attribute__((always_inline)) static inline void write_line_polled(char const* buf, size_t n)
{
#if KICKOS_CONSOLE_CRLF
    static char const CRLF[2] = { '\r', '\n' };
    size_t start = 0;
    for (size_t i = 0; i < n; i++)
    {
        if (buf[i] != '\n')
        {
            continue;
        }
        if (i > start and not arch_console_write_sync(buf + start, i - start))
        {
            return;
        }
        if (not arch_console_write_sync(CRLF, sizeof(CRLF)))
        {
            return;
        }
        start = i + 1;
    }
    if (n > start)
    {
        (void)arch_console_write_sync(buf + start, n - start);
    }
#else
    (void)arch_console_write_sync(buf, n);
#endif
}

// The line writer of last resort. IrqLock spans the whole transmission, which is the masked
// window the ring exists to avoid, and is what keeps the line atomic without one.
extern "C" void console_write_line_sync(char const* buf, size_t n)
{
    kickos::IrqLock lock;
    write_line_polled(buf, n);
}

#if KICKOS_CONSOLE_CHIP
namespace
{
    // One wire byte of a line console_write_line_owned took. noinline: inlined, the byte's slot
    // joins the take's frame under the exit red zone check_trap_redzone.sh measures.
    __attribute__((noinline)) bool line_owned_put(char c)
    {
        return arch_console_write_sync(&c, 1);
    }
}

// Each wire byte its own masked span, the line this context's until it is written, except that
// a fault record goes ahead of a thread's line. A stalled channel drops the rest. It wakes
// nobody, or the scheduler would sit under every line this path writes: a writer it refuses
// retries (console_line_wait).
extern "C" int console_write_line_owned(char const* buf, size_t n)
{
    void const* const key = line_key();
    bool const record = in_record(key);
    {
        kickos::IrqLock lock;
        if (line_taken(key, lock))
        {
            return 0;
        }
        if (record)
        {
            line().record = static_cast<kickos::Thread const*>(key);
        }
        else
        {
            line().writer = key;
            line().writer_wakes = false;
        }
    }
    if (record or key == &g_line_isr)
    {
        if (record)
        {
            record_line_open();
        }
        (void)kickos::console_retry::write_masked(
            buf, n, nullptr, [](char c) { return line_owned_put(c); });
    }
    else
    {
        (void)kickos::console_retry::write(buf, n, nullptr, [](char c) {
            return give_way([c]() { return arch_console_write_sync(&c, 1); });
        });
    }
    kickos::IrqLock lock;
    if (record)
    {
        line().record = nullptr;
    }
    else
    {
        line().writer = nullptr;
    }
    return static_cast<int>(n);
}
#else
extern "C" int console_write_line_owned(char const*, size_t n)
{
    return static_cast<int>(n);
}
#endif

namespace kickos
{
#if KICKOS_CONSOLE_CHIP
    // The count is taken under the same masked read that selects the transport, so publish
    // either drains this writer or the writer never reaches the device. RECLAIMED needs the
    // bracket as much as KERNEL_OWNED does: a console published again after a reclaim flips
    // straight out of it.
    static int console_emit(char const* buf, size_t n, bool* cr_pending, uint8_t flags)
    {
        // A panic's lines take no lock: a core may hold it bare and never release it. Lines of
        // two failing cores can then interleave byte by byte.
        if (g_console_state == ConsoleState::PANICKED)
        {
            if (cr_pending != nullptr)
            {
                *cr_pending = false;
            }
            write_line_polled(buf, n);
            return static_cast<int>(n);
        }
        ChipEntry const entry = chip_writer_enter(buf, n, flags);
        // The dark window: the writer waits it out and offers the same bytes here again, so a
        // CR already on the wire stays owed.
        if (entry == ChipEntry::OWED)
        {
            return CONSOLE_DARK;
        }
        if (cr_pending != nullptr and entry != ChipEntry::KERNEL_OWNED)
        {
            *cr_pending = false;
        }
        if (entry == ChipEntry::KERNEL_OWNED or entry == ChipEntry::RECLAIMED)
        {
            // A line the ring refuses does not go out. A reclaimed console's ring is disarmed, so
            // its line is written polled (console_write_line_owned).
#if KICKOS_AMP_CONSOLE_CLAIM
            int took = 0;
            if (entry == ChipEntry::KERNEL_OWNED)
            {
                took = line_write(buf, n, cr_pending);
            }
            else
            {
                took = console_tx_insert_line(buf, n, KICKOS_CONSOLE_CRLF);
            }
#else
            int const took = console_tx_insert_line(buf, n, KICKOS_CONSOLE_CRLF);
#endif
            console_chip_writer_leave();
            return took;
        }
        if (entry == ChipEntry::PANICKED)
        {
            write_line_polled(buf, n);
            return static_cast<int>(n);
        }
        // A thread whose send failed before a publish lands here after it: its bytes now
        // belong on the endpoint, which takes them.
        if (entry == ChipEntry::SERVED)
        {
            return -KOS_EBUSY;
        }
        // The driver owns the UART: reported as taken, since no retry of this route can win. A
        // fault record's line was held for the driver instead.
        return static_cast<int>(n);
    }
#endif

#if KICKOS_CONSOLE_RTT
    // A panic's lines take no lock, as on the chip: a core may hold it bare and never release it.
    static void rtt_write_line(char const* buf, size_t n)
    {
        if (g_console_state == ConsoleState::PANICKED)
        {
            kickos_rtt_write(buf, n);
            return;
        }
        IrqLock lock;
        kickos_rtt_write(buf, n);
    }
#endif

    // Locking is PER BACKEND. RTT's WrOff RMW is written from thread, ISR and fault
    // context, so it takes the crit section for the few microseconds it needs. The chip
    // transport masks the ring COPY.
    //
    // An offered line goes to RTT only as far as the chip took it: offers repeat. A record line
    // offered again went to RTT on its first offer.
    static int kconsole_write_impl(char const* buf, size_t n, bool* cr_pending = nullptr,
                                   uint8_t flags = 0)
    {
        (void)cr_pending;
        (void)flags;
#if !KICKOS_CONSOLE_CHIP && !KICKOS_CONSOLE_RTT
        // KICKOS_CONSOLE=none: the writer is a sink. Panic, fault and boot still run their
        // full paths and terminate the same way.
        (void)buf;
        (void)n;
#endif
#if KICKOS_CONSOLE_RTT
        if ((flags & (WRITE_OFFERED | WRITE_AGAIN)) == 0)
        {
            rtt_write_line(buf, n);
        }
#endif
#if KICKOS_CONSOLE_CHIP
        // RAW: the '\n' lowering happens at the device end of the path, where one line stays
        // one emit. RTT stays raw either way, its viewer cooking.
        int const took = console_emit(buf, n, cr_pending, flags);
#else
        int const took = static_cast<int>(n);
#endif
#if KICKOS_CONSOLE_RTT
        if ((flags & WRITE_OFFERED) != 0 and took > 0)
        {
            rtt_write_line(buf, static_cast<size_t>(took));
        }
#endif
        return took;
    }

    // The bytes of buf that went out or were queued. A ring takes a line whole or refuses it,
    // and a refused line does not go out at all; an AMP chip sharing its UART stops at the
    // first byte its claim no longer covers. Do not grow a retry here: a caller that can afford
    // to wait offers the line again itself (kprintf_record).
    int kconsole_write(char const* buf, size_t n)
    {
        return kconsole_write_impl(buf, n);
    }

    int kconsole_write_user(char const* buf, size_t n, bool wait)
    {
        Thread* const t = sched::current();
        while (true)
        {
#if KICKOS_AMP_CONSOLE_CLAIM && KICKOS_CONSOLE_CHIP
            bool pending = t->console_cr_pending != 0;
            int const took = kconsole_write_impl(buf, n, &pending, WRITE_USER | WRITE_OFFERED);
            t->console_cr_pending = pending;
#else
            int const took = kconsole_write_impl(buf, n, nullptr, WRITE_USER | WRITE_OFFERED);
#endif
            if (took != CONSOLE_DARK and (took != 0 or n == 0))
            {
                return took;
            }
            if (not wait)
            {
                return -KOS_ETIMEDOUT;
            }
            int woke = 0;
            if (took == CONSOLE_DARK)
            {
                woke = console_dark_wait();
            }
            else
            {
                // A drain this writer's own task can stop never comes.
                if (console_window_own(t))
                {
                    return static_cast<int>(n);
                }
#if KICKOS_AMP_CONSOLE_CLAIM
                woke = console_claim_wait();
#else
                woke = console_room_wait(console_tx_line_needed(buf, n, KICKOS_CONSOLE_CRLF),
                                         KOS_TIMEOUT_NONE);
#endif
            }
            if (woke < 0)
            {
                return woke;
            }
        }
    }

    void kputs(char const* s)
    {
        kconsole_write(s, kstrlen(s));
    }

    namespace
    {
        // The buffer belongs to the CALLER, and that is the whole reason this is not one
        // function with one array: kvprintf_fault descends under a trap red zone measured by
        // tests/static/check_trap_redzone.sh and cannot spend what kprintf spends.
        void kvprintf_route(char* buf, size_t cap, char const* fmt, va_list ap, uint8_t flags)
        {
            kfmt_vsnprintf(buf, cap, fmt, ap);
            (void)kconsole_write_impl(buf, kstrlen(buf), nullptr, flags);
        }

        // Opens the current thread's fault record at its first line. noinline: inlined, its
        // frame joins kvprintf_fault's under the exit red zone check_trap_redzone.sh measures.
        __attribute__((noinline)) void record_begin(void)
        {
            Thread* const t = sched::current();
            if (t == nullptr or arch_in_isr() != 0)
            {
                return;
            }
            IrqLock lock;
            if (t->console_record != 0)
            {
                return;
            }
            t->console_record = 1;
#if KICKOS_CONSOLE_CHIP
            line().records = static_cast<uint16_t>(line().records + 1u);
#endif
        }

        // KDIAG_FAULT_LINE_MAX and not 256: this runs on the dying thread's block below the
        // exit red zone, where the array is the largest term of the whole descent. A line past
        // the bound is truncated, and tests/unit/faultline proves every record of the catalogue
        // fits at its worst case. 0 once the console took the line, else the room its refusal
        // waits for. noinline: tests/static/console_reach_roots.txt names this node.
        __attribute__((noinline)) uint32_t kvprintf_fault(bool again, char const* fmt,
                                                           va_list ap)
        {
            char buf[KDIAG_FAULT_LINE_MAX];
            kfmt_vsnprintf(buf, sizeof(buf), fmt, ap);
            // After the format: ahead of it, fmt and ap stay live across the call and the
            // frame grows.
            record_begin();
            size_t const n = kstrlen(buf);
            uint8_t flags = WRITE_RECORD;
            if (again)
            {
                flags = WRITE_RECORD | WRITE_AGAIN;
            }
            if (n == 0 or kconsole_write_impl(buf, n, nullptr, flags) != 0)
            {
                return 0;
            }
#if KICKOS_AMP_CONSOLE_CLAIM
            return static_cast<uint32_t>(n);
#else
            return console_tx_line_needed(buf, n, KICKOS_CONSOLE_CRLF);
#endif
        }

        // A record goes ahead of another thread's line wherever that line stands.
        static_assert(KDIAG_F_THREAD_FAULT[0] == '\n' and KDIAG_F_TASK_PANIC[0] == '\n',
                      "a fault record starts with a line ending");

        void record_end(Thread* t, Held held)
        {
            if (t == nullptr or t->console_record == 0)
            {
                return;
            }
            t->console_record = 0;
#if KICKOS_CONSOLE_CHIP
            line().records = static_cast<uint16_t>(line().records - 1u);
            line_wake(held);
#else
            (void)held;
#endif
        }
    }

    void kprintf(char const* fmt, ...)
    {
        char buf[KICKOS_DIAG_LINE_MAX];
        va_list ap;
        va_start(ap, fmt);
        kvprintf_route(buf, sizeof(buf), fmt, ap, 0);
        va_end(ap);
    }

    uint32_t kprintf_fault(char const* fmt, ...)
    {
        va_list ap;
        va_start(ap, fmt);
        uint32_t const need = kvprintf_fault(false, fmt, ap);
        va_end(ap);
        return need;
    }

    // The wait stays out of the console route: a park reaches the panic terminal that
    // tests/static/console_reach_roots.txt forbids there.
    void kprintf_record(char const* fmt, ...)
    {
        va_list ap;
        va_start(ap, fmt);
        va_list line;
        va_copy(line, ap);
        uint32_t need = kvprintf_fault(false, fmt, line);
        va_end(line);
        while (need != 0 and kconsole_record_wait(need))
        {
            va_copy(line, ap);
            need = kvprintf_fault(true, fmt, line);
            va_end(line);
        }
        va_end(ap);
    }

    bool kconsole_record_wait(uint32_t need)
    {
        Thread* const t = sched::current();
        // A drain or a line the faulting thread's own task can hold off would wait on itself.
        if (t == nullptr or console_window_own(t))
        {
            return false;
        }
#if KICKOS_CONSOLE_CHIP
        {
            IrqLock lock;
            if (line_taken(t, lock) and t->task != nullptr)
            {
                Thread const* const speaker = line_speaker();
                if (speaker != nullptr and speaker->task == t->task)
                {
                    return false;
                }
            }
        }
#endif
#if KICKOS_AMP_CONSOLE_CLAIM
        (void)need;
        return console_claim_wait() == 0;
#else
        return console_room_wait(need, RECORD_STALL_US) == 0;
#endif
    }

#if KICKOS_CONSOLE_CHIP
    uint32_t console_line_wait(Thread const* t, Held held)
    {
        if (not line_taken(t, held))
        {
            return 0;
        }
        line().waiting = true;
        // A record's line ends waking nobody, and the record's end wakes every writer.
        if (line().record != nullptr)
        {
            return LINE_RETRY_US;
        }
        if (line().records != 0 and line().writer == nullptr)
        {
            return KOS_TIMEOUT_NONE;
        }
#if KICKOS_AMP_CONSOLE_CLAIM
        if (line().writer == nullptr)
        {
            return arch_console_claim_left();
        }
#endif
        if (not line().writer_wakes)
        {
            return LINE_RETRY_US;
        }
        return KOS_TIMEOUT_NONE;
    }
#else
    uint32_t console_line_wait(Thread const*, Held)
    {
        return 0;
    }
#endif

    void krecord_end(void)
    {
        IrqLock lock;
        record_end(sched::current(), lock);
        ConsoleState const state = g_console_state;
        if (console_held_commit(held_key()) != 0 and state != ConsoleState::RECLAIMED
            and state != ConsoleState::PANICKED)
        {
            cap_console_deliver(lock);
        }
    }

    void kconsole_slain(void)
    {
        IrqLock lock;
        Thread* const t = sched::current();
        record_end(t, lock);
        console_held_abandon(held_key());
        if (t->console_writing != 0)
        {
            console_chip_writer_leave();
        }
#if KICKOS_CONSOLE_CHIP
        if (line().writer == t)
        {
            line().writer = nullptr;
            line_wake(lock);
        }
#endif
#if KICKOS_AMP_CONSOLE_CLAIM && KICKOS_CONSOLE_CHIP
        if (line().open == t)
        {
            line().open = nullptr;
            line_wake(lock);
        }
#endif
        // A publisher slain in its drain never hands the console over: it stays the kernel's,
        // polled, until the next publish, and what the hand-off held goes out now.
        if (g_publisher == t and g_console_state == ConsoleState::HANDING_OFF)
        {
            g_publisher = nullptr;
            g_console_state = ConsoleState::RECLAIMED;
            console_held_flush_sync();
            console_dark_wake(lock);
        }
    }

    bool console_window_held_outside(Task const* t)
    {
        ConsoleWindow const w = console_window();
        return w.size != 0 and dev_window_held_outside(w.base, w.size, t);
    }

    bool console_window_withheld(uintptr_t base, size_t size, Task const* t)
    {
        if (not published(g_console_state) or task_serves_console(t))
        {
            return false;
        }
        ConsoleWindow const w = console_window();
        return w.size != 0
               and grant_ranges_overlap(base, base + size - 1u, w.base, w.base + w.size - 1u);
    }

#if KICKOS_KERNEL_STACKS && KICKOS_KSTACK_REPORT
    struct UsableText
    {
        char s[12];
    };

    constexpr UsableText usable_text()
    {
        UsableText t = {};
        size_t v = KICKOS_KERNEL_STACK_SIZE - sizeof(uint32_t);
        size_t n = sizeof(t.s) - 1;
        do
        {
            n = n - 1;
            t.s[n] = static_cast<char>('0' + (v % 10));
            v = v / 10;
        } while (v != 0 and n != 0);
        size_t j = 0;
        while (n < sizeof(t.s) - 1)
        {
            t.s[j] = t.s[n];
            j++;
            n++;
        }
        t.s[j] = '\0';
        return t;
    }

    constexpr UsableText kUsableText = usable_text();

    // Filled here and emitted by the panic tail. Nothing in this reporter may descend to the
    // console: a frame between kpanic and kputs is charged to the trap red zone.
    char g_kstack_report[96];

    size_t report_append(size_t j, char const* src)
    {
        while (*src != '\0' and j < sizeof(g_kstack_report) - 1)
        {
            g_kstack_report[j] = *src;
            j++;
            src++;
        }
        return j;
    }

    // Called after the banner, so the fill boundary already carries the console tail this
    // panic descended. Answers an empty string when no thread is current.
    char const* kstack_report_text()
    {
        Thread const* const c = kernel().current(kickos_kernel_core());
        if (c == nullptr)
        {
            return "";
        }
        size_t used = kstack_high_water(kernel().threads.index_of(c));
        char digits[12];
        size_t n = sizeof(digits) - 1;
        digits[n] = '\0';
        do
        {
            n = n - 1;
            digits[n] = static_cast<char>('0' + (used % 10));
            used = used / 10;
        } while (used != 0 and n != 0);
        size_t j = report_append(0, "KSTACK HIGH WATER: ");
        j = report_append(j, &digits[n]);
        j = report_append(j, " of ");
        j = report_append(j, kUsableText.s);
        j = report_append(j, " bytes reached on this slot\n");
        g_kstack_report[j] = '\0';
        return g_kstack_report;
    }
#endif

    namespace
    {
#if KICKOS_PANIC_STACK_SIZE > 0
        // ONE PER CORE ONE KERNEL SCHEDULES, and per core is not spare generosity: nothing
        // stops two cores of a shared kernel panicking together, and each has to descend the
        // console somewhere the other is not writing. It is not per thread SLOT, the reporter
        // being terminal, so a core runs it once.
        alignas(KICKOS_STACK_ALIGN) uint8_t g_panic_stack[KICKOS_KERNEL_CORES]
                                                         [KICKOS_PANIC_STACK_SIZE];
        static_assert(KICKOS_PANIC_STACK_SIZE % KICKOS_STACK_ALIGN == 0,
                      "the panic stack top must land on the alignment a call boundary needs");
#endif

        // always_inline: at -Os this would otherwise be a call, and its frame would stand on
        // the stack the switch below exists to leave.
        __attribute__((always_inline)) inline uintptr_t panic_stack_top(void)
        {
#if KICKOS_PANIC_STACK_SIZE > 0
            return reinterpret_cast<uintptr_t>(
                &g_panic_stack[kickos_kernel_core()][KICKOS_PANIC_STACK_SIZE]);
#else
            return 0;
#endif
        }
    }

    // The seat and the message go in REGISTERS, never in .bss: a shared cell for either would
    // let a second core panicking concurrently decide where this one lands.
    void kpanic(char const* msg)
    {
        kickos_panic_stack_enter(msg, nullptr, 0, panic_stack_top());
    }
}

// Entered by kickos_panic_stack_enter with the stack already moved, so it starts at the top of
// this core's slice whatever depth the assertion fired at. Its console descent is measured as
// the PANIC class of check_trap_redzone.sh and reaches no trap red zone; the FAULT reporter is
// a different chain and still ends several of them.
namespace
{
    // kickos_panic_report's `file` for a refusal under no lock: a line, or a head and a value.
    constinit char const g_refusal_line[1] = {};
    constinit char const g_refusal_hex[1] = {};

    // noinline: its frame stays off the panic report's own, which every panic descends from.
    [[noreturn]] __attribute__((noinline)) void refusal_report(char const* msg, bool with_value,
                                                               unsigned value)
    {
        kpanic_enter_unlocked();
        kpanic_write_unlocked(msg, kstrlen(msg));
        if (with_value)
        {
            char digits[9];
            size_t n = sizeof(digits) - 1;
            digits[n] = '\n';
            do
            {
                n = n - 1;
                digits[n] = "0123456789abcdef"[value & 0xFu];
                value = value >> 4;
            } while (value != 0);
            kpanic_write_unlocked(&digits[n], sizeof(digits) - n);
        }
        kfault_terminate();
    }
}

// See kernel.h. On the panic stack, so its descent is the PANIC class's and not the trap
// class's of the wait that refused.
extern "C" void kpanic_refuse_unlocked(char const* line)
{
    kickos_panic_stack_enter(line, g_refusal_line, 0, kickos::panic_stack_top());
}

extern "C" void kpanic_refuse_unlocked_hex(char const* head, unsigned value)
{
    kickos_panic_stack_enter(head, g_refusal_hex, value, kickos::panic_stack_top());
}

extern "C" void kickos_panic_report(char const* msg, char const* file, unsigned line)
{
    if (file == g_refusal_line or file == g_refusal_hex)
    {
        refusal_report(msg, file == g_refusal_hex, line);
    }
    kpanic_enter();
#if KICKOS_PANIC_STACK_SIZE > 0
    // tests/lib/panic.ere matches this line verbatim.
    uintptr_t const sp = reinterpret_cast<uintptr_t>(__builtin_stack_address());
    uintptr_t const top = kickos::panic_stack_top();
    if (sp < top - KICKOS_PANIC_STACK_SIZE or sp > top)
    {
        kickos::kputs("\nPANIC STACK NOT SEATED\n");
    }
#endif
    kickos::kputs("\nKERNEL PANIC: ");
    kickos::kputs(msg);
    kickos::kputs("\n");
#if KICKOS_KERNEL_STACKS && KICKOS_KSTACK_REPORT
    kickos::kputs(kickos::kstack_report_text());
#endif
    kfault_terminate();
}

namespace
{
    // Taken once, by whichever panic reclaims the device.
    constinit uint32_t g_panic_claim = 0u;

    // Masked caller. True for the first claimant only.
    bool panic_claim(void)
    {
#if KICKOS_KERNEL_CORES > 1
        return arch_swap_u32(&g_panic_claim, 1u) == 0u;
#else
        bool const first = g_panic_claim == 0u;
        g_panic_claim = 1u;
        return first;
#endif
    }
}

// See kernel.h. The order below is load-bearing: mask FIRST so no ISR can enqueue after, then
// reclaim, flush what is already queued, disarm the ring and store PANICKED last, under one lock:
// until that store another core's line waits on the lock instead of writing under this flush.
extern "C" void kpanic_enter(void)
{
    (void)arch_irq_save(); // never restored: the panic/fault path does not return
    // Both paths disarm before they store PANICKED, so a panic that reads it returns without the
    // lock another core may hold bare.
    ConsoleState const seen = g_console_state;
#if KICKOS_KERNEL_CORES > 1
    std::atomic_thread_fence(std::memory_order_acquire);
#endif
    if (seen == ConsoleState::PANICKED and console_tx_armed() == 0)
    {
        return;
    }
    kickos::IrqLock lock;
    // Reclaims from ANY prior state, not only USER_OWNED: the ownership state tracks a
    // PUBLISH, never whether the device is garbled, and a thread granted the console window can
    // wreck the channel with no publish at all; a driver death's reclaim already ran. The claim
    // comes BEFORE the call so a synchronous fault inside the reclaim body re-enters here and
    // stops instead of recursing, and so the body runs exactly once: a second run could
    // truncate the byte in the shift register. Every chip body is idempotent absolute stores
    // (arch.h), safe on a device no driver ever touched. Only a TERMINAL fault exit may
    // reclaim: a kill-and-resume path must gate this on "this fault terminates the system".
    if (panic_claim() and g_console_state != ConsoleState::RECLAIMED)
    {
        console_flush_then_reclaim();
    }
    console_tx_flush_sync();
    console_tx_disarm();
#if KICKOS_KERNEL_CORES > 1
    // A line written polled on reading PANICKED must follow this flush on the device.
    kickos::fence_release();
#endif
    g_console_state = ConsoleState::PANICKED;
}

extern "C" void kickos_fault_report_begin(void)
{
    // Never cleared: the report ends in a terminal outcome, which never returns. A third arrival
    // is a fault inside that outcome itself, which can only stop.
    static uint8_t arrivals[KICKOS_KERNEL_CORES];
    uint8_t& mine = arrivals[kickos_kernel_core()];
    if (mine >= 2u)
    {
        while (true)
        {
        }
    }
    mine = static_cast<uint8_t>(mine + 1u);
    if (mine == 2u)
    {
        kfault_terminate();
    }
}

// See kernel.h.
extern "C" void kpanic_enter_unlocked(void)
{
    (void)arch_irq_save();
    console_tx_disarm();
#if KICKOS_KERNEL_CORES > 1
    kickos::fence_release();
#endif
    ConsoleState const state = g_console_state;
    g_console_state = ConsoleState::PANICKED;
    if (panic_claim() and state != ConsoleState::RECLAIMED)
    {
        arch_console_flush_sync();
        arch_console_reclaim();
    }
}

extern "C" void kpanic_write_unlocked(char const* buf, size_t n)
{
    (void)buf;
    (void)n;
#if KICKOS_CONSOLE_RTT
    // kickos_rtt_write takes no lock of its own: unbracketed, it can tear another core's line.
    kickos_rtt_write(buf, n);
#endif
#if KICKOS_CONSOLE_CHIP
    write_line_polled(buf, n);
#endif
}

// See kernel.h.
extern "C" void kickos_terminate(int status)
{
    console_tx_flush_sync();
    // The RING being empty is not the DEVICE being idle: arch_shutdown can stop the core with
    // a byte still in the UART FIFO or shift register, truncating the last line.
    arch_console_flush_sync();
    arch_shutdown(status);
}

extern "C" void kickos_isr_fault(uintptr_t addr, int is_write)
{
    // Must run FIRST: a terminal fault in USER_OWNED has to reclaim the UART, the likeliest
    // post-handover faulter being the console driver itself, else the report prints to a
    // device the kernel no longer owns and the system halts silently. kpanic_enter is
    // idempotent and subsumes the flush.
    kpanic_enter();
    ::kickos::Thread* c = ::kickos::sched::current();
    char const* who = "?";
    if (c != nullptr)
    {
        who = c->name;
    }
    char const* dir = "read";
    if (is_write)
    {
        dir = "write";
    }
    ::kickos::kprintf(KDIAG_F_MPU_FAULT, who, dir, reinterpret_cast<void*>(addr));
    // The bounds are what separate a thread running off its own stack from a wild write,
    // which is the difference between a provisioning bug in the image and one thread
    // misbehaving.
    if (c != nullptr and c->stack_base != nullptr)
    {
        uintptr_t const lo = reinterpret_cast<uintptr_t>(c->stack_base);
        ::kickos::kprintf(KDIAG_F_MPU_FAULT_STACK, reinterpret_cast<void*>(lo),
                          reinterpret_cast<void*>(lo + c->stack_size));
    }
    console_tx_flush_sync();
    arch_console_flush_sync();
    kickos_terminal_outcome_apply();
    kickos_terminate(0);
}
