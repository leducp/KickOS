/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * NOT a C header: these macros expand to GNU ld expressions. Included from every chip
 * linker script, which arch/CMakeLists.txt runs through cpp before the link.
 */
#ifndef KICKOS_ARCH_COMMON_BOOT_ARENA_LD_H
#define KICKOS_ARCH_COMMON_BOOT_ARENA_LD_H

/* Link-time replay of kmain's first arch_ram_alloc() calls: the idle boot stack, the root
 * boot stack, the init's private block, then the pool. Each base pays its alignment run-up:
 * on a pow2-descriptor arch a block sits on its own rounded size, on a base+limit arch on
 * the MPU granule.
 *
 * KICKOS_BOOT_* and KICKOS_POOL_* arrive as -D from cmake/boot_arena.cmake, sizes and
 * alignments as arch_ram_region_size() and arch_ram_region_align() answer them on this
 * build. Every *_ALIGN is a power of two, which ALIGN_UP needs. The arena symbols are
 * arguments because the RX ABI spells them with an extra leading underscore.
 */
#define KICKOS_BOOT_ALIGN_UP(v, a) (((v) + ((a) - 1)) & ~((a) - 1))

#define KICKOS_BOOT_IDLE_BASE(ram_start) \
    KICKOS_BOOT_ALIGN_UP(ram_start, KICKOS_BOOT_IDLE_ALIGN)

#define KICKOS_BOOT_ROOT_BASE(ram_start)                                       \
    KICKOS_BOOT_ALIGN_UP(KICKOS_BOOT_IDLE_BASE(ram_start) + KICKOS_BOOT_IDLE_SIZE, \
                         KICKOS_BOOT_ROOT_ALIGN)

#define KICKOS_BOOT_ARENA_ASSERT(ram_start, ram_end)                              \
    ASSERT(KICKOS_BOOT_ROOT_BASE(ram_start) + KICKOS_BOOT_ROOT_SIZE <= (ram_end), \
           "KickOS: the user-RAM arena cannot hold the idle + root boot stacks once MPU natural alignment is paid; kmain would kpanic before root ever runs. Lower KICKOS_ROOT_STACK_SIZE / KICKOS_IDLE_STACK_SIZE in the board's variant defconfig (boards/<board>/configs/<variant>/defconfig), or cut this image's static footprint, which where KICKOS_KERNEL_STACKS is 1 includes KICKOS_THREAD_SLOTS blocks of KICKOS_KERNEL_STACK_SIZE in kernel .bss below the arena.")

/* The init's private block holds one record per task and per shared region; one record is
 * the least any system with a task reserves.
 */
#define KICKOS_BOOT_PRIVATE_BASE(ram_start)                                          \
    KICKOS_BOOT_ALIGN_UP(KICKOS_BOOT_ROOT_BASE(ram_start) + KICKOS_BOOT_ROOT_SIZE,   \
                         KICKOS_BOOT_PRIVATE_ALIGN)

/* The pool holds KICKOS_MAX_THREADS blocks of KICKOS_USER_STACK_SIZE, the slots but root's,
 * whose stack is the root boot stack. Where the alignment exceeds the size, as KICKOS_TLS
 * makes it so the thread pointer can be SP masked, EVERY pool block pays a run-up.
 */
#define KICKOS_POOL_STRIDE \
    KICKOS_BOOT_ALIGN_UP(KICKOS_POOL_STACK_SIZE, KICKOS_POOL_STACK_ALIGN)

#define KICKOS_POOL_BASE(ram_start)                                                      \
    KICKOS_BOOT_ALIGN_UP(KICKOS_BOOT_PRIVATE_BASE(ram_start) + KICKOS_BOOT_PRIVATE_SIZE, \
                         KICKOS_POOL_STACK_ALIGN)

#define KICKOS_POOL_TOP(ram_start) \
    (KICKOS_POOL_BASE(ram_start) + KICKOS_POOL_STACK_COUNT * KICKOS_POOL_STRIDE)

/* Every linker script the fleet links must invoke it, board-local ones included; omitting it
 * fails the configure. It binds the FATTEST image on the board: where __kickos_ram_start
 * floats with .bss, the heaviest image caps KICKOS_MAX_THREADS. The sections between .bss and
 * the arena (.userheap, and .appdata on an enforcing chip) move ram_start, and so does the
 * per-thread kernel-stack block, which is .bss itself.
 */
#define KICKOS_POOL_ARENA_ASSERT(ram_start, ram_end)                    \
    ASSERT(KICKOS_POOL_TOP(ram_start) <= (ram_end),                     \
           "KickOS: the user-RAM arena cannot back KICKOS_MAX_THREADS default stacks of KICKOS_USER_STACK_SIZE past the two boot stacks and the init's private block, so this board advertises thread slots it cannot seat; kos_thread_create would return -KOS_ENOMEM for a slot the board claims to have, indistinguishable at runtime from a full slot table. Lower KICKOS_MAX_THREADS / KICKOS_USER_STACK_SIZE in the board's variant defconfig (boards/<board>/configs/<variant>/defconfig), or cut this image's static footprint, which where KICKOS_KERNEL_STACKS is 1 includes KICKOS_THREAD_SLOTS blocks of KICKOS_KERNEL_STACK_SIZE in kernel .bss below the arena.")

#endif
