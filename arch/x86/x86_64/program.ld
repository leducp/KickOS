/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * NOT a C header: the GNU ld script an x86_64 program links with on its own, an ELF executable at
 * the fixed user base (<kickos/arch/program_base.h>). The kernel's PE32+ link carries its loadable
 * bytes (tools/image-link.py), and every space maps them at the addresses linked here, so the
 * program holds no relocation. arch/CMakeLists.txt preprocesses it.
 *
 * The text, the data and the bss each start on a page: the kernel's link mirrors the program one
 * PE section per part, at the same offsets from its first byte.
 */
#ifndef KICKOS_ARCH_X86_64_PROGRAM_LD
#define KICKOS_ARCH_X86_64_PROGRAM_LD

#include <sections.ld.h>
#include <kickos/arch/program_base.h>

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(kickos_root_entry)

/* Read and execute, then read and write: the two parts the kernel's link mirrors. */
PHDRS
{
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    /* The header first, so the image's code span starts at the base. */
    .kickos_image0_header KICKOS_X86_64_PROGRAM_BASE :
    {
        __kickos_app_rom_start = .;
        __kickos_image0_code_start = .;
        KEEP(*(.kickos_image_header))
    } :text

    /* kickos_root_entry walks it unprivileged. */
    .kickos_app_init_array (READONLY) : ALIGN(8)
    {
        __kickos_app_init_array_start = .;
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        __kickos_app_init_array_end = .;
    }

    .apptext : ALIGN(16)
    {
        *(.text .text.*)
        KICKOS_CODE_DEBRIS_SECTIONS
    }

    /* system/cxx/vterminate.cc registers the unwind tables from here. The zero word is the
       terminator crtend would supply, which no image links. */
    .approdata : ALIGN(8)
    {
        *(.rodata .rodata.*)
        *(.gcc_except_table .gcc_except_table.*)
        . = ALIGN(8);
        __eh_frame_start = .;
        KEEP(*(.eh_frame .eh_frame.*))
        LONG(0)
        . = ALIGN(8);
    }

    .kickos_app_fini_array (READONLY) : ALIGN(8)
    {
        __kickos_app_fini_array_start = .;
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array))
        __kickos_app_fini_array_end = .;
    }

    /* Collected so KICKOS_TLS_FIT_ASSERT can refuse a thread_local by its size. */
    .tdata : { *(.tdata .tdata.* .gnu.linkonce.td.*) }
    .tbss (NOLOAD) : { *(.tbss .tbss.* .gnu.linkonce.tb.*) *(.tcommon) }

    __kickos_app_rom_end = .;

    /* The guard word keeps the data part non-empty, so the kernel's link always has a data
       section to mirror. A weak undefined symbol is reached through the GOT. */
    .appdata : ALIGN(4096)
    {
        __kickos_app_sram_start = .;
        LONG(0); LONG(0);
        *(.got) *(.got.plt) *(.igot.plt)
        *(.data .data.*)
        . = ALIGN(8);
    } :data

    /* Not NOLOAD, so the data segment spans it and the heap after it. */
    .appbss : ALIGN(4096)
    {
        __kickos_appbss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(8);
        __kickos_appbss_end = .;
        _kickos_heap_start = .;
        . += KICKOS_USER_HEAP_SIZE;
        _kickos_heap_limit = .;
    }

    __kickos_app_sram_end = _kickos_heap_limit;

    __kickos_image0_code_end = __kickos_app_rom_end;
    __kickos_image0_window_start = ADDR(.appdata);
    __kickos_image0_window_end = _kickos_heap_limit;
    __kickos_image0_data_load = ADDR(.appdata);
    __kickos_image0_data_start = ADDR(.appdata);
    __kickos_image0_data_end = ADDR(.appdata) + SIZEOF(.appdata);
    __kickos_image0_bss_end = __kickos_appbss_end;
    __kickos_image0_heap_start = _kickos_heap_start;
    __kickos_image0_heap_end = _kickos_heap_limit;
    __kickos_image0_ctor_start = __kickos_app_init_array_start;
    __kickos_image0_ctor_end = __kickos_app_init_array_end;
    __kickos_image0_eh_frame = __eh_frame_start;
    /* No thread-local segment: KICKOS_TLS is off on this arch. */
    __kickos_tls_segment_start = ABSOLUTE(0);
    __kickos_tls_segment_end = ABSOLUTE(0);
    __kickos_image0_tls_template = ABSOLUTE(0);
    __kickos_image0_tls_offset = ABSOLUTE(0);
    __kickos_image0_tls_init = ABSOLUTE(0);
    __kickos_image0_tls_total = ABSOLUTE(0);
    __kickos_image0_tls_align = ABSOLUTE(1);
    KICKOS_IMAGE0_OWN_NAMES()

    /DISCARD/ : { *(.note.gnu.build-id) *(.note.gnu.property) }

    KICKOS_NONALLOC_SECTIONS
}

KICKOS_STATIC_RELOC_ASSERT()
KICKOS_TLS_FIT_ASSERT()
KICKOS_FINI_EMPTY_ASSERT()
KICKOS_HEAP_MIN_ASSERT()

ASSERT(__kickos_app_sram_start % 4096 == 0 && __kickos_appbss_start % 4096 == 0,
       "KickOS: the program's data or bss does not start on a page, so the kernel's link cannot mirror it")

#endif
