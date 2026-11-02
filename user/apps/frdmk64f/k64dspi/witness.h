// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What the DSPI0 slot witness's two programs share: holder.cc owns the window and drives the bus,
// watch.cc, the image's main, waits for its end and probes the slot.

#ifndef KICKOS_USER_APPS_FRDMK64F_K64DSPI_WITNESS_H
#define KICKOS_USER_APPS_FRDMK64F_K64DSPI_WITNESS_H

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>

#include <kickos/chip_mmap.h>

#include <stdint.h>
#include <stdlib.h>

namespace k64dspi_witness
{
    namespace mmap = kickos::mk64f::mmap;

    constexpr uintptr_t MCR_OFFSET = 0x00u;
    constexpr uint32_t MCR_MSTR = 1u << 31;
    constexpr uint32_t MCR_HALT = 1u << 0;

    constexpr uint8_t SIBLING_PRIO = 9;

    inline volatile uint32_t& r32(uintptr_t a)
    {
        return *reinterpret_cast<volatile uint32_t*>(a);
    }

    // fmt takes one %d.
    inline void say(char const* fmt, int32_t a)
    {
        char s[112];
        ksnprintf(s, sizeof(s), fmt, static_cast<int>(a));
        kos::print(s);
    }

    [[noreturn]] inline void give_up(char const* what, int32_t rc)
    {
        char s[96];
        ksnprintf(s, sizeof(s), "[k64dspi] ERROR: %s rc %d\n", what, static_cast<int>(rc));
        kos::print(s);
        exit(1);
    }

    // Holds no window either; on a closed slot the read faults and the line after it never prints.
    inline void probe(void* arg)
    {
        uintptr_t const win = reinterpret_cast<uintptr_t>(arg);
        uint32_t const mcr = r32(win + MCR_OFFSET);
        char s[80];
        ksnprintf(s, sizeof(s), "[k64dspi] the probe read MCR=0x%x: the slot stayed open\n",
                  static_cast<unsigned>(mcr));
        kos::print(s);
    }
}

#endif
