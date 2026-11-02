// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Every comparison is overflow-safe: a span is tested as end >= start first, and a sized one as
// size <= limit - base, so no sum of two header fields is ever formed.
//
// Where nothing is enforced the check is the header's format and the kernel's entry points; the
// spans an enforcing board's isolation rests on, and what each declaration states, are checked
// only there.

#include <kickos/image.h>
#include <kickos/sys/abi.h> // KOS_IRQ_LEVEL
#include <kickos/sys/init.h>

namespace kickos
{
    namespace
    {
        bool aligned(uintptr_t v, uintptr_t a)
        {
            return (v & (a - 1u)) == 0;
        }

#if KICKOS_IMAGE_CHECK_SPANS
        bool byte_within(uintptr_t p, ImageSpan const& m)
        {
            return p != 0 and p >= m.start and p < m.end;
        }

        bool span_ok(uintptr_t start, uintptr_t end)
        {
            return end >= start;
        }

        bool within(uintptr_t start, uintptr_t end, ImageSpan const& m)
        {
            return span_ok(start, end) and start >= m.start and end <= m.end;
        }

        bool sized_within(uintptr_t base, uintptr_t size, ImageSpan const& m)
        {
            return base >= m.start and base <= m.end and size <= m.end - base;
        }

        bool meets(uintptr_t start, uintptr_t end, ImageSpan const& m)
        {
            if (end == start or m.end == m.start)
            {
                return false;
            }
            return start < m.end and m.start < end;
        }

        bool pow2(uintptr_t v)
        {
            return v != 0 and (v & (v - 1u)) == 0;
        }

        void const* view(ImageBounds const& b, uintptr_t a)
        {
            if (b.view != nullptr)
            {
                return b.view(b.image, a);
            }
            return reinterpret_cast<void const*>(a);
        }

        // The string at p, below end, holds its NUL before end. The kernel reads every name the
        // header states to its NUL with no bound of its own: this is that bound.
        bool string_within(ImageBounds const& b, uintptr_t p, uintptr_t end)
        {
            char const* const s = static_cast<char const*>(view(b, p));
            for (uintptr_t k = 0; k < end - p; k++)
            {
                if (s[k] == '\0')
                {
                    return true;
                }
            }
            return false;
        }

