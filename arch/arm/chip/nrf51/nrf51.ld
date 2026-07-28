/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Linker script for the Nordic nRF51822 (BBC micro:bit v1), Cortex-M0. This is
 * the runnable armv6m CI/verification target under QEMU (-M microbit).
 *   Flash : 256 KiB @ 0x0000_0000
 *   SRAM  :  16 KiB @ 0x2000_0000   (tiny -- provisioning is sized down to match)
 */

ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 256K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 16K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);
_kernel_stack_size = 2K;

/* Newlib heap: EMPTY on this chip. The .userheap carve comes out of RAM ahead of the
   thread arena, and 16 KiB cannot pay for both: the selftest image's statics already
   leave only ~5.5 KiB of arena, and a 4K heap takes enough of that to starve the pool
   of thread stacks -- every kos::thread::spawn then fails. No app built for this board
   allocates, so 0 is the honest budget and the arena gets all of it. A variant that
   does need a heap asks with -DKICKOS_USER_HEAP_SIZE=<n> and pays for it in stacks.

   The section stays at zero length rather than being deleted, because deleting it
   would break EVERY link on this board, not just an allocating one: _sbrk shares a TU
   with _exit/_write (user/src/newlib_stubs.cc) and the link force-links it with
   -u,_exit, so its STRONG reference to _kickos_heap_start is present in every image
   and outranks the weak one kmain's banner uses. The heap model's link-time fail-loud
   ("undefined reference to _kickos_heap_start", intended to fire only for an app that
   allocates) is therefore not reachable from a linker script; here an allocating app
   fails at RUNTIME instead -- _sbrk refuses every request against an empty window and
   malloc returns NULL. Boot banner reports `heap none`. */
#ifndef KICKOS_USER_HEAP_SIZE
#define KICKOS_USER_HEAP_SIZE 0
#endif

SECTIONS
{
    .text :
    {
        KEEP(*(.isr_vector))
        *(.text .text.*)
        *(.rodata .rodata.*)
        *(.glue_7) *(.glue_7t)
        KEEP(*(.init))
        KEEP(*(.fini))
        . = ALIGN(4);
    } > FLASH

    /* Global ctors SPLIT: .init_array keeps ONLY the closed KickOS-owned set
       (kernel/arch/chip/lib), run early by Reset_Handler; other (app/lib) ctors
       land in .kickos_app_init_array and run from root_entry with the kernel live. */
    .init_array :
    {
        __init_array_start = .;
        KEEP(*libkickos_kernel.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_arch_armv6m.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_chip_nrf51.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_lib.a:*(SORT(.init_array.*) .init_array))
        __init_array_end = .;
    } > FLASH

    /* Teardown mirror of the ctor split: place .fini_array on the APP side (never the
       privileged bucket) so an unplaced one cannot be ORPHANED outside every stated
       boundary, and ASSERT it empty -- nothing walks it. Full rationale in
       arch/arm/chip/stm32f411/stm32f411.ld. */
    .kickos_app_fini_array :
    {
        __kickos_app_fini_array_start = .;
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array))
        __kickos_app_fini_array_end = .;
    } > FLASH

    .kickos_app_init_array :
    {
        __kickos_app_init_array_start = .;
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        __kickos_app_init_array_end = .;
    } > FLASH

    .ARM.exidx :
    {
        __exidx_start = .;
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
        __exidx_end = .;
    } > FLASH

    _sidata = LOADADDR(.data);

    .data : ALIGN(4)
    {
        _sdata = .;
        *(.data .data.*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH

    .bss (NOLOAD) : ALIGN(4)
    {
        _sbss = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    .userheap (NOLOAD) :
    {
        . = ALIGN(8);
        _kickos_heap_start = .;
        . += KICKOS_USER_HEAP_SIZE;
        _kickos_heap_limit = .;
    } > RAM

    . = ALIGN(32);
    __kickos_ram_start = .;
    __kickos_ram_end = _estack - _kernel_stack_size;

    /DISCARD/ : { *(.ARM.attributes) }
}

ASSERT(__kickos_ram_start <= __kickos_ram_end,
       "KickOS: static data overflows RAM (no room for the user-RAM pool + stack)")

/* Static destructors are unsupported: nothing walks .fini_array (see the section
   comment), so a global with a non-trivial destructor is silently dropped rather than
   run. Fail the link instead of shipping a destructor that never fires. */
ASSERT(__kickos_app_fini_array_end == __kickos_app_fini_array_start,
       "KickOS: .fini_array is non-empty -- a global with a non-trivial destructor? Static destructors are not supported; nothing would ever run it")

/* Opt-in link-time heap floor (undefined by default). The default heap here is empty,
   so any floor at all fails the link unless KICKOS_USER_HEAP_SIZE is raised to match --
   which is the point: it is the one way to make this board's heap budget a link error
   rather than a runtime NULL. */
#ifdef KICKOS_HEAP_MIN
ASSERT(_kickos_heap_limit - _kickos_heap_start >= KICKOS_HEAP_MIN,
       "KickOS: heap below KICKOS_HEAP_MIN")
#endif
