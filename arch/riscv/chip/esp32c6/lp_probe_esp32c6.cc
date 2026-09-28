// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// M9.9 silicon bring-up witness for c6lpprobe. This member is extracted only by
// a link that names kickos_c6_lp_marker; arch_init reaches the body through a
// weak reference, so every other C6 image carries neither.

#include <kickos/arch/arch.h>

#if !KICKOS_HAVE_MPU && !KICKOS_AMP_OWN_IMAGE

#include <stdint.h>

#include "regs/apm.h"

namespace reg = kickos::esp32c6::reg;

extern "C"
{
    extern uint32_t kickos_c6_lp_probe_start[];
    extern uint32_t kickos_c6_lp_probe_end[];
    extern uint32_t kickos_c6_lp_payload_start[];
    extern uint32_t kickos_c6_lp_payload_end[];
    alignas(64) uint32_t kickos_c6_lp_marker[28] = {};
}

namespace
{
    inline volatile uint32_t& r32(uintptr_t a)
    {
        return *reinterpret_cast<volatile uint32_t*>(a);
    }
}

extern "C" void kickos_c6_lp_probe_boot(void)
{
    // The LP domain registers are inaccessible to an ordinary U-mode thread, even
    // in the flat memory posture, so the HP kernel loads and wakes the probe
    // before kmain.
    constexpr uintptr_t lp_mem = 0x50000000u;
    constexpr uintptr_t lp_image = 0x40840000u;
    uintptr_t const marker = reinterpret_cast<uintptr_t>(kickos_c6_lp_marker);
    constexpr uintptr_t lpbus = 0x600B1048u;
    constexpr uintptr_t lp_apm = 0x600B3800u;  // LP CPU -> LP SRAM
    constexpr uintptr_t lp_apm0 = 0x60099800u; // LP_APM0, not HP_APM at 0x60099000
    constexpr uintptr_t pwr0 = 0x600B017Cu;
    constexpr uintptr_t pwr1 = 0x600B0180u;
    constexpr uintptr_t comm = 0x600B0184u;
    uintptr_t const size = reinterpret_cast<uintptr_t>(kickos_c6_lp_probe_end)
                           - reinterpret_cast<uintptr_t>(kickos_c6_lp_probe_start);
    uintptr_t const payload_size = reinterpret_cast<uintptr_t>(kickos_c6_lp_payload_end)
                                   - reinterpret_cast<uintptr_t>(kickos_c6_lp_payload_start);
    if (size == 0u or size > 1024u or (size & 3u) != 0u
        or payload_size == 0u or payload_size > 1024u or (payload_size & 3u) != 0u)
    {
        return;
    }
    uint32_t const count = static_cast<uint32_t>(size / 4u);
    uint32_t const payload_count = static_cast<uint32_t>(payload_size / 4u);
    r32(marker) = 0;
    r32(lp_mem + 0x200u) = 0;
    r32(lp_mem + 0x204u) = 0;
    r32(lp_mem + 0x208u) = 0;
    r32(lp_mem + 0x20Cu) = 0;
    r32(lp_mem + 0x210u) = 0;
    r32(lp_mem + 0x214u) = 0;
    r32(lp_mem + 0x218u) = 0;
    r32(lp_mem + 0x21Cu) = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        r32(lp_mem + 4u * i) = kickos_c6_lp_probe_start[i];
    }
    for (uint32_t i = 0; i < payload_count; i++)
    {
        r32(lp_image + 4u * i) = kickos_c6_lp_payload_start[i];
    }
    __asm volatile("fence iorw, iorw" ::: "memory");
    r32(lpbus) = (r32(lpbus) & ~(1u << 31)) | (1u << 30);
    // LP CPU remains in its reset REE2 mode. The reset APM catch-all
    // denies every access, so grant only this stub's executable LP SRAM
    // and its one HP SRAM marker page. Region 1 overlaps the deny-all
    // region 0, and TRM 16.3.2.3 resolves that overlap to the permit.
    // The LP APM exception recorder reports the reset fetch at
    // 0x70000080, the LP bus alias of the 0x50000080 reset PC.
    r32(lp_apm + 0x1Cu) = 0x70000000u;
    r32(lp_apm + 0x20u) = 0x700003FFu;
    r32(lp_apm + 0x24u) = 0x700u;
    r32(lp_apm + 0x00u) |= 1u << 2;
    r32(lp_apm + 0x10u) = 0x600B0000u;
    r32(lp_apm + 0x14u) = 0x600B03FFu;
    r32(lp_apm + 0x18u) = 0x600u; // PMU REE2 R/W
    r32(lp_apm + 0x28u) = 0x600B2800u;
    r32(lp_apm + 0x2Cu) = 0x600B2BFFu;
    r32(lp_apm + 0x30u) = 0x400u; // LPPERI REE2 read
    r32(lp_apm + 0x00u) |= (1u << 1) | (1u << 3);
    // The HP APM gates the LP CPU's HP SRAM store.
    r32(reg::apm::region_addr_start(4u)) = marker;
    r32(reg::apm::region_addr_end(4u)) = marker + 0x3Fu;
    r32(reg::apm::region_attr(4u)) = 0x600u; // REE2 R/W
    r32(reg::apm::FILTER_EN) |= reg::apm::region_en(4u);
    r32(reg::apm::region_addr_start(5u)) = lp_image;
    r32(reg::apm::region_addr_end(5u)) = lp_image + 0x3FFu;
    r32(reg::apm::region_attr(5u)) = 0x500u; // REE2 R/X
    r32(reg::apm::FILTER_EN) |= reg::apm::region_en(5u);
    r32(pwr0) = r32(pwr0) | (3u << 30);
    r32(pwr1) = r32(pwr1) | 1u;
    r32(0x600B0174u) |= 1u << 31; // HP trigger -> LP IRQ 30
    r32(comm) = 1u << 31;
    for (uint32_t spin = 0; spin < 1000000u; spin++)
    {
        __asm volatile("nop");
    }
    r32(marker + 4u) = r32(lp_mem + 128u);
    r32(marker + 8u) = r32(pwr1);
    r32(marker + 12u) = r32(lpbus);
    r32(marker + 16u) = r32(pwr0);
    r32(marker + 20u) = r32(lp_mem + 0x200u);
    r32(marker + 40u) = r32(lp_mem + 0x204u);
    r32(marker + 44u) = r32(lp_mem + 0x208u);
    r32(marker + 48u) = r32(lp_mem + 0x20Cu);
    r32(marker + 24u) = r32(0x600B015Cu); // PMU raw: LP exception bit 27
    r32(marker + 28u) = r32(0x600B016Cu); // LP raw: wake bit 20, HP SW bit 31
    r32(marker + 32u) = r32(0x600B018Cu); // PMU main state
    r32(marker + 36u) = r32(0x600B0190u); // PMU power state
    r32(marker + 52u) = r32(lp_apm + 0x00u);
    r32(marker + 56u) = r32(lp_apm0 + 0x00u);
    r32(marker + 60u) = r32(lp_apm + 0xC8u);
    r32(marker + 64u) = r32(lp_apm + 0xD0u);
    r32(marker + 68u) = r32(lp_apm + 0xD4u);
    r32(marker + 72u) = r32(lp_apm0 + 0xC8u);
    r32(marker + 76u) = r32(lp_apm0 + 0xD0u);
    r32(marker + 80u) = r32(lp_apm0 + 0xD4u);
    r32(marker + 84u) = r32(lp_mem + 0x210u);
    r32(marker + 88u) = r32(lp_mem + 0x214u);
    r32(marker + 92u) = r32(lp_mem + 0x218u);
    r32(marker + 96u) = r32(lp_mem + 0x21Cu);
    r32(marker + 100u) = r32(lp_apm + 0xD8u);
    r32(marker + 104u) = r32(lp_apm + 0xE0u);
    r32(marker + 108u) = r32(lp_apm + 0xE4u);
}

#endif
