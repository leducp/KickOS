// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The image header: one per image, at the image's first code byte, read by the kernel in place
// and checked at boot (kernel/task/image_check.cc) before any field is used. Every field is
// pointer-wide so a C initialiser can take each one from a link-time address; the final link's
// script states the bounds, the image its own entry points.
//
// An address is the image's own view of it: the task's on a translating board, the physical one
// elsewhere. A span is [start, end) or [start, start + size), and an empty one has end == start.

#ifndef KICKOS_SYS_IMAGE_H
#define KICKOS_SYS_IMAGE_H

#include <stdint.h>

#include <kickos/sys/budget.h>

#define KOS_IMAGE_MAGIC 0x474d494bu // "KIMG" little-endian
#define KOS_IMAGE_VERSION 5u

// The section the header object places it in, KEEP'd first in the image by the final script.
#define KOS_IMAGE_HEADER_SECTION ".kickos_image_header"

// Where the init is an image of its own (KICKOS_INIT_IMAGE), the image link puts an object
// marked KOS_IMAGE_INIT_OBJECT() in the init's image alone, one marked KOS_IMAGE_BOTH_OBJECT()
// in every image, one marked KOS_IMAGE_TASK_OBJECT(k) in the image of declaration k alone, one
// marked KOS_IMAGE_SYSTEM_OBJECT() in the image of every declaration, a program's included, and
// any other in the image of every declaration not running a program of another target. Each
// names a local absolute symbol and costs no byte. Written at file scope, with no semicolon after
// it; k is an integer literal or a macro expanding to one.
#if KICKOS_INIT_IMAGE
#define KOS_IMAGE_INIT_OBJECT() __asm__(".set __kickos_image_init_object, 1");
#define KOS_IMAGE_BOTH_OBJECT() __asm__(".set __kickos_image_both_object, 1");
#define KOS_IMAGE_SYSTEM_OBJECT() __asm__(".set __kickos_image_system_object, 1");
#define KOS_IMAGE_TASK_STR_(k) #k
#define KOS_IMAGE_TASK_STR(k) KOS_IMAGE_TASK_STR_(k)
#define KOS_IMAGE_TASK_OBJECT(k) __asm__(".set __kickos_image_task_object, " KOS_IMAGE_TASK_STR(k));
// In a task image's object, beside KOS_IMAGE_TASK_OBJECT: that task is restarted or created on request.
#define KOS_IMAGE_RESTARTS_OBJECT() __asm__(".set __kickos_image_task_restarts, 1");
#else
#define KOS_IMAGE_INIT_OBJECT()
#define KOS_IMAGE_BOTH_OBJECT()
#define KOS_IMAGE_SYSTEM_OBJECT()
#define KOS_IMAGE_TASK_OBJECT(k)
#define KOS_IMAGE_RESTARTS_OBJECT()
#endif

