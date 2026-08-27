// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Minimal in-kernel debug console: write-only, routed to the arch console bottom edge
// (sim: host stdout). Reserved for panic, early boot and fault reporting.

#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/arch/arch.h>
#include <kickos/cap.h> // cap_console_deliver (the fault record's route to a published console)
#include <kickos/console_tx.h>
#include <kickos/irqlock.h>
#include <kickos/kruntime.h>
#include <kickos/sys/atomic.h>

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

namespace
{
    using kickos::Atomic;
    using kickos::Order;

    // Forces the polled path once a panic has started: the ring's drain ISR is masked
    // from that point on.
    constinit Atomic<bool, Order::RELAXED> g_console_panicking = false;

    // Who owns the UART TX register. Must be consulted BEFORE the buffered/sync
    // sub-decision: in USER_OWNED the kernel may touch the device on NO path at all.
    // See docs/design-m3-console-handover-stageii.md.
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

    // Set by the cap layer when the published console endpoint loses its last WAIT-bearing
    // cap. Sticky across a refused reclaim (see console_tx.h). It names ONE published
    // console, so a re-publish retires it (console_owner_set_user).
    constinit Atomic<bool, Order::RELAXED> g_console_driver_died = false;

    // In-flight kernel chip writers. kos_console_publish enters HANDING_OFF first and then
    // spins on this, so a writer that raced past a stale device-owning read is off the
    // device before the userspace driver touches it. That argument holds ONLY because
    // nothing increments after that flip, which is what chip_writer_enter enforces.
    constinit Atomic<int, Order::RELAXED> g_chip_writers = 0;

    // The state read and the increment are ONE masked operation, or publish's drain is
    // blind to a writer that read a device-owning state just before the flip: the drain
    // sees 0, the driver starts, and the woken writer then bit-bangs a UART it no longer
    // owns. Refuses HANDING_OFF as well as USER_OWNED: a publish that admitted new writers
    // would have nothing left to make its drain converge. `out_state` IS the decisive read:
    // a caller must not re-read the state.
    bool chip_writer_enter(ConsoleState* out_state)
    {
        kickos::IrqLock lock;
        ConsoleState const state = g_console_state;
        if (state == ConsoleState::USER_OWNED or state == ConsoleState::HANDING_OFF)
        {
            return false;
        }
        *out_state = state;
        g_chip_writers = g_chip_writers + 1;
        return true;
    }
}

extern "C" int console_owner_is_kernel(void)
{
    return static_cast<int>(g_console_state == ConsoleState::KERNEL_OWNED);
}

// TRUE through a handover as well as at rest: the writer a handover is draining must still
// reach the device, which no driver has taken yet.
extern "C" int console_chip_writable(void)
{
    return static_cast<int>(g_console_state != ConsoleState::USER_OWNED);
}

// Publish's first half. Leaves the UART kernel-owned so a writer already inside the bracket
// can finish on it. The caller MUST drain console_chip_writers to zero before
// console_owner_set_user, else that writer lands on the driver's UART, and the drain
// converges only because of the refusal installed here. Idempotent.
extern "C" void console_handover_begin(void)
{
    kickos::IrqLock lock;
    if (g_console_state == ConsoleState::USER_OWNED)
    {
        return;
    }
    g_console_state = ConsoleState::HANDING_OFF;
    console_tx_deinit();
}

// Publish's LAST step. A writer still counted here is one the drain was meant to wait for,
// and it would finish its message on the driver's UART.
extern "C" void console_owner_set_user(void)
{
    KICKOS_ASSERT(console_chip_writers() == 0);
    // Voids any pending death note: it belongs to the console being replaced, and
    // USER_OWNED is the only thing console_on_driver_death checks, so a note left standing
    // here reclaims the NEW driver's UART when the OLD driver's last thread exits.
    g_console_driver_died = false;
    g_console_state = ConsoleState::USER_OWNED;
}

