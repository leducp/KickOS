/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The image header (<kickos/sys/image.h>), compiled into each image by kickos_emit_image. C and
 * not C++: an address cast to uintptr_t is a constant initialiser only in C, and a dynamically
 * initialised header would be read by the kernel before any constructor runs.
 *
 * The __kickos_image_* bounds are the final link's. Where the image is linked on its own first
 * (tools/image-link.py), each is renamed to that image's own before the final link.
 *
 * Where the init is an image of its own (KICKOS_INIT_IMAGE), init_image_header.c compiles this
 * as the init's header (KICKOS_IMAGE_INIT): root's entry, no declaration. Each declared task's
 * image then carries its own, compiled beside its table copy by its system target with
 * KICKOS_IMAGE_TASK, its declaration's index: that declaration alone, started at its entry. The
 * executable's own copy then only names the program, unless the task runs a program of another
 * target, which KICKOS_IMAGE_PROGRAM_NAME names.
 */

#include <kickos/sys/image.h>
#include <kickos/sys/init.h>

#include <stddef.h>
#include <stdint.h>

#if KICKOS_LIBC_REENT
#include <sys/reent.h>
#endif

#if not defined(KICKOS_APP_IMAGE) and not defined(KICKOS_IMAGE_TASK)
#error "image_header.c is compiled into an executable, which names the image"
#endif

#ifndef KICKOS_IMAGE_INIT
#define KICKOS_IMAGE_INIT 0
#endif

#if KICKOS_INIT_IMAGE and not defined(KICKOS_IMAGE_TASK) and not KICKOS_IMAGE_INIT
char const kickos_image_program[] = KICKOS_APP_IMAGE;
#else

#if KICKOS_ARCH_SIM
#define KOS_IMAGE_BOUND(name) 0
#else
#define KOS_IMAGE_BOUND(name) ((uintptr_t)name)
extern char __kickos_image_code_start[];
extern char __kickos_image_code_end[];
extern char __kickos_image_window_start[];
extern char __kickos_image_window_size[];
extern char __kickos_image_data_load[];
extern char __kickos_image_data_start[];
extern char __kickos_image_data_end[];
extern char __kickos_image_bss_end[];
extern char __kickos_image_heap_start[];
extern char __kickos_image_heap_end[];
extern char __kickos_image_tls_template[];
extern char __kickos_image_tls_offset[];
extern char __kickos_image_tls_init[];
extern char __kickos_image_tls_total[];
extern char __kickos_image_tls_align[];
extern char __kickos_image_ctor_start[];
extern char __kickos_image_ctor_end[];
extern char __kickos_image_eh_frame[];
extern char __kickos_build_id[];
#endif

void kickos_user_thread_return(void);

/* The system target's table defines them, one per task it declares, and the target states their
 * count. With no system target the link fails naming kickos_link_one_system_target. */
#ifndef KICKOS_IMAGE_DECL_COUNT
#define KICKOS_IMAGE_DECL_COUNT 0
#endif
#if KICKOS_IMAGE_INIT
#define KOS_IMAGE_DECLS 0
KOS_IMAGE_INIT_OBJECT()
char const kickos_init_app_program[] = KICKOS_APP_IMAGE;
#define KOS_IMAGE_PROGRAM kos_image_program
static char const kos_image_program[] = "init";
#define KOS_IMAGE_START kickos_root_entry
#elif defined(KICKOS_IMAGE_TASK)
#define KOS_IMAGE_DECLS 1
KOS_IMAGE_TASK_OBJECT(KICKOS_IMAGE_TASK)
#ifdef KICKOS_IMAGE_PROGRAM_NAME
#define KOS_IMAGE_PROGRAM kos_image_program
static char const kos_image_program[] = KICKOS_IMAGE_PROGRAM_NAME;
#else
#define KOS_IMAGE_PROGRAM kickos_image_program
#endif
#ifdef KICKOS_IMAGE_TASK_PROGRAM
#define KOS_IMAGE_START kickos_program_entry
#else
#define KOS_IMAGE_START kickos_task_entry
#endif
#else
#define KOS_IMAGE_DECLS KICKOS_IMAGE_DECL_COUNT
#define KOS_IMAGE_PROGRAM kos_image_program
static char const kos_image_program[] = KICKOS_APP_IMAGE;
#define KOS_IMAGE_START kickos_root_entry
#endif
void kickos_task_entry(void* args);
void kickos_program_entry(void* args);
/* A program's image starts its task at kickos_program_entry, any other at kickos_task_entry. */
#ifdef KICKOS_IMAGE_TASK_PROGRAM
#define KOS_IMAGE_DECL_START kickos_program_entry
#else
#define KOS_IMAGE_DECL_START kickos_task_entry
#endif
#if KOS_IMAGE_DECLS > 0
/* Its first member is the array of KOS_IMAGE_DECLS declarations. */
extern struct kickos_image_decls const kickos_image_decls;
#endif