        ImageCheck check_spans(kos_image_header const& h, ImageBounds const& b,
                               kos_image_header const* const* earlier, size_t n)
        {
            if (h.tls_init > h.tls_total)
            {
                return ImageCheck::TLS_SIZES;
            }
            if (not pow2(h.tls_align) or h.tls_align > b.tls_align_max)
            {
                return ImageCheck::TLS_ALIGN;
            }
            uintptr_t const segment = b.tls.end - b.tls.start;
            if (h.tls_offset > segment or h.tls_total > segment - h.tls_offset)
            {
                return ImageCheck::TLS_SEGMENT;
            }

            ImageSpan const code{h.code_start, h.code_end};
            if (not within(code.start, code.end, b.code) or code.end == code.start)
            {
                return ImageCheck::CODE_SPAN;
            }
            if (not byte_within(h.start, code))
            {
                return ImageCheck::START;
            }
            if (not byte_within(h.thread_return, code))
            {
                return ImageCheck::THREAD_RETURN;
            }
            if (h.task_start != 0 and not byte_within(h.task_start, code))
            {
                return ImageCheck::TASK_START;
            }
            if (h.decl_start != 0 and not byte_within(h.decl_start, code))
            {
                return ImageCheck::DECL_START;
            }
            if (h.cas_start != 0 and not within(h.cas_start, h.cas_store + 1u, code))
            {
                return ImageCheck::CAS;
            }
            if (meets(code.start, code.end, b.kernel_code))
            {
                return ImageCheck::CODE_KERNEL;
            }
            for (size_t i = 0; i < n; i++)
            {
                if (meets(code.start, code.end, {earlier[i]->code_start, earlier[i]->code_end}))
                {
                    return ImageCheck::CODE_OVERLAP;
                }
            }

            if (not sized_within(h.window_start, h.window_size, b.ram))
            {
                return ImageCheck::WINDOW_SPAN;
            }
            ImageSpan const win{h.window_start, h.window_start + h.window_size};
            if (meets(win.start, win.end, b.kernel_code) or meets(win.start, win.end, b.kernel_data)
                or meets(win.start, win.end, b.kernel_stacks) or meets(win.start, win.end, b.arena))
            {
                return ImageCheck::WINDOW_KERNEL;
            }
            // Where code and windows share one memory, the code memory holds every image's code.
            if (meets(win.start, win.end, b.code))
            {
                return ImageCheck::WINDOW_CODE;
            }
            for (size_t i = 0; i < n; i++)
            {
                ImageSpan const other{earlier[i]->window_start,
                                      earlier[i]->window_start + earlier[i]->window_size};
                if (meets(win.start, win.end, other))
                {
                    return ImageCheck::WINDOW_OVERLAP;
                }
            }
            if (b.window_encodable != nullptr and h.window_size != 0
                and not b.window_encodable(h.window_start, h.window_size))
            {
                return ImageCheck::WINDOW_ENCODABLE;
            }

            // kmain writes root's arguments here.
            if (not sized_within(h.init_args, sizeof(kos_init_args), win))
            {
                return ImageCheck::INIT_ARGS;
            }
            if (not within(h.data_start, h.data_end, win) or not within(h.data_end, h.bss_end, win))
            {
                return ImageCheck::DATA;
            }
            if (not within(h.heap_start, h.heap_end, win) or h.heap_start < h.bss_end
                or not aligned(h.heap_start, 8))
            {
                return ImageCheck::HEAP;
            }
            // The kernel stores to this word at every switch.
            if (h.reent_seat != 0
                and (not aligned(h.reent_seat, sizeof(void*))
                     or not sized_within(h.reent_seat, sizeof(void*), win)))
            {
                return ImageCheck::REENT_SEAT;
            }
            // Primed into every thread's own state with reent_width bytes, from the image's copy
            // of its .data.
            if (h.reent_width != 0
                and not sized_within(h.reent_template, h.reent_width,
                                     ImageSpan{h.data_start, h.data_end}))
            {
                return ImageCheck::REENT_TEMPLATE;
            }
            uintptr_t const data_size = h.data_end - h.data_start;
            // Where .data loads in place, a template apart from it is the kernel's to fill at boot.
            if (not (b.data_in_place and h.data_load == h.data_start)
                and not sized_within(h.data_load, data_size, code))
            {
                return ImageCheck::DATA_LOAD;
            }
            // The template is the image's own part of the one segment.
            if (h.tls_template != b.tls.start + h.tls_offset
                or not sized_within(h.tls_template, h.tls_init, b.tls))
            {
                return ImageCheck::TLS_TEMPLATE;
            }
            // Walked unprivileged by the image's own entry, from its code or its data.
            if ((not within(h.ctor_start, h.ctor_end, code)
                 and not within(h.ctor_start, h.ctor_end, win))
                or not aligned(h.ctor_end - h.ctor_start, sizeof(void*)))
            {
                return ImageCheck::CTOR;
            }
            if (h.eh_frame != 0 and not byte_within(h.eh_frame, code))
            {
                return ImageCheck::EH_FRAME;
            }
            if (not byte_within(h.program, code) or not string_within(b, h.program, code.end))
            {
                return ImageCheck::PROGRAM;
            }
            if (not byte_within(h.stamp, code) or not string_within(b, h.stamp, code.end))
            {
                return ImageCheck::STAMP;
            }
            return ImageCheck::OK;
        }

        // The array of `count` records of `width` bytes at `base`, inside the code span: count is
        // bounded by the caller, so the product cannot wrap.
        bool array_ok(uintptr_t base, uintptr_t count, uintptr_t width, uintptr_t align,
                      kos_image_header const& h, bool spans)
        {
            if (base == 0 or not aligned(base, align))
            {
                return false;
            }
            if (spans)
            {
                return sized_within(base, count * width, {h.code_start, h.code_end});
            }
            return true;
        }

