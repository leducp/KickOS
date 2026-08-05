// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// K64F/UART0 userspace polled UART TX console driver (see <kickos/driver/k64uart.h>).
// An UNPRIVILEGED thread owns the granted UART0 register window and serves a console
// endpoint: kos_recv() a byte batch, then for each byte poll S1.TDRE until the data
// register is free and write UART0_D. S1.TDRE clear means the data register still holds
// a byte the shifter has not taken, so loading the next byte before it sets drops one on
// the wire.
//
// The driver touches no SIM or PORT register itself: the kernel's uart0_init()
// configured clock and pins at boot and left the UART TX-capable in a polled state,
// and kos_periph_enable() covers the clock gate plus the bus-side supervisor-protect.
// Every other access is INSIDE the granted window (S1 0x04, D 0x07, and BDH/BDL/C4
// for the driver-owned divisor).
//
// K64F is BYTE-mapped for the UART: every UART access below is an 8-bit load/store
// (a 32-bit access at base+4 spans S1/S2/C3/D and reading D pops the RX FIFO).
//
// HARD RULE (design D7): NO libc stdio. printf/puts route through _write ->
// kos_send(0, ..) -> this driver's own endpoint, a self-send that deadlocks because
// the driver holds the sole CAP_WAIT recv cap, so recv_holders never reaches 0.
// Diagnostics go direct to the UART0 window (win_puts below) or via kos::print, which
// bypasses the endpoint.
//
// Register addresses / bit fields are from the K64 Sub-Family Reference Manual
// (K64P144M120SF5RM); consistent with the chip layer's clean-room regs.

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/driver/k64uart.h>

#include <kickos/sys/service.h>        // kos_service_cfg (base/window/prio as data)
#include <kickos/sys/driver_bringup.h> // kickos::driver::spawn_unprivileged
#include <kickos/io/mmio.h>            // r8

#include <uart_class.h> // Rule 6 class-driver leaf: shared UART transmit-ready read

#include <stdint.h>

namespace
{
    // NOTE (coarse-AIPS): the DEV window grant is INERT for the peripheral. AIPS
    // bridges are not SYSMPU slave ports (RM 3.3.6.2), so UART0 is reachable by any
    // unprivileged thread once the AIPS PACR is open (kos_periph_enable below). The
    // grant is still what AUTHORISES that call (possession of the exact base), and it
    // keeps the spawn signature portable with the PMSA/PMP template.

    constexpr uintptr_t D_OFFSET = 0x07u; // UART Data Register (RM 52.3.11)

    // Baud-divisor registers (RM 52.3): BDH/BDL hold SBR[12:0], C4 the BRFA 1/32
    // fine-adjust. All three lie inside the granted 0x20 window.
    constexpr uintptr_t BDH_OFFSET = 0x00u;
    constexpr uintptr_t BDL_OFFSET = 0x01u;
    constexpr uintptr_t C4_OFFSET = 0x0Au;

    // Bounded so a mis-configured baud/enable never HANGS the driver thread on a single
    // byte (which would wedge every stdout client parked on send). Far exceeds any real
    // per-byte wait at 115200 baud; on timeout the byte is dropped and the loop
    // continues (mirrors the kernel writer's give-up-don't-hang policy).
    constexpr uint32_t TX_POLL_TIMEOUT = 1000000u;

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

    // Ask the kernel for UART0's branch clock and re-derive SBR/BRFA for 115200 from it,
    // then write BDH/BDL/C4 inside the already-granted window. On a 0 return the oracle
    // does not know this block, so the kernel-programmed baud stands. Same K64 formula the
    // chip layer's uart0_init uses.
    void rederive_baud(uintptr_t win)
    {
        uint32_t const clk = kos_periph_clock_hz(win);
        if (clk == 0u)
        {
            return;
        }
        uint32_t const baud = 115200u;
        uint32_t const sbr = clk / (16u * baud);
        uint32_t const brfa = (clk * 2u) / baud - sbr * 32u;
        r8(win + BDH_OFFSET) = static_cast<uint8_t>((sbr >> 8) & 0x1F);
        r8(win + BDL_OFFSET) = static_cast<uint8_t>(sbr & 0xFF);
        r8(win + C4_OFFSET) = static_cast<uint8_t>(brfa & 0x1F);
    }

