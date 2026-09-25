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
 * itself.
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

/* FAIL CLOSED WHERE THE FEATURE IS OFF, AND WHERE THE BLOCK WOULD NOT FIT.
 *
 * At KICKOS_TLS=0 the template above still COLLECTS .tdata/.tbss, so nothing is orphaned.
 * Only ARM then fails, and only by accident: __aeabi_read_tp goes undefined. rv32imac resolves
 * a TPREL access against tp with no undefined symbol at all, and rx-elf satisfies
 * ___emutls_get_address out of libgcc's process-global emutls, so both would link a
 * thread_local every thread shares. The emptiness is asserted instead.
 *
 * At KICKOS_TLS=1 a template no default stack can carry would have every spawn refused.
 *
 * EMPTINESS IS DECIDED ON THE SIZES AND THE FIT ON THE SPAN, the same split kernel/thread/
 * tls.cc makes: with no thread_local anywhere the two symbols are not a span at all.
 *
 * ROUNDED, AND STRICTLY LESS, because that is what tls_stack_admissible asks: `size > block`
 * with block = align_up(TCB + span, KICKOS_STACK_ALIGN). An unrounded `<=` admits a payload
 * that rounds up to exactly the stride, which links and is then refused at every spawn.
 *
 * A DEFAULT STACK PAYS THE CARVE ON TOP OF ITS FLOOR, as a spawn charges a caller's stack: the
 * user and root stacks keep KICKOS_MIN_STACK_SIZE above their block, and where the thread
 * pointer is SEATED so does idle's stack keep the arch's KICKOS_ARCH_IDLE_FLOOR
 * (<kickos/arch/idle_floor.h>), or the thread overruns its stack on its first interrupt. With
 * no thread_local the carve is the control block alone, under KICKOS_REENT_IN_TCB. A masking
 * arch refuses idle's stack and carves nothing.
 */
#define KICKOS_TLS_CARVE_ASSERT(size, floor, tls_msg, tcb_msg)                 \
    ASSERT(SIZEOF(.tdata) + SIZEOF(.tbss) == 0                                \
               || (size) >= (floor)                                           \
                      + ALIGN(__kickos_tbss_end - __kickos_tdata_start        \
                                  + KICKOS_ARCH_TLS_TCB,                      \
                              KICKOS_STACK_ALIGN),                            \
           tls_msg)                                                           \
    ASSERT(SIZEOF(.tdata) + SIZEOF(.tbss) != 0                                \
               || (size) >= (floor)                                           \
                      + KICKOS_REENT_IN_TCB                                   \
                            * ALIGN(KICKOS_ARCH_TLS_TCB, KICKOS_STACK_ALIGN), \
           tcb_msg)

#if defined(KICKOS_TLS) && KICKOS_TLS && !KICKOS_TLS_FROM_SP
#include <kickos/arch/idle_floor.h>
#define KICKOS_TLS_IDLE_ASSERT()                                              \
    KICKOS_TLS_CARVE_ASSERT(KICKOS_IDLE_STACK_SIZE, KICKOS_ARCH_IDLE_FLOOR,   \
           "KickOS: KICKOS_IDLE_STACK_SIZE cannot hold idle's thread-local block (the __kickos_tdata_start..__kickos_tbss_end template plus KICKOS_ARCH_TLS_TCB) above KICKOS_ARCH_IDLE_FLOOR, so idle would overrun its stack on its first interrupt. Declare fewer or smaller thread_local objects, or raise KICKOS_IDLE_STACK_SIZE in boards/<board>/configs/<variant>/defconfig.", \
           "KickOS: KICKOS_IDLE_STACK_SIZE cannot hold idle's TLS control block above KICKOS_ARCH_IDLE_FLOOR, so idle would overrun its stack on its first interrupt. Raise KICKOS_IDLE_STACK_SIZE in boards/<board>/configs/<variant>/defconfig.")
#else
#define KICKOS_TLS_IDLE_ASSERT()
#endif

