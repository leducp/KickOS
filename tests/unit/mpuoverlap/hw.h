// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Each backend's real encoder, and the access its hardware grants user mode at one address,
// decoded out of the image that encoder wrote. Each hw_<backend>.cc includes its own
// mpu_encoded.h ahead of this header, so the struct this header's arch.h names is that backend's.

#ifndef KICKOS_TESTS_UNIT_MPUOVERLAP_HW_H
#define KICKOS_TESTS_UNIT_MPUOVERLAP_HW_H

#include <kickos/arch/arch.h>

#include <stddef.h>
#include <stdint.h>

struct HwAccess
{
    bool read;
    bool write;
};

// The regions in slot order through the backend's encoder; answers the user access at `addr`
// and the encoder's seating.
using HwRun = HwAccess (*)(arch_mpu_region const* regions, size_t n, uintptr_t addr,
                           uint32_t* seated);

HwAccess hw_armv7m_pmsav7(arch_mpu_region const*, size_t, uintptr_t, uint32_t*);
HwAccess hw_armv7m_pmsav8(arch_mpu_region const*, size_t, uintptr_t, uint32_t*);
HwAccess hw_armv7m_sysmpu(arch_mpu_region const*, size_t, uintptr_t, uint32_t*);
HwAccess hw_rv32imac(arch_mpu_region const*, size_t, uintptr_t, uint32_t*);
HwAccess hw_rxv3(arch_mpu_region const*, size_t, uintptr_t, uint32_t*);
HwAccess hw_sim(arch_mpu_region const*, size_t, uintptr_t, uint32_t*);

// What every encoder here is told it can name: a power of two of at least 32 bytes on its own
// alignment, which each backend's own rule admits too.
inline bool hw_encodable(uintptr_t base, size_t size)
{
    return size >= 32u and (size & (size - 1u)) == 0 and (base & (size - 1u)) == 0;
}

#endif
