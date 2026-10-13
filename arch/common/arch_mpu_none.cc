// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// The region-unit seam of a backend whose protection is its page tables: no region unit.

#include <kickos/arch/arch.h>
#include <kickos/chip_limits.h>

#include <stddef.h>
#include <stdint.h>

static_assert(KICKOS_CHIP_REGION_GRANULE == 0, "the chip file states a region unit beside its page tables");

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
