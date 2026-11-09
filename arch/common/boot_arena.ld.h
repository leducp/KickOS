/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * NOT a C header: these macros expand to GNU ld expressions. Included from every chip
 * linker script, which arch/CMakeLists.txt runs through cpp before the link.
 */
#ifndef KICKOS_ARCH_COMMON_BOOT_ARENA_LD_H
#define KICKOS_ARCH_COMMON_BOOT_ARENA_LD_H

/* Link-time replay of kmain's first arch_ram_alloc() calls, through the allocator's rule
 * (arch/common/ram_arena.h): the idle boot stack, the root boot stack, the init's private block,
 * then the pool. A block takes the one kept free range when it fits, else it is bumped on its
 * alignment, and a larger run-up then replaces the kept range. On a pow2-descriptor arch a block
 * sits on its own rounded size, on a base+limit arch on the MPU granule.
 *
 * KICKOS_BOOT_* and KICKOS_POOL_* arrive as -D from cmake/boot_arena.cmake, sizes and
 * alignments as arch_ram_region_size() and arch_ram_region_align() answer them on this
 * build. Every *_ALIGN is a power of two, which ALIGN_UP needs. The arena symbols are
 * arguments because the RX ABI spells them with an extra leading underscore. Comparisons are
 * ld's 0 or 1, so a choice is a product.
 */
#define KICKOS_BOOT_ALIGN_UP(v, a) (((v) + ((a) - 1)) & ~((a) - 1))

/* One allocation: state (c, gs, ge) is the bump cursor and the kept range [gs, ge) after tag
 * `t`, from the state after tag `p`.
 */
