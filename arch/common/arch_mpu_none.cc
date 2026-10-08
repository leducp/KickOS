// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// The region-unit seam of a backend whose protection is its page tables: no region unit.
// cmake/boot_arena.cmake SCRAPES arch_mpu_min_region and arch_mpu_region_pow2 textually, so
// each body must stay a plain integer return. min_region 0 makes arch_ram_region_size 16-byte
// granular, so region_pow2 is never read.

#include <kickos/arch/arch.h>

#include <stddef.h>
#include <stdint.h>

void arch_mpu_apply(struct arch_mpu_region const* regions, size_t n,
                    struct arch_mpu_encoded const* image)
{
    (void)regions;
    (void)n;
    (void)image;
}

void kickos_arch_mpu_commit(void) {}

// Nothing is deferred, so the set is already live when apply returns.
void arch_mpu_apply_now(struct arch_mpu_region const* regions, size_t n,
                        struct arch_mpu_encoded const* image)
{
    arch_mpu_apply(regions, n, image);
}

size_t arch_mpu_min_region(void)
{
    return 0;
}

int arch_mpu_region_pow2(void)
{
    return 0;
}

bool arch_mpu_region_encodable(uintptr_t base, size_t size)
{
    (void)base;
    (void)size;
    return false;
}

int arch_mpu_nocache_support(void)
{
    return ARCH_MPU_NOCACHE_REFUSED;
}
