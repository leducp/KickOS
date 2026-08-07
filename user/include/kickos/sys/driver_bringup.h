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

#if KICKOS_TIMED_WAIT
// A driver that hangs in bring-up must not cost the board more than this.
constexpr uint32_t KOS_DRIVER_HANDOVER_PROBE_US = 1000000;
#endif

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
// returns. A driver that HANGS in bring-up instead of dying never EPIPEs the probe: the
// probe is bounded by KOS_DRIVER_HANDOVER_PROBE_US only where KICKOS_TIMED_WAIT is on, and
// without it the probe cannot give up at all, so such a driver parks the handing-over
// thread with nothing to recover it. Per-chip bring-up polls are bounded by
// KICKOS_POLL_SPIN_MAX either way.
//
// The two failures are different failures, and only one of them can be answered here.
//
// -KOS_EPIPE means the service thread DIED: recv_holders reached 0, which is what gave the
// console back, so the tag reaches the wire. `irq_thread` is MANDATORY for a two-thread
// driver, because the console comes back only once the register window is free and the
// dead thread is not the one holding it; the cancel therefore precedes the tag. It relies
// on the driver threads sitting ABOVE root, so the cancelled thread runs to its exit
// before this call returns. A default-constructed Handle means a single-thread driver,
// which released the window at its own death.
//
// Any other refusal, -KOS_ETIMEDOUT above all where the probe is bounded, leaves the
// service thread ALIVE and still the sole receiver: recv_holders never reaches 0, so the
// console is NOT reclaimed and every kernel-console write is dropped for the rest of the
// run. Nothing here recovers from that. kos_thread_kill is cooperative and wakes only a
// thread parked in kos_irq_wait, so it cannot end one hung anywhere else, and cancelling
// the IRQ thread of a driver that may yet start serving would break a merely slow one. The
// code is returned unchanged and no tag is printed: a print into a console nobody gave
// back is a report that was never made, and the caller must treat the code as the whole
// outcome.
inline int console_handover_finish(kos_cap_t ep, char const* fail_tag,
                                   kos::thread::Handle irq_thread = {})
{
    kos_handle_close(ep);
#if KICKOS_TIMED_WAIT
    int const rc = kos_send_timed(KOS_CAP_STDOUT, "", 0, KOS_DRIVER_HANDOVER_PROBE_US);
#else
    int const rc = kos_send(KOS_CAP_STDOUT, "", 0);
#endif
    if (rc >= 0)
    {
        return 0;
    }
    if (rc != -KOS_EPIPE)
    {
        return rc;
    }
    if (irq_thread.valid())
    {
        (void)irq_thread.kill();
    }
    kos::print(fail_tag);
    return rc;
}

} // namespace driver
} // namespace kickos

#endif
