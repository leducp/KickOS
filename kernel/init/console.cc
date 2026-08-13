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
#include <kickos/libc/string.h>
#include <kickos/libc/fmt.h>

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
    // Forces the polled path once a panic has started: the ring's drain ISR is masked
    // from that point on. Only carries a panic that does NOT reclaim, since RECLAIMED
    // already routes polled.
    constinit volatile bool g_console_panicking = false;

    // Who owns the UART TX register. Must be consulted BEFORE the buffered/sync
    // sub-decision: in USER_OWNED the kernel may touch the device on NO path at all.
    // See docs/design-m3-console-handover-stageii.md.
    enum class ConsoleState : uint8_t
    {
        KERNEL_OWNED, // boot default; kernel drives the UART (buffered ring or polled)
        USER_OWNED,   // a userspace driver owns the UART; kernel chip path DROPS
        RECLAIMED     // the kernel forcibly took the UART back (panic, or driver death);
                      // polled-only
    };
    constinit volatile ConsoleState g_console_state = ConsoleState::KERNEL_OWNED;

    // Set by the cap layer when the published console endpoint loses its last
    // WAIT-bearing cap; consumed by console_on_driver_death at the end of exit_current.
    // A flag rather than an immediate reclaim; see console_tx.h.
    constinit volatile bool g_console_driver_died = false;

    // In-flight kernel chip writers: incremented under the same state read that decided
    // to poke the device while KERNEL_OWNED, decremented after. kos_console_publish flips
    // the state first and then spins on this, so a writer that raced past a stale
    // KERNEL_OWNED read is off the device before the userspace driver touches it. Nothing
    // increments it after the flip, so it strictly drains to 0.
    constinit volatile int g_chip_writers = 0;
}

// Every access to the chip-writer count, mutators and reader alike, MUST run under
// IrqLock: console_emit can run in ISR/fault context, so an unlocked volatile RMW tears
// against a thread producer's and an unlocked reader can observe the intermediate.
extern "C" int console_owner_is_kernel(void)
{
    return static_cast<int>(g_console_state == ConsoleState::KERNEL_OWNED);
}

extern "C" void console_owner_set_user(void)
{
    g_console_state = ConsoleState::USER_OWNED;
}

extern "C" void console_chip_writer_enter(void)
{
    kickos::IrqLock lock;
    g_chip_writers = g_chip_writers + 1; // explicit RMW: '++' on volatile is deprecated (C++20)
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
    // The note fires on the endpoint's last RECEIVER, which is the service thread, not
    // necessarily the thread holding the registers: a driver is a THREAD GROUP.
    // Reclaiming on the note alone would reprogram the UART under a live IRQ thread that
    // owns those registers and silence its source (INT_ENA=0), parking it forever. So the
    // precondition is asked of the DEVICE: nobody may still hold the window
    // arch_console_reclaim is about to write. The note stays SET across a refusal and
    // every exit_current and voluntary close re-runs this, so the LAST holder's own exit
    // reclaims.
    uintptr_t win_base = 0;
    size_t win_size = 0;
    arch_console_reclaim_window(&win_base, &win_size);
    if (win_size != 0 and not kickos::dev_window_free(win_base, win_size))
    {
        return;
    }
    g_console_driver_died = false;
    // Only a PUBLISHED console can lose its driver, so any other state means the flag
    // outlived what set it (a re-publish, or a panic that already reclaimed). RECLAIMED is
    // stored before the body so a fault inside that body cannot recurse, and so the body
    // runs exactly once.
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
    // Takes an already-CRLF-expanded chunk. The buffered path is reachable only from
    // ordinary thread context with the ring armed; panic, ISR/fault context and pre-arm
    // boot fall back to the bounded polled writer. This is the single choke point that
    // keeps the ring a true single-producer, so no other site may enqueue.
    static void console_emit(char const* buf, size_t n)
    {
        switch (g_console_state)
        {
        case ConsoleState::KERNEL_OWNED:
        {
            // The poke must be bracketed by the in-flight count under the SAME state read
            // that selected this branch. The polled poke has no other serialisation, so
            // this count is what publish drains against.
            console_chip_writer_enter();
            if (console_tx_armed() != 0 and arch_in_isr() == 0 and not g_console_panicking)
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
        case ConsoleState::USER_OWNED:
        {
            return; // DROP: the driver owns the UART (RTT still carries it, see kconsole_write)
        }
        case ConsoleState::RECLAIMED:
        {
            arch_console_write_sync(buf, n); // polled only, whatever the ring's state
            return;
        }
        }
    }
#endif

    // Locking is PER BACKEND. RTT's WrOff RMW is written from thread, ISR and fault
    // context, so it takes the crit section for the few microseconds it needs. The chip
    // transport locks internally and must NEVER be held under IrqLock across a whole
    // transmission: a 256 B write at 115200 would mask interrupts for ~22 ms.
    void kconsole_write(char const* buf, size_t n)
    {
#if !KICKOS_CONSOLE_CHIP && !KICKOS_CONSOLE_RTT
        // KICKOS_CONSOLE=none: every backend is compiled out and this is deliberately a
        // sink. Panic, fault and boot behaviour is unchanged; they just say nothing.
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
                console_emit(cooked, j);
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
            console_emit(cooked, j);
        }
#else
        console_emit(buf, n);
#endif
#endif
    }

    void kputs(char const* s)
    {
        kconsole_write(s, strlen(s));
    }

    namespace
    {
        void kvprintf_route(char const* fmt, va_list ap, bool route)
        {
            char buf[256];
            kvsnprintf(buf, sizeof(buf), fmt, ap);
            size_t const n = strlen(buf);
            kconsole_write(buf, n);
            // USER_OWNED only. RECLAIMED means the kernel has the device back and the driver
            // is gone, so routing there would send into an endpoint nobody serves.
            if (route and g_console_state == ConsoleState::USER_OWNED)
            {
                (void)cap_console_deliver(buf, n);
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
    // Deliberately not kprintf: an assert fires in whatever thread context tripped it,
    // and kvsnprintf's 256-byte frame does not fit the 512-byte idle stack the boards
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
    // Must run BEFORE the flush.
    // Only a TERMINAL fault exit may reclaim. When a kill-and-resume fault path arrives,
    // gate this on "this fault terminates the system", not on "a fault happened": the
    // driver keeps the device and a dark report there is correct.
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
    kickos_terminate(0);
}