#define KICKOS_BOOT_STEP(t, p, size, align)                                                      \
    __kickos_boot_f_##t = (KICKOS_BOOT_ALIGN_UP(__kickos_boot_gs_##p, align) + (size))           \
                          <= __kickos_boot_ge_##p;                                               \
    __kickos_boot_a_##t = KICKOS_BOOT_ALIGN_UP(__kickos_boot_gs_##p, align);                     \
    __kickos_boot_b_##t = KICKOS_BOOT_ALIGN_UP(__kickos_boot_c_##p, align);                      \
    __kickos_boot_ak_##t = __kickos_boot_f_##t                                                   \
        * ((__kickos_boot_ge_##p - __kickos_boot_a_##t - (size))                                 \
           >= (__kickos_boot_a_##t - __kickos_boot_gs_##p));                                     \
    __kickos_boot_kb_##t = __kickos_boot_f_##t - __kickos_boot_ak_##t;                           \
    __kickos_boot_rp_##t = (1 - __kickos_boot_f_##t)                                             \
        * ((__kickos_boot_b_##t - __kickos_boot_c_##p)                                           \
           > (__kickos_boot_ge_##p - __kickos_boot_gs_##p));                                     \
    __kickos_boot_c_##t = __kickos_boot_f_##t * __kickos_boot_c_##p                              \
                          + (1 - __kickos_boot_f_##t) * (__kickos_boot_b_##t + (size));          \
    __kickos_boot_gs_##t = __kickos_boot_rp_##t * __kickos_boot_c_##p                            \
        + __kickos_boot_ak_##t * (__kickos_boot_a_##t + (size))                                  \
        + (1 - __kickos_boot_rp_##t - __kickos_boot_ak_##t) * __kickos_boot_gs_##p;              \
    __kickos_boot_ge_##t = __kickos_boot_rp_##t * __kickos_boot_b_##t                            \
        + __kickos_boot_kb_##t * __kickos_boot_a_##t                                             \
        + (1 - __kickos_boot_rp_##t - __kickos_boot_kb_##t) * __kickos_boot_ge_##p;

#define KICKOS_BOOT_ARENA_ASSERT(ram_start, ram_end)                                             \
    __kickos_boot_c_start = ram_start;                                                           \
    __kickos_boot_gs_start = ram_start;                                                          \
    __kickos_boot_ge_start = ram_start;                                                          \
    KICKOS_BOOT_STEP(idle, start, KICKOS_BOOT_IDLE_SIZE, KICKOS_BOOT_IDLE_ALIGN)                 \
    KICKOS_BOOT_STEP(root, idle, KICKOS_BOOT_ROOT_SIZE, KICKOS_BOOT_ROOT_ALIGN)                  \
    ASSERT(__kickos_boot_c_root <= (ram_end),                                                    \
           "KickOS: the user-RAM arena cannot hold the idle + root boot stacks once MPU natural alignment is paid; kmain would kpanic before root ever runs. Lower KICKOS_ROOT_STACK_SIZE / KICKOS_IDLE_STACK_SIZE in the board's variant defconfig (boards/<board>/configs/<variant>/defconfig), or cut this image's static footprint, which includes the system's pools and, where KICKOS_KERNEL_STACKS is 1, a KICKOS_KERNEL_STACK_SIZE block per thread slot in kernel .bss below the arena.")

/* The init's private block holds one record per task and per shared region; one record is
 * the least any system with a task reserves.
 *
 * The pool holds a block of KICKOS_USER_STACK_SIZE per thread slot but root's, whose stack is
 * the root boot stack: KICKOS_POOL_STACK_COUNT names the symbol the system's pools TU defines,
 * provided as 0 to a link carrying no system, which its system check refuses. Consecutive
 * blocks sit one KICKOS_POOL_STRIDE apart: where the alignment exceeds the size, as KICKOS_TLS
 * makes it so the thread pointer can be SP masked, EVERY pool block pays a run-up. The blocks the
 * kept range takes are counted, the rest are bumped.
 */
#define KICKOS_POOL_STRIDE \
    KICKOS_BOOT_ALIGN_UP(KICKOS_POOL_STACK_SIZE, KICKOS_POOL_STACK_ALIGN)

/* Every linker script the fleet links must invoke it, board-local ones included; omitting it
 * fails the configure. It binds the FATTEST image on the board: where __kickos_ram_start
 * floats with .bss, the heaviest image caps the thread pool. The sections between .bss and
 * the arena (.userheap, and .appdata on an enforcing chip) move ram_start, and so does the
 * per-thread kernel-stack block, which is .bss itself.
 */
#define KICKOS_POOL_ARENA_ASSERT(ram_start, ram_end)                                             \
    PROVIDE(KICKOS_POOL_STACK_COUNT = 0);                                                        \
    KICKOS_BOOT_STEP(priv, root, KICKOS_BOOT_PRIVATE_SIZE, KICKOS_BOOT_PRIVATE_ALIGN)            \
    __kickos_boot_pa = KICKOS_BOOT_ALIGN_UP(__kickos_boot_gs_priv, KICKOS_POOL_STACK_ALIGN);     \
    __kickos_boot_pf = (__kickos_boot_pa + KICKOS_POOL_STACK_SIZE) <= __kickos_boot_ge_priv;     \
    __kickos_boot_pk = __kickos_boot_pf                                                          \
        * (1 + (__kickos_boot_ge_priv - __kickos_boot_pa - KICKOS_POOL_STACK_SIZE)               \
                   / KICKOS_POOL_STRIDE);                                                        \
    __kickos_boot_pl = __kickos_boot_pk < KICKOS_POOL_STACK_COUNT;                               \
    __kickos_boot_pn = KICKOS_POOL_STACK_COUNT                                                   \
        - (__kickos_boot_pl * __kickos_boot_pk + (1 - __kickos_boot_pl) * KICKOS_POOL_STACK_COUNT); \
    __kickos_boot_pg = __kickos_boot_pn > 0;                                                     \
    __kickos_boot_top = __kickos_boot_pg                                                         \
        * (KICKOS_BOOT_ALIGN_UP(__kickos_boot_c_priv, KICKOS_POOL_STACK_ALIGN)                   \
           + (__kickos_boot_pn - 1) * KICKOS_POOL_STRIDE + KICKOS_POOL_STACK_SIZE)               \
        + (1 - __kickos_boot_pg) * __kickos_boot_c_priv;                                         \
    ASSERT(__kickos_boot_top <= (ram_end),                                                       \
           "KickOS: the user-RAM arena cannot back a default stack of KICKOS_USER_STACK_SIZE for every thread slot the composition's THREADS budgets size, past the two boot stacks and the init's private block, so a task within its budget would get -KOS_ENOMEM from kos_thread_create. Lower the THREADS budgets in the composition, or KICKOS_USER_STACK_SIZE in the board's variant defconfig (boards/<board>/configs/<variant>/defconfig), or cut this image's static footprint, which includes the system's pools and, where KICKOS_KERNEL_STACKS is 1, a KICKOS_KERNEL_STACK_SIZE block per thread slot in kernel .bss below the arena.")

#endif
