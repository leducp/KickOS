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
 * debris: a section that carries content belongs in a region named in the chip script.
 *
 * rx-elf 14.2 emits DWARF 4 (.debug_loc, .debug_ranges) and the other four backends DWARF 5
 * (.debug_loclists, .debug_rnglists). One macro serves all five, so both sets are named.
 *
 * .kickos_relocs comes out empty on every board the fleet builds, riscv32-none-elf still
 * offering the input sections to the script. Named and ASSERTed empty rather than discarded,
 * so a link that ever does keep one fails loudly instead of dropping relocations nothing
 * would apply. KICKOS_CODE_DEBRIS_SECTIONS claims .rel.iplt and .rela.iplt first, being
 * invoked inside the code section above this.
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
 * went. The EH tables are NOT here: those carry bytes, and each chip script homes them in
 * the region its code grant covers.
 */
#define KICKOS_CODE_DEBRIS_SECTIONS                                           \
    *(.glue_7) *(.glue_7t) *(.vfp11_veneer) *(.v4_bx)                         \
    *(.iplt) *(.igot.plt) *(.rel.iplt) *(.rela.iplt)

#define KICKOS_STATIC_RELOC_ASSERT()                                          \
    ASSERT(__kickos_relocs_end == __kickos_relocs_start,                      \
           "KickOS: the link kept dynamic relocations, which nothing in a KickOS image applies; a PIE or shared-object flag reached the link")

/* The PT_TLS template: the bytes each thread's TLS block is initialised FROM, not the block
 * itself. Every thread gets its own copy carved off the low end of its stack, so this is
 * link-time constant data.
 *
 * .tbss MUST immediately follow .tdata. ld hard-errors on a non-adjacent pair, and the
 * offsets the compiler computes are relative to the two laid out back to back.
 *
 * ALIGN(8) aligns the TEMPLATE the copy reads from; the per-thread block's alignment comes
 * from the carve and not from here.
 *
 * With no thread_local anywhere both sections come out empty and the carve is skipped.
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

/* On a variant 1 arch the ABI bias below the thread pointer is
 * align_up(KICKOS_ARCH_TLS_TCB, tls_align), not the constant the header states: an object
 * needing 16-byte alignment moves the first thread_local from tp+8 to tp+16 and every offset
 * with it. That alignment is known to the linker and not to C, so the case is refused here
 * rather than derived at runtime.
 *
 * Guarded on SIZEOF because ALIGNOF of an absent output section is not meaningful.
 */
/* FAIL CLOSED WHERE THE FEATURE IS OFF, AND WHERE THE BLOCK WOULD NOT FIT.
 *
 * At KICKOS_TLS=0 the template above still COLLECTS .tdata/.tbss, being invoked
 * unconditionally, so nothing is orphaned and --orphan-handling=error stays quiet. Only ARM
 * then fails, and only by accident: __aeabi_read_tp goes undefined. rv32imac resolves a TPREL
 * access against tp with no undefined symbol at all, and rx-elf satisfies
 * ___emutls_get_address out of libgcc's process-global emutls. Both would link an image whose
 * thread_local is shared by every thread. So the emptiness is asserted here instead.
 *
 * At KICKOS_TLS=1 the carve gives each thread exactly one stride and seats the block at its
 * base, so a template larger than a stride minus the ABI bias cannot be seated. That is a
 * link-time fact and was a boot-time kpanic.
 *
 * EMPTINESS IS DECIDED ON THE SIZES AND THE FIT ON THE SPAN, the same split kernel/thread/
 * tls.cc makes. With no thread_local anywhere the two symbols are not a span at all and their
 * difference is meaningless, so testing it alone refuses every image that declares none.
 *
 * ROUNDED, AND STRICTLY LESS, because that is what tls_stack_admissible asks: it compares the
 * stride against tls_block_size(), which is align_up(TCB + span, KICKOS_STACK_ALIGN), with
 * `size > block`. An unrounded `<=` here admits a payload that rounds up to exactly the
 * stride, which links and is then refused at every spawn.
 */
#if defined(KICKOS_TLS) && KICKOS_TLS
#define KICKOS_TLS_FIT_ASSERT()                                               \
    ASSERT(SIZEOF(.tdata) + SIZEOF(.tbss) == 0                                \
               || ALIGN(__kickos_tbss_end - __kickos_tdata_start              \
                            + KICKOS_ARCH_TLS_TCB,                            \
                        KICKOS_STACK_ALIGN) < KICKOS_TLS_STRIDE,              \
           "KickOS: the thread_local template plus the ABI bias below the thread pointer does not fit one KICKOS_TLS_STRIDE, so no thread's block can hold it and every spawn would be refused. Declare fewer or smaller thread_local objects, or raise this board's stack size (which is the stride) in boards/<board>/configs/<variant>/defconfig.")
#else
#define KICKOS_TLS_FIT_ASSERT()                                               \
    ASSERT(SIZEOF(.tdata) == 0,                                               \
           "KickOS: this image declares a thread_local with KICKOS_TLS=n, and on this arch that links silently into storage every thread shares. Set KICKOS_TLS=y in boards/<board>/configs/<variant>/defconfig, or remove the thread_local.")   \
    ASSERT(SIZEOF(.tbss) == 0,                                                \
           "KickOS: this image declares a thread_local with KICKOS_TLS=n, and on this arch that links silently into storage every thread shares. Set KICKOS_TLS=y in boards/<board>/configs/<variant>/defconfig, or remove the thread_local.")
#endif

/* THE SAME FAIL-CLOSED QUESTION WHERE THE ARCH HAS NO TLS SECTIONS AT ALL. GNURX emits
 * neither .tdata nor .tbss: a thread_local becomes an emutls control block in
 * .data.__emutls_v.* plus a call to ___emutls_get_address, so the assert above reads two
 * empty sections and passes while libgcc's single-threaded emutls answers the call and hands
 * every thread the same object. The chip script gathers those control blocks between two
 * symbols for its own override to index; at KICKOS_TLS=0 that span must be empty.
 *
 * Invoked by the rxv3 chip script only, and arch/CMakeLists.txt requires it there.
 */
#if defined(KICKOS_TLS) && KICKOS_TLS
#define KICKOS_TLS_EMUTLS_ASSERT(start, end) /* the override answers the calls */
#else
#define KICKOS_TLS_EMUTLS_ASSERT(start, end)                                  \
    ASSERT(end == start,                                                      \
           "KickOS: this image declares a thread_local with KICKOS_TLS=n, and on this arch that is an emutls control block libgcc's single-threaded emutls would answer, handing every thread the same object. Set KICKOS_TLS=y in boards/<board>/configs/<variant>/defconfig, or remove the thread_local.")
#endif

#define KICKOS_TLS_ALIGN_ASSERT()                                             \
    ASSERT(SIZEOF(.tdata) == 0 || ALIGNOF(.tdata) <= 8,                       \
           "KickOS: a thread_local needs more than 8-byte alignment, which moves the ABI bias below the thread pointer and every TLS offset with it. KICKOS_ARCH_TLS_TCB states a fixed bias, so this is refused rather than mis-seated.")    \
    ASSERT(SIZEOF(.tbss) == 0 || ALIGNOF(.tbss) <= 8,                         \
           "KickOS: a thread_local needs more than 8-byte alignment, which moves the ABI bias below the thread pointer and every TLS offset with it. KICKOS_ARCH_TLS_TCB states a fixed bias, so this is refused rather than mis-seated.")

#endif