        // `offset` from the declarations lands in the code span with `bytes` after it. Read once
        // the declarations themselves passed array_ok.
        bool offset_ok(uintptr_t offset, uintptr_t bytes, kos_image_header const& h, bool spans)
        {
            if (spans)
            {
                uintptr_t const room = h.code_end - h.decls;
                return offset <= room and bytes <= room - offset;
            }
            return true;
        }

        ImageCheck check_grants(kos_task_decl const& d, ImageBounds const& b,
                                kos_image_header const& h, bool spans)
        {
            if (d.grant_count == 0)
            {
                if (d.grants != 0)
                {
                    return ImageCheck::DECL_GRANTS;
                }
                return ImageCheck::OK;
            }
            if (d.grant_count > b.windows_max + b.lines_max
                or not aligned(h.decls + d.grants, alignof(kos_decl_grant))
                or not offset_ok(d.grants, d.grant_count * sizeof(kos_decl_grant), h, spans))
            {
                return ImageCheck::DECL_GRANTS;
            }
            kos_decl_grant const* const g =
                static_cast<kos_decl_grant const*>(view(b, h.decls + d.grants));
            uint16_t windows = 0;
            uint16_t lines = 0;
            for (uint16_t k = 0; k < d.grant_count; k++)
            {
                if (g[k].kind == KOS_DECL_DEVICE)
                {
                    windows++;
                    if (b.device_ok != nullptr and not b.device_ok(g[k].base, g[k].size))
                    {
                        return ImageCheck::DECL_DEVICE;
                    }
                }
                else if (g[k].kind == KOS_DECL_PORTS)
                {
                    windows++;
                    if (b.ports_ok != nullptr and not b.ports_ok(g[k].base, g[k].size))
                    {
                        return ImageCheck::DECL_PORTS;
                    }
                }
                else if (g[k].kind == KOS_DECL_LINE)
                {
                    lines++;
                    if ((b.line_ok != nullptr and not b.line_ok(g[k].line))
                        or (g[k].trigger & ~static_cast<uint8_t>(KOS_IRQ_LEVEL)) != 0)
                    {
                        return ImageCheck::DECL_LINE;
                    }
                }
                else
                {
                    return ImageCheck::DECL_KIND;
                }
            }
            if (windows > b.windows_max)
            {
                return ImageCheck::DECL_WINDOWS;
            }
            // A driver's lines are bound to a notification its create names.
            if (lines > b.lines_max
                or (lines != 0 and (d.flags & KOS_DECL_DRIVER) != 0 and d.notify == 0))
            {
                return ImageCheck::DECL_LINES;
            }
            return ImageCheck::OK;
        }