// Every access to the chip-writer count, mutators and reader alike, MUST run under IrqLock:
// console_emit can run in ISR/fault context, so an unlocked read-modify-write tears against
// a thread producer's and an unlocked reader can observe the intermediate. This increment is
// unconditional, so it must only ever nest inside a chip_writer_enter bracket that already
// passed the state gate (console_tx.cc's write_unbuffered is the one caller); reached on its
// own it would keep publish's drain from converging.
extern "C" void console_chip_writer_enter(void)
{
    kickos::IrqLock lock;
    g_chip_writers = g_chip_writers + 1;
}

extern "C" void console_chip_writer_leave(void)
{
    kickos::IrqLock lock;
    g_chip_writers = g_chip_writers - 1;
}

extern "C" void console_note_driver_death(void)
{
    g_console_driver_died = true;
}

extern "C" void console_on_driver_death(void)
{
    if (not g_console_driver_died)
    {
        return;
    }
    // The note fires on the endpoint's last RECEIVER, the service thread, not necessarily
    // the thread holding the registers: a driver is a THREAD GROUP. Reclaiming on the note
    // alone would reprogram the UART under a live IRQ thread that owns those registers and
    // silence its source (INT_ENA=0), parking it forever. So the precondition is asked of
    // the DEVICE: nobody may still hold the window arch_console_reclaim is about to write.
    // A cancelled peer is still a holder: thread_cancel marks it, and only its own exit sets
    // `dying`, so the note stays set across the refusal and the LAST holder's exit_current
    // reclaims.
    uintptr_t win_base = 0;
    size_t win_size = 0;
    arch_console_reclaim_window(&win_base, &win_size);
    if (win_size != 0 and not kickos::dev_window_free(win_base, win_size))
    {
        return;
    }
    g_console_driver_died = false;
    // Only a PUBLISHED console can lose its driver, so any other state means a panic
    // already reclaimed. A re-publish cannot reach here with a stale note: it clears the
    // note itself, because USER_OWNED alone does not say WHICH console it refers to.
    // RECLAIMED is stored before the body so a fault inside that body cannot recurse, and
    // so the body runs exactly once.
    if (g_console_state != ConsoleState::USER_OWNED)
    {
        return;
    }
    g_console_state = ConsoleState::RECLAIMED;
    arch_console_reclaim();
}

// A stale zero here makes kos_console_publish's handover drain give the UART to a
// userspace driver while a kernel writer is still on the device, hence the lock.
extern "C" int console_chip_writers(void)
{
    kickos::IrqLock lock;
    return g_chip_writers;
}

namespace kickos
{
#if KICKOS_CONSOLE_CHIP
    // Takes an already-CRLF-expanded chunk. This is the single choke point that keeps the
    // ring a true single-producer, so no other site may enqueue.
    static void console_emit(char const* buf, size_t n, bool force_sync)
    {
        // The count is taken under the same masked read that selects the transport, so
        // publish either drains this writer or the writer never reaches the device. The
        // polled poke has no other serialisation, and RECLAIMED needs the bracket as much as
        // KERNEL_OWNED does: a console published again after a reclaim flips straight out
        // of it.
        ConsoleState state = ConsoleState::KERNEL_OWNED;
        if (chip_writer_enter(&state))
        {
            if (state == ConsoleState::KERNEL_OWNED and console_tx_armed() != 0
                and arch_in_isr() == 0 and not g_console_panicking)
            {
                arch_console_write(buf, n);
            }
            else
            {
                arch_console_write_sync(buf, n);
            }
            console_chip_writer_leave();
            return;
        }
        // USER_OWNED: DROP, the driver owns the UART (RTT still carries it, see
        // kconsole_write). force_sync accepts interleaving with the driver's in-flight
        // bytes, and is set only after the published route has already refused these ones.
        if (force_sync)
        {
            arch_console_write_sync(buf, n);
        }
    }
#endif

