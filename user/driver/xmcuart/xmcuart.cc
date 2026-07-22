// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// XMC4800 userspace polled UART TX console driver (see <kickos/driver/xmcuart.h>).
// An UNPRIVILEGED thread owns the granted USIC0 CH0 (U0C0) register window and
// serves a console endpoint: kos_recv() a byte batch, then for each byte poll the
// transmit-buffer status (TCSR.TDV) until the buffer is free and write TBUF0. This
// mirrors the kernel polled writer (arch/arm/chip/xmc4800/usic_uart.cc
// kickos_xmc_usic_write / tx_ready): TDV set means "the transmit buffer still holds
// a word pending transfer" -- wait for it to clear before loading the next byte, or
// the pending word is overwritten and a byte is dropped/garbled on the wire.
//
// The driver does NOT touch clock/pins/baud: the kernel's kickos_xmc_usic_init()
// configured them at boot and console_tx_deinit() left the channel ASC-mode,
// pinned, and TX-capable in a polled state. Only registers INSIDE the granted
// window (TCSR 0x038, TBUF0 0x080) are poked. SCU_CGATCLR0/PRCLR0 (clock) and
// P1_IOCR4 (pin mux) live in separate privileged peripherals outside the window --
// unreachable, and left intact.
//
// HARD RULE (design D7): NO libc stdio. printf/puts route through _write ->
// kos_send(0, ..) -> this driver's own endpoint (a self-send that deadlocks, since
// the driver holds the sole CAP_WAIT recv cap so recv_holders never reaches 0 and
// no EPIPE fires). Diagnostics go direct to the USIC window (poll_put below) or via
// kos_kconsole_write (kos::print -> the RTT / kernel debug path, which does NOT
// route through the endpoint).
//
// Register addresses / bit fields are clean-room from the XMC4700/XMC4800 Reference
// Manual (V1.3, 2016-07); no XMCLib/DAVE/CMSIS vendor source.

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/driver/xmcuart.h>

#include <stdint.h>

namespace
{
    // USIC0 channel 0 register block (RM Table 18-21 "Registers Address Space":
    // USIC0_CH0 = 0x4003_0000 .. 0x4003_01FF). This is the console channel the
    // kernel inited (U0C0); the driver is granted the whole 512 B channel window.
    constexpr uintptr_t U0C0_BASE = 0x40030000u;

    // U0C0 window granted to the driver: base = channel base, size = 0x200 (512 B),
    // R|W|DEV. PMSA-encodable: 512 is pow2 >= the 32 B PMSA minimum and 0x4003_0000
    // is 0x200-aligned -> one descriptor, exact-cover, no pad/split. Every register
    // the driver touches (TCSR 0x038, TBUF0 0x080) lies in 0x000..0x1FF; the sibling
    // channel U0C1 (0x4003_0200) and the SCU/IOCR peripherals are OUTSIDE it.
    constexpr uint32_t U0C0_WINDOW = 0x200u;

    // Per-channel offsets (RM Table 18-20 "USIC Kernel-Related and Kernel Registers").
    constexpr uintptr_t TCSR_OFFSET = 0x038u;  // Transmit Control/Status
    constexpr uintptr_t TBUF0_OFFSET = 0x080u; // Transmit Buffer input location 0

    // TCSR.TDV (RM p.18-189): the transmit buffer still holds a word pending
    // transfer -> NOT ready to accept the next byte. Same poll bit the kernel
    // polled writer uses (usic.cc tx_ready: (TCSR & TDV) == 0).
    constexpr uint32_t TCSR_TDV = 1u << 7;

    // Bounded so a mis-configured baud/enable never HANGS the driver thread on a
    // single byte (which would wedge every stdout client parked on send). Far
    // exceeds any real per-byte wait at 115200 baud; on timeout the byte is dropped
    // and the loop continues (mirrors the kernel writer's give-up-don't-hang policy).
    constexpr uint32_t TX_POLL_TIMEOUT = 1000000u;

    inline volatile uint32_t& r32(uintptr_t a)
    {
        return *reinterpret_cast<volatile uint32_t*>(a);
    }

