// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Buffered, IRQ-drained console TX ring (see console_tx.h). Lives in the kernel
// rather than lib/ because the producer runs under IrqLock (arch-coupled); lib/
// stays a leaf. The producer (console_tx_write) runs the whole enqueue under
// IrqLock: that serializes multiple thread producers (concurrent kos_print) AND
// keeps it atomic against the single consumer (console_tx_isr). The routing guard
// in console.cc keeps the producer out of ISR context. Storage, the per-chip TX
// edge, and the TX IRQ line come from the chip via arch_console_tx_backend.

#include <kickos/console_tx.h>

#include <kickos/irq.h>
#include <kickos/irqlock.h>
#include <kickos/arch/arch.h>

namespace kickos
{
    void kpanic(char const* msg) __attribute__((noreturn)); // fail-loud on a bad attach
}

namespace
{
    // All console-TX ring state in one object. Only head/tail are shared between the
    // thread producer and the drain ISR -> volatile; the rest are set once at init and
    // read-only after (marking the whole struct volatile would pessimize those + mislead).
    struct ConsoleTxRing
    {
        console_tx_backend const* backend = nullptr;
        char* buf = nullptr;
        uint32_t size = 0; // power of two; usable capacity = size - 1
        uint32_t mask = 0;
        volatile uint32_t head = 0; // producer advances (bytes queued)
        volatile uint32_t tail = 0; // ISR advances (bytes drained)
        int irq_line = -1;          // TX IRQ line (from the backend); console_tx_deinit detaches it
        bool armed = false;

        // Indices stay in [0, size); power-of-two size makes (head - tail) & mask the
        // used count (unsigned wrap reduces mod size). One slot reserved so head==tail
        // is unambiguously empty.
        uint32_t used() const { return (head - tail) & mask; }
        uint32_t space() const { return size - 1u - used(); }
    };

    ConsoleTxRing g_tx;

    // A dead/misconfigured TX channel must NEVER hang panic/fault/boot, so every
    // synchronous poll is bounded (matches the chips' own TX_POLL_TIMEOUT) and bails
    // rather than spinning forever. Cap dwarfs a real per-byte wait at any baud.
    constexpr uint32_t DRAIN_POLL_CAP = 1000000u;

    bool wait_slot()
    {
        for (uint32_t i = 0; i < DRAIN_POLL_CAP; i++)
        {
            if (g_tx.backend->slot_free() != 0)
            {
                return true;
            }
        }
        return false;
    }

    // Poll-push [tail, head) straight to the peripheral with the TX IRQ already
    // disabled. Shared by the overflow and panic-flush paths. Bounded per byte; on a
    // stuck channel it gives up and resets the ring (drops the undrained bytes)
    // rather than hang.
    void drain_sync()
    {
        uint32_t const head = g_tx.head;
        while (g_tx.tail != head)
        {
            if (not wait_slot())
            {
                g_tx.tail = head; // stuck TX: drop the undrained bytes, don't hang
                return;
            }
            g_tx.backend->push(static_cast<uint8_t>(g_tx.buf[g_tx.tail]));
            // Publish AFTER each byte, not once at the end: a synchronous CPU fault
            // (illegal instr / MPU / bus) can land mid-loop -- it is not gated by the
            // interrupt mask -- and its handler flushes again. A stale tail would
            // make that flush re-push bytes already sent, doubling output before the
            // panic banner. Per-byte publish keeps tail a truthful "already sent".
            g_tx.tail = (g_tx.tail + 1u) & g_tx.mask;
        }
    }

    void console_tx_isr_trampoline(void*) { console_tx_isr(); }
}

