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

/* Reset_Handler walks the privileged .init_array before kmain, inside the kernel, and the
 * chip script fills it with the closed KickOS-owned archive set alone. Those archives' statics
 * are constinit, so the window is empty, and an app constructor never runs there.
 */
#define KICKOS_KERNEL_CTORS_NONE_ASSERT()                                     \
    ASSERT(__init_array_end == __init_array_start,                            \
           "KickOS: the privileged .init_array holds a constructor. The KickOS-owned archives' statics are constinit, so make the new one constinit; an app constructor belongs in .kickos_app_init_array, which root_entry runs")

#define KICKOS_STATIC_RELOC_ASSERT()                                          \
    ASSERT(__kickos_relocs_end == __kickos_relocs_start,                      \
           "KickOS: the link kept dynamic relocations, which nothing in a KickOS image applies; a PIE or shared-object flag reached the link")

#define KICKOS_FINI_EMPTY_ASSERT()                                            \
    ASSERT(KICKOS_LD_C_SYM(__kickos_app_fini_array_end)                       \
               == KICKOS_LD_C_SYM(__kickos_app_fini_array_start),             \
           "KickOS: .fini_array is non-empty: a global with a non-trivial destructor? Static destructors are not supported; nothing would ever run it")

/* An opt-in floor under the newlib heap, undefined by default. */
#ifdef KICKOS_HEAP_MIN
#define KICKOS_HEAP_MIN_ASSERT()                                              \
    ASSERT(KICKOS_LD_C_SYM(_kickos_heap_limit) - KICKOS_LD_C_SYM(_kickos_heap_start) \
               >= KICKOS_HEAP_MIN,                                            \
           "KickOS: heap below KICKOS_HEAP_MIN")
#else
#define KICKOS_HEAP_MIN_ASSERT()
#endif

/* The PT_TLS template: the bytes each thread's TLS block is initialised FROM, not the block
 * itself.
 *
 * .tbss MUST immediately follow .tdata. ld hard-errors on a non-adjacent pair, and the
 * offsets the compiler computes are relative to the two laid out back to back.
 *
 * ALIGN(8) aligns the TEMPLATE the copy reads from; the per-thread block's alignment comes
 * from the carve and not from here.
 *
 * With no thread_local anywhere both sections come out empty and the carve is skipped. An
 * argument after the region is the template's AT(), where it loads elsewhere than it links.
 */
#if KICKOS_IMAGES_PLACED
/* One image per declared task beside the init's. Each image's own relocatable link places its
 * .tbss in its .tdata as zeros (cmake/image_partial.lds), so every image's thread-local storage is
 * one span of .tdata: image 1's, then each later task image's in turn, which tools/image-link.py
 * writes per link (kickos_images_tdata.ld), then the init's. Each image's span starts on 8
 * bytes, the most a thread_local may ask (KICKOS_TLS_ALIGN_ASSERT), so its offset in the segment
 * keeps every alignment of its own. .tbss is left empty.
 */
#define KICKOS_TLS_TEMPLATE(region, ...)                                      \
    .tdata : __VA_ARGS__ ALIGN(8)                                             \
    {                                                                         \
        __kickos_tdata_start = .;                                             \
        KICKOS_IMAGE1_SECTIONS(.tdata .tdata.* .gnu.linkonce.td.*)            \
        __kickos_tls1_tdata_end = .;                                          \
        INCLUDE kickos_images_tdata.ld                                        \
        . = ALIGN(8);                                                         \
        __kickos_tls0_tdata_start = .;                                        \
        KICKOS_IMAGE0_SECTIONS(.tdata .tdata.* .gnu.linkonce.td.*)            \
        __kickos_tdata_end = .;                                               \
    } > region                                                                \
    .tbss (NOLOAD) :                                                          \
    {                                                                         \
        __kickos_tbss_start = .;                                              \
        *(.tbss .tbss.* .gnu.linkonce.tb.*) *(.tcommon)                       \
        __kickos_tbss_end = .;                                                \
    } > region

/* A later task image n's span of .tdata, which kickos_images_tdata.ld holds in its turn. The
 * bounds are __kickos_tls<n>_*, apart from the __kickos_image<n>_tbss_* names an image's references
 * to the segment's own bounds take. */
#define KICKOS_IMAGE_N_TDATA(n)                                               \
    . = ALIGN(8);                                                             \
    __kickos_tls##n##_tdata_start = .;                                        \
    *kickos_image##n.o(.tdata .tdata.* .gnu.linkonce.td.*)                    \
    __kickos_tls##n##_tdata_end = .;
