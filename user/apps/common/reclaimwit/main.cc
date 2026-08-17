// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Board-agnostic witness that arch_console_reclaim gives the console back when a published
// console driver is slain, and (KICKOS_RW_MODE 1) that kickos_terminate drains the console
// before the core stops.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/cap_index.h>
#include <kickos/sys/emit.h>
#include <kickos/sys/errno.h>
#include <kickos/sys/init.h>
#include <kickos/libc/fmt.h>

#include <stdint.h>

#ifndef KICKOS_RW_MODE
#error "KICKOS_RW_MODE must be set by this app's CMakeLists"
#endif
#ifndef KICKOS_RW_RTT
#define KICKOS_RW_RTT 0
#endif

using kickos::emit;

namespace
{
    // At or above every stdout client's priority: the console rendezvous carries no
    // priority inheritance.
    constexpr uint8_t DRIVER_PRIO = 12;

    // -KOS_ETIMEDOUT means condemned but not yet swept, so a timeout costs the sweep's
    // timing, not the death.
    constexpr uint32_t SLAY_TIMEOUT_US = 5000000u;

    // The published console driver. It must stay a pure sink: one console write here
    // destroys what separates a fired reclaim from a driver that never died.
    void console_sink(void*)
    {
        uint8_t buf[KOS_EP_MSG_MAX];
        while (true)
        {
            int32_t const n = kos_recv(KOS_SPAWN_DELEGATED_CAP0, buf, sizeof(buf), nullptr);
            if (n < 0)
            {
                break;
            }
        }
        kos_exit(0);
    }

    __attribute__((noreturn)) void park_forever(void)
    {
        // Sleep fallback for an unmintable semaphore, which would otherwise spin failing
        // syscalls.
        kos_cap_t idle = KOS_CAP_NONE;
        (void)kos_sem_create(0, &idle);
        while (true)
        {
            if (idle == KOS_CAP_NONE)
            {
                kos_sleep_ns(1000000000ull);
                continue;
            }
            (void)kos_sem_wait(idle);
        }
    }

    void print_rc(char const* what, int rc)
    {
        char line[96];
        ksnprintf(line, sizeof(line), "[reclaimwit]   %s rc=%d\n", what, rc);
        kos::print(line);
    }

    void print_reading_key(void)
    {
        kos::print("[reclaimwit] console reclaim + terminate drain witness\n");
        kos::print("[reclaimwit] HOW TO READ THIS CAPTURE:\n");
        kos::print("[reclaimwit]  1. this block is on the wire, so the kernel owns the console\n");
        kos::print("[reclaimwit]  2. the app now publishes the console to a driver it spawns\n");
        kos::print("[reclaimwit]  3. that driver never writes to a console, so the wire must go\n");
        kos::print("[reclaimwit]     silent: a MUTE line below must occur ZERO times\n");
        kos::print("[reclaimwit]  4. the app then SLAYS the driver and prints a LIVE line with\n");
        kos::print("[reclaimwit]     the same kos_print call the MUTE line used\n");
        kos::print("[reclaimwit]  5. LIVE present + MUTE absent == arch_console_reclaim fired.\n");
        kos::print("[reclaimwit]     MUTE present == the publish never took, verdict void.\n");
        kos::print("[reclaimwit]     both absent == the reclaim did not fire, console still dark.\n");
#if KICKOS_RW_RTT
        // kconsole_write feeds RTT in every ownership state, so the MUTE line reaches an
        // RTT viewer even on a correct run.
        kos::print("[reclaimwit] NOTE: this image also carries RTT. Read the CHIP UART capture;\n");
        kos::print("[reclaimwit] NOTE: the RTT stream carries kernel writes in every state.\n");
#endif
    }

    __attribute__((noreturn)) void refuse_published_console(void)
    {
        // emit() reaches the published driver through cap 0; kos_print would be dropped.
        emit("[reclaimwit] REFUSE: this image already publishes a userspace console.\n");
        emit("[reclaimwit] REFUSE: the witness needs to kill the console's driver, and here\n");
        emit("[reclaimwit] REFUSE: that driver carries the terminal you are reading.\n");
        emit("[reclaimwit] REFUSE: on a USB CDC console the kernel reclaims a DIFFERENT device\n");
        emit("[reclaimwit] REFUSE: (arch_console_reclaim_window reports the chip UART), so the\n");
        emit("[reclaimwit] REFUSE: post-death bytes would land on a wire nobody is watching.\n");
        emit("[reclaimwit] REFUSE: rebuild with -DKICKOS_SERVICE_LIST=kickos_services_none.\n");
        park_forever();
    }
}