    // Direct-to-window diagnostic, not stdio and not the endpoint.
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

    // Open UART0's AIPS0 slot 106 SP bit (RM 20.2.3) through the grant this thread holds.
    // Until it returns, every register access below is supervisor-only and faults.
    // SCGC4_UART0 is already set by uart0_init, so the clock half is a no-op here.
    //
    // NO TRANSPORT SURVIVES THIS FAILURE on the chip backend: the start published before
    // spawning, so console_emit's USER_OWNED arm drops the line below and it reaches the
    // wire only on a build that also carries RTT. kickos::emit is NOT the remedy it is for
    // every other driver, because this thread's stdout cap (index 0) is the console
    // endpoint it was spawned to SERVE, so its send parks on an endpoint with no receiver.
    // The window is supervisor-only at this point, so win_puts cannot report either.
    if (kos_periph_enable(win) != 0)
    {
        kos::print("[k64uart] ERROR: periph_enable failed, UART0 unreachable\n");
        kos_exit(-1);
    }

    rederive_baud(win);

    win_puts(win, "[k64uart] driver up (polled TX)\n");

    int const ep = KOS_SPAWN_DELEGATED_CAP0; // delegated recv cap
    char buf[KOS_EP_MSG_MAX];
    while (true)
    {
        // Info-less recv: the console hosts plain sends only, so a client kos_call
        // bounces cleanly (-KOS_ENOSYS) instead of minting a reply cap here.
        long const n = kos_recv(ep, buf, sizeof(buf), nullptr);
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

int k64uart_console_start(struct kos_service_cfg const* cfg)
{
    uintptr_t const win_base = cfg->mmio_base;
    uint32_t const win_size = cfg->mmio_window;
    uint8_t const driver_prio = cfg->prio;

    kos_cap_t ep = KOS_CAP_NONE;
    if (kos_endpoint_create(&ep) != 0)
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

    // No PFIFO poke here: PFIFO resets to 0 (FIFOs off, single-datum mode), uart0_init
    // never sets it, and the reclaim path re-forces it. It is also unwritable while C2
    // TE/RE are set anyway. So there is nothing to disable for the polled path.

    // 3. Spawn the UNPRIVILEGED driver: granted the UART0 window (R|W|DEV; SYSMPU does not
    //    gate it, but possession is the sole authorisation for the kos_periph_enable above,
    //    so removing the grant kills the console) and a narrowed {E | WAIT}
    //    recv cap (lands at the child's table index 1). No SIGNAL/TRANSFER on the child
    //    cap: the driver receives, it does not send or re-delegate. driver_prio must be
    //    >= every client (D9: rendezvous has no PI).
    //    On failure the helper closes ep FIRST, which reclaims the console, and only
    //    then reports, so the tag reaches the wire.
    auto const drv = kickos::driver::spawn_unprivileged(
        k64uart_console_driver, win_base, win_size, cfg->name, driver_prio, ep,
        "[k64uart] ERROR: driver spawn failed\n");
    if (not drv.valid())
    {
        return -1;
    }

    // 4. Close root's OWN WAIT-bearing cap on E (S4), then PROVE the driver is serving
    //    before returning: a zero-length rendezvous on cap 0 returns only once the driver
    //    has received it, which closes the dark window between the publish and the driver
    //    serving. g_stdout_target survives on the kernel's own ref (S3).
    return kickos::driver::console_handover_finish(
        ep, "[k64uart] ERROR: driver died during bring-up\n");
}

}
