/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * NOT a C header: these macros expand to GNU ld statements. Included from every chip
 * linker script, which arch/CMakeLists.txt runs through cpp before the link.
 */
#ifndef KICKOS_ARCH_COMMON_SECTIONS_LD_H
#define KICKOS_ARCH_COMMON_SECTIONS_LD_H

/* Every image links with --orphan-handling=error, so an input section no rule names is a
 * link failure that prints the section. Before adding one here, check it is toolchain
 * debris: a section that carries content belongs in a region, named where a reader of the
 * chip script can see where it lands.
 *
 * rx-elf 14.2 emits DWARF 4 (.debug_loc, .debug_ranges) and the other four backends emit
 * DWARF 5 (.debug_loclists, .debug_rnglists). Both sets are named because one macro serves
 * all five.
 *
 * A static link consumes its relocations and .kickos_relocs comes out empty on every board
 * the fleet builds; riscv32-none-elf still offers the input sections to the script, and
 * with no rule ld would place them in a .rela.dyn the output does not keep. Named and
 * ASSERTed empty rather than discarded, so a link that ever does keep one fails loudly
 * instead of dropping relocations nothing would apply. KICKOS_CODE_DEBRIS_SECTIONS claims
 * .rel.iplt and .rela.iplt first, being invoked inside the code section above this.
 */
#define KICKOS_NONALLOC_SECTIONS                                              \
    .comment 0 : { *(.comment) }                                              \
    .note.GNU-stack 0 : { *(.note.GNU-stack) }                                \
    .debug_abbrev 0 : { *(.debug_abbrev) }                                    \
    .debug_addr 0 : { *(.debug_addr) }                                        \
    .debug_aranges 0 : { *(.debug_aranges) }                                  \
    .debug_frame 0 : { *(.debug_frame) }                                      \
    .debug_info 0 : { *(.debug_info .gnu.linkonce.wi.*) }                     \
    .debug_line 0 : { *(.debug_line .debug_line.* .debug_line_end) }          \
    .debug_line_str 0 : { *(.debug_line_str) }                                \
    .debug_loc 0 : { *(.debug_loc) }                                          \
    .debug_loclists 0 : { *(.debug_loclists) }                                \
    .debug_macro 0 : { *(.debug_macro) }                                      \
    .debug_names 0 : { *(.debug_names) }                                      \
    .debug_pubnames 0 : { *(.debug_pubnames) }                                \
    .debug_pubtypes 0 : { *(.debug_pubtypes) }                                \
    .debug_ranges 0 : { *(.debug_ranges) }                                    \
    .debug_rnglists 0 : { *(.debug_rnglists) }                                \
    .debug_str 0 : { *(.debug_str) }                                          \
    .debug_str_offsets 0 : { *(.debug_str_offsets) }                          \
    .debug_sup 0 : { *(.debug_sup) }                                          \
    .debug_types 0 : { *(.debug_types) }                                      \
    .kickos_relocs 0 :                                                        \
    {                                                                         \
        __kickos_relocs_start = .;                                            \
        *(.rel.*) *(.rela.*)                                                  \
        __kickos_relocs_end = .;                                              \
    }

/* Invoked INSIDE the output section that holds code. ld synthesises all of these from no
 * input object (ARM call veneers and ifunc machinery), so they belong wherever the code
 * went; they are empty on every board the fleet builds and are named so the gate has a
 * rule. The EH tables are NOT here: those carry bytes, and each chip script homes them
 * itself in the region its code grant covers.
 */
#define KICKOS_CODE_DEBRIS_SECTIONS                                           \
    *(.glue_7) *(.glue_7t) *(.vfp11_veneer) *(.v4_bx)                         \
    *(.iplt) *(.igot.plt) *(.rel.iplt) *(.rela.iplt)

#define KICKOS_STATIC_RELOC_ASSERT()                                          \
    ASSERT(__kickos_relocs_end == __kickos_relocs_start,                      \
           "KickOS: the link kept dynamic relocations, which nothing in a KickOS image applies; a PIE or shared-object flag reached the link")

/* The PT_TLS template: the bytes each thread's TLS block is initialised FROM, not the block
 * itself. Every thread gets its own copy carved off the low end of its stack, so this is
 * link-time constant data and lives in the region the script hands it.
 *
 * .tbss MUST immediately follow .tdata. ld hard-errors on a non-adjacent pair, and the
 * offsets the compiler computes are relative to the two laid out back to back.
 *
 * ALIGN(8) puts the template start on a word pair wherever the region leaves it, which the
 * copy into each block reads from. It does NOT set the per-thread block's alignment: the
 * carve does that.
 *
 * With no thread_local anywhere both sections come out empty, the two sizes are zero, and
 * the carve is skipped. Empty, they cost nothing: every allocated section of every image on
 * every preset keeps the address and size it had before this macro existed.
 */
#define KICKOS_TLS_TEMPLATE(region)                                           \
    .tdata : ALIGN(8)                                                         \
    {                                                                         \
        __kickos_tdata_start = .;                                             \
        *(.tdata .tdata.* .gnu.linkonce.td.*)                                 \
        __kickos_tdata_end = .;                                               \
    } > region                                                                \
    .tbss (NOLOAD) :                                                          \
    {                                                                         \
        __kickos_tbss_start = .;                                              \
        *(.tbss .tbss.* .gnu.linkonce.tb.*) *(.tcommon)                       \
        __kickos_tbss_end = .;                                                \
    } > region

#endif
