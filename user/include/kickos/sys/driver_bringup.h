// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Shared tail of every unprivileged-driver bring-up: spawn the driver thread with
// its granted MMIO window (passed as both the arg VALUE and the grant), a WAIT-only
// recv cap on the service endpoint (child table index 1), and optionally a tier-1
// IRQ line cap the caller has already claimed (child index 2). On a spawn failure the
// helper closes the endpoint and prints the tag; the caller keeps its own return and its
// success-path endpoint handling (a console closes its parent cap, an SPI service
// keeps it). No register access lives here (the privileged bring-up stays per-class).
//
// The line cap is claimed by the CALLER, never by the driver: minting needs AUTH_IRQ
// and a driver runs at authority 0 (design-m4.6-irq-driver.md section 3).

#ifndef KICKOS_SYS_DRIVER_BRINGUP_H
#define KICKOS_SYS_DRIVER_BRINGUP_H

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/cap_index.h> // KOS_CAP_STDOUT (the handover probe rides cap 0)

#include <stdint.h>

// Child cap indices this helper produces. A driver NAMES them, it does not choose them.
// Not in <cap_index.h>: these are delegation positions, not the frozen well-known slots.
enum
{
    KOS_DRIVER_CAP_EP = KOS_SPAWN_DELEGATED_CAP0,     // the service endpoint (WAIT)
    KOS_DRIVER_CAP_IRQ = KOS_SPAWN_DELEGATED_CAP0 + 1 // the tier-1 line, when delegated
};

namespace kickos
{
namespace driver
{

inline kos::thread::Handle spawn_unprivileged(void (*entry)(void*), uintptr_t win_base,
                                              uint32_t win_size, char const* name, uint8_t prio,
                                              kos_cap_t ep, char const* fail_tag,
                                              kos_cap_t irq_cap = KOS_CAP_NONE)
{
    kos_cap_grant const caps[2] = {
        { /*source_cap=*/ep, /*rights_mask=*/KOS_CAP_WAIT },
        { /*source_cap=*/irq_cap, /*rights_mask=*/KOS_CAP_WAIT },
    };
    uint8_t cap_count = 1;
    if (irq_cap != KOS_CAP_NONE)
    {
        cap_count = 2;
    }
    kos::thread::Handle const drv = kos::thread::spawn(
        entry, reinterpret_cast<void*>(win_base), name,
        prio, KOS_POLICY_FIFO, /*quantum_ns=*/0, /*privileged=*/false,
        /*mem=*/nullptr, /*mem_size=*/0, /*stack=*/nullptr, /*stack_size=*/0,
        /*mmio=*/reinterpret_cast<void*>(win_base), win_size,
        caps, cap_count);
    if (not drv.valid())
    {
        // CLOSE BEFORE PRINTING on a console service: the publish already flipped the
        // console to USER_OWNED, so a kernel-console write here is DROPPED. Closing takes
        // the endpoint's last receiver holder to zero, which reclaims the console
        // (kernel/init/console.cc), so the tag below reaches the wire.
        kos_handle_close(ep);
        kos::print(fail_tag);
    }
    return drv;
}

// The LAST two steps of a console handover, in this order because either order wrong
// fails silently. Closes the caller's own WAIT-bearing cap on E, then PROVES the driver
// is serving before any console client runs. Returns 0, or the probe's negative rc.
//
// Closing FIRST leaves the driver as the SOLE receiver, so its death takes recv_holders
// to 0, which is what both EPIPEs the probe below AND reclaims the console. Probing while
// the caller still held a WAIT cap would keep recv_holders at 1, so a dead driver would
// neither wake this send nor give the console back: a hang on a dark console. (Closing is
// also S4: it is what makes a later driver death EPIPE-wake parked clients at all.)
//
// The probe is a ZERO-LENGTH rendezvous on cap 0, the same route every client uses. A
// rendezvous send returns only once a receiver has actually taken it, so on success the
// DARK WINDOW between the publish and the driver serving is closed by the time this
// returns. A driver that hangs in bring-up instead of dying parks this send indefinitely;
// per-chip bring-up polls are bounded by KICKOS_POLL_SPIN_MAX.
//
// `irq_thread` is MANDATORY for a two-thread driver: the console comes back only once the
// register window is free, and on a probe failure the service thread is the one that died,
// so the IRQ thread is still holding it. Without the cancel the tag below prints into a
// console nothing has given back. It relies on the driver threads sitting ABOVE root, so
// the cancelled thread runs to its exit before this call returns. A default-constructed
// Handle means a single-thread driver, which released the window at its own death.
inline int console_handover_finish(kos_cap_t ep, char const* fail_tag,
                                   kos::thread::Handle irq_thread = {})
{
    kos_handle_close(ep);
    int const rc = kos_send(KOS_CAP_STDOUT, "", 0);
    if (rc < 0)
    {
        if (irq_thread.valid())
        {
            (void)irq_thread.kill();
        }
        kos::print(fail_tag);
        return rc;
    }
    return 0;
}

} // namespace driver
} // namespace kickos

#endif