/* The arches with no atomic: a switch rewinds a thread interrupted inside this window. */
#if defined(__ARM_ARCH_6M__) or defined(__RX__)
#define KOS_IMAGE_CAS 1
void kickos_user_cas(void);
void kickos_user_cas_store(void);
#endif

__attribute__((section(KOS_IMAGE_HEADER_SECTION), used))
struct kos_image_header const kickos_image_header = {
    .magic = KOS_IMAGE_MAGIC,
    .version = KOS_IMAGE_VERSION,
    .size = (uint16_t)sizeof(struct kos_image_header),
    .build_id = KOS_IMAGE_BOUND(__kickos_build_id),
    .program = (uintptr_t)KOS_IMAGE_PROGRAM,
    .code_start = KOS_IMAGE_BOUND(__kickos_image_code_start),
    .code_end = KOS_IMAGE_BOUND(__kickos_image_code_end),
    .window_start = KOS_IMAGE_BOUND(__kickos_image_window_start),
    .window_size = KOS_IMAGE_BOUND(__kickos_image_window_size),
    .data_load = KOS_IMAGE_BOUND(__kickos_image_data_load),
    .data_start = KOS_IMAGE_BOUND(__kickos_image_data_start),
    .data_end = KOS_IMAGE_BOUND(__kickos_image_data_end),
    .bss_end = KOS_IMAGE_BOUND(__kickos_image_bss_end),
    .heap_start = KOS_IMAGE_BOUND(__kickos_image_heap_start),
    .heap_end = KOS_IMAGE_BOUND(__kickos_image_heap_end),
    .tls_template = KOS_IMAGE_BOUND(__kickos_image_tls_template),
    .tls_offset = KOS_IMAGE_BOUND(__kickos_image_tls_offset),
    .tls_init = KOS_IMAGE_BOUND(__kickos_image_tls_init),
    .tls_total = KOS_IMAGE_BOUND(__kickos_image_tls_total),
    .tls_align = KOS_IMAGE_BOUND(__kickos_image_tls_align),
    .ctor_start = KOS_IMAGE_BOUND(__kickos_image_ctor_start),
    .ctor_end = KOS_IMAGE_BOUND(__kickos_image_ctor_end),
    .eh_frame = KOS_IMAGE_BOUND(__kickos_image_eh_frame),
    .start = (uintptr_t)KOS_IMAGE_START,
#if KICKOS_LIBC_REENT
    .reent_template = (uintptr_t)_GLOBAL_REENT,
    .reent_width = sizeof(struct _reent),
#if not KICKOS_REENT_PER_THREAD
    .reent_seat = (uintptr_t)&_impure_ptr,
#endif
#endif
#if KOS_IMAGE_DECLS > 0
    .decl_start = (uintptr_t)KOS_IMAGE_DECL_START,
    .decls = (uintptr_t)&kickos_image_decls,
    .decl_count = KOS_IMAGE_DECLS,
#endif
    .stamp = (uintptr_t)kickos_app_stamp,
    .init_args = (uintptr_t)&kickos_init_args,
    .thread_return = (uintptr_t)kickos_user_thread_return,
#if KICKOS_HAVE_ASPACE
    .task_start = (uintptr_t)kickos_task_start,
#endif
#ifdef KOS_IMAGE_CAS
    .cas_start = (uintptr_t)kickos_user_cas,
    .cas_store = (uintptr_t)kickos_user_cas_store,
#endif
};
#endif
