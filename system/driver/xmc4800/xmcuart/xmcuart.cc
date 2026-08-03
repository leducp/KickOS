// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// XMC4800 userspace polled UART TX console driver (see <kickos/driver/xmcuart.h>).
// An UNPRIVILEGED thread owns the granted USIC0 CH0 (U0C0) register window and
// serves a console endpoint: kos_recv() a byte batch, then for each byte poll the
// transmit-buffer status (TCSR.TDV) until the buffer is free and write TBUF0. TDV set
// means the transmit buffer still holds a word pending transfer, so loading the next
// byte before it clears overwrites that word and drops or garbles a byte on the wire.
//
// The driver does NOT touch clock/pins/baud: the kernel's kickos_xmc_usic_init()
// configured them at boot and console_tx_deinit() left the channel ASC-mode,
// pinned, and TX-capable in a polled state. Only registers INSIDE the granted
// window (TCSR 0x038, TBUF0 0x080) are poked. SCU_CGATCLR0/PRCLR0 (clock) and
// P1_IOCR4 (pin mux) live in separate privileged peripherals outside the window,
// unreachable and left intact.
//
// HARD RULE (design D7): NO libc stdio. printf/puts route through _write ->
// kos_send(0, ..) -> this driver's own endpoint, a self-send that deadlocks because
// the driver holds the sole CAP_WAIT recv cap, so recv_holders never reaches 0 and
// no EPIPE fires. Diagnostics go direct to the USIC window (poll_put below) or via
// kos::print, which does NOT route through the endpoint.
//
// Register addresses / bit fields are clean-room from the XMC4700/XMC4800 Reference
// Manual (V1.3, 2016-07); no XMCLib/DAVE/CMSIS vendor source.

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/driver/xmcuart.h>

#include <kickos/sys/service.h>        // kos_service_cfg (base/window/prio as data)
#include <kickos/sys/driver_bringup.h> // kickos::driver::spawn_unprivileged
#include <kickos/io/mmio.h>            // r32

#include <usic_class.h> // Rule 6 class-driver leaf: shared USIC transmit-ready read

#include <regs/usic.h> // shared USIC register offsets (off::TBUF0)

#include <stdint.h>

namespace
{
    // On ARMv7-M PMSA the granted DEV window IS a genuine per-thread capability, so a 512 B
    // (0x200) window at the 0x200-aligned channel base is one exact-cover descriptor;
    // the sibling channel U0C1 (base + 0x200) and the SCU/IOCR peripherals stay outside.

    using namespace kickos::xmc::reg::usic;

    // Bounded so a mis-configured baud/enable never HANGS the driver thread on a
    // single byte (which would wedge every stdout client parked on send). Far
    // exceeds any real per-byte wait at 115200 baud; on timeout the byte is dropped
    // and the loop continues (mirrors the kernel writer's give-up-don't-hang policy).
    constexpr uint32_t TX_POLL_TIMEOUT = 1000000u;

    bool poll_put(uintptr_t win, uint8_t v)
    {
        for (uint32_t i = 0; i < TX_POLL_TIMEOUT; i++)
        {
            if (kickos::xmc::driver::usic_tx_ready(win))
            {
                r32(win + off::TBUF0) = v;
                return true;
            }
        }
        return false;
    }

    // Direct-to-window diagnostic, not stdio and not the endpoint.
    void win_puts(uintptr_t win, char const* s)
    {
        for (; *s != '\0'; s++)
        {
            (void)poll_put(win, static_cast<uint8_t>(*s));
        }
    }

    // Query the branch clock feeding U0C0 and report it through kos::print, which bypasses
    // the endpoint (D7). Reporting only: the driver does not touch baud.
    void print_periph_clock(uintptr_t win)
    {
        uint32_t const hz = kos_periph_clock_hz(win);
        char buf[16];
        size_t i = sizeof(buf);
        buf[--i] = '\0';
        uint32_t v = hz;
        do
        {
            buf[--i] = static_cast<char>('0' + (v % 10u));
            v /= 10u;
        } while (v != 0u and i != 0);
        kos::print("[xmcuart] U0C0 branch clock (Hz): ");
        kos::print(&buf[i]);
        kos::print("\n");
    }
}

extern "C"
{

void xmcuart_console_driver(void* arg)
{
    uintptr_t const win = reinterpret_cast<uintptr_t>(arg); // U0C0 window base

    print_periph_clock(win);

    win_puts(win, "[xmcuart] driver up (polled TX)\n");

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

int xmcuart_console_start(struct kos_service_cfg const* cfg)
{
    uintptr_t const win_base = cfg->mmio_base;
    uint32_t const win_size = cfg->mmio_window;
    uint8_t const driver_prio = cfg->prio;

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
    //    On failure the helper closes ep FIRST, which reclaims the console, and only
    //    then reports, so the tag reaches the wire.
    int const drv = kickos::driver::spawn_unprivileged(
        xmcuart_console_driver, win_base, win_size, cfg->name, driver_prio, ep,
        "[xmcuart] ERROR: driver spawn failed\n");
    if (drv < 0)
    {
        return -1;
    }

    // 4. Close root's OWN WAIT-bearing cap on E (S4), then PROVE the driver is serving
    //    before returning: a zero-length rendezvous on cap 0 returns only once the
    //    driver has received it, which closes the dark window between the publish and
    //    the driver serving. g_stdout_target survives on the kernel's own ref (S3), so
    //    the close does not tear the endpoint down.
    return kickos::driver::console_handover_finish(
        ep, "[xmcuart] ERROR: driver died during bring-up\n");
}

}
