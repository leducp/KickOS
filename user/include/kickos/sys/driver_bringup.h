// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Shared tail of every unprivileged-driver bring-up: spawn the driver thread with
// its granted MMIO window (passed as both the arg VALUE and the grant) and a
// WAIT-only recv cap on the service endpoint (child table index 1). The 14-arg
// kos::thread::spawn shape is identical across all driver classes; only the entry,
// window, name, priority and the failure tag vary. On a spawn failure the helper
// prints the tag and closes the endpoint; the caller keeps its own return and its
// success-path endpoint handling (a console closes its parent cap, an SPI service
// keeps it). No register access lives here (the privileged bring-up stays per-class).

#ifndef KICKOS_SYS_DRIVER_BRINGUP_H
#define KICKOS_SYS_DRIVER_BRINGUP_H

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <stdint.h>

namespace kickos
{
namespace driver
{

inline int spawn_unprivileged(void (*entry)(void*), uintptr_t win_base, uint32_t win_size,
                              char const* name, uint8_t prio, int ep, char const* fail_tag)
{
    kos_cap_grant const caps[1] = {
        { /*source_cap=*/ep, /*rights_mask=*/KOS_CAP_WAIT },
    };
    int const drv = kos::thread::spawn(
        entry, reinterpret_cast<void*>(win_base), name,
        prio, KOS_POLICY_FIFO, /*quantum_ns=*/0, /*privileged=*/false,
        /*mem=*/nullptr, /*mem_size=*/0, /*stack=*/nullptr, /*stack_size=*/0,
        /*mmio=*/reinterpret_cast<void*>(win_base), win_size,
        caps, /*cap_count=*/1);
    if (drv < 0)
    {
        kos::print(fail_tag);
        kos_handle_close(ep);
    }
    return drv;
}

} // namespace driver
} // namespace kickos

#endif
