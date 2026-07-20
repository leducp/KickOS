// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Minimal in-kernel debug console: write-only, unbuffered, routed to the arch
// console bottom edge (sim: host stdout). The standard microkernel exception
// for panic/early-boot/fault reporting.

#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/arch/arch.h>
#include <kickos/console_tx.h>
#include <kickos/irqlock.h>
#include <kickos/libc/string.h>
#include <kickos/libc/fmt.h>

#include <stdarg.h>

// Console backend selection (from the build; see KICKOS_CONSOLE). Default to the
// chip transport only so a standalone compile still prints.
#ifndef KICKOS_CONSOLE_CHIP
#define KICKOS_CONSOLE_CHIP 1
#endif
#ifndef KICKOS_CONSOLE_RTT
#define KICKOS_CONSOLE_RTT 0
#endif
// Lower '\n' to CR+LF on the chip UART only (bare metal). Off by default so a
// standalone/sim compile stays raw. Set by the build (see KICKOS_CONSOLE_CRLF).
#ifndef KICKOS_CONSOLE_CRLF
#define KICKOS_CONSOLE_CRLF 0
#endif

#if KICKOS_CONSOLE_RTT
#include <kickos/rtt.h>
#endif

namespace
{
    // Set at the top of kpanic so all subsequent console output takes the polled
    // path (the buffered ring's ISR is being torn down / cannot be trusted). Only
    // load-bearing while KERNEL_OWNED; RECLAIMED subsumes it once handed over.
    volatile bool g_console_panicking = false;

    // Console device-ownership axis (orthogonal to the buffered-vs-sync decision):
    // who owns the UART TX register. Consulted BEFORE the buffered/sync sub-decision,
    // because in USER_OWNED the kernel must touch the device on NO path at all.
    // See docs/design-m3-console-handover-stageii.md (D1).
    enum class ConsoleState : uint8_t
    {
        KERNEL_OWNED, // boot default; kernel drives the UART (buffered ring or polled)
        USER_OWNED,   // a userspace driver owns the UART; kernel chip path DROPS
        RECLAIMED     // panic forcibly took the UART back; polled-only
    };
    volatile ConsoleState g_console_state = ConsoleState::KERNEL_OWNED;

    // In-flight kernel chip writers (B1): incremented under the same read that decided
    // to poke the device while KERNEL_OWNED, decremented after. kos_console_publish
    // spins on this (state already flipped to USER_OWNED) so a writer that raced past a
    // stale KERNEL_OWNED read drains off the device before the userspace driver touches
    // it. After the flip NO path increments it, so it strictly drains to 0.
    volatile int g_chip_writers = 0;
}

// Console-ownership seam shared with console_tx.cc (disarm-fallback gate) and the
// kos_console_publish syscall (handover). Declared in console_tx.h. The chip-writer
// count RMW runs under IrqLock: console_emit can run in ISR/fault context, so a plain
// volatile ++/-- could tear against a thread producer's ++/--.
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

extern "C" int console_chip_writers(void)
{
    return g_chip_writers;
}

namespace kickos
{
    // Route one already-CRLF-expanded chunk to the chip. The buffered path is used
    // only in ordinary thread context with the ring armed; panic, any ISR/fault
    // context, and pre-arm boot fall back to the bounded polled writer. This is the
    // single choke point that keeps the ring a true single-producer (never entered
    // from ISR context).
    static void console_emit(char const* buf, size_t n)
    {
        switch (g_console_state)
        {
        case ConsoleState::KERNEL_OWNED:
            // Bracket the device poke with the in-flight count under the SAME state read
            // (B1). The buffered branch's poke happens inside console_tx_write's IrqLock
            // (serialized with deinit); the else branch's polled poke does not, so the
            // count is what publish drains against.
            console_chip_writer_enter();
            if (console_tx_armed() != 0 and arch_in_isr() == 0 and not g_console_panicking)
            {
                arch_console_write(buf, n); // buffered ring
            }
            else
            {
                arch_console_write_sync(buf, n); // polled
            }
            console_chip_writer_leave();
            return;
        case ConsoleState::USER_OWNED:
            return; // DROP: the driver owns the UART (RTT still carries it, see kconsole_write)
        case ConsoleState::RECLAIMED:
            arch_console_write_sync(buf, n); // panic reclaimed it -> polled only
            return;
        }
    }