#if defined(KICKOS_TLS) && KICKOS_TLS
#define KICKOS_TLS_FIT_ASSERT()                                               \
    ASSERT(SIZEOF(.tdata) + SIZEOF(.tbss) == 0                                \
               || ALIGN(__kickos_tbss_end - __kickos_tdata_start              \
                            + KICKOS_ARCH_TLS_TCB,                            \
                        KICKOS_STACK_ALIGN) < KICKOS_TLS_STRIDE,              \
           "KickOS: the thread_local template plus the ABI bias below the thread pointer does not fit one KICKOS_TLS_STRIDE, so no thread's block can hold it and every spawn would be refused. Declare fewer or smaller thread_local objects, or raise this board's stack size (which is the stride) in boards/<board>/configs/<variant>/defconfig.") \
    KICKOS_TLS_CARVE_ASSERT(KICKOS_USER_STACK_SIZE, KICKOS_MIN_STACK_SIZE,    \
           "KickOS: KICKOS_USER_STACK_SIZE cannot hold a thread's thread-local block (the __kickos_tdata_start..__kickos_tbss_end template plus KICKOS_ARCH_TLS_TCB) above KICKOS_MIN_STACK_SIZE, so a thread on the default stack runs below the arch's syscall stack floor. Declare fewer or smaller thread_local objects, or raise KICKOS_USER_STACK_SIZE in boards/<board>/configs/<variant>/defconfig.", \
           "KickOS: KICKOS_USER_STACK_SIZE cannot hold a thread's TLS control block above KICKOS_MIN_STACK_SIZE, so a thread on the default stack runs below the arch's syscall stack floor. Raise KICKOS_USER_STACK_SIZE in boards/<board>/configs/<variant>/defconfig.") \
    KICKOS_TLS_CARVE_ASSERT(KICKOS_ROOT_STACK_SIZE, KICKOS_MIN_STACK_SIZE,    \
           "KickOS: KICKOS_ROOT_STACK_SIZE cannot hold root's thread-local block (the __kickos_tdata_start..__kickos_tbss_end template plus KICKOS_ARCH_TLS_TCB) above KICKOS_MIN_STACK_SIZE, so root runs below the arch's syscall stack floor. Declare fewer or smaller thread_local objects, or raise KICKOS_ROOT_STACK_SIZE in boards/<board>/configs/<variant>/defconfig.", \
           "KickOS: KICKOS_ROOT_STACK_SIZE cannot hold root's TLS control block above KICKOS_MIN_STACK_SIZE, so root runs below the arch's syscall stack floor. Raise KICKOS_ROOT_STACK_SIZE in boards/<board>/configs/<variant>/defconfig.") \
    KICKOS_TLS_IDLE_ASSERT()
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
 * The rxv3 chip script must invoke it: arch/CMakeLists.txt requires it there.
 */
#if defined(KICKOS_TLS) && KICKOS_TLS
#define KICKOS_TLS_EMUTLS_ASSERT(start, end) /* the override answers the calls */
#else
#define KICKOS_TLS_EMUTLS_ASSERT(start, end)                                  \
    ASSERT(end == start,                                                      \
           "KickOS: this image declares a thread_local with KICKOS_TLS=n, and on this arch that is an emutls control block libgcc's single-threaded emutls would answer, handing every thread the same object. Set KICKOS_TLS=y in boards/<board>/configs/<variant>/defconfig, or remove the thread_local.")
#endif

/* On a variant 1 arch the ABI bias below the thread pointer is
 * align_up(KICKOS_ARCH_TLS_TCB, tls_align), not the constant the header states: an object
 * needing 16-byte alignment moves the first thread_local from tp+8 to tp+16 and every offset
 * with it. That alignment is known to the linker and not to C, so the case is refused here
 * rather than derived at runtime.
 *
 * Guarded on SIZEOF because ALIGNOF of an absent output section is not meaningful.
 */
#define KICKOS_TLS_ALIGN_ASSERT()                                             \
    ASSERT(SIZEOF(.tdata) == 0 || ALIGNOF(.tdata) <= 8,                       \
           "KickOS: a thread_local needs more than 8-byte alignment, which moves the ABI bias below the thread pointer and every TLS offset with it. KICKOS_ARCH_TLS_TCB states a fixed bias, so this is refused rather than mis-seated.")    \
    ASSERT(SIZEOF(.tbss) == 0 || ALIGNOF(.tbss) <= 8,                         \
           "KickOS: a thread_local needs more than 8-byte alignment, which moves the ABI bias below the thread pointer and every TLS offset with it. KICKOS_ARCH_TLS_TCB states a fixed bias, so this is refused rather than mis-seated.")

/* KICKOS_LD_C_SYM spells a C identifier the way the backend's psABI does. rx-elf prepends one
 * underscore to every C name, so the rxv3 chip script defines this before including this file.
 */
#ifndef KICKOS_LD_C_SYM
#define KICKOS_LD_C_SYM(name) name
#endif

/* THE WINDOWS A CHIP DOES NOT CARVE, STATED EMPTY.
 *
 * Every bound below is referenced STRONGLY (include/kickos/klink.h), so a script that states
 * none fails the link naming the symbol, and a weak reference would make absent read as an
 * empty window (docs/reference/invariants.md, ctors-run-before-init-entry). Every reader tests
 * end > start, so a pair of zeros admits nothing.
 */
#define KICKOS_APP_CODE_WINDOW_NONE()                                         \
    KICKOS_LD_C_SYM(__kickos_code_start) = 0;                                 \
    KICKOS_LD_C_SYM(__kickos_code_end) = 0;

#define KICKOS_APP_DATA_WINDOW_NONE()                                         \
    KICKOS_LD_C_SYM(__kickos_appdata_start) = 0;                              \
    KICKOS_LD_C_SYM(__kickos_appdata_end) = 0;

/* An image no loader splits: no app-only rom/sram window, and an app virtual address already
 * names the frame it was loaded into, so the delta is zero.
 */
#define KICKOS_APP_IMAGE_SPLIT_NONE()                                         \
    KICKOS_LD_C_SYM(__kickos_app_rom_start) = 0;                              \
    KICKOS_LD_C_SYM(__kickos_app_rom_end) = 0;                                \
    KICKOS_LD_C_SYM(__kickos_app_sram_start) = 0;                             \
    KICKOS_LD_C_SYM(__kickos_app_sram_end) = 0;                               \
    KICKOS_LD_C_SYM(__kickos_app_load_delta) = 0;

#endif