// KOS_AUTH_CONSOLE for kos_console_publish; KOS_AUTH_SYSTEM only on the drain arm, whose
// main returns and takes root through kos_shutdown.
#if KICKOS_RW_MODE == 1
KICKOS_APP_AUTHORITY(KOS_AUTH_MEMORY | KOS_AUTH_SYSTEM | KOS_AUTH_CONSOLE);
#else
KICKOS_APP_AUTHORITY(KOS_AUTH_MEMORY | KOS_AUTH_CONSOLE);
#endif

int main(int, char**)
{
    // Root's slot 0 stays empty until something publishes, so -KOS_EBADF is the only
    // answer an unpublished image can give. A zero-length send is the flush idiom, so this
    // puts no byte on a live console's wire.
    char const probe = '\0';
    int32_t const seated = kos_send(KOS_CAP_STDOUT, &probe, 0);
    if (seated != -KOS_EBADF)
    {
        refuse_published_console();
    }

    print_reading_key();

    kos_cap_t ep = KOS_CAP_NONE;
    int const ep_rc = kos_endpoint_create(&ep);
    if (ep_rc != 0)
    {
        print_rc("FAIL endpoint_create", ep_rc);
        park_forever();
    }

    // Spawned before the publish so a refused spawn still reports on a kernel-owned
    // console. Unprivileged: kos_thread_slay refuses a privileged target.
    kos_cap_grant const caps[1] = {{ep, KOS_CAP_WAIT}};
    auto const drv = kos::thread::spawn_caps(console_sink, nullptr, "rwdrv", DRIVER_PRIO,
                                             caps, /*cap_count=*/1);
    if (not drv.valid())
    {
        print_rc("FAIL driver spawn", drv.error());
        park_forever();
    }

    int const pub_rc = kos_console_publish(ep);
    if (pub_rc != 0)
    {
        print_rc("FAIL console_publish", pub_rc);
        park_forever();
    }

    // Root's own WAIT-bearing cap must go, or the driver's death leaves recv_holders at 1
    // and no death is noted. The kernel's stdout ref keeps the endpoint alive.
    int const close_rc = kos_handle_close(ep);

    // The same kos_print call as the LIVE line below. Its absence from the capture is the
    // assertion that the publish took.
    kos::print("[reclaimwit] MUTE kernel console while the driver holds it\n");

    // Returns only once the driver has TAKEN the bytes, so the published route is served
    // and not merely created.
    char const served[] = "[reclaimwit] routed through the driver, which discards it\n";
    int32_t const serve_rc = kos_send(KOS_CAP_STDOUT, served, sizeof(served) - 1u);

    // Forcible, not cooperative: the driver never gets the window in which it would have
    // quieted its device. 0 means EXITED and swept, so the reclaim has already been attempted.
    int const slay_rc = drv.slay(SLAY_TIMEOUT_US);

    // recv_holders is 0, so this either refuses at once or parks and is released by the
    // sweep with the same code.
    char const dead = 'x';
    int32_t const epipe_rc = kos_send(KOS_CAP_STDOUT, &dead, 1);

    kos::print("[reclaimwit] LIVE kernel console after the driver died\n");
    print_rc("handle_close", close_rc);
    print_rc("driver serve bytes", serve_rc);
    print_rc("slay", slay_rc);
    print_rc("post-death send (want -KOS_EPIPE)", epipe_rc);

    bool const ok = (close_rc == 0 and serve_rc == static_cast<int32_t>(sizeof(served) - 1u)
                     and slay_rc == 0 and epipe_rc == -KOS_EPIPE);
    if (ok)
    {
        kos::print("[reclaimwit] PASS reclaim fired: driver slain, endpoint EPIPE, wire back\n");
    }
    else
    {
        kos::print("[reclaimwit] FAIL see the rc lines above\n");
    }

#if KICKOS_RW_MODE == 1
    kos::print("[reclaimwit] drain arm: kickos_terminate follows. The ring is disarmed in\n");
    kos::print("[reclaimwit] RECLAIMED, so arch_console_flush_sync alone holds the core until\n");
    kos::print("[reclaimwit] the shift register empties. The capture must END with the next\n");
    kos::print("[reclaimwit] line INTACT, sentinel included; a short tail is a drain that\n");
    kos::print("[reclaimwit] did not complete before arch_shutdown stopped the core.\n");
    kos::print("[reclaimwit] DRAINTAIL 0123456789abcdef0123456789abcdef <<<DRAIN-END>>>\n");
    if (ok)
    {
        return 0;
    }
    return 1;
#else
    kos::print("[reclaimwit] park arm: the system stays up, nothing further is printed\n");
    park_forever();
#endif
}