    // Fan-out to every enabled backend (compile-time). Per-backend locking: the
    // RTT ring is a WrOff read-modify-write written from thread/ISR/fault contexts,
    // so it runs under the crit section (microseconds); the chip transport routes
    // through console_emit (buffered enqueue, or the bounded polled writer) and does
    // its own brief locking internally -- never held under IrqLock across a full
    // transmission (a 256B write at 115200 would mask interrupts for ~22 ms).
    void kconsole_write(char const* buf, size_t n)
    {
#if KICKOS_CONSOLE_RTT
        {
            IrqLock lock;
            kickos_rtt_write(buf, n);
        }
#endif
#if KICKOS_CONSOLE_CHIP
#if KICKOS_CONSOLE_CRLF
        // Expand '\n' -> "\r\n" once into a scratch buffer and emit whole chunks:
        // a typical line becomes ONE console_emit (one enqueue / one lock) instead
        // of one per fragment. The kernel never emits '\r' itself, so no doubling.
        // RTT above stays raw (its viewer cooks). Chunked, so correctness does not
        // depend on the buffer size; flush leaving room for a possible '\r'+'\n'.
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

    void kprintf(char const* fmt, ...)
    {
        char buf[256];
        va_list ap;
        va_start(ap, fmt);
        kvsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        kconsole_write(buf, strlen(buf));
    }

    void kpanic(char const* msg)
    {
        kpanic_enter(); // mask IRQs + force sync path + flush queued bytes
        kputs("\nKERNEL PANIC: ");
        kputs(msg);
        kputs("\n");
        kfault_terminate(); // blink forever (real HW) or exit with a fault status (host/QEMU)
    }
}

// See kernel.h. Defined here so it can touch the file-local panic flag; extern "C"
// so the arch fault reporters (separate TUs) can call it. Ordering matters: mask
// FIRST (no ISR can enqueue after), then set the flag, then flush what is already
// queued.
extern "C" void kpanic_enter(void)
{
    (void)arch_irq_save(); // never restored: the panic/fault path does not return
    // Force the UART back to a polled-ready channel if a userspace driver held it, so the
    // panic banner reaches the wire. Idempotent + re-entrant (a nested fault re-enters
    // here with state still USER_OWNED and re-runs reclaim cleanly). Runs BEFORE the flush
    // (which is a no-op post-handover -- the ring is disarmed). M2 DEPENDENCY: only a
    // TERMINAL fault exit may reclaim; a future kill-and-resume fault path must NOT (the
    // driver keeps the device, a dark report on that path is correct) -- gate reclaim on
    // "this fault terminates the system," not on "a fault happened."
    if (g_console_state == ConsoleState::USER_OWNED)
    {
        arch_console_reclaim();
        g_console_state = ConsoleState::RECLAIMED;
    }
    g_console_panicking = true;
    console_tx_flush_sync();
}

namespace
{
    // Wall-clock delay for the panic blink, so the pattern is the SAME real duration
    // on every board (a fixed nop count blurs into fast flicker on a fast core). Uses
    // arch_clock_now (up on any post-boot fault); a wrap-proof iteration guard bails
    // rather than hanging if the clock is dead (a pre-clock-init fault).
    void panic_delay_ms(uint32_t ms)
    {
        uint64_t const start = arch_clock_now();
        uint64_t const span = static_cast<uint64_t>(ms) * 1000000ull;
        // A dead clock (a pre-clock-init fault) never advances, so the wait below
        // would hang forever. Guard on NO-ADVANCE, not an iteration count: probe
        // until the clock ticks at least once. If it never moves across a bounded
        // number of reads, treat it as stopped and give up (the blink degrades, but
        // the panic path does not hang). Once it moves we trust it and wait for real.
        uint32_t probe = 0;
        while (arch_clock_now() == start)
        {
            probe++;
            if (probe >= (1u << 24)) // ~16M reads, no tick: clock is stopped
            {
                return;
            }
        }
        while (arch_clock_now() - start < span)
        {
        }
    }
}

// Weak: the real-hardware dead-end. A distinctive heartbeat -- three 0.2 s blinks
// then a 2 s gap, forever -- says "panicked" at a glance on a board with no console.
// Overridden by the host/QEMU chips to exit with a fault status (see kernel.h).
extern "C" __attribute__((weak, noreturn)) void kfault_terminate(void)
{
    kpanic_enter(); // idempotent; masks IRQs for any path reaching here directly
    for (;;)
    {
        for (int b = 0; b < 3; b++)
        {
            ::kickos::kdiag_led_set(true);
            panic_delay_ms(200);
            ::kickos::kdiag_led_set(false);
            panic_delay_ms(200);
        }
        panic_delay_ms(2000); // 2 s dark gap before the next burst
    }
}

// Fallback synchronous writer. Every chip with a buffered console (K64F, XMC, RX72M,
// ESP32, ESP32-C6, the STM32/RP2040/SAM3X fleet, and the sim) MUST override this with its
// polled writer -- otherwise the weak alias below routes back into the buffered
// ring, and panic/fault output enqueues into a ring whose drain ISR is masked and
// never runs. Polled-only chips (mps2/virt/nrf51) reuse arch_console_write,
// which is already their polled writer, so the weak default is correct for them.
extern "C" __attribute__((weak)) void arch_console_write_sync(char const* buf, size_t n)
{
    arch_console_write(buf, n);
}

// Force the UART back to a known polled-ready channel after a userspace driver may have
// left its registers garbled (panic reclaim, D6). WEAK no-op default: boards that never
// hand over need nothing, and this pass only WIRES the call -- the real per-chip bodies
// (XMC/K64F full-window rewrite) land in the next pass. Must be written as idempotent
// straight-line register STORES (safe to repeat from a partial nested-fault state).
extern "C" __attribute__((weak)) void arch_console_reclaim(void) {}

// Memory-protection violation caught by the arch backend (sim: SIGSEGV over
// the guard page). Report the offending task + address through the console.
// M0: the intended wild-write demo is the final act, so we shut down cleanly
// after reporting. M2 will turn this into per-task fault + resume.
extern "C" void kickos_isr_fault(uintptr_t addr, int is_write)
{
    // Funnel through kpanic_enter FIRST (B2): a terminal fault in USER_OWNED must reclaim
    // the UART (the likeliest post-handover faulter IS the console driver itself), else the
    // report prints to a device the kernel no longer owns and the system halts silently.
    // kpanic_enter is idempotent and subsumes the flush, preserving the terminal behavior.
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
    ::kickos::kprintf("\nMPU FAULT: task '%s' attempted %s at %p -- reported\n",
                      who, dir, reinterpret_cast<void*>(addr));
    arch_shutdown(0);
}