#else
#define KICKOS_TLS_TEMPLATE(region, ...)                                      \
    .tdata : __VA_ARGS__ ALIGN(8)                                             \
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
 * ROUNDED, AND STRICTLY LESS, because that is what tls_stack_admissible asks: `size > carve`
 * with block = align_up(TCB + span, KICKOS_STACK_ALIGN). An unrounded `<=` admits a payload
 * that rounds up to exactly the stride, which links and is then refused at every spawn.
 *
 * A POOL THREAD'S STACK PAYS KICKOS_REENT_CARVE, its libc state, below the block, and idle's
 * pays the block alone. A default stack pays the carve on top of its floor, as a spawn charges a
 * caller's stack: the user and root stacks keep KICKOS_MIN_STACK_SIZE above it, and where the
 * thread pointer is SEATED idle's stack keeps the arch's KICKOS_ARCH_IDLE_FLOOR
 * (<kickos/arch/idle_floor.h>) above its block, or the thread overruns its stack on its first
 * interrupt. kernel/init/kmain.cc holds each stack to the libc state alone, a constant. A
 * masking arch refuses idle's stack and carves nothing.
 *
 * EACH STACK PAYS ITS OWN IMAGE'S BLOCK: root's and idle's image 0's, a thread's of a task image
 * that image's. Where each image is linked on its own first, image n's block is sized from its
 * own __kickos_image<n>_tls_total, and the image template's asserts part holds the stride and the
 * user stack to it (KICKOS_TLS_IMAGE_FIT_ASSERT); elsewhere the link holds one image, the whole
 * segment. A task program's own link (__kickos_task_program, tools/image-link.py) holds no root
 * nor idle.
 *
 * ONE PREDICATE carries the carve, and a stack with no room at all is asserted refused by it,
 * so a term dropped from it fails every link. Idle's assert defines __kickos_tls_idle_checked,
 * which the link requires where the thread pointer is seated.
 */
#define KICKOS_TLS_BLOCK_OF(total)                                            \
    (((total) != 0) * ALIGN((total) + KICKOS_ARCH_TLS_TCB, KICKOS_STACK_ALIGN))
#define KICKOS_TLS_BLOCK                                                      \
    ((SIZEOF(.tdata) + SIZEOF(.tbss) != 0)                                    \
         * ALIGN(__kickos_tbss_end - __kickos_tdata_start + KICKOS_ARCH_TLS_TCB, \
                 KICKOS_STACK_ALIGN))
#if KICKOS_IMAGES_PLACED
#define KICKOS_TLS_IMAGE_BLOCK(n) KICKOS_TLS_BLOCK_OF(KICKOS_LD_IMAGE_SYM(n, tls_total))
#else
#define KICKOS_TLS_IMAGE_BLOCK(n) KICKOS_TLS_BLOCK
#endif
#define KICKOS_TLS_CLEARS(size, floor, reent, block)                          \
    ((size) >= (floor) + (reent) + (block))
#define KICKOS_TLS_CARVE_ASSERT(size, floor, reent, block, tls_msg)           \
    ASSERT(KICKOS_TLS_CLEARS(size, floor, reent, block), tls_msg)
#define KICKOS_TLS_ROOT_CARVE_ASSERT(size, floor, reent, tls_msg)             \
    ASSERT(DEFINED(__kickos_task_program)                                     \
               || KICKOS_TLS_CLEARS(size, floor, reent, KICKOS_TLS_IMAGE_BLOCK(0)), tls_msg)

#if KICKOS_TLS && !KICKOS_TLS_FROM_SP
#include <kickos/arch/idle_floor.h>
#define KICKOS_TLS_IDLE_ASSERT()                                              \
    KICKOS_LD_C_SYM(__kickos_tls_idle_checked) = 1;                           \
    KICKOS_TLS_ROOT_CARVE_ASSERT(KICKOS_IDLE_STACK_SIZE, KICKOS_ARCH_IDLE_FLOOR, 0, \
           "KickOS: KICKOS_IDLE_STACK_SIZE cannot hold idle's thread-local block (image 0's thread_local template plus KICKOS_ARCH_TLS_TCB) above KICKOS_ARCH_IDLE_FLOOR, so idle would overrun its stack on its first interrupt. Declare fewer or smaller thread_local objects, or raise KICKOS_IDLE_STACK_SIZE in boards/<board>/configs/<variant>/defconfig.")
#else
#define KICKOS_TLS_IDLE_ASSERT()
#endif

