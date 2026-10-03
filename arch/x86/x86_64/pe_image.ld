/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * PE32+ layout based on ld -m i386pep. Collect .data.*, .rodata.* and .bss.*
 * to avoid a separate PE section for each compiler-generated input section.
 * Too many sections extend headers beyond 0x1000 and EDK II rejects the image.
 * Keep kernel and app constructors in separate ranges.
 */

/* Default linker script, for normal executables */
/* Copyright (C) 2014-2026 Free Software Foundation, Inc.
   Copying and distribution of this script, with or without modification,
   are permitted in any medium without royalty provided the copyright
   notice and this notice are preserved.  */
OUTPUT_FORMAT(pei-x86-64)
SEARCH_DIR("=/usr/local/lib/x86_64-linux-gnu"); SEARCH_DIR("=/lib/x86_64-linux-gnu"); SEARCH_DIR("=/usr/lib/x86_64-linux-gnu"); SEARCH_DIR("=/usr/local/lib"); SEARCH_DIR("=/lib"); SEARCH_DIR("=/usr/lib"); SEARCH_DIR("=/usr/x86_64-pep/lib");
SECTIONS
{
  /* Keep virtual and file offsets aligned when section alignment is below page size. */
  . = SIZEOF_HEADERS;
  . = ALIGN(__section_alignment__);
  /* The kernel's half: the kernel, arch and chip archives and the boot objects, named by
   * archive or object directory. Everything the selectors below do not claim is the app's
   * and goes to the app window further down, which a task reaches at its alias (the loader
   * address plus the user offset) and which is the only part of this image a ring-3 access
   * can reach. An input section goes to the first output section that names it.
   */
  .text  __image_base__ + ( __section_alignment__ < 0x1000 ? . : __section_alignment__ ) :
  {
    KEEP (*(SORT_NONE(.init)))
    *libkickos_kernel.a:*(.text .text.*)
    *libkickos_arch_x86_64.a:*(.text .text.*)
    *libkickos_chip_q35.a:*(.text .text.*)
    *kickos_x86_64_boot.dir/*(.text .text.*)
    *kickos_x86_64_boot_ap.dir/*(.text .text.*)
    *kickos_x86_64_landed_kernel.dir/*(.text .text.*)
    *kickos_x86_64_nobench.dir/*(.text .text.*)
    *kickos_x86_64_nokernel.dir/*(.text .text.*)
    . = ALIGN(8);
    KEEP (*(SORT_NONE(.fini)))
    PROVIDE (etext = .);
  }
  .data BLOCK(__section_alignment__) :
  {
    __data_start__ = . ;
    *libkickos_kernel.a:*(.data .data.*)
    *libkickos_arch_x86_64.a:*(.data .data.*)
    *libkickos_chip_q35.a:*(.data .data.*)
    *kickos_x86_64_boot.dir/*(.data .data.*)
    *kickos_x86_64_boot_ap.dir/*(.data .data.*)
    *kickos_x86_64_landed_kernel.dir/*(.data .data.*)
    *kickos_x86_64_nobench.dir/*(.data .data.*)
    *kickos_x86_64_nokernel.dir/*(.data .data.*)
    __data_end__ = . ;
  }
  /* PE section names must fit in eight bytes without an image string table. Only the
   * kernel's constructors run before kmain; the app's are in the app window and run from
   * kickos_root_entry, unprivileged.
   */
  .kosinit BLOCK(__section_alignment__) :
  {
    /* A word of its own, so an image with no kernel constructor keeps the section and its two
     * symbols stay where they are assigned (see the app window below). */
    LONG (0); LONG (0);
    . = ALIGN(8);
    __init_array_start = . ;
    KEEP (*libkickos_kernel.a:*(SORT(.init_array.*) .init_array))
    KEEP (*libkickos_arch_x86_64.a:*(SORT(.init_array.*) .init_array))
    KEEP (*libkickos_chip_q35.a:*(SORT(.init_array.*) .init_array))
    KEEP (*kickos_x86_64_boot.dir/*(SORT(.init_array.*) .init_array))
    KEEP (*kickos_x86_64_boot_ap.dir/*(SORT(.init_array.*) .init_array))
    KEEP (*kickos_x86_64_landed_kernel.dir/*(SORT(.init_array.*) .init_array))
    KEEP (*kickos_x86_64_nobench.dir/*(SORT(.init_array.*) .init_array))
    KEEP (*kickos_x86_64_nokernel.dir/*(SORT(.init_array.*) .init_array))
    __init_array_end = . ;
  }
  .rdata BLOCK(__section_alignment__) :
  {
    *libkickos_kernel.a:*(.rodata .rodata.*)
    *libkickos_arch_x86_64.a:*(.rodata .rodata.*)
    *libkickos_chip_q35.a:*(.rodata .rodata.*)
    *kickos_x86_64_boot.dir/*(.rodata .rodata.*)
    *kickos_x86_64_boot_ap.dir/*(.rodata .rodata.*)
    *kickos_x86_64_landed_kernel.dir/*(.rodata .rodata.*)
    *kickos_x86_64_nobench.dir/*(.rodata .rodata.*)
    *kickos_x86_64_nokernel.dir/*(.rodata .rodata.*)
    . = ALIGN(4);
    __rt_psrelocs_start = .;
    KEEP(*(.rdata_runtime_pseudo_reloc))
    __rt_psrelocs_end = .;
    /* read-only parts of .didat */
    /* This cannot currently be handled with grouped sections.
	See pe.em:sort_sections.  */
    . = ALIGN(8);
    __DELAY_IMPORT_DIRECTORY_start__ = .;
    KEEP (SORT(*)(.didat$2))
    KEEP (SORT(*)(.didat$3))
    __DELAY_IMPORT_DIRECTORY_end__ = .;
    /* These zeroes mark the end of the import list.  */
    . += (__DELAY_IMPORT_DIRECTORY_end__ - __DELAY_IMPORT_DIRECTORY_start__) ? 8*4 : 0;
    . = ALIGN(8);
    KEEP (SORT(*)(.didat$4))
    KEEP (SORT(*)(.didat$6))
    KEEP (SORT(*)(.didat$7))
    /* .ctors & .dtors */
    . = ALIGN(8);
       /* Define constructor-list symbols directly to override libgcc common symbols;
        * PROVIDE would leave those symbols unresolved to this list (binutils PR 22762).
        * These definitions also override user lists and ld -Ur output (PR 46).
        */
       ___CTOR_LIST__ = .;
       __CTOR_LIST__ = .;
       LONG (-1); LONG (-1);
       KEEP (*(.ctors));
       KEEP (*(.ctor));
       KEEP (*(SORT_BY_NAME(.ctors.*)));
       LONG (0); LONG (0);
       /* Destructor lists require the same direct definitions as constructor lists. */
       ___DTOR_LIST__ = .;
       __DTOR_LIST__ = .;
       LONG (-1); LONG (-1);
       KEEP (*(.dtors));
       KEEP (*(.dtor));
       KEEP (*(SORT_BY_NAME(.dtors.*)));
       LONG (0); LONG (0);
    /* .CRT */
    ___crt_xc_start__ = . ;
    KEEP (*(SORT(.CRT$XC*)))  /* C initialization */
    ___crt_xc_end__ = . ;
    ___crt_xi_start__ = . ;
    KEEP (*(SORT(.CRT$XI*)))  /* C++ initialization */
    ___crt_xi_end__ = . ;
    ___crt_xl_start__ = . ;
    KEEP (*(SORT(.CRT$XL*)))  /* TLS callbacks */
    /* ___crt_xl_end__ is defined in the TLS Directory support code */
    ___crt_xp_start__ = . ;
    KEEP (*(SORT(.CRT$XP*)))  /* Pre-termination */
    ___crt_xp_end__ = . ;
    ___crt_xt_start__ = . ;
    KEEP (*(SORT(.CRT$XT*)))  /* Termination */
    ___crt_xt_end__ = . ;
    ___crt_xd_start__ = . ;
    KEEP (*(SORT(.CRT$XD*)))  /* Dynamic TLS Initializer */
    ___crt_xd_end__ = . ;
  }
  ___RUNTIME_PSEUDO_RELOC_LIST_END__ = __rt_psrelocs_end;
  __RUNTIME_PSEUDO_RELOC_LIST_END__ = __rt_psrelocs_end;
  ___RUNTIME_PSEUDO_RELOC_LIST__ = __rt_psrelocs_start;
  __RUNTIME_PSEUDO_RELOC_LIST__ = __rt_psrelocs_start;
  .pdata BLOCK(__section_alignment__) :
  {
    KEEP(*(.pdata*))
  }
  .xdata BLOCK(__section_alignment__) :
  {
    KEEP(*(.xdata*))
  }
  .bss BLOCK(__section_alignment__) :
  {
    __bss_start__ = . ;
    *libkickos_kernel.a:*(.bss .bss.* COMMON)
    *libkickos_arch_x86_64.a:*(.bss .bss.* COMMON)
    *libkickos_chip_q35.a:*(.bss .bss.* COMMON)
    *kickos_x86_64_boot.dir/*(.bss .bss.* COMMON)
    *kickos_x86_64_boot_ap.dir/*(.bss .bss.* COMMON)
    *kickos_x86_64_landed_kernel.dir/*(.bss .bss.* COMMON)
    *kickos_x86_64_nobench.dir/*(.bss .bss.* COMMON)
    *kickos_x86_64_nokernel.dir/*(.bss .bss.* COMMON)
    __bss_end__ = . ;
  }
  /* The app window: contiguous from __kickos_app_rom_start to __kickos_app_sram_end, so one
   * offset carries all of it. aspace_image_seed maps the text read-execute in every space
   * and gives each space its own copy of the rest.
   *
   * NONE OF ITS SECTIONS MAY BE EMPTY. ld -m i386pep drops an empty output section after
   * assigning its symbols and re-bases them onto the image's first section, so an app with no
   * data would state __kickos_app_sram_start at the kernel's .text and the seed would hand
   * every space the kernel's own pages. Each section below therefore carries a word of its own,
   * and tests/static/check_x86_64_app_split.py refuses an image whose symbols moved.
   *
   * .apptrap is the syscall trap (switch.S), its own section name because the arch archive
   * holding it is the kernel's and its .text is claimed above.
   */
  .apptext BLOCK(__section_alignment__) :
  {
    __kickos_app_rom_start = . ;
    *(.apptrap)
    *(.text .text.*)
    *(SORT(.text$*))
    *(.gnu.linkonce.t.*)
    *(.gcc_exc)
  }
  /* The app's read-only data: its own section so no disassembler reads it as code, and still
   * inside the read-execute half of the window.
   */
  .appro BLOCK(__section_alignment__) :
  {
    LONG (0); LONG (0);
    *(.rodata .rodata.* .rdata)
    /* The C++ exception tables and the unwind tables, which ring 3's unwinder reads:
     * system/cxx/vterminate.cc registers the latter from here. -ffunction-sections names a
     * table per function, and one left unnamed here lands in the kernel's half. The zero word
     * is the terminator crtend would supply, which no image links. */
    KEEP (*(.gcc_except_table .gcc_except_table.*))
    . = ALIGN(8);
    __eh_frame_start = . ;
    KEEP (*(.eh_frame*))
    LONG (0);
    . = ALIGN(8);
    __kickos_app_rom_end = . ;
  }
  /* On the section alignment PE32+ requires of every section, which is this board's page, and
   * granular after it: no power-of-two window to lose, and nothing above the app window slides
   * with it (tests/static/check_kernel_data_reserve.sh).
   */
  .appdata BLOCK(__section_alignment__) : ALIGN(8)
  {
    __kickos_app_sram_start = . ;
    LONG (0); LONG (0);
    . = ALIGN(8);
    __kickos_app_init_array_start = . ;
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array))
    __kickos_app_init_array_end = . ;
    . = ALIGN(8);
    __fini_array_start = . ;
    KEEP (*(SORT(.fini_array.*)))
    KEEP (*(.fini_array))
    __fini_array_end = . ;
    *(.data .data.*)
    *(.data2)
    *(SORT(.data$*))
    KEEP(*(.jcr))
    *(.data_cygwin_nocopy)
  }
  .appbss BLOCK(__section_alignment__) :
  {
    . += 8;
    *(.bss .bss.*)
    *(COMMON)
    . = ALIGN(8);
    /* newlib's heap, KICKOS_USER_HEAP_SIZE bytes, a link symbol. In
     * the app window, so every address space gets a copy of its own. */
    _kickos_heap_start = . ;
    . += KICKOS_USER_HEAP_SIZE;
    _kickos_heap_limit = . ;
    __kickos_app_sram_end = . ;
  }
  /* Opt-in link-time heap floor, --defsym KICKOS_HEAP_MIN=<bytes> (undefined by default). */
  ASSERT (DEFINED (KICKOS_HEAP_MIN) ? _kickos_heap_limit - _kickos_heap_start >= KICKOS_HEAP_MIN : 1,
          "KickOS: heap below KICKOS_HEAP_MIN")
  /* The block a stack pays for thread-local data, which a system target's asserts read
   * (arch/common/sections.ld.h). This board has no KICKOS_TLS, so a stack pays none. */
  __kickos_tls_carve = 0;
  .edata BLOCK(__section_alignment__) :
  {
    *(.edata)
  }
  /* Absolute, so no base-relocation record moves them. The kernel's view is the loader's
   * identity map, and the app is not linked where tasks reach it: the user offset, which the
   * boot measures, is what separates the two.
   */
  __kickos_frame_pool_delta = 0;
  __kickos_app_load_delta = 0;
  /* Where the image was linked to run. Firmware rewrites the loaded header's ImageBase to the
   * address it chose, so only a word no relocation touches still holds the preferred one.
   */
  __kickos_link_image_base = ABSOLUTE (__image_base__);
  /DISCARD/ :
  {
    *(.debug$S)
    *(.debug$T)
    *(.debug$F)
     *(.drectve)
     *(.note.GNU-stack)
     *(.gnu.lto_*)
     *(.gnu_object_only)
     /* The toolchain's libraries are compiled without -fno-ident, and PE32+ has no place for
      * the compiler's version string below the image base. */
     *(.comment)
  }
  .idata BLOCK(__section_alignment__) :
  {
    /* This cannot currently be handled with grouped sections.
	See pep.em:sort_sections.  */
    KEEP (SORT(*)(.idata$2))
    KEEP (SORT(*)(.idata$3))
    /* These zeroes mark the end of the import list.  */
    LONG (0); LONG (0); LONG (0); LONG (0); LONG (0);
    . = ALIGN(8);
    KEEP (SORT(*)(.idata$4))
    __IAT_start__ = .;
    SORT(*)(.idata$5)
    __IAT_end__ = .;
    KEEP (SORT(*)(.idata$6))
    KEEP (SORT(*)(.idata$7))
  }
  .didat BLOCK(__section_alignment__) :
  {
    /* This cannot currently be handled with grouped sections.
	See pep.em:sort_sections.  */
    SORT(*)(.didat$5)
  }
  /* Keep .tls$AAA first and .tls$ZZZ last. _tls_start must begin the section
   * for SECREL32 TLS relocations.
   */
  .tls BLOCK(__section_alignment__) :
  {
    ___tls_start__ = . ;
    KEEP (*(.tls$AAA))
    KEEP (*(.tls))
    KEEP (*(.tls$))
    KEEP (*(SORT(.tls$*)))
    KEEP (*(.tls$ZZZ))
    ___tls_end__ = . ;
  }
  .endjunk BLOCK(__section_alignment__) :
  {
    /* Kept for the same reason as .kosinit's word. */
    LONG (0); LONG (0);
    /* end is deprecated, don't use it */
    PROVIDE (end = .);
    PROVIDE ( _end = .);
     __end__ = .;
    /* Define all window symbols strongly: PE has no GOT, so unresolved weak
     * references can become indirect loads from the symbol itself. Empty windows
     * have start == end. See include/kickos/klink.h.
     */
    . = ALIGN(8);
    __kickos_code_start = . ;
    __kickos_code_end = . ;
    __kickos_appdata_start = . ;
    __kickos_appdata_end = . ;
  }
  .rsrc BLOCK(__section_alignment__) : SUBALIGN(4)
  {
    KEEP (*(.rsrc))
    KEEP (*(.rsrc$*))
  }
  .reloc BLOCK(__section_alignment__) :
  {
    *(.reloc)
  }
  /* The retained copy of .reloc, which the boot reads to relocate the app window a second
   * time (cmake/x86_64_image.cmake links it in on a second pass). After every section a
   * fixup can sit in and carrying none itself, so its presence leaves .reloc unchanged.
   */
  .krel BLOCK(__section_alignment__) :
  {
    __kickos_krel_start = . ;
    KEEP (*(.krel))
    __kickos_krel_end = . ;
  }
  .stab BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.stab)
  }
  .stabstr BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.stabstr)
  }
  /* DWARF symbols are section-relative. PE forbids the zero section VMA
   * used by some other targets.
   */
  /* DWARF 1.1 and DWARF 2.  */
  .debug_aranges BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.debug_aranges)
  }
  .zdebug_aranges BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.zdebug_aranges)
  }
  .debug_pubnames BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.debug_pubnames)
  }
  .zdebug_pubnames BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.zdebug_pubnames)
  }
  /* DWARF 2.  */
  .debug_info BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.debug_info .gnu.linkonce.wi.*)
  }
  .zdebug_info BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.zdebug_info .zdebug.gnu.linkonce.wi.*)
  }
  .debug_abbrev BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.debug_abbrev)
  }
  .zdebug_abbrev BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.zdebug_abbrev)
  }
  .debug_line BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.debug_line)
  }
  .zdebug_line BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.zdebug_line)
  }
  .debug_frame BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.debug_frame*)
  }
  .zdebug_frame BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.zdebug_frame*)
  }
  .debug_str BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.debug_str)
  }
  .zdebug_str BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.zdebug_str)
  }
  .debug_loc BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.debug_loc)
  }
  .zdebug_loc BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.zdebug_loc)
  }
  .debug_macinfo BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.debug_macinfo)
  }
  .zdebug_macinfo BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.zdebug_macinfo)
  }
  /* SGI/MIPS DWARF 2 extensions.  */
  .debug_weaknames BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.debug_weaknames)
  }
  .zdebug_weaknames BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.zdebug_weaknames)
  }
  .debug_funcnames BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.debug_funcnames)
  }
  .zdebug_funcnames BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.zdebug_funcnames)
  }
  .debug_typenames BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.debug_typenames)
  }
  .zdebug_typenames BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.zdebug_typenames)
  }
  .debug_varnames BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.debug_varnames)
  }
  .zdebug_varnames BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.zdebug_varnames)
  }
  /* DWARF 3.  */
  .debug_pubtypes BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.debug_pubtypes)
  }
  .zdebug_pubtypes BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.zdebug_pubtypes)
  }
  .debug_ranges BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.debug_ranges)
  }
  .zdebug_ranges BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.zdebug_ranges)
  }
  /* DWARF 4.  */
  .debug_types BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.debug_types .gnu.linkonce.wt.*)
  }
  .zdebug_types BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.zdebug_types .gnu.linkonce.wt.*)
  }
  /* DWARF 5.  */
  .debug_addr BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.debug_addr)
  }
  .zdebug_addr BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.zdebug_addr)
  }
  .debug_line_str BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.debug_line_str)
  }
  .zdebug_line_str BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.zdebug_line_str)
  }
  .debug_loclists BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.debug_loclists)
  }
  .zdebug_loclists BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.zdebug_loclists)
  }
  .debug_macro BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.debug_macro)
  }
  .zdebug_macro BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.zdebug_macro)
  }
  .debug_names BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.debug_names)
  }
  .zdebug_names BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.zdebug_names)
  }
  .debug_rnglists BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.debug_rnglists)
  }
  .zdebug_rnglists BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.zdebug_rnglists)
  }
  .debug_str_offsets BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.debug_str_offsets)
  }
  .zdebug_str_offsets BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.zdebug_str_offsets)
  }
  .debug_sup BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.debug_sup)
  }
  /* For Go and Rust.  */
  .debug_gdb_scripts BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.debug_gdb_scripts)
  }
  .zdebug_gdb_scripts BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.zdebug_gdb_scripts)
  }
}
