// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// K64F/UART0 userspace polled UART TX console driver (see <kickos/driver/k64uart.h>).
// An UNPRIVILEGED thread owns the granted UART0 register window and serves a console
// endpoint: kos_recv() a byte batch, then for each byte poll S1.TDRE until the data
// register is free and write UART0_D. This mirrors the kernel polled writer
// (arch/arm/chip/mk64f/chip_mk64f.cc arch_console_write_sync): S1.TDRE clear means
// the data register still holds a byte the shifter has not taken; wait for it to set
// before loading the next byte, or a byte is dropped on the wire.
//
// The driver does NOT touch clock/pins/baud: the kernel's uart0_init() configured
// them at boot and left the UART TX-capable in a polled state. Only UART0 data-path
// registers INSIDE the granted window (S1 0x04, D 0x07) are touched. SIM clock gates
// and PORTB pin mux live in separate privileged peripherals outside the window.
//
// K64F is BYTE-mapped for the UART: every UART access below is an 8-bit load/store
// (a 32-bit access at base+4 spans S1/S2/C3/D and reading D pops the RX FIFO). The
// one exception is the AIPS PACR open in the start shim, which is a genuine 32-bit
// register.
//
// HARD RULE (design D7): NO libc stdio. printf/puts route through _write ->
// kos_send(0, ..) -> this driver's own endpoint (a self-send that deadlocks, since
// the driver holds the sole CAP_WAIT recv cap so recv_holders never reaches 0).
// Diagnostics go direct to the UART0 window (win_puts below) or via kos::print
// (kos_kconsole_write -> the RTT / kernel debug path, which bypasses the endpoint).
//
// Register addresses / bit fields are from the K64 Sub-Family Reference Manual
// (K64P144M120SF5RM); consistent with the chip layer's clean-room regs.

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/driver/k64uart.h>

#include <uart_class.h> // Rule 6 class-driver leaf: shared UART transmit-ready read

#include <stdint.h>

namespace
{
    // UART0 register block (RM ch.52; base = AIPS0 slot 106 @ 0x4006_A000). The kernel
    // inited this UART (OpenSDA VCOM on PTB16/17); the driver is granted a 32 B window
    // that covers the low register file it touches.
    constexpr uintptr_t UART0_BASE = 0x4006A000u;

    // 32 B window (base .. base+0x1F), R|W|DEV. SYSMPU-encodable: 32 is the byte-granular
    // minimum and 0x4006_A000 is 32-aligned; also pow2 + aligned so the identical grant
    // encodes on PMSA/PMP. The registers the driver touches (S1 0x04, D 0x07) lie inside.
    // NOTE (coarse-AIPS): this DEV grant is INERT for the peripheral. AIPS bridges are
    // not SYSMPU slave ports (RM 3.3.6.2), so UART0 is reachable by any unprivileged
    // thread once the AIPS PACR is open. The window is kept only for spawn-signature
    // portability with the PMSA/PMP driver template.
    constexpr uint32_t UART0_WINDOW = 0x20u;

    // Byte offsets within the UART0 block (RM ch.52 register map). Only the TX data
    // path is used here; S1 comes from the class leaf.
    constexpr uintptr_t D_OFFSET = 0x07u; // UART Data Register (RM 52.3.11)

    // Bounded so a mis-configured baud/enable never HANGS the driver thread on a single
    // byte (which would wedge every stdout client parked on send). Far exceeds any real
    // per-byte wait at 115200 baud; on timeout the byte is dropped and the loop
    // continues (mirrors the kernel writer's give-up-don't-hang policy).
    constexpr uint32_t TX_POLL_TIMEOUT = 1000000u;

    inline volatile uint8_t& r8(uintptr_t a)
    {
        return *reinterpret_cast<volatile uint8_t*>(a);
    }

    // Poll S1.TDRE set (via the shared leaf), then write one byte to UART0_D. Returns
    // false on timeout (byte dropped) so the caller keeps making progress.
    bool poll_put(uintptr_t win, uint8_t v)
    {
        for (uint32_t i = 0; i < TX_POLL_TIMEOUT; i++)
        {
            if (kickos::mk64f::driver::uart0_tx_ready(win))
            {
                r8(win + D_OFFSET) = v;
                return true;
            }
        }
        return false;
    }

