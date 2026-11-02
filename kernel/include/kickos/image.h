// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The image records: every image's header (<kickos/sys/image.h>), checked at boot before the
// kernel reads any field of it. image_header_check is pure, so tests/unit/imagecheck runs it on
// the host over malformed headers.

#ifndef KICKOS_IMAGE_H
#define KICKOS_IMAGE_H

#include <kickos/config/system.h>
#include <kickos/sys/image_offsets.h>
#include <kickos/sys/image.h>

#include <stddef.h>
#include <stdint.h>

#ifndef KICKOS_IMAGE_CHECK_SPANS
#define KICKOS_IMAGE_CHECK_SPANS KICKOS_MEMORY_ENFORCED
#endif

// The arches with no atomic, whose switch restarts a user CAS interrupted in its window.
#if defined(__ARM_ARCH_6M__) or defined(__RX__)
#define KICKOS_IMAGE_CAS 1
#else
#define KICKOS_IMAGE_CAS 0
#endif

namespace kickos
{
    struct ImageSpan
    {
        uintptr_t start;
        uintptr_t end;
    };

    // What every header is held to. An empty span (end == start) contains and meets nothing.
    struct ImageBounds
    {
        uintptr_t build_id;
        bool spans;               // false: the image states no span to check (the sim)
        bool data_in_place;       // .data is loaded where it runs, so data_load is data_start
        bool task_start;          // the board enters every unprivileged thread at task_start
        ImageSpan code;           // where an image's code span may sit
        ImageSpan kernel_code;
        ImageSpan ram;            // where a window may sit
        ImageSpan kernel_data;
        ImageSpan kernel_stacks;
        ImageSpan arena;
        ImageSpan tls;            // the link's one thread-local segment
        uintptr_t tls_align_max;
        bool (*window_encodable)(uintptr_t base, uintptr_t size); // null: no region unit
        // What a declaration is held to.
        uintptr_t decls_max;
        uint8_t prio_min;
        uint8_t prio_max;
        uint32_t cores;     // every core of this kernel
        uint32_t authority; // every KOS_AUTH_* bit
        uint32_t stack_align;
        uint16_t windows_max; // device and port windows, KICKOS_MAX_THREAD_WINDOWS
        uint16_t lines_max;   // KICKOS_MAX_SPAWN_GRANTS
        bool (*device_ok)(uintptr_t base, uintptr_t size); // null: none admissible
        bool (*ports_ok)(uintptr_t base, uintptr_t count); // null: none admissible
        bool (*line_ok)(uint16_t line);
        // The kernel's view of an address in image `image`'s, null where they are the same.
        void const* (*view)(size_t image, uintptr_t image_address);
        size_t image; // the image whose header is checked
    };

    // The field a check refused, in the order image_header_check tests them. Where nothing is
    // enforced the panic prints this number and not the name.
    enum class ImageCheck : uint8_t
    {
        OK,
        MAGIC,
        VERSION,
        SIZE,
        BUILD_ID,
        NEEDS,
        DECLS,
        START,
        INIT_ARGS,
        THREAD_RETURN,
        TASK_START,
        DECL_START,
        CAS,
        CODE_SPAN,
        CODE_KERNEL,
        CODE_OVERLAP,
        WINDOW_SPAN,
        WINDOW_KERNEL,
        WINDOW_CODE,
        WINDOW_OVERLAP,
        WINDOW_ENCODABLE,
        DATA,
        HEAP,
        REENT_SEAT,
        REENT_TEMPLATE,
        DATA_LOAD,
        TLS_TEMPLATE,
        TLS_SIZES,
        TLS_ALIGN,
        TLS_SEGMENT,
        CTOR,
        EH_FRAME,
        PROGRAM,
        STAMP,
        DECL_SPAN,
        DECL_NAME,
        DECL_PRIORITY,
        DECL_CEILING,
        DECL_CORES,
        DECL_STACK,
        DECL_AUTHORITY,
        DECL_FLAGS,
        DECL_GRANTS,
        DECL_KIND,
        DECL_WINDOWS,
        DECL_DEVICE,
        DECL_PORTS,
        DECL_LINES,
        DECL_LINE,
        COUNT
    };

    // `earlier` holds the n images already checked, which this one must not meet.
    ImageCheck image_header_check(kos_image_header const& h, ImageBounds const& b,
                                  kos_image_header const* const* earlier, size_t n);

#if KICKOS_IMAGE_CHECK_SPANS
    // The name of the field a check refused.
    char const* image_check_message(ImageCheck c);
#endif

    // Before root exists: checks every header in the link's table and publishes it, or panics
    // naming the first field refused.
    void image_records_publish(void);

    size_t image_count(void);

    // Published records only, read through the kernel's own view of the header.
    kos_image_header const& image_record(size_t i);

    // The declarations of every image, numbered in image order: declaration i through the
    // kernel's view, null past the last.
    kos_task_decl const* image_decl(size_t i);

    // How many declarations every image holds together.
    size_t image_decl_count(void);

    // Image `image`'s declarations, numbered from *first, *count of them; none past the table.
    void image_decls_of(int image, size_t* first, size_t* count);

    // The image holding declaration i, image_count() past the last.
    size_t image_of_decl(size_t i);

    // Whether images a and b run one program: their program names are equal.
    bool image_same_program(size_t a, size_t b);

    // The entry the entry thread of declaration i's task takes, in its image.
    uintptr_t image_decl_start(size_t i);

    // A declaration's NUL-terminated name, through the kernel's view.
    char const* image_decl_name(kos_task_decl const* d);

    // A declaration's grant_count static grants, through the kernel's view.
    kos_decl_grant const* image_decl_grants(kos_task_decl const* d);

    // Image i's program name, through the kernel's view.
    char const* image_program_name(int i);

    // Image i's record, which a program capability names, or null past the published count.
    kos_image_header const* image_program(int i);

    // Seats kickos_image_thread_return with image i's, which the next arch_context_init plants.
    // Caller holds IrqLock.
    void image_thread_return_select(size_t i);
}

// Where an unprivileged thread's entry function returns to, in its image's view, published with
// image 0's record and selected per thread by image_thread_return_select; the arch backends
// plant it.
extern "C" void (*kickos_image_thread_return)(void);

#endif