    // Poll TCSR.TDV clear, then write one byte to TBUF0. Returns false on timeout
    // (byte dropped) so the caller keeps making progress rather than hanging.
    bool poll_put(uintptr_t win, uint8_t v)
    {
        for (uint32_t i = 0; i < TX_POLL_TIMEOUT; i++)
        {
            if ((r32(win + TCSR_OFFSET) & TCSR_TDV) == 0u)
            {
                r32(win + TBUF0_OFFSET) = v;
                return true;
            }
        }
        return false;
    }

    // Direct-to-window diagnostic (NOT stdio, NOT the endpoint): exercises the exact
    // poll+TBUF path so first-light on silicon is visible before any endpoint traffic.
    void win_puts(uintptr_t win, char const* s)
    {
        for (; *s != '\0'; s++)
        {
            (void)poll_put(win, static_cast<uint8_t>(*s));
        }
    }
}

extern "C"
{

void xmcuart_console_driver(void* arg)
{
    uintptr_t const win = reinterpret_cast<uintptr_t>(arg); // U0C0 window base

    // First-light banner straight to the granted window (proves the window map +
    // poll/TBUF path independent of the endpoint). NOT libc stdio.
    win_puts(win, "[xmcuart] driver up (polled TX)\n");

    int const ep = 1; // delegated recv cap lands at child table index 1
    char buf[KOS_EP_MSG_MAX];
    for (;;)
    {
        uint32_t badge = 0;
        long const n = kos_recv(ep, buf, sizeof(buf), &badge);
        if (n < 0)
        {
            // Endpoint dead / EPIPE (root closed the last non-driver recv holder and
            // the object tore down, or a bad cap): unrecoverable -- exit and let root
            // respawn + re-publish (D8). Do NOT diagnose via stdio here.
            break;
        }
        for (long i = 0; i < n; i++)
        {
            (void)poll_put(win, static_cast<uint8_t>(buf[i]));
        }
    }

    kos_exit(0);
}

int xmcuart_console_start(uint8_t driver_prio)
{
    // 1. Create the console endpoint E (full rights: WAIT|SIGNAL|TRANSFER).
    int const ep = kos_endpoint_create();
    if (ep < 0)
    {
        kos::print("[xmcuart] ERROR: endpoint_create failed\n");
        return -1;
    }

    // 2. Relinquish the kernel UART and route stdout to E (privileged syscall 29).
    //    On return the kernel chip path is dark and any stale chip writer has drained
    //    (B1), so the UART is safe for the driver to take.
    if (kos_console_publish(ep) != 0)
    {
        kos::print("[xmcuart] ERROR: console_publish failed\n");
        kos_handle_close(ep);
        return -1;
    }

    // 3. Spawn the UNPRIVILEGED driver: granted the U0C0 window (R|W|DEV) and a
    //    narrowed {E | WAIT} recv cap (lands at the child's table index 1). No
    //    SIGNAL/TRANSFER on the child cap: the driver receives, it does not send or
    //    re-delegate. driver_prio must be >= every client (D9: rendezvous has no PI).
    kos_cap_grant const caps[1] = {
        { /*source_cap=*/ep, /*rights_mask=*/KOS_CAP_WAIT },
    };
    int const drv = kos::thread::spawn(
        xmcuart_console_driver, reinterpret_cast<void*>(U0C0_BASE), "xmcuart",
        driver_prio, KOS_POLICY_FIFO, /*quantum_ns=*/0, /*privileged=*/false,
        /*mem=*/nullptr, /*mem_size=*/0, /*stack=*/nullptr, /*stack_size=*/0,
        /*mmio=*/reinterpret_cast<void*>(U0C0_BASE), U0C0_WINDOW,
        caps, /*cap_count=*/1);
    if (drv < 0)
    {
        // Publish already flipped USER_OWNED; the console is dark until a driver
        // exists. Report and fail -- the caller MUST NOT spawn console-dependent
        // apps after this (S6). RTT path only (kos::print), never stdio.
        kos::print("[xmcuart] ERROR: driver spawn failed\n");
        kos_handle_close(ep);
        return -1;
    }

    // 4. Close root's OWN WAIT-bearing cap on E immediately (S4). At spawn
    //    recv_holders == 2 (root + driver); dropping root's copy leaves the driver as
    //    the sole receiver, so the driver's eventual death drops recv_holders to 0 and
    //    EPIPE-wakes parked senders. Keeping it would hang clients on driver death.
    //    g_stdout_target survives on the kernel's own ref (S3), so closing here does
    //    not tear the endpoint down.
    kos_handle_close(ep);
    return 0;
}

}