/* __kickos_tls_carve: the most a pool thread's stack pays, its libc state and the thread-local
 * block of the whole segment, as KICKOS_TLS_CARVE_ASSERT counts it. Image n's own,
 * __kickos_image<n>_tls_carve, is what a thread of image n pays; a system target's asserts read
 * those. A comparison is 0 or 1.
 */
#if KICKOS_TLS
#define KICKOS_TLS_CARVE_SYMBOL()                                             \
    KICKOS_LD_C_SYM(__kickos_tls_carve) = KICKOS_REENT_CARVE + KICKOS_TLS_BLOCK;
#define KICKOS_TLS_CARVE_OF(n) (KICKOS_REENT_CARVE + KICKOS_TLS_IMAGE_BLOCK(n))
#else
#define KICKOS_TLS_CARVE_SYMBOL()                                             \
    KICKOS_LD_C_SYM(__kickos_tls_carve) = KICKOS_REENT_CARVE;
#define KICKOS_TLS_CARVE_OF(n) KICKOS_REENT_CARVE
#endif

#if KICKOS_TLS
/* Image n's stride and default user stack, each holding the libc state and image n's block. */
#define KICKOS_TLS_IMAGE_FIT_ASSERT(n)                                        \
    ASSERT(KICKOS_REENT_CARVE + KICKOS_TLS_IMAGE_BLOCK(n) < KICKOS_TLS_STRIDE, \
           "KickOS: the libc state, an image's thread_local template and the ABI bias below the thread pointer do not fit one KICKOS_TLS_STRIDE, so no thread's stack can hold them and every spawn would be refused. Declare fewer or smaller thread_local objects, or raise this board's stack size (which is the stride) in boards/<board>/configs/<variant>/defconfig.") \
    KICKOS_TLS_CARVE_ASSERT(KICKOS_USER_STACK_SIZE, KICKOS_MIN_STACK_SIZE,    \
                            KICKOS_REENT_CARVE, KICKOS_TLS_IMAGE_BLOCK(n),    \
           "KickOS: KICKOS_USER_STACK_SIZE cannot hold a thread's libc state and its image's thread-local block (its thread_local template plus KICKOS_ARCH_TLS_TCB) above KICKOS_MIN_STACK_SIZE, so a thread on the default stack runs below the arch's syscall stack floor. Declare fewer or smaller thread_local objects, or raise KICKOS_USER_STACK_SIZE in boards/<board>/configs/<variant>/defconfig.")
#if KICKOS_IMAGES_PLACED
#define KICKOS_TLS_ONE_IMAGE_FIT_ASSERT()
#else
#define KICKOS_TLS_ONE_IMAGE_FIT_ASSERT() KICKOS_TLS_IMAGE_FIT_ASSERT(0)
#endif
#define KICKOS_TLS_FIT_ASSERT()                                               \
    KICKOS_TLS_CARVE_SYMBOL()                                                 \
    ASSERT(KICKOS_REENT_CARVE + KICKOS_TLS_BLOCK == 0                         \
               || !KICKOS_TLS_CLEARS(0, 0, KICKOS_REENT_CARVE, KICKOS_TLS_BLOCK), \
           "KickOS: KICKOS_TLS_CLEARS admits a stack with no room for the libc state and the thread-local block, so no carve assert below it can fire") \
    KICKOS_TLS_ONE_IMAGE_FIT_ASSERT()                                         \
    KICKOS_TLS_ROOT_CARVE_ASSERT(KICKOS_ROOT_STACK_SIZE, KICKOS_MIN_STACK_SIZE, \
                                 KICKOS_REENT_CARVE,                          \
           "KickOS: KICKOS_ROOT_STACK_SIZE cannot hold root's libc state and thread-local block (image 0's thread_local template plus KICKOS_ARCH_TLS_TCB) above KICKOS_MIN_STACK_SIZE, so root runs below the arch's syscall stack floor. Declare fewer or smaller thread_local objects, or raise KICKOS_ROOT_STACK_SIZE in boards/<board>/configs/<variant>/defconfig.") \
    KICKOS_TLS_IDLE_ASSERT()
#else
#define KICKOS_TLS_IMAGE_FIT_ASSERT(n)
#define KICKOS_TLS_FIT_ASSERT()                                               \
    KICKOS_TLS_CARVE_SYMBOL()                                                 \
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
#if KICKOS_TLS
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
 * underscore to every C name, so the rxv3 chip script defines this before including this file,
 * in two levels so that an argument is expanded before the paste.
 */
#ifndef KICKOS_LD_C_SYM
#define KICKOS_LD_C_SYM(name) name
#endif