#ifdef __cplusplus
extern "C"
{
#endif

// kos_decl_grant.kind
enum kos_decl_grant_kind
{
    KOS_DECL_DEVICE = 1, // a device window of the entry thread: base, size
    KOS_DECL_PORTS = 2,  // a port window of the entry thread: first port, count
    KOS_DECL_LINE = 3    // an interrupt line: line, line_index
};

// kos_task_decl.flags
enum kos_task_decl_flags
{
    KOS_DECL_CONSOLE = 1 << 0, // serves the console its create publishes
    KOS_DECL_DRIVER = 1 << 1,  // a packaged driver, whose create applies its grants
    KOS_DECL_ON_REQUEST = 1 << 2, // started by any creator holding KOS_AUTH_TASKS, never the init
    KOS_DECL_RESTARTS = 1 << 3    // declared `restart`: started again once its last start ended
};

// A static grant of a declaration.
struct kos_decl_grant
{
    uint8_t kind;        // enum kos_decl_grant_kind
    uint8_t trigger;     // KOS_DECL_LINE: KOS_IRQ_EDGE or KOS_IRQ_LEVEL
    uint16_t line;       // KOS_DECL_LINE: the line at the interrupt controller
    uint16_t line_index; // KOS_DECL_LINE: its index among its device's lines
    // KOS_DECL_LINE of a packaged driver: the entry thread's capability index the kernel claims
    // the line into; 0 elsewhere.
    uint16_t slot;
    uint32_t size;       // bytes, or ports
    uintptr_t base;      // a window's physical base, or the first port
};

// One declared task over the image: its static facts, which the kernel reads in place. It holds
// no address, so it needs no relocation and stays read-only data on every link.
struct kos_task_decl
{
    uint32_t name;   // the NUL-terminated name's offset from the image's decls
    uint32_t grants; // grant_count kos_decl_grant at this offset from the image's decls
    uint32_t core_mask;
    uint32_t authority; // KOS_AUTH_* bits
    uint32_t stack;     // the entry thread's stack in bytes
    uint16_t grant_count;
    uint16_t flags; // enum kos_task_decl_flags
    uint8_t priority;
    uint8_t ceiling; // at least priority
    // The delegated capability, by its position in the create's list plus one, whose endpoint
    // a member's first receive on makes the task ready, and which a console publishes; 0 for
    // none.
    uint16_t ready;
    // A packaged driver's: the delegated notification, by its position in the create's list plus
    // one, its lines are bound to, line k raising bit k; 0 for none.
    uint16_t notify;
    // The capability-table width of a thread of the task that asks none, its entry thread's
    // included: a whole number of chunks.
    uint16_t cap_width;
    // The most of each kind the task may hold, by enum kos_budget_kind: the authority every
    // allocation of the task is admitted against.
    uint32_t budgets[KOS_BUDGET_KIND_COUNT];
};

struct kos_image_header
{
    uint32_t magic;
    uint16_t version;
    uint16_t size;            // sizeof(struct kos_image_header)
    uintptr_t build_id;       // the final link's: equal in every image of one link
    uintptr_t program;        // NUL-terminated name, in the code span
    uintptr_t code_start;     // text, rodata, unwind tables, templates, the .data load image
    uintptr_t code_end;
    uintptr_t window_start;   // the image's writable window: .data, .bss and the heap
    uintptr_t window_size;
    uintptr_t data_load;      // .data's bytes in the code span
    uintptr_t data_start;     // .data's run, then .bss to bss_end
    uintptr_t data_end;
    uintptr_t bss_end;
    uintptr_t heap_start;
    uintptr_t heap_end;
    uintptr_t tls_template;   // the image's .tdata bytes
    uintptr_t tls_offset;     // its offset in the link's thread-local segment
    uintptr_t tls_init;       // its .tdata size
    uintptr_t tls_total;      // its .tdata and .tbss size
    uintptr_t tls_align;
    uintptr_t ctor_start;     // the constructor window the image's entry walks
    uintptr_t ctor_end;
    uintptr_t eh_frame;       // DWARF unwind tables, 0 where the arch unwinds by EHABI
    uintptr_t start;          // the entry thread's entry
    uintptr_t decl_start;     // a declared task's entry thread's, which takes struct kos_task_args
    uintptr_t reent_template; // libc's state in .data; the image's copy of it primes each thread
    uintptr_t reent_width;
    uintptr_t reent_seat;     // the word the kernel seats at each switch, 0 where none is
    uintptr_t needs;          // shared code images this image calls into
    uintptr_t needs_count;
    uintptr_t decls;          // decl_count kos_task_decl, the tasks declared over this image
    uintptr_t decl_count;
    uintptr_t stamp;          // the image's name and compile time, as the banner prints it
    uintptr_t init_args;      // struct kos_init_args, which kmain fills for the entry
    uintptr_t thread_return;  // where an unprivileged thread's entry function returns to
    uintptr_t task_start;     // a translating board's entry for every unprivileged thread
    uintptr_t cas_start;      // where the arch has no atomic, the user CAS a switch restarts
    uintptr_t cas_store;      // and its store, the window's last instruction; 0 elsewhere
};

// The image's own header, in the image.
extern struct kos_image_header const kickos_image_header;

// Where the init is an image of its own, the program name of the app's images beside it, in the
// init's image, and the same name in each of the app's.
extern char const kickos_init_app_program[];
extern char const kickos_image_program[];

#ifdef __cplusplus
}
#endif

#endif