        // Read only once the format check bounded decl_count, and on an enforcing board once
        // every span the declarations are reached through is the image's own code.
        ImageCheck check_decls(kos_image_header const& h, ImageBounds const& b, bool spans)
        {
            if (h.decl_count == 0)
            {
                return ImageCheck::OK;
            }
            if (not array_ok(h.decls, h.decl_count, sizeof(kos_task_decl), alignof(kos_task_decl),
                             h, spans))
            {
                return ImageCheck::DECL_SPAN;
            }
            kos_task_decl const* const decls =
                static_cast<kos_task_decl const*>(view(b, h.decls));
            for (uintptr_t i = 0; i < h.decl_count; i++)
            {
                kos_task_decl const& d = decls[i];
                // Offset 0 is the first declaration itself.
                if (d.name == 0 or not offset_ok(d.name, 1, h, spans)
                    or (spans and not string_within(b, h.decls + d.name, h.code_end)))
                {
                    return ImageCheck::DECL_NAME;
                }
                if (d.priority < b.prio_min or d.priority > b.prio_max)
                {
                    return ImageCheck::DECL_PRIORITY;
                }
                if (d.ceiling < d.priority or d.ceiling > b.prio_max)
                {
                    return ImageCheck::DECL_CEILING;
                }
                if (d.core_mask != 0 and (d.core_mask & b.cores) == 0)
                {
                    return ImageCheck::DECL_CORES;
                }
                uint16_t const known = KOS_DECL_CONSOLE | KOS_DECL_DRIVER | KOS_DECL_ON_REQUEST
                                       | KOS_DECL_RESTARTS;
                // A driver any creator may start holds no device, port or line, and takes the
                // console.
                bool const driver_on_request =
                    (d.flags & (KOS_DECL_DRIVER | KOS_DECL_ON_REQUEST))
                    == (KOS_DECL_DRIVER | KOS_DECL_ON_REQUEST);
                if ((d.flags & ~known) != 0
                    or (driver_on_request
                        and (d.grant_count != 0 or (d.flags & KOS_DECL_CONSOLE) == 0)))
                {
                    return ImageCheck::DECL_FLAGS;
                }
                if (not aligned(d.stack, b.stack_align)
                    or (d.stack == 0 and (d.flags & KOS_DECL_DRIVER) == 0))
                {
                    return ImageCheck::DECL_STACK;
                }
                if ((d.authority & ~b.authority) != 0)
                {
                    return ImageCheck::DECL_AUTHORITY;
                }
                ImageCheck const c = check_grants(d, b, h, spans);
                if (c != ImageCheck::OK)
                {
                    return c;
                }
            }
            return ImageCheck::OK;
        }
#endif
    }