/* Field `f` of image n's bounds, __kickos_image<n>_<f> as C spells it. */
#define KICKOS_LD_IMAGE_SYM_(n, f) __kickos_image##n##_##f
#define KICKOS_LD_IMAGE_SYM(n, f) KICKOS_LD_C_SYM(KICKOS_LD_IMAGE_SYM_(n, f))

/* Copy and zero tables read by kickos_ranges_init from flash, before .data exists. Copy
 * triples are {src,dst,len}, zero pairs {dst,len}. The images' ranges come first, the app's
 * .appdata and, where the init is an image of its own, the init's. Each zero range covers its
 * image's whole window past the loaded data: the pre-bss alignment gap, the bss and the heap, so
 * no stale read-back is left anywhere in the window. A later task image's start stub copies and
 * zeroes its own at every start.
 */
#define KICKOS_INIT_TABLES_IMAGE_ZERO(n)                                      \
    LONG(KICKOS_LD_IMAGE_SYM(n, data_end));                                   \
    LONG(KICKOS_LD_IMAGE_SYM(n, window_end) - KICKOS_LD_IMAGE_SYM(n, data_end));
#if KICKOS_IMAGES_PLACED
#define KICKOS_INIT_TABLES_IMAGE_COPY(n)                                      \
    LONG(LOADADDR(.kickos_image##n##_data)); LONG(ADDR(.kickos_image##n##_data)); \
    LONG(SIZEOF(.kickos_image##n##_data));
#define KICKOS_INIT_TABLES_APP_COPY()                                         \
    KICKOS_INIT_TABLES_IMAGE_COPY(1)                                          \
    KICKOS_INIT_TABLES_IMAGE_COPY(0)
#define KICKOS_INIT_TABLES_APP_ZERO()                                         \
    KICKOS_INIT_TABLES_IMAGE_ZERO(1)                                          \
    KICKOS_INIT_TABLES_IMAGE_ZERO(0)
#else
#define KICKOS_INIT_TABLES_APP_COPY()                                         \
    LONG(LOADADDR(.appdata)); LONG(ADDR(.appdata)); LONG(SIZEOF(.appdata));
#define KICKOS_INIT_TABLES_APP_ZERO() KICKOS_INIT_TABLES_IMAGE_ZERO(0)
#endif

#define KICKOS_INIT_TABLES(region)                                            \
    .kickos_init_tables : ALIGN(4)                                            \
    {                                                                         \
        KICKOS_LD_C_SYM(__kickos_copy_table_start) = .;                       \
        KICKOS_INIT_TABLES_APP_COPY()                                         \
        LONG(LOADADDR(.data)); LONG(ADDR(.data)); LONG(SIZEOF(.data));        \
        KICKOS_LD_C_SYM(__kickos_copy_table_end) = .;                         \
        KICKOS_LD_C_SYM(__kickos_zero_table_start) = .;                       \
        KICKOS_INIT_TABLES_APP_ZERO()                                         \
        LONG(ADDR(.bss)); LONG(SIZEOF(.bss));                                 \
        KICKOS_LD_C_SYM(__kickos_zero_table_end) = .;                         \
    } > region

/* THE WINDOWS A CHIP DOES NOT CARVE, STATED EMPTY.
 *
 * Every bound below is referenced STRONGLY (include/kickos/klink.h), so a script that states
 * none fails the link naming the symbol, and a weak reference would make absent read as an
 * empty window (docs/reference/invariants.md, ctors-run-before-their-image-entry). Every reader tests
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

/* THE IMAGES. Each input section is the kernel's or an image's. Where every image is linked on
 * its own first (KICKOS_IMAGE_SPLIT), each image is the one object tools/image-link.py wrote for
 * it and the kernel is every other input: image 0 the init's and image k + 1 declaration k's
 * where the init is an image of its own (KICKOS_IMAGES_PLACED), image 0 the whole app's otherwise.
 * Without the split image 0 is every input outside the kernel's closed set of archives, which is
 * then selected first. KICKOS_APP_IMAGE_N is image 1, the first declaration's, or the one image,
 * KICKOS_APP_SECTIONS its inputs.
 *
 * EXCLUDE_FILE in front of the pattern, never inside its section list: there it binds only the
 * one section name that follows it.
 */
#if KICKOS_IMAGES_PLACED
#define KICKOS_IMAGE0_SECTIONS(...) *kickos_image0.o(__VA_ARGS__)
#define KICKOS_IMAGE1_SECTIONS(...) *kickos_image1.o(__VA_ARGS__)
#define KICKOS_KERNEL_SECTIONS(...) EXCLUDE_FILE(*kickos_image*.o) *(__VA_ARGS__)
#define KICKOS_APP_IMAGE_N 1
#define KICKOS_APP_SECTIONS KICKOS_IMAGE1_SECTIONS
#elif KICKOS_IMAGE_SPLIT
#define KICKOS_IMAGE0_SECTIONS(...) *kickos_image0.o(__VA_ARGS__)
#define KICKOS_KERNEL_SECTIONS(...) EXCLUDE_FILE(*kickos_image0.o) *(__VA_ARGS__)
#else
#define KICKOS_IMAGE0_SECTIONS(...) *(__VA_ARGS__)
#define KICKOS_KERNEL_SECTIONS(...)                                           \
    *libkickos_kernel.a:*(__VA_ARGS__)                                        \
    *KICKOS_LD_ARCH_LIB:*(__VA_ARGS__)                                        \
    *KICKOS_LD_CHIP_LIB:*(__VA_ARGS__)                                        \
    *libkickos_lib.a:*(__VA_ARGS__)
#endif
#ifndef KICKOS_APP_IMAGE_N
#define KICKOS_APP_IMAGE_N 0
#define KICKOS_APP_SECTIONS KICKOS_IMAGE0_SECTIONS
#endif
#define KICKOS_LD_APP_SYM(f) KICKOS_LD_IMAGE_SYM(KICKOS_APP_IMAGE_N, f)

/* An image's window past its .bss, at window start `start`: under the MPU one region of the
 * chip's unit, a power of two of at least its granule where the unit asks one, and a whole
 * number of granules otherwise; on 8 bytes where nothing is enforced. ALIGN of an offset and
 * not of `.`, which inside a section body aligns relative to the section's start. A window
 * placed at the end of a window of a larger power of two starts aligned to its own size; the
 * assert holds image n's to that.
 */
#ifndef KICKOS_CHIP_REGION_GRANULE
#define KICKOS_CHIP_REGION_GRANULE 32
#define KICKOS_CHIP_REGION_POW2 1
#endif
#if KICKOS_HAVE_MPU && KICKOS_CHIP_REGION_POW2
#define KICKOS_IMAGE_WINDOW_END(start)                                        \
    . = MAX(., (start) + (1 << LOG2CEIL(MAX(KICKOS_CHIP_REGION_GRANULE, . - (start)))));
#define KICKOS_IMAGE_WINDOW_ASSERT(n)                                         \
    ASSERT(KICKOS_LD_IMAGE_SYM(n, window_start)                               \
               % (KICKOS_LD_IMAGE_SYM(n, window_end) - KICKOS_LD_IMAGE_SYM(n, window_start)) == 0, \
           "KickOS: an image's window does not start on a multiple of its own power-of-two size, being larger than the window it follows, which ends where it starts (raise KICKOS_APPDATA_SIZE)")
#elif KICKOS_HAVE_MPU
#define KICKOS_IMAGE_WINDOW_END(start)                                        \
    . = MAX(., (start) + ALIGN(. - (start), KICKOS_CHIP_REGION_GRANULE));
#define KICKOS_IMAGE_WINDOW_ASSERT(n)                                         \
    ASSERT(KICKOS_LD_IMAGE_SYM(n, window_start) % KICKOS_CHIP_REGION_GRANULE == 0, \
           "KickOS: an image's window does not start on a granule of the chip's protection unit")
#else
#define KICKOS_IMAGE_WINDOW_END(start) . = MAX(., (start) + ALIGN(. - (start), 8));
#define KICKOS_IMAGE_WINDOW_ASSERT(n)
#endif
#if KICKOS_HAVE_MPU
#define KICKOS_IMAGE_WINDOW_GRANULE KICKOS_CHIP_REGION_GRANULE
#else
#define KICKOS_IMAGE_WINDOW_GRANULE 8
#endif

/* The table of image headers kernel/task/image.cc walks at boot, in the kernel's rodata: where
 * the init is an image of its own, every task image's after image 0's, which
 * kickos_images_table.ld holds (KICKOS_IMAGE_N_ENTRY in its family's word).
 */
#if KICKOS_IMAGES_PLACED
#define KICKOS_IMAGE_TABLE_REST(word)                                         \
    INCLUDE kickos_images_table.ld
#else
#define KICKOS_IMAGE_TABLE_REST(word)
#endif
#define KICKOS_IMAGE_N_ENTRY(n, word) word(ADDR(.kickos_image##n##_header));
#define KICKOS_IMAGE_TABLE(region, word)                                      \
    .kickos_image_table : ALIGN(4)                                            \
    {                                                                         \
        KICKOS_LD_C_SYM(__kickos_image_table) = .;                            \
        word(ADDR(.kickos_image0_header));                                    \
        KICKOS_IMAGE_TABLE_REST(word)                                         \
        KICKOS_LD_C_SYM(__kickos_image_table_end) = .;                        \
    } > region

/* Image 0's header, the first byte of its code span, and image 1's. */
#define KICKOS_IMAGE0_HEADER(region)                                          \
    .kickos_image0_header : ALIGN(4)                                          \
    {                                                                         \
        KICKOS_LD_C_SYM(__kickos_image0_code_start) = .;                      \
        KEEP(KICKOS_IMAGE0_SECTIONS(.kickos_image_header))                    \
    } > region
#define KICKOS_IMAGE1_HEADER(region)                                          \
    .kickos_image1_header : ALIGN(4)                                          \
    {                                                                         \
        KICKOS_LD_C_SYM(__kickos_image1_code_start) = .;                      \
        KEEP(KICKOS_IMAGE1_SECTIONS(.kickos_image_header))                    \
    } > region
/* The app's header, image 1's where the init is an image of its own. */
#if KICKOS_IMAGES_PLACED
#define KICKOS_APP_HEADER KICKOS_IMAGE1_HEADER
#else
#define KICKOS_APP_HEADER KICKOS_IMAGE0_HEADER
#endif

/* What the boot check holds every header to: the memory an image's code and its window may sit
 * in, and the kernel's own code, data and stacks. in_place is 1 where .data loads where it runs,
 * with no load image to copy. The arena is __kickos_ram_start/_end and the thread-local segment
 * __kickos_tls_segment_start/_end. ABSOLUTE: ld files an address assigned outside every section
 * under some section, where tools/amp/build-partition.sh would read it as a symbol of .amp_shared.
 */
#define KICKOS_IMAGE_BOUNDS(code_lo, code_hi, kcode_lo, kcode_hi, ram_lo, ram_hi, \
                            kdata_lo, kdata_hi, kstack_lo, kstack_hi, in_place) \
    KICKOS_LD_C_SYM(__kickos_codemem_start) = ABSOLUTE(code_lo);              \
    KICKOS_LD_C_SYM(__kickos_codemem_end) = ABSOLUTE(code_hi);                \
    KICKOS_LD_C_SYM(__kickos_kernel_code_start) = ABSOLUTE(kcode_lo);         \
    KICKOS_LD_C_SYM(__kickos_kernel_code_end) = ABSOLUTE(kcode_hi);           \
    KICKOS_LD_C_SYM(__kickos_rammem_start) = ABSOLUTE(ram_lo);                \
    KICKOS_LD_C_SYM(__kickos_rammem_end) = ABSOLUTE(ram_hi);                  \
    KICKOS_LD_C_SYM(__kickos_kernel_data_start) = ABSOLUTE(kdata_lo);         \
    KICKOS_LD_C_SYM(__kickos_kernel_data_end) = ABSOLUTE(kdata_hi);           \
    KICKOS_LD_C_SYM(__kickos_kernel_stack_start) = ABSOLUTE(kstack_lo);       \
    KICKOS_LD_C_SYM(__kickos_kernel_stack_end) = ABSOLUTE(kstack_hi);         \
    KICKOS_LD_C_SYM(__kickos_image_data_in_place) = ABSOLUTE(in_place);

/* Each image's TLS fields, over the one thread-local segment every image's template is in. An
 * empty .tbss can sit below an aligned empty .tdata, so the segment is sized from SIZEOF, and an
 * image's span from its own parts' sizes. Image 1's span, the app's, is the whole segment.
 * The segment ends at the later of the two ends: rx ld places an empty .tbss at the START of a
 * .tdata it emits as NOBITS.
 */
#define KICKOS_TLS_ALIGN_OF_SEGMENT                                          \
    MAX(MAX(1, (SIZEOF(.tdata) != 0) * ALIGNOF(.tdata)),                      \
        (SIZEOF(.tbss) != 0) * ALIGNOF(.tbss))
#define KICKOS_IMAGE_APP_TLS(n, tdata_end)                                    \
    KICKOS_LD_IMAGE_SYM(n, tls_template) = __kickos_tdata_start;              \
    KICKOS_LD_IMAGE_SYM(n, tls_offset) = 0;                                   \
    KICKOS_LD_IMAGE_SYM(n, tls_init) = (SIZEOF(.tdata) != 0)                  \
        * ((tdata_end) - __kickos_tdata_start);                               \
    KICKOS_LD_IMAGE_SYM(n, tls_total) =                                       \
        KICKOS_LD_C_SYM(__kickos_tls_segment_end) - __kickos_tdata_start;     \
    KICKOS_LD_IMAGE_SYM(n, tls_align) = KICKOS_TLS_ALIGN_OF_SEGMENT;
#define KICKOS_IMAGE_TLS_SEGMENT()                                            \
    KICKOS_LD_C_SYM(__kickos_tls_segment_start) = __kickos_tdata_start;       \
    KICKOS_LD_C_SYM(__kickos_tls_segment_end) = __kickos_tdata_start         \
        + (SIZEOF(.tdata) + SIZEOF(.tbss) != 0)                               \
              * (MAX(ABSOLUTE(__kickos_tdata_end), ABSOLUTE(__kickos_tbss_end))  \
                 - ABSOLUTE(__kickos_tdata_start));
#if KICKOS_IMAGES_PLACED
/* Image n's TLS fields, over its span [start, end) of .tdata, its .tbss in it. ABSOLUTE: each bound
 * is a symbol of an output section, which ld would otherwise take as an offset into it. */
#define KICKOS_IMAGE_SPAN_TLS(n, start, end)                                  \
    KICKOS_LD_IMAGE_SYM(n, tls_init) = (SIZEOF(.tdata) != 0)                  \
        * (ABSOLUTE(end) - ABSOLUTE(start));                                  \
    KICKOS_LD_IMAGE_SYM(n, tls_template) = __kickos_tdata_start               \
        + (KICKOS_LD_IMAGE_SYM(n, tls_init) != 0)                             \
            * (ABSOLUTE(start) - ABSOLUTE(__kickos_tdata_start));             \
    KICKOS_LD_IMAGE_SYM(n, tls_offset) =                                      \
        KICKOS_LD_IMAGE_SYM(n, tls_template) - __kickos_tdata_start;          \
    KICKOS_LD_IMAGE_SYM(n, tls_total) = KICKOS_LD_IMAGE_SYM(n, tls_init);     \
    KICKOS_LD_IMAGE_SYM(n, tls_align) = KICKOS_TLS_ALIGN_OF_SEGMENT;
#define KICKOS_IMAGES_TLS()                                                   \
    KICKOS_IMAGE_TLS_SEGMENT()                                                \
    KICKOS_IMAGE_SPAN_TLS(1, __kickos_tdata_start, __kickos_tls1_tdata_end)   \
    KICKOS_IMAGE_SPAN_TLS(0, __kickos_tls0_tdata_start, __kickos_tdata_end)
/* A later task image n's, over the bounds KICKOS_IMAGE_N_TDATA states. */
#define KICKOS_IMAGE_N_TLS(n)                                                 \
    KICKOS_IMAGE_SPAN_TLS(n, __kickos_tls##n##_tdata_start, __kickos_tls##n##_tdata_end)
#else
#define KICKOS_IMAGES_TLS()                                                   \
    KICKOS_IMAGE_TLS_SEGMENT()                                                \
    KICKOS_IMAGE_APP_TLS(0, __kickos_tdata_end)
#endif
/* The name every script still invokes. */
#define KICKOS_IMAGE0_TLS() KICKOS_IMAGES_TLS()

/* The names image_header.c and an image's runtime reference. Where images are linked on their
 * own first, tools/image-link.py renames each in image n to image n's and refuses any it left, so
 * the generic set, the app image's, satisfies nothing there. A program's own link (the program
 * form of an MMU board's script) states the generic set alone.
 */
#define KICKOS_IMAGE_WINDOW_SIZE(n)                                           \
    KICKOS_LD_IMAGE_SYM(n, window_size) =                                     \
        KICKOS_LD_IMAGE_SYM(n, window_end) - KICKOS_LD_IMAGE_SYM(n, window_start);
#define KICKOS_IMAGE_OWN_NAMES(n)                                             \
    KICKOS_IMAGE_WINDOW_SIZE(n)                                               \
    KICKOS_LD_C_SYM(__kickos_image_code_start) = KICKOS_LD_IMAGE_SYM(n, code_start); \
    KICKOS_LD_C_SYM(__kickos_image_code_end) = KICKOS_LD_IMAGE_SYM(n, code_end); \
    KICKOS_LD_C_SYM(__kickos_image_window_start) = KICKOS_LD_IMAGE_SYM(n, window_start); \
    KICKOS_LD_C_SYM(__kickos_image_window_size) = KICKOS_LD_IMAGE_SYM(n, window_size); \
    KICKOS_LD_C_SYM(__kickos_image_data_load) = KICKOS_LD_IMAGE_SYM(n, data_load); \
    KICKOS_LD_C_SYM(__kickos_image_data_start) = KICKOS_LD_IMAGE_SYM(n, data_start); \
    KICKOS_LD_C_SYM(__kickos_image_data_end) = KICKOS_LD_IMAGE_SYM(n, data_end); \
    KICKOS_LD_C_SYM(__kickos_image_bss_end) = KICKOS_LD_IMAGE_SYM(n, bss_end); \
    KICKOS_LD_C_SYM(__kickos_image_heap_start) = KICKOS_LD_IMAGE_SYM(n, heap_start); \
    KICKOS_LD_C_SYM(__kickos_image_heap_end) = KICKOS_LD_IMAGE_SYM(n, heap_end); \
    KICKOS_LD_C_SYM(__kickos_image_tls_template) = KICKOS_LD_IMAGE_SYM(n, tls_template); \
    KICKOS_LD_C_SYM(__kickos_image_tls_offset) = KICKOS_LD_IMAGE_SYM(n, tls_offset); \
    KICKOS_LD_C_SYM(__kickos_image_tls_init) = KICKOS_LD_IMAGE_SYM(n, tls_init); \
    KICKOS_LD_C_SYM(__kickos_image_tls_total) = KICKOS_LD_IMAGE_SYM(n, tls_total); \
    KICKOS_LD_C_SYM(__kickos_image_tls_align) = KICKOS_LD_IMAGE_SYM(n, tls_align); \
    KICKOS_LD_C_SYM(__kickos_image_ctor_start) = KICKOS_LD_IMAGE_SYM(n, ctor_start); \
    KICKOS_LD_C_SYM(__kickos_image_ctor_end) = KICKOS_LD_IMAGE_SYM(n, ctor_end); \
    KICKOS_LD_C_SYM(__kickos_image_eh_frame) = KICKOS_LD_IMAGE_SYM(n, eh_frame);
#define KICKOS_IMAGE0_OWN_NAMES() KICKOS_IMAGE_OWN_NAMES(0)

/* The rest of image n's renamed names, each from a field above or a kernel bound. */
#define KICKOS_IMAGE_NAMES(n)                                                 \
    KICKOS_LD_IMAGE_SYM(n, app_init_array_start) = KICKOS_LD_IMAGE_SYM(n, ctor_start); \
    KICKOS_LD_IMAGE_SYM(n, app_init_array_end) = KICKOS_LD_IMAGE_SYM(n, ctor_end); \
    KICKOS_LD_IMAGE_SYM(n, newlib_heap_start) = KICKOS_LD_IMAGE_SYM(n, heap_start); \
    KICKOS_LD_IMAGE_SYM(n, newlib_heap_end) = KICKOS_LD_IMAGE_SYM(n, heap_end); \
    KICKOS_LD_IMAGE_SYM(n, eh_frame_start) = KICKOS_LD_IMAGE_SYM(n, eh_frame); \
    KICKOS_LD_IMAGE_SYM(n, tls_carve) = KICKOS_TLS_CARVE_OF(n);              \
    KICKOS_LD_IMAGE_SYM(n, kernel_data) = KICKOS_LD_C_SYM(_sdata);            \
    KICKOS_LD_IMAGE_SYM(n, kernel_bss_end) = KICKOS_LD_C_SYM(_ebss);          \
    KICKOS_LD_IMAGE_SYM(n, tbss_start) = KICKOS_LD_C_SYM(__kickos_tbss_start); \
    KICKOS_LD_IMAGE_SYM(n, tbss_end) = KICKOS_LD_C_SYM(__kickos_tbss_end);    \
    KICKOS_LD_IMAGE_SYM(n, code_window_start) = KICKOS_LD_C_SYM(__kickos_code_start); \
    KICKOS_LD_IMAGE_SYM(n, code_window_end) = KICKOS_LD_C_SYM(__kickos_code_end);

/* Image 1's names, the app's; where the init is an image of its own, every image's are stated
 * with its window (kickos_images.ld). */
#if KICKOS_IMAGES_PLACED
#define KICKOS_IMAGE0_NAMES()                                                 \
    KICKOS_IMAGE_OWN_NAMES(1)                                                 \
    PROVIDE(KICKOS_LD_C_SYM(__kickos_build_id) = 0);
#else
#define KICKOS_IMAGE0_NAMES()                                                 \
    KICKOS_IMAGE_OWN_NAMES(KICKOS_APP_IMAGE_N)                                \
    KICKOS_IMAGE_NAMES(0)                                                     \
    PROVIDE(KICKOS_LD_C_SYM(__kickos_build_id) = 0);
#endif

#endif
