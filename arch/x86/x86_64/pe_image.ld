/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The x86_64 kernel's PE32+ link (tools/x86_64-link.sh), whose one input is the pre-linked object.
 *
 * The archive and object-directory lines match nothing here: tools/x86_64-link.sh copies each,
 * verbatim, into the pre-link's script, and cmake/x86_64_boot.cmake gives the kernel posture to
 * every target a .text line names. Keep each a whole input section description on one line, and
 * start no other line, a comment's included, with a star and such a selector.
 *
 * ld -m i386pep drops an empty output section and re-bases the symbols assigned in it onto the
 * first section, so every section-relative symbol here is assigned in .rdata, which the image
 * table's words keep from being empty.
 *
 * No symbol here may become a PROVIDE: the ELF twin copies only plain assignments.
 */

/* -b elf64-x86-64 would otherwise also name the output's format. */
OUTPUT_FORMAT(pei-x86-64)

SECTIONS
{
    /DISCARD/ :
    {
        *(.debug_*)
        *(.eh_frame)
        *(.fini_array)
    }

    .text __image_base__ + __section_alignment__ :
    {
        *(.kickos.kernel.text)
        *libkickos_kernel.a:*(.text .text.*)
        *libkickos_arch_x86_64.a:*(.text .text.*)
        *libkickos_chip_q35.a:*(.text .text.*)
        *kickos_x86_64_boot.dir/*(.text .text.*)
        *kickos_x86_64_boot_ap.dir/*(.text .text.*)
        *kickos_x86_64_landed_kernel.dir/*(.text .text.*)
        *kickos_x86_64_nokernel.dir/*(.text .text.*)
        *(.text .text.*)
    }

    .data ALIGN(__section_alignment__) :
    {
        *(.kickos.kernel.data)
        *libkickos_kernel.a:*(.data .data.*)
        *libkickos_arch_x86_64.a:*(.data .data.*)
        *libkickos_chip_q35.a:*(.data .data.*)
        *kickos_x86_64_boot.dir/*(.data .data.*)
        *kickos_x86_64_boot_ap.dir/*(.data .data.*)
        *kickos_x86_64_landed_kernel.dir/*(.data .data.*)
        *kickos_x86_64_nokernel.dir/*(.data .data.*)
        *(.data .data.*)
    }

    .bss ALIGN(__section_alignment__) :
    {
        *(.kickos.kernel.bss)
        *libkickos_kernel.a:*(.bss .bss.* COMMON)
        *libkickos_arch_x86_64.a:*(.bss .bss.* COMMON)
        *libkickos_chip_q35.a:*(.bss .bss.* COMMON)
        *kickos_x86_64_boot.dir/*(.bss .bss.* COMMON)
        *kickos_x86_64_boot_ap.dir/*(.bss .bss.* COMMON)
        *kickos_x86_64_landed_kernel.dir/*(.bss .bss.* COMMON)
        *kickos_x86_64_nokernel.dir/*(.bss .bss.* COMMON)
        *(.bss .bss.* COMMON)
    }

    INCLUDE kickos_image0.ld

    /* Each store must start on a page, and its input section states no alignment. */
    .kimgs ALIGN(__section_alignment__) : SUBALIGN(0x1000)
    {
        KEEP (*(.kickos_images))
    }

    /* Nothing may sit between .kimgs and .rdata: __kickos_images_load_end is .rdata's first
     * byte, the page after every embedded image whether or not .kimgs holds one. */
    .rdata ALIGN(__section_alignment__) :
    {
        __kickos_images_load_end = .;
        __kickos_code_start = .;
        __kickos_code_end = .;
        __kickos_appdata_start = .;
        __kickos_appdata_end = .;

        . = ALIGN(8);
        __kickos_image_table = .;
        INCLUDE kickos_image_table.ld
        __kickos_image_table_end = .;

        __kickos_image_frames = .;
        INCLUDE kickos_image_frames.ld
        __kickos_image_frames_end = .;

        __init_array_start = .;
        KEEP (*(.kickos.kernel.init_array))
        KEEP (*libkickos_kernel.a:*(SORT_BY_INIT_PRIORITY(.init_array.*))) KEEP (*libkickos_kernel.a:*(.init_array))
        KEEP (*libkickos_arch_x86_64.a:*(SORT_BY_INIT_PRIORITY(.init_array.*))) KEEP (*libkickos_arch_x86_64.a:*(.init_array))
        KEEP (*libkickos_chip_q35.a:*(SORT_BY_INIT_PRIORITY(.init_array.*))) KEEP (*libkickos_chip_q35.a:*(.init_array))
        KEEP (*kickos_x86_64_boot.dir/*(SORT_BY_INIT_PRIORITY(.init_array.*))) KEEP (*kickos_x86_64_boot.dir/*(.init_array))
        KEEP (*kickos_x86_64_boot_ap.dir/*(SORT_BY_INIT_PRIORITY(.init_array.*))) KEEP (*kickos_x86_64_boot_ap.dir/*(.init_array))
        KEEP (*kickos_x86_64_landed_kernel.dir/*(SORT_BY_INIT_PRIORITY(.init_array.*))) KEEP (*kickos_x86_64_landed_kernel.dir/*(.init_array))
        KEEP (*kickos_x86_64_nokernel.dir/*(SORT_BY_INIT_PRIORITY(.init_array.*))) KEEP (*kickos_x86_64_nokernel.dir/*(.init_array))
        KEEP (*(.init_array))
        __init_array_end = .;

        *(.kickos.kernel.rodata)
        *libkickos_kernel.a:*(.rodata .rodata.*)
        *libkickos_arch_x86_64.a:*(.rodata .rodata.*)
        *libkickos_chip_q35.a:*(.rodata .rodata.*)
        *kickos_x86_64_boot.dir/*(.rodata .rodata.*)
        *kickos_x86_64_boot_ap.dir/*(.rodata .rodata.*)
        *kickos_x86_64_landed_kernel.dir/*(.rodata .rodata.*)
        *kickos_x86_64_nokernel.dir/*(.rodata .rodata.*)
        *(.rodata .rodata.*)
    }

    /* ld sizes .reloc after layout: a section after it would move. */
    .reloc ALIGN(__section_alignment__) :
    {
        *(.reloc)
    }

    /* Absolute, so no base relocation moves them. tests/static/check_x86_64_app_split.py
     * classifies each by this spelling where the symbol table drops its value. */
    __kickos_link_image_base = ABSOLUTE(__image_base__);
    __kickos_frame_pool_delta = 0x0;
}
