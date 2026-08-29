/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The PE32+ section layout for every x86_64 image on this board.
 *
 * WITHOUT THIS FILE A FULL IMAGE LINKS CLEAN AND FIRMWARE REFUSES TO LOAD IT. `ld -m i386pep`
 * collects *(.text.*) but names no wildcard for .data.*, .rodata or .bss.*, and this arch
 * compiles -ffunction-sections -fdata-sections, so each becomes a page-aligned PE section of
 * its own; past about seventy sections SizeOfHeaders passes 0x1000 and EDK II's loader refuses
 * a section whose virtual address lies inside the headers. It presents as `failed to load ...:
 * Unsupported` with nothing on the serial line. --strip-debug does not fix it.
 *
 * Everything but three wildcards (.data.* into .data, .rodata and .rodata.* into .rdata,
 * .bss.* into .bss) is `ld -m i386pep --verbose` VERBATIM, so a binutils whose own layout
 * moves is a diff against a regenerated dump.
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
  /* Make the virtual address and file offset synced if the alignment is
     lower than the target page size. */
  . = SIZEOF_HEADERS;
  . = ALIGN(__section_alignment__);
  .text  __image_base__ + ( __section_alignment__ < 0x1000 ? . : __section_alignment__ ) :
  {
    KEEP (*(SORT_NONE(.init)))
    *(.text)
    *(SORT(.text$*))
     *(.text.*)
     *(.gnu.linkonce.t.*)
    *(.glue_7t)
    *(.glue_7)
    . = ALIGN(8);
    KEEP (*(SORT_NONE(.fini)))
    /* ??? Why is .gcc_exc here?  */
     *(.gcc_exc)
    PROVIDE (etext = .);
     KEEP (*(.gcc_except_table))
  }
  /* The Cygwin32 library uses a section to avoid copying certain data
     on fork.  This used to be named ".data".  The linker used
     to include this between __data_start__ and __data_end__, but that
     breaks building the cygwin32 dll.  Instead, we name the section
     ".data_cygwin_nocopy" and explicitly include it after __data_end__. */
  .data BLOCK(__section_alignment__) :
  {
    __data_start__ = . ;
    *(.data)
    *(.data.*)
    *(.data2)
    *(SORT(.data$*))
    KEEP(*(.jcr))
    __data_end__ = . ;
    *(.data_cygwin_nocopy)
  }
  /* The global-ctor split. The section name is EIGHT characters: a PE image stores a section
     name inline in eight bytes, and a longer one needs a string table an image may not carry.

     .init_array keeps ONLY the closed KickOS-owned archive set, which the boot tail
     (arch/x86/x86_64/landed_kernel_x86_64.cc) walks before kmain; everything else lands in
     the app window and runs from kickos_root_entry with the kernel live.
     tests/static/check_kernel_ctor_placement.sh is the gate on the same rule for the enforced
     armv7m boards. */
  .kosinit BLOCK(__section_alignment__) :
  {
    . = ALIGN(8);
    __init_array_start = . ;
    KEEP (*libkickos_kernel.a:*(SORT(.init_array.*) .init_array))
    KEEP (*libkickos_arch_x86_64.a:*(SORT(.init_array.*) .init_array))
    KEEP (*libkickos_chip_q35.a:*(SORT(.init_array.*) .init_array))
    KEEP (*libkickos_lib.a:*(SORT(.init_array.*) .init_array))
    __init_array_end = . ;
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
  }
  .rdata BLOCK(__section_alignment__) :
  {
    *(.rdata)
    *(.rodata)
    *(.rodata.*)
	     *(SORT(.rdata$*))
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
       /* Note: we always define __CTOR_LIST__ and ___CTOR_LIST__ here,
          we do not PROVIDE them.  This is because the ctors.o startup
	  code in libgcc defines them as common symbols, with the
          expectation that they will be overridden by the definitions
	  here.  If we PROVIDE the symbols then they will not be
	  overridden and global constructors will not be run.
	  See PR 22762 for more details.

	  This does mean that it is not possible for a user to define
	  their own __CTOR_LIST__ and __DTOR_LIST__ symbols; if they do,
	  the content from those variables are included but the symbols
	  defined here silently take precedence.  If they truly need to
	  be redefined, a custom linker script will have to be used.
	  (The custom script can just be a copy of this script with the
	  PROVIDE() qualifiers added).
	  In particular this means that ld -Ur does not work, because
	  the proper __CTOR_LIST__ set by ld -Ur is overridden by a
	  bogus __CTOR_LIST__ set by the final link.  See PR 46.  */
       ___CTOR_LIST__ = .;
       __CTOR_LIST__ = .;
       LONG (-1); LONG (-1);
       KEEP (*(.ctors));
       KEEP (*(.ctor));
       KEEP (*(SORT_BY_NAME(.ctors.*)));
       LONG (0); LONG (0);
       /* See comment about __CTOR_LIST__ above.  The same reasoning
          applies here too.  */
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
  .eh_frame BLOCK(__section_alignment__) :
  {
    KEEP (*(.eh_frame*))
  }
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
    *(.bss)
    *(.bss.*)
    *(COMMON)
    __bss_end__ = . ;
  }
  .edata BLOCK(__section_alignment__) :
  {
    *(.edata)
  }
  /DISCARD/ :
  {
    *(.debug$S)
    *(.debug$T)
    *(.debug$F)
     *(.drectve)
     *(.note.GNU-stack)
     *(.gnu.lto_*)
     *(.gnu_object_only)
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
  /* Windows TLS expects .tls$AAA to be at the start and .tls$ZZZ to be
     at the end of the .tls section.  This is important because _tls_start MUST
     be at the beginning of the section to enable SECREL32 relocations with TLS
     data.  */
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
    /* end is deprecated, don't use it */
    PROVIDE (end = .);
    PROVIDE ( _end = .);
     __end__ = .;
    /* Every window is STATED here: `ld -m i386pep` builds no global offset table, so a
       weak-undefined reference survives as an indirect LOAD whose displacement resolves to
       the symbol itself. The kernel declares each of these strong
       (kernel/include/kickos/klink.h), and an empty window is start == end. */
    . = ALIGN(8);
    _kickos_heap_start = . ;
    _kickos_heap_limit = . ;
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
  .stab BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.stab)
  }
  .stabstr BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.stabstr)
  }
  /* DWARF debug sections.
     Symbols in the DWARF debugging sections are relative to the beginning
     of the section.  Unlike other targets that fake this by putting the
     section VMA at 0, the PE format will not allow it.  */
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