    // Locking is PER BACKEND. RTT's WrOff RMW is written from thread, ISR and fault
    // context, so it takes the crit section for the few microseconds it needs. The chip
    // transport locks internally and must NEVER be held under IrqLock across a whole
    // transmission: a 256 B write at 115200 would mask interrupts for ~22 ms.
    static void kconsole_write_impl(char const* buf, size_t n, bool force_sync)
    {
        (void)force_sync;
#if !KICKOS_CONSOLE_CHIP && !KICKOS_CONSOLE_RTT
        // KICKOS_CONSOLE=none: the writer is a sink. Panic, fault and boot still run their
        // full paths and terminate the same way.
        (void)buf;
        (void)n;
#endif
#if KICKOS_CONSOLE_RTT
        {
            IrqLock lock;
            kickos_rtt_write(buf, n);
        }
#endif
#if KICKOS_CONSOLE_CHIP
#if KICKOS_CONSOLE_CRLF
        // The kernel never emits '\r' itself, so this cannot double one. RTT above stays
        // raw because its viewer cooks. The flush leaves room for a '\r'+'\n' pair, so
        // correctness does not depend on the scratch size.
        char cooked[128];
        size_t j = 0;
        for (size_t i = 0; i < n; i++)
        {
            if (j > sizeof(cooked) - 2)
            {
                console_emit(cooked, j, force_sync);
                j = 0;
            }
            if (buf[i] == '\n')
            {
                cooked[j++] = '\r';
            }
            cooked[j++] = buf[i];
        }
        if (j > 0)
        {
            console_emit(cooked, j, force_sync);
        }
#else
        console_emit(buf, n, force_sync);
#endif
#endif
    }

    void kconsole_write(char const* buf, size_t n)
    {
        kconsole_write_impl(buf, n, false);
    }

    void kputs(char const* s)
    {
        kconsole_write(s, kstrlen(s));
    }

    namespace
    {
        void kvprintf_route(char const* fmt, va_list ap, bool route)
        {
            char buf[256];
            kfmt_vsnprintf(buf, sizeof(buf), fmt, ap);
            size_t const n = kstrlen(buf);
            kconsole_write(buf, n);
            // USER_OWNED only. RECLAIMED means the kernel has the device back and the driver
            // is gone, so routing there would send into an endpoint nobody serves.
            if (route and g_console_state == ConsoleState::USER_OWNED)
            {
                // 0 means nothing was delivered, and the chip write above already dropped,
                // so without this the record reaches nobody.
                if (cap_console_deliver(buf, n) == 0)
                {
                    kconsole_write_impl(buf, n, true);
                }
            }
        }
    }

    void kprintf(char const* fmt, ...)
    {
        va_list ap;
        va_start(ap, fmt);
        kvprintf_route(fmt, ap, false);
        va_end(ap);
    }

    void kprintf_fault(char const* fmt, ...)
    {
        va_list ap;
        va_start(ap, fmt);
        kvprintf_route(fmt, ap, true);
        va_end(ap);
    }

    void kpanic(char const* msg)
    {
        kpanic_enter();
        kputs("\nKERNEL PANIC: ");
        kputs(msg);
        kputs("\n");
        kfault_terminate(); // blink forever (real HW) or exit with a fault status (host/QEMU)
    }

#if KICKOS_DIAG_TERSE
    // kputs plus a hand-rolled decimal: an assert fires in whatever thread context tripped
    // it, and kfmt_vsnprintf's 256-byte frame does not fit the 512-byte idle stack the boards
    // that select this posture provision.
    void kpanic_at(char const* file, unsigned line)
    {
        kpanic_enter();
        kputs("\nKERNEL PANIC: assert ");
        kputs(file);
        kputs(":");
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
        kputs(&digits[n]);
        kputs("\n");
        kfault_terminate();
    }
#endif
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
        arch_console_reclaim();
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
