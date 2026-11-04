// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RISC-V PMP (privileged spec 3.7.1): the lowest-numbered entry matching an address decides a
// U-mode access, and one no entry matches is refused.

#define arch_mpu_encode hw_rv32imac_encode_unused
#define arch_mpu_apply hw_rv32imac_apply_unused
#define arch_mpu_apply_now hw_rv32imac_apply_now_unused
#define arch_mpu_encoded hw_pmp_encoded
#define arch_mpu_encoded_seated hw_pmp_seated
#define arch_mpu_region_encodable hw_pmp_encodable

#include "arch/riscv/rv32imac/include/kickos/arch/mpu_encoded.h"
#include "hw.h"

#include "arch/riscv/rv32imac/pmp_encode.h"

extern "C" bool hw_pmp_encodable(uintptr_t base, size_t size)
{
    return hw_encodable(base, size);
}

namespace
{
    // The cfg byte of the lowest-numbered entry of `count` matching `at`, or -1 for none.
    int pmp_match(uint32_t const* addr, uint32_t const* cfg_words, unsigned count, uintptr_t at)
    {
        uint64_t below = 0;
        for (unsigned i = 0; i < count; i++)
        {
            uint32_t const cfg = (cfg_words[i / 4u] >> (8u * (i % 4u))) & 0xFFu;
            uint64_t const y = addr[i];
            uint64_t base = 0;
            uint64_t size = 0;
            uint32_t const a = (cfg >> 3) & 3u;
            if (a == 1u) // TOR
            {
                base = below << 2;
                if ((y << 2) > base)
                {
                    size = (y << 2) - base;
                }
            }
            else if (a == 2u) // NA4
            {
                base = y << 2;
                size = 4u;
            }
            else if (a == 3u) // NAPOT
            {
                unsigned ones = 0;
                while (((y >> ones) & 1u) != 0)
                {
                    ones++;
                }
                base = (y & ~((uint64_t(1) << (ones + 1u)) - 1u)) << 2;
                size = uint64_t(1) << (ones + 3u);
            }
            below = y;
            if (size != 0 and at >= base and at - base < size)
            {
                return static_cast<int>(cfg);
            }
        }
        return -1;
    }
}

HwAccess hw_rv32imac(arch_mpu_region const* regions, size_t n, uintptr_t addr, uint32_t* seated)
{
    hw_pmp_encoded img = {};
    *seated = kickos::rv32::pmp_encode(regions, n, &img);
    int const cfg = pmp_match(img.addr, img.cfg, ARCH_MPU_ENCODED_SLOTS, addr);
    if (cfg < 0)
    {
        return {false, false};
    }
    return {(cfg & 1) != 0, (cfg & 2) != 0};
}

// What user mode is granted at `at` by a thread's set `regions` under the boot code window
// [base, top): the thread's entries 0..7, then entries 8 and 9. `exec` is the X bit.
HwAccess hw_rv32imac_code_window(arch_mpu_region const* regions, size_t n, uintptr_t base,
                                 uintptr_t top, uintptr_t at, bool* exec)
{
    hw_pmp_encoded img = {};
    (void)kickos::rv32::pmp_encode(regions, n, &img);
    kickos::rv32::PmpCodeWindow const w = kickos::rv32::pmp_code_window(base, top);
    uint32_t addr[16] = {};
    uint32_t cfg[4] = {img.cfg[0], img.cfg[1], w.cfg2, 0};
    for (unsigned i = 0; i < ARCH_MPU_ENCODED_SLOTS; i++)
    {
        addr[i] = img.addr[i];
    }
    addr[kickos::rv32::PMP_CODE_BASE_ENTRY] = w.base_addr;
    addr[kickos::rv32::PMP_CODE_TOP_ENTRY] = w.top_addr;
    int const hit = pmp_match(addr, cfg, 16, at);
    *exec = hit >= 0 and (hit & 4) != 0;
    if (hit < 0)
    {
        return {false, false};
    }
    return {(hit & 1) != 0, (hit & 2) != 0};
}