    ImageCheck image_header_check(kos_image_header const& h, ImageBounds const& b,
                                  kos_image_header const* const* earlier, size_t n)
    {
        if (h.magic != KOS_IMAGE_MAGIC)
        {
            return ImageCheck::MAGIC;
        }
        if (h.version != KOS_IMAGE_VERSION)
        {
            return ImageCheck::VERSION;
        }
        if (h.size != sizeof(kos_image_header))
        {
            return ImageCheck::SIZE;
        }
        if (h.build_id != b.build_id)
        {
            return ImageCheck::BUILD_ID;
        }
        // This format carries neither: a header naming either is from a later format.
        if (h.needs != 0 or h.needs_count != 0)
        {
            return ImageCheck::NEEDS;
        }
        if ((h.decls == 0) != (h.decl_count == 0) or h.decl_count > b.decls_max
            or not aligned(h.decls, alignof(kos_task_decl)))
        {
            return ImageCheck::DECLS;
        }

        if (h.start == 0)
        {
            return ImageCheck::START;
        }
        if (h.init_args == 0 or not aligned(h.init_args, sizeof(void*)))
        {
            return ImageCheck::INIT_ARGS;
        }
        if (h.thread_return == 0)
        {
            return ImageCheck::THREAD_RETURN;
        }
        if (b.task_start != (h.task_start != 0))
        {
            return ImageCheck::TASK_START;
        }
        if ((h.decl_start == 0) != (h.decl_count == 0))
        {
            return ImageCheck::DECL_START;
        }
        if (h.cas_store < h.cas_start or (h.cas_start == 0) != (h.cas_store == 0))
        {
            return ImageCheck::CAS;
        }

#if KICKOS_IMAGE_CHECK_SPANS
        if (b.spans)
        {
            ImageCheck const c = check_spans(h, b, earlier, n);
            if (c != ImageCheck::OK)
            {
                return c;
            }
        }
        return check_decls(h, b, b.spans);
#else
        (void)earlier;
        (void)n;
        return ImageCheck::OK;
#endif
    }

#if KICKOS_IMAGE_CHECK_SPANS
    char const* image_check_message(ImageCheck c)
    {
        switch (c)
        {
            case ImageCheck::OK:
            {
                return "accepted";
            }
            case ImageCheck::MAGIC:
            {
                return "magic";
            }
            case ImageCheck::VERSION:
            {
                return "version";
            }
            case ImageCheck::SIZE:
            {
                return "size";
            }
            case ImageCheck::BUILD_ID:
            {
                return "build_id";
            }
            case ImageCheck::NEEDS:
            {
                return "needs";
            }
            case ImageCheck::DECLS:
            {
                return "decls";
            }
            case ImageCheck::START:
            {
                return "start";
            }
            case ImageCheck::INIT_ARGS:
            {
                return "init_args";
            }
            case ImageCheck::THREAD_RETURN:
            {
                return "thread_return";
            }
            case ImageCheck::TASK_START:
            {
                return "task_start";
            }
            case ImageCheck::DECL_START:
            {
                return "decl_start";
            }
            case ImageCheck::CAS:
            {
                return "cas_start";
            }
            case ImageCheck::CODE_SPAN:
            {
                return "code span";
            }
            case ImageCheck::CODE_KERNEL:
            {
                return "code span meets the kernel";
            }
            case ImageCheck::CODE_OVERLAP:
            {
                return "code span meets an image";
            }
            case ImageCheck::WINDOW_SPAN:
            {
                return "window";
            }
            case ImageCheck::WINDOW_KERNEL:
            {
                return "window meets the kernel";
            }
            case ImageCheck::WINDOW_CODE:
            {
                return "window meets code memory";
            }
            case ImageCheck::WINDOW_OVERLAP:
            {
                return "window meets an image";
            }
            case ImageCheck::WINDOW_ENCODABLE:
            {
                return "window is not one region";
            }
            case ImageCheck::DATA:
            {
                return "data_start to bss_end";
            }
            case ImageCheck::HEAP:
            {
                return "heap";
            }
            case ImageCheck::REENT_SEAT:
            {
                return "reent_seat";
            }
            case ImageCheck::REENT_TEMPLATE:
            {
                return "reent_template";
            }
            case ImageCheck::DATA_LOAD:
            {
                return "data_load";
            }
            case ImageCheck::TLS_TEMPLATE:
            {
                return "tls_template";
            }
            case ImageCheck::TLS_SIZES:
            {
                return "tls_init";
            }
            case ImageCheck::TLS_ALIGN:
            {
                return "tls_align";
            }
            case ImageCheck::TLS_SEGMENT:
            {
                return "tls_offset";
            }
            case ImageCheck::CTOR:
            {
                return "ctor window";
            }
            case ImageCheck::EH_FRAME:
            {
                return "eh_frame";
            }
            case ImageCheck::PROGRAM:
            {
                return "program";
            }
            case ImageCheck::STAMP:
            {
                return "stamp";
            }
            case ImageCheck::DECL_SPAN:
            {
                return "decls outside the code span";
            }
            case ImageCheck::DECL_NAME:
            {
                return "declaration name";
            }
            case ImageCheck::DECL_PRIORITY:
            {
                return "declaration priority";
            }
            case ImageCheck::DECL_CEILING:
            {
                return "declaration ceiling";
            }
            case ImageCheck::DECL_CORES:
            {
                return "declaration cores";
            }
            case ImageCheck::DECL_STACK:
            {
                return "declaration stack";
            }
            case ImageCheck::DECL_AUTHORITY:
            {
                return "declaration authority";
            }
            case ImageCheck::DECL_FLAGS:
            {
                return "declaration flags";
            }
            case ImageCheck::DECL_GRANTS:
            {
                return "declaration grants";
            }
            case ImageCheck::DECL_KIND:
            {
                return "declaration grant kind";
            }
            case ImageCheck::DECL_WINDOWS:
            {
                return "declaration window count";
            }
            case ImageCheck::DECL_DEVICE:
            {
                return "declaration device window";
            }
            case ImageCheck::DECL_PORTS:
            {
                return "declaration port window";
            }
            case ImageCheck::DECL_LINES:
            {
                return "declaration line count";
            }
            case ImageCheck::DECL_LINE:
            {
                return "declaration line";
            }
            default:
            {
                break;
            }
        }
        return "refused";
    }
#endif
}
