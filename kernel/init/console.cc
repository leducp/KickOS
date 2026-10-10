// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Minimal in-kernel debug console: write-only, routed to the arch console bottom edge
// (sim: host stdout). Reserved for panic, early boot and fault reporting.

#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/arch/arch.h>
#include <kickos/cap.h>
#include <kickos/console_tx.h>
#include <kickos/grant.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kruntime.h>
#include <kickos/sync.h>
#include <kickos/task.h>
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

    // console_emit's answer in the dark window. Never a -KOS_E*, and it never leaves
    // kconsole_write_user.
    constexpr int CONSOLE_DARK = -1000;

#if KICKOS_AMP_OWN_IMAGE
    // How long a user writer parks before it offers a line a peer node's claim refused again.
    constexpr uint32_t CONSOLE_CLAIM_POLL_US = 1000u;
#endif

    // Forces the polled path once a panic has started: the ring's drain ISR is masked
    // from that point on.
    constinit Atomic<bool, Order::RELAXED> g_console_panicking = false;

    // Who owns the UART TX register. Must be consulted BEFORE the buffered/sync
    // sub-decision: in USER_OWNED the kernel may touch the device on NO path at all.
    // No path restores KERNEL_OWNED, and only a publish leaves RECLAIMED.
    enum class ConsoleState : uint8_t
    {
        KERNEL_OWNED, // boot default; buffered ring or polled
        HANDING_OFF,  // a publish is in progress: NEW kernel writers are refused, the UART is
                      // still the kernel's, and a writer already inside the bracket finishes
                      // on it
        USER_OWNED,   // a userspace driver owns the UART; kernel chip path DROPS
        RECLAIMED     // the kernel forcibly took the UART back (panic, or driver death);
                      // polled-only
    };
    constinit Atomic<ConsoleState, Order::RELAXED> g_console_state = ConsoleState::KERNEL_OWNED;

    // Set by the cap layer when the published console's task ends, and sticky until the reclaim
    // goes through: a refused one, and a hand-off still under way, both leave it for a later
    // step to act on. Only a new publish retires it (console_handover_begin).
    constinit Atomic<bool, Order::RELAXED> g_console_driver_died = false;

    // In-flight kernel chip writers. kos_console_publish enters HANDING_OFF first and then
    // spins on this, so a writer that raced past a stale device-owning read is off the
    // device before the userspace driver touches it. That argument holds ONLY because
    // nothing increments after that flip, which is what chip_writer_enter enforces.
    constinit Atomic<int, Order::RELAXED> g_chip_writers = 0;

    // The printing thread's TCB address, truncated: TCBs sit in one pool far narrower than
    // 4 GiB, so two live threads never share a key.
    uint32_t held_key(void)
    {
        return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(kickos::sched::current()));
    }

    constexpr uint8_t WRITE_RECORD = 1u;  // a line of a fault record
    constexpr uint8_t WRITE_OFFERED = 2u; // offered again where the chip took part of it
    constexpr uint8_t WRITE_USER = 4u;    // the current thread's own write