    // Direct-to-window diagnostic (NOT stdio, NOT the endpoint): exercises the exact
    // poll+D path so first-light on silicon is visible before any endpoint traffic.
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

void k64uart_console_driver(void* arg)
{
    uintptr_t const win = reinterpret_cast<uintptr_t>(arg); // UART0 window base

    // First-light banner straight to the window (proves the reachability + poll/D path
    // independent of the endpoint). NOT libc stdio.
    win_puts(win, "[k64uart] driver up (polled TX)\n");

    int const ep = 1; // delegated recv cap lands at child table index 1
    char buf[KOS_EP_MSG_MAX];
    for (;;)
    {
        uint32_t badge = 0;
        long const n = kos_recv(ep, buf, sizeof(buf), &badge);
        if (n < 0)
        {
            // Endpoint dead / EPIPE (root closed the last non-driver recv holder and
            // the object tore down, or a bad cap): unrecoverable. Exit and let root
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

int k64uart_console_start(uint8_t driver_prio)
{
    // 1. Create the console endpoint E (full rights: WAIT|SIGNAL|TRANSFER).
    int const ep = kos_endpoint_create();
    if (ep < 0)
    {
        kos::print("[k64uart] ERROR: endpoint_create failed\n");
        return -1;
    }

    // 2. Relinquish the kernel UART and route stdout to E (privileged syscall 29). On
    //    return the kernel chip path is dark and any stale chip writer has drained
    //    (B1), so the UART is safe for the driver to take.
    if (kos_console_publish(ep) != 0)
    {
        kos::print("[k64uart] ERROR: console_publish failed\n");
        kos_handle_close(ep);
        return -1;
    }

    // 3. Open UART0's AIPS slot to user mode BEFORE the spawn. UART0 @0x4006_A000 is
    //    AIPS0 slot 106 (0x6A); its Supervisor-Protect bit is PACRN[SP2]. RM 4.5.2
    //    peripheral-bridge map (0x4006_A000 -> slot 106) + RM 20.2.3 (PACRN @0x4000_0064
    //    holds PACR104..111; SP0=bit30, SP1=bit26, SP2=bit22, step -4 => slot%8==2 =>
    //    SP2 => bit 22). Clearing it drops UART0 to no-supervisor-required, the ACTUAL
    //    enabler on K64F (the SYSMPU window grant below is inert for the peripheral).
    //    This is a genuine 32-bit register (unlike the byte-wide UART regs), so a single
    //    32-bit RMW inline, NOT the byte helper. ORDER: this must precede the spawn,
    //    because driver_prio >= root and the spawn can preempt straight into the driver's
    //    first-light UART0_D write, which would fault if the slot were still supervisor-only.
    constexpr uintptr_t AIPS0_PACRN = 0x40000064u;
    constexpr uint32_t PACR106_SP = 1u << 22;
    *reinterpret_cast<volatile uint32_t*>(AIPS0_PACRN) &= ~PACR106_SP;

    // No PFIFO poke here: PFIFO resets to 0 (FIFOs off, single-datum mode), uart0_init
    // never sets it, and the reclaim path re-forces it. It is also unwritable while C2
    // TE/RE are set anyway. So there is nothing to disable for the polled path.

    // 4. Spawn the UNPRIVILEGED driver: granted the UART0 window (R|W|DEV, inert on
    //    coarse-AIPS but kept for spawn-signature portability) and a narrowed {E | WAIT}
    //    recv cap (lands at the child's table index 1). No SIGNAL/TRANSFER on the child
    //    cap: the driver receives, it does not send or re-delegate. driver_prio must be
    //    >= every client (D9: rendezvous has no PI).
    kos_cap_grant const caps[1] = {
        { /*source_cap=*/ep, /*rights_mask=*/KOS_CAP_WAIT },
    };
    int const drv = kos::thread::spawn(
        k64uart_console_driver, reinterpret_cast<void*>(UART0_BASE), "k64uart",
        driver_prio, KOS_POLICY_FIFO, /*quantum_ns=*/0, /*privileged=*/false,
        /*mem=*/nullptr, /*mem_size=*/0, /*stack=*/nullptr, /*stack_size=*/0,
        /*mmio=*/reinterpret_cast<void*>(UART0_BASE), UART0_WINDOW,
        caps, /*cap_count=*/1);
    if (drv < 0)
    {
        // Publish already flipped USER_OWNED; the console is dark until a driver exists.
        // Report and fail. The caller MUST NOT spawn console-dependent apps after this
        // (S6). RTT path only (kos::print), never stdio.
        kos::print("[k64uart] ERROR: driver spawn failed\n");
        kos_handle_close(ep);
        return -1;
    }

    // 5. Close root's OWN WAIT-bearing cap on E immediately (S4). At spawn recv_holders
    //    == 2 (root + driver); dropping root's copy leaves the driver as the sole
    //    receiver, so the driver's eventual death drops recv_holders to 0 and EPIPE-wakes
    //    parked senders. g_stdout_target survives on the kernel's own ref (S3).
    kos_handle_close(ep);
    return 0;
}

}
