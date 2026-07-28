// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Host-sim service-list provider: the sim analogue of the xmc4800-relax / frdmk64f
// lists, so the POSTURE THOSE BOARDS SHIP -- a userspace driver owns the console and
// the kernel chip path is dark -- is reachable on the host, under a debugger, in CI.
// Without it the publish path had no automated coverage at all: every sim and QEMU
// gate runs `kickos_services_none`, where the kernel keeps the UART, cap index 0 is
// unseated and `_write` never touches an endpoint -- so a regression that silences a
// published console (exactly what M4.5 shipped) stays green everywhere.
//
// The driver thread stands in for xmcuart/k64uart: instead of poking a UART window it
// write(2)s to host fd 1. That is the faithful analogue of poking TBUF0 -- it reaches
// the console WITHOUT going through kconsole_write, so bytes appear if and only if
// they travelled the endpoint -> driver route. Sim-only by construction (host libc),
// gated to KICKOS_ARCH == sim in system/CMakeLists.txt.
//
// Selected with -DKICKOS_SERVICE_LIST=kickos_services_sim; the sim default stays
// kickos_services_none so the ordinary sim gate keeps testing the pre-publish route.

#include <kickos/sys/service.h>

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/driver_bringup.h>

#include <stdint.h>

// The host write(2), declared rather than included: this TU is built freestanding
// (kickos_apply_freestanding) and must not pull host headers. fd 1 is "the wire".
extern "C" long write(int, void const*, unsigned long);

namespace
{
    void wire_puts(char const* s)
    {
        unsigned long n = 0;
        while (s[n] != '\0')
        {
            n++;
        }
        (void)write(1, s, n);
    }
}

extern "C"
{
    // Unprivileged console driver: drain the published endpoint to the "wire" forever.
    // Mirrors xmcuart's shape -- a kos::print diagnostic (which a published board DROPS,
    // by design) followed by a banner written straight to the wire (which survives).
    void simconsole_driver(void*)
    {
        kos::print("[simcon] kos::print diagnostic (kernel console path -- dropped post-publish)\n");
        wire_puts("[simcon] driver up (host fd 1)\n");

        int const ep = KOS_SPAWN_DELEGATED_CAP0; // delegated {E | WAIT} recv cap
        char buf[KOS_EP_MSG_MAX];
        while (true)
        {
            long const n = kos_recv(ep, buf, sizeof(buf), nullptr);
            if (n < 0)
            {
                break; // last SIGNAL holder gone: nothing left to serve
            }
            (void)write(1, buf, static_cast<unsigned long>(n));
        }
        kos_exit(0);
    }

    // Console bring-up choreography, identical in shape to every chip console service:
    // create the endpoint, publish it (which seats the CALLER's cap 0 too, so init and
    // the app print through the driver), spawn the unprivileged driver with a WAIT-only
    // cap, then drop the parent's cap so the driver is the sole receiver.
    static int simconsole_start(struct kos_service_cfg const* cfg)
    {
        if (cfg == nullptr or cfg->kind != KOS_SVC_CONSOLE)
        {
            return -1; // cfg authored for another service class
        }
        int const ep = kos_endpoint_create();
        if (ep < 0)
        {
            return ep;
        }
        int const pub = kos_console_publish(ep);
        if (pub != 0)
        {
            kos_handle_close(ep);
            return pub;
        }
        // No MMIO window: the sim's "device" is fd 1, so there is nothing to grant.
        int const drv = kickos::driver::spawn_unprivileged(
            simconsole_driver, /*win_base=*/0, /*win_size=*/0, cfg->name, cfg->prio, ep,
            "[simcon] ERROR: driver spawn failed\n");
        if (drv < 0)
        {
            return drv; // the helper already closed ep
        }
        kos_handle_close(ep); // the driver holds the only WAIT cap from here
        return 0;
    }

    // prio 12 matches the silicon console services: it must sit at or above every
    // stdout client's priority (there is no PI on the console rendezvous).
    static struct kos_service_cfg const simcon_cfg = {
        /*name=*/"simcon", /*mmio_base=*/0, /*mmio_window=*/0,
        /*hz=*/0, /*addr=*/0, /*prio=*/12, /*kind=*/KOS_SVC_CONSOLE,
        /*cs_policy=*/KOS_SVC_CS_NONE, /*cs_index=*/0, /*rsv=*/{ 0, 0 }
    };

    static struct kos_service_bringup const sim_services[] = {
        { simconsole_start, &simcon_cfg },
    };
    struct kos_service_list const kickos_board_services = { sim_services, 1 };
}
