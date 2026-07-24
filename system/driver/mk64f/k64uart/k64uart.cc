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

#include <kickos/sys/service.h> // kos_service_cfg (base/window/prio as data)
#include <kickos/io/mmio.h>     // r8

#include <uart_class.h> // Rule 6 class-driver leaf: shared UART transmit-ready read

#include <stdint.h>

namespace
{
    // UART0 window base/size now travel in kos_service_cfg (mmio_base/mmio_window),
    // not literals here: the driver takes its register base from the spawn grant.
    // NOTE (coarse-AIPS): the DEV window grant is INERT for the peripheral -- AIPS
    // bridges are not SYSMPU slave ports (RM 3.3.6.2), so UART0 is reachable by any
    // unprivileged thread once the AIPS PACR is open (the start shim below). The
    // window is kept only for spawn-signature portability with the PMSA/PMP template.

    // Byte offsets within the UART0 block (RM ch.52 register map). Only the TX data
    // path is used here; S1 comes from the class leaf.
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

    // Own the baud divisor from the queried branch clock: ask the kernel for UART0's
    // branch clock and re-derive SBR/BRFA for 115200 from it, then write BDH/BDL/C4
    // inside the already-granted window. This is the driver owning its divisor from the
    // queried branch, the pattern the whole coming driver era uses (the kernel no longer
    // hardwires the baud for it). On a 0 (the oracle does not know this block) keep the
    // kernel-programmed baud. Same K64 formula the chip layer's uart0_init uses; UART0 is
    // byte-mapped so r8. The re-derived value equals what the kernel already programmed,
    // so the wire stays legible; the write proves the driver can own the divisor.
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

    // Re-derive the baud from the queried branch clock BEFORE first light, so the
    // banner already rides the driver-owned divisor.
    rederive_baud(win);

    // First-light banner straight to the window (proves the reachability + poll/D path
    // independent of the endpoint). NOT libc stdio.
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
    // Base/window/priority come from the service cfg (data), not literals here.
    uintptr_t const win_base = cfg->mmio_base;
    uint32_t const win_size = cfg->mmio_window;
    uint8_t const driver_prio = cfg->prio;

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
        k64uart_console_driver, reinterpret_cast<void*>(win_base), cfg->name,
        driver_prio, KOS_POLICY_FIFO, /*quantum_ns=*/0, /*privileged=*/false,
        /*mem=*/nullptr, /*mem_size=*/0, /*stack=*/nullptr, /*stack_size=*/0,
        /*mmio=*/reinterpret_cast<void*>(win_base), win_size,
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