#if KICKOS_CONSOLE_CHIP
    enum class ChipEntry : uint8_t
    {
        KERNEL_OWNED,
        RECLAIMED,
        SERVED, // the writer's own stdout send would be taken now
        OWED,   // the console is dark: the writer waits it out
        DROPPED
    };

    // The state read and the increment are ONE masked operation, or publish's drain is
    // blind to a writer that read a device-owning state just before the flip: the drain
    // sees 0, the driver starts, and the woken writer then bit-bangs a UART it no longer
    // owns. Refuses HANDING_OFF as well as USER_OWNED: a publish that admitted new writers
    // would have nothing left to make its drain converge. The answer IS the decisive read:
    // a caller must not re-read the state. A record line is held under the same read, so the
    // reclaim, which flips the state and writes the open records under this lock, sends each
    // line to exactly one of the device and the driver.
    ChipEntry chip_writer_enter(char const* buf, size_t n, uint8_t flags)
    {
        kickos::IrqLock lock;
        ConsoleState const state = g_console_state;
        if (state == ConsoleState::USER_OWNED or state == ConsoleState::HANDING_OFF)
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

// Publish's first half. Leaves the UART kernel-owned so a writer already inside the bracket
// can finish on it. The caller MUST drain console_chip_writers to zero before
// console_owner_set_user, else that writer lands on the driver's UART, and the drain
// converges only because of the refusal installed here. A note standing here names the
// console this publish replaces; one taken from here on names the task this publish serves.
extern "C" void console_handover_begin(void)
{
    kickos::IrqLock lock;
    g_console_driver_died = false;
    g_console_state = ConsoleState::HANDING_OFF;
    console_tx_deinit();
}

// Publish's LAST step. A writer still counted here is one the drain was meant to wait for,
// and it would finish its message on the driver's UART. A death noted during the hand-off is
// acted on here, as console_on_driver_death would have under USER_OWNED.
extern "C" void console_owner_set_user(void)
{
    KICKOS_ASSERT(console_chip_writers() == 0);
    kickos::IrqLock lock;
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
// a thread producer's and an unlocked reader can observe the intermediate.
extern "C" void console_chip_writer_leave(void)
{
    kickos::IrqLock lock;
    g_chip_writers = g_chip_writers - 1;
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

// A chip's reclaim silences and reprograms the device, destroying in-flight TX: a FIFO reset
// on esp32, a transmitter held in reset on esp32c6, UARTEN/UE/TE cleared on the PL011, USART,
// LPUART, SCI and USIC parts. arch_console_write_sync hands bytes to a FIFO and not to the
// wire, so the flush has to come first. It stays OUT of the chip body: arch.h scopes
// arch_console_reclaim to straight-line absolute stores, since it may run in a partial
// nested-fault state. The held records go out after, on the device the reclaim reset. It
// runs once per reclaim: a second run could truncate the byte in the shift register.
static void console_flush_then_reclaim(void)
{
    arch_console_flush_sync();
    arch_console_reclaim();
    console_held_flush_sync();
}

extern "C" int console_dark(void)
{
    ConsoleState const state = g_console_state;
    return static_cast<int>(g_console_driver_died
                            and (state == ConsoleState::USER_OWNED
                                 or state == ConsoleState::HANDING_OFF));
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
    uintptr_t win_base = 0;
    size_t win_size = 0;
    arch_console_reclaim_window(&win_base, &win_size);
    if (win_size != 0 and not kickos::dev_window_free(win_base, win_size))
    {
        return;
    }
    g_console_driver_died = false;
    // RECLAIMED here means a panic reclaimed first. RECLAIMED is stored before the body so a
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
    uintptr_t base = 0;
    size_t size = 0;
    arch_console_reclaim_window(&base, &size);
    kickos::IrqLock lock;
    return size != 0 and not kickos::dev_window_free(base, size)
           and not kickos::dev_window_held_outside(base, size, t->task);
}

// A stale zero here makes kos_console_publish's handover drain give the UART to a
// userspace driver while a kernel writer is still on the device, hence the lock.
extern "C" int console_chip_writers(void)
{
    kickos::IrqLock lock;
    return g_chip_writers;
}

// The line writer of last resort. IrqLock spans the whole transmission, which is the masked
// window the ring exists to avoid, and is what keeps the line atomic without one.
//
// CR+LF goes out as its own segment: a cooked buffer here would stand on a descent the trap
// red-zone gate measures (kickos/diag.h). The first stalled segment ends the line, so a wedged
// channel costs one stall per line.
extern "C" void console_write_line_sync(char const* buf, size_t n)
{
    kickos::IrqLock lock;
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

namespace kickos
{
#if KICKOS_CONSOLE_CHIP
    static int console_emit(char const* buf, size_t n, bool* cr_pending, uint8_t flags)
    {
        // The count is taken under the same masked read that selects the transport, so
        // publish either drains this writer or the writer never reaches the device. The
        // polled poke has no other serialisation, and RECLAIMED needs the bracket as much as
        // KERNEL_OWNED does: a console published again after a reclaim flips straight out
        // of it.
        ChipEntry const entry = chip_writer_enter(buf, n, flags);
        if (entry == ChipEntry::KERNEL_OWNED or entry == ChipEntry::RECLAIMED)
        {
            // PANIC KEEPS THE SYNCHRONOUS PATH. The system stops after a panic, so a line
            // left queued is a line nobody reads.
            //
            // The chip seam is the only door into the ring: every arch_console_write inserts
            // the line, and a line the ring refuses does not go out, unless it is a fault
            // record's, which makes its own room.
            int took = static_cast<int>(n);
            if (entry == ChipEntry::KERNEL_OWNED and not g_console_panicking)
            {
#if KICKOS_AMP_OWN_IMAGE
                if (cr_pending != nullptr)
                {
                    took = arch_console_write_retry(buf, n, cr_pending);
                }
                else
#endif
                took = arch_console_write(buf, n);
                if (took == 0 and (flags & WRITE_RECORD) != 0)
                {
                    took = console_tx_insert_record_line(buf, n, KICKOS_CONSOLE_CRLF);
                }
            }
            else
            {
                if (cr_pending != nullptr)
                {
                    *cr_pending = false;
                }
                console_write_line_sync(buf, n);
            }
            console_chip_writer_leave();
            return took;
        }
        // A thread whose send failed before a publish lands here after it: its bytes now
        // belong on the endpoint, which takes them.
        if (entry == ChipEntry::SERVED)
        {
            if (cr_pending != nullptr)
            {
                *cr_pending = false;
            }
            return -KOS_EBUSY;
        }
        // The dark window: the writer waits it out and offers the same bytes here again, so a
        // CR already on the wire stays owed.
        if (entry == ChipEntry::OWED)
        {
            return CONSOLE_DARK;
        }
        if (cr_pending != nullptr)
        {
            *cr_pending = false;
        }
        // USER_OWNED: the driver owns the UART and a kernel chip write is dropped BY DESIGN,
        // not by pressure. Reported as taken, because the distinction the caller acts on is
        // "the ring is full, try again" and no retry can win this one: the route is simply
        // not the chip any more. A fault record's line was held for the driver instead.
        return static_cast<int>(n);
    }
#endif

    // Locking is PER BACKEND. RTT's WrOff RMW is written from thread, ISR and fault
    // context, so it takes the crit section for the few microseconds it needs. The chip
    // transport masks the ring COPY, and a fault record's refused line also masks the polled
    // send of at most its own length of queued bytes.
    //
    // An offered line goes to RTT only as far as the chip took it: offers repeat.
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
        if ((flags & WRITE_OFFERED) == 0)
        {
            IrqLock lock;
            kickos_rtt_write(buf, n);
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
            IrqLock lock;
            kickos_rtt_write(buf, static_cast<size_t>(took));
        }
#endif
        return took;
    }

    // The bytes of buf that went out or were queued. A ring takes a line whole or refuses it,
    // and a refused line does not go out at all; an AMP chip sharing its UART stops at the
    // first byte its claim no longer covers. kputs and kvprintf_route below drop that answer
    // on purpose. Do not grow a retry here:
    // kprintf_paced is the one caller that can afford to wait, and it already does.
    int kconsole_write(char const* buf, size_t n)
    {
        return kconsole_write_impl(buf, n);
    }

    int kconsole_write_user(char const* buf, size_t n, bool wait)
    {
        Thread* const t = sched::current();
        while (true)
        {
#if KICKOS_AMP_OWN_IMAGE && KICKOS_CONSOLE_CHIP
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
                if (took == CONSOLE_DARK)
                {
                    return -KOS_EAGAIN;
                }
                return 0;
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
#if KICKOS_AMP_OWN_IMAGE
                // A sleep would end a cancelled writer here, past this frame's depth budget.
                woke = console_claim_wait(CONSOLE_CLAIM_POLL_US);
#else
                woke = console_room_wait(buf, n, KICKOS_CONSOLE_CRLF);
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
        // function with one array: kprintf_fault descends under a trap red zone measured by
        // tests/static/check_trap_redzone.sh and cannot spend what kprintf spends.
        void kvprintf_route(char* buf, size_t cap, char const* fmt, va_list ap, uint8_t flags)
        {
            kfmt_vsnprintf(buf, cap, fmt, ap);
            (void)kconsole_write_impl(buf, kstrlen(buf), nullptr, flags);
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

#if KICKOS_BENCH
    // It does NOT route to a published console: a report belongs on the wire the bench is
    // captured from.
    //
    // The progress test spans the WHOLE attempt, not console_tx_wait_progress alone: the offer
    // itself opens mask gaps, and on a backend whose drain lands in one of them that is where
    // the bytes leave. The attempt bound is the ring's own size because one attempt that makes
    // progress frees at least one byte and no line needs more than the ring to fit.
    void kprintf_paced(char const* fmt, ...)
    {
        char buf[KICKOS_DIAG_LINE_MAX];
        va_list ap;
        va_start(ap, fmt);
        kfmt_vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        size_t const n = kstrlen(buf);
        uint32_t queued = console_tx_used();
        for (uint32_t attempt = 0; attempt < KICKOS_CONSOLE_TX_SIZE; attempt++)
        {
            if (kconsole_write_impl(buf, n, nullptr, WRITE_OFFERED) != 0)
            {
                return;
            }
            console_tx_wait_progress();
            uint32_t const now = console_tx_used();
            if (now >= queued)
            {
                return; // a dead or unread channel: drop the line rather than hang the run
            }
            queued = now;
        }
    }
#endif

    // KDIAG_FAULT_LINE_MAX and not 256: this runs on the dying thread's block below the exit
    // red zone, where the array is the largest term of the whole descent. A line past the
    // bound is truncated, and tests/unit/faultline proves every record of the catalogue fits
    // at its worst case.
    void kprintf_fault(char const* fmt, ...)
    {
        char buf[KDIAG_FAULT_LINE_MAX];
        va_list ap;
        va_start(ap, fmt);
        kvprintf_route(buf, sizeof(buf), fmt, ap, WRITE_RECORD);
        va_end(ap);
    }

    void krecord_end(void)
    {
        IrqLock lock;
        if (console_held_commit(held_key()) != 0 and g_console_state != ConsoleState::RECLAIMED)
        {
            cap_console_deliver(lock);
        }
    }

    void krecord_abandon(void)
    {
        IrqLock lock;
        console_held_abandon(held_key());
    }

    bool console_window_held_outside(Task const* t)
    {
        uintptr_t base = 0;
        size_t size = 0;
        arch_console_reclaim_window(&base, &size);
        return size != 0 and dev_window_held_outside(base, size, t);
    }

    bool console_window_withheld(uintptr_t base, size_t size, Task const* t)
    {
        ConsoleState const state = g_console_state;
        if ((state != ConsoleState::USER_OWNED and state != ConsoleState::HANDING_OFF)
            or task_serves_console(t))
        {
            return false;
        }
        uintptr_t win_base = 0;
        size_t win_size = 0;
        arch_console_reclaim_window(&win_base, &win_size);
        return win_size != 0
               and grant_ranges_overlap(base, base + size - 1u, win_base,
                                        win_base + win_size - 1u);
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

#if KICKOS_DIAG_TERSE
    void kpanic_at(char const* file, unsigned line)
    {
        kickos_panic_stack_enter(nullptr, file, line, panic_stack_top());
    }
#endif
}

// Entered by kickos_panic_stack_enter with the stack already moved, so it starts at the top of
// this core's slice whatever depth the assertion fired at. Its console descent is measured as
// the PANIC class of check_trap_redzone.sh and reaches no trap red zone; the FAULT reporter is
// a different chain and still ends several of them.
extern "C" void kickos_panic_report(char const* msg, char const* file, unsigned line)
{
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
#if !KICKOS_DIAG_TERSE
    (void)file;
    (void)line;
#endif
#if KICKOS_DIAG_TERSE
    // kputs plus a hand-rolled decimal: kfmt_vsnprintf's 256-byte frame would be most of what
    // this stack has to hold, and the boards selecting this posture are the small ones.
    if (file != nullptr)
    {
        kickos::kputs("assert ");
        kickos::kputs(file);
        kickos::kputs(":");
        char digits[12];
        size_t n = sizeof(digits) - 1;
        digits[n] = '\0';
        unsigned value = line;
        do
        {
            n = n - 1;
            digits[n] = static_cast<char>('0' + (value % 10));
            value = value / 10;
        } while (value != 0 and n != 0);
        kickos::kputs(&digits[n]);
    }
    else
#endif
    {
        kickos::kputs(msg);
    }
    kickos::kputs("\n");
#if KICKOS_KERNEL_STACKS && KICKOS_KSTACK_REPORT
    kickos::kputs(kickos::kstack_report_text());
#endif
    kfault_terminate();
}

// See kernel.h. The order below is load-bearing: mask FIRST so no ISR can enqueue after,
// then set the flag, then flush what is already queued.
extern "C" void kpanic_enter(void)
{
    (void)arch_irq_save(); // never restored: the panic/fault path does not return
    // Reclaims from ANY prior state, not only USER_OWNED: the ownership state tracks a
    // PUBLISH, never whether the device is garbled, and a thread granted the console
    // window can wreck the channel with no publish at all. RECLAIMED is stored BEFORE the
    // call so a synchronous fault inside the reclaim body re-enters here and stops instead
    // of recursing, and so the body runs exactly once: a second run could truncate the
    // byte in the shift register and cut the banner it just printed. Every chip body is
    // idempotent absolute stores (arch.h), safe on a device no driver ever touched.
    // Only a TERMINAL fault exit may reclaim: a kill-and-resume path must gate this on "this
    // fault terminates the system", where the driver keeps the device and a dark report is
    // correct.
    if (g_console_state != ConsoleState::RECLAIMED)
    {
        g_console_state = ConsoleState::RECLAIMED;
        console_flush_then_reclaim();
    }
    g_console_panicking = true;
    console_tx_flush_sync();
}

// MUST NOT be file-local: kfault_terminate's fallback body is a separate translation unit
// (arch/common/kfault_terminate_default.cc) and shares the once-flag through this symbol.
#if KICKOS_SHUTDOWN_TO_BOOTLOADER
namespace
{
    constinit bool g_handover_tried = false;
}

extern "C" void kickos_bootloader_handover(void)
{
    // arch_reboot kpanics if the ROM call returns, kpanic ends in kfault_terminate, and
    // that lands back here. Exactly once, or it recurses.
    if (g_handover_tried)
    {
        return;
    }
    g_handover_tried = true;
    console_tx_flush_sync();
    // console_tx_flush_sync only empties the ring. The bootrom reboots after a short delay
    // (10 ms on RP2350), which a byte still in the UART FIFO or shift register can outrun,
    // truncating the very dump the image was flashed to produce.
    arch_console_flush_sync();
    (void)arch_reboot(); // -KOS_ENOSYS on a chip with no bootloader entry: caller halts
}
#else
extern "C" void kickos_bootloader_handover(void)
{
}
#endif

// See kernel.h. Every ordered terminal path must funnel here, so the drain-then-hand-over
// order exists in exactly one place upstream of the per-chip arch_shutdown.
extern "C" void kickos_terminate(int status)
{
    console_tx_flush_sync();
    // The RING being empty is not the DEVICE being idle: arch_shutdown can stop the core with
    // a byte still in the UART FIFO or shift register, truncating the last line. It must NOT
    // move back inside kickos_bootloader_handover, which compiles to an empty body on a board
    // without KICKOS_SHUTDOWN_TO_BOOTLOADER.
    arch_console_flush_sync();
    kickos_bootloader_handover();
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
    kickos_terminate(0);
}