extern "C"
{

void console_tx_init(console_tx_backend const* be, char* storage, uint32_t size, int irq_line)
{
    g_tx.backend = be;
    g_tx.buf = storage;
    g_tx.size = size;
    g_tx.mask = size - 1u;
    g_tx.head = 0;
    g_tx.tail = 0;
    g_tx.irq_line = irq_line; // set BEFORE armed: deinit must never see armed with a stale line
    g_tx.armed = true;
}

int console_tx_armed(void) { return static_cast<int>(g_tx.armed); }

void console_tx_write(char const* buf, size_t n)
{
    if (not g_tx.armed)
    {
        // Disarm fallback. Re-read ownership (B1): a producer that raced past the arm
        // check must DROP the chip write unless the kernel still owns the UART, else it
        // poll-pokes a device a userspace driver now owns. Bracket the poke with the
        // in-flight count so publish drains a stale writer before the driver starts.
        if (console_owner_is_kernel() == 0)
        {
            return;
        }
        console_chip_writer_enter();
        arch_console_write_sync(buf, n);
        console_chip_writer_leave();
        return;
    }

    // The whole enqueue runs under IrqLock: it serializes concurrent thread
    // producers and is atomic against the drain ISR. The fast-path copy is bounded
    // (<= ring) -- microseconds, not the transmission the buffering moves off-caller.
    kickos::IrqLock lock;

    // Fast path: the burst fits.
    if (n <= g_tx.space())
    {
        bool const was_empty = (g_tx.used() == 0);
        uint32_t idx = g_tx.head;
        for (size_t i = 0; i < n; i++)
        {
            g_tx.buf[idx] = buf[i];
            idx = (idx + 1u) & g_tx.mask;
        }
        KICKOS_CONSOLE_TX_BARRIER();
        g_tx.head = idx;
        g_tx.backend->irq_enable();
        // Prime the pump on the idle->busy transition. On an edge/transition-
        // triggered TX interrupt (XMC USIC TBIEN, and -- pending HW confirmation --
        // the PL011 with FEN=0 and the RX SCI TXI), enabling the IRQ on an idle
        // channel raises nothing, so push the first byte directly to start the
        // transfer; its completion event then drives the drain ISR. The prime is
        // load-bearing there, NOT redundant. On a truly level-triggered part (K64F
        // TDRE, asserted while the register is empty) it is a harmless immediate send.
        if (was_empty and g_tx.head != g_tx.tail and g_tx.backend->slot_free() != 0)
        {
            g_tx.backend->push(static_cast<uint8_t>(g_tx.buf[g_tx.tail]));
            g_tx.tail = (g_tx.tail + 1u) & g_tx.mask;
        }
        return;
    }

    // Overflow (rare: sustained > line-rate output): drain the ring + send the burst
    // synchronously, TX IRQ off, bounded so a stuck channel cannot hang. Still under
    // the lock -- a full drain can mask IRQs for up to the drain time, an accepted
    // debug-console-flooding tradeoff in exchange for no producer/ISR race. Dropping
    // kernel debug output would be worse than the stall.
    g_tx.backend->irq_disable();
    drain_sync();
    for (size_t i = 0; i < n; i++)
    {
        if (not wait_slot())
        {
            return; // stuck TX: give up rather than hang
        }
        g_tx.backend->push(static_cast<uint8_t>(buf[i]));
    }
}

// The ISR drain pokes the device but is deliberately NOT bracketed by the B1
// chip-writer count: console_tx_deinit detaches the handler and NVIC-masks the TX
// line under IrqLock strictly BEFORE kos_console_publish flips the state, so this ISR
// can never fire once the console is USER_OWNED -- there is no stale-writer window for
// the count to guard here.
void console_tx_isr(void)
{
    uint32_t const head = g_tx.head; // producer cannot run during this ISR (priority)
    while (g_tx.tail != head and g_tx.backend->slot_free() != 0)
    {
        g_tx.backend->push(static_cast<uint8_t>(g_tx.buf[g_tx.tail]));
        // Publish per byte (not once at the end): a synchronous fault mid-drain
        // flushes again, and a stale tail would re-push already-sent bytes.
        g_tx.tail = (g_tx.tail + 1u) & g_tx.mask;
    }
    if (g_tx.tail == head)
    {
        g_tx.backend->irq_disable();
    }
}

void console_tx_flush_sync(void)
{
    if (not g_tx.armed)
    {
        return;
    }
    // Under IrqLock so "disable the TX IRQ + snapshot [tail, head)" is atomic against
    // the drain ISR and any thread producer: without it a producer racing between the
    // disable and drain_sync's head read could re-enable the IRQ or extend head while
    // we drain. Idempotent -- a second flush finds head==tail and does nothing. Panic
    // callers have already masked (kpanic_enter), where this nests harmlessly.
    kickos::IrqLock lock;
    g_tx.backend->irq_disable();
    drain_sync();
}

// Weak default: chips without a buffered console leave the console synchronous.
__attribute__((weak)) console_tx_backend const* arch_console_tx_backend(char**, uint32_t*, int*)
{
    return nullptr;
}

// Called once from kmain after irq_init(). If the chip offers a backend, bind the
// drain ISR, unmask the line (priority lands in the IrqLock-maskable band), and
// arm the ring. No-op on sim / polled-only chips.
void console_buffer_init(void)
{
    char* buf = nullptr;
    uint32_t size = 0;
    int line = -1;
    console_tx_backend const* be = arch_console_tx_backend(&buf, &size, &line);
    if (be == nullptr or buf == nullptr or size == 0 or line < 0)
    {
        return;
    }
    // Fail loud: a silently-dropped attach would leave the ring armed but never
    // drained -- output would fill it, fall back to the bounded sync path, and
    // "look like it works" while every buffered write stalled. A misconfigured TX
    // line at boot is a build/port bug, not a runtime condition to paper over.
    if (not kickos::irq_attach(line, console_tx_isr_trampoline, nullptr))
    {
        kickos::kpanic("console_buffer_init: irq_attach failed");
    }
    // Arm g_tx (backend + ring) BEFORE the line can fire: the latch-and-coalesce
    // contract redelivers any pend latched on this line before boot the instant
    // ISER is set, and console_tx_isr on a zero-init g_tx would deref a NULL backend.
    // So init first, then discard pre-boot garbage, then enable last.
    console_tx_init(be, buf, size, line); // line folded in: set atomically with armed
    arch_irq_clear_pending(line);
    arch_irq_unmask(line);
}

// Relinquish the buffered TX path so a userspace driver can take the UART (D2). One
// IrqLock makes the four steps atomic against every buffered producer and the drain ISR
// (all IrqLock); flush_sync's own IrqLock nests harmlessly. Idempotent: the null-backend
// guard also covers polled-only chips (mps2/virt/nrf51 never arm) and a re-publish (the
// ring already disarmed). The caller (kos_console_publish) flips g_console_state to
// USER_OWNED strictly AFTER this returns, so a synchronous fault mid-deinit still panics
// on a kernel-owned, kernel-inited UART.
void console_tx_deinit(void)
{
    if (not g_tx.armed)
    {
        return;
    }
    kickos::IrqLock lock;
    console_tx_flush_sync();     // a. drain queued bytes on a still-kernel-owned UART
    g_tx.backend->irq_disable();       // b. stop the TX-empty IRQ at the peripheral
    kickos::irq_detach(g_tx.irq_line); // c. null the handler + NVIC-mask the line
    g_tx.armed = false;          // d. disarm the ring (producer stops buffering)
}

} // extern "C"
