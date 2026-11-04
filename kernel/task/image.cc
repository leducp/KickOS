// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/image.h>

#include <kickos/arch/arch.h>
#include <kickos/aspace.h>
#include <kickos/config/system.h>
#include <kickos/grant.h>
#include <kickos/instance_local.h>
#include <kickos/irq.h>
#include <kickos/kernel.h>
#include <kickos/klink.h>
#include <kickos/pools.h>
#include <kickos/kruntime.h>

extern "C"
{
    // The final link's table of header addresses, each the image's first code byte.
    extern kos_image_header const* const __kickos_image_table[] KICKOS_LINK_BOUND;
    extern kos_image_header const* const __kickos_image_table_end[] KICKOS_LINK_BOUND;

#if not KICKOS_ARCH_SIM
    extern unsigned char __kickos_build_id[] KICKOS_LINK_BOUND;
#if not KICKOS_HAVE_ASPACE
    extern unsigned char __kickos_image_data_in_place[] KICKOS_LINK_BOUND;
    extern unsigned char __kickos_codemem_start[] KICKOS_LINK_BOUND;
    extern unsigned char __kickos_codemem_end[] KICKOS_LINK_BOUND;
    extern unsigned char __kickos_kernel_code_start[] KICKOS_LINK_BOUND;
    extern unsigned char __kickos_kernel_code_end[] KICKOS_LINK_BOUND;
    extern unsigned char __kickos_rammem_start[] KICKOS_LINK_BOUND;
    extern unsigned char __kickos_rammem_end[] KICKOS_LINK_BOUND;
    extern unsigned char __kickos_kernel_data_start[] KICKOS_LINK_BOUND;
    extern unsigned char __kickos_kernel_data_end[] KICKOS_LINK_BOUND;
    extern unsigned char __kickos_kernel_stack_start[] KICKOS_LINK_BOUND;
    extern unsigned char __kickos_kernel_stack_end[] KICKOS_LINK_BOUND;
    extern unsigned char __kickos_ram_start[] KICKOS_LINK_BOUND;
    extern unsigned char __kickos_ram_end[] KICKOS_LINK_BOUND;
    extern unsigned char __kickos_tls_segment_start[] KICKOS_LINK_BOUND;
    extern unsigned char __kickos_tls_segment_end[] KICKOS_LINK_BOUND;
#endif
#endif

#if KICKOS_IMAGE_CAS
    // [lowest load, highest store] over every image's CAS window, which the switcher tests
    // before it walks the image table. Empty until the records are published.
    uintptr_t g_kickos_ucas_span[2] = {UINTPTR_MAX, 0};
#endif
}

namespace kickos
{
    namespace
    {
        // The table and every bound are link-time words, read through volatile so none folds
        // into a constant the kernel's code model cannot materialise.
        kos_image_header const* const* const volatile g_table = __kickos_image_table;
        kos_image_header const* const* const volatile g_table_end = __kickos_image_table_end;
#if not KICKOS_ARCH_SIM
        unsigned char* const volatile g_build_id = __kickos_build_id;
#if not KICKOS_HAVE_ASPACE
        unsigned char* const volatile g_data_in_place = __kickos_image_data_in_place;
#endif
#if KICKOS_IMAGE_CHECK_SPANS and not KICKOS_HAVE_ASPACE
        unsigned char* const volatile g_tls_start = __kickos_tls_segment_start;
        unsigned char* const volatile g_tls_end = __kickos_tls_segment_end;
#endif
#endif

        // The images a link may hold: the init's and one per declared task, which the task pool
        // bounds.
        size_t images_max()
        {
            return static_cast<size_t>(kickos_kernel_pools.task_count) + 1u;
        }

#if KICKOS_IMAGE_CAS
        static_assert(offsetof(kos_image_header, cas_start) == KICKOS_IMAGE_HEADER_OFF_CAS_START
                          and offsetof(kos_image_header, cas_store)
                                  == KICKOS_IMAGE_HEADER_OFF_CAS_STORE,
                      "the switch reads each header's CAS window at these offsets");

        // Widens the switcher's span to the CAS window of a checked header `h`.
        void ucas_span_add(kos_image_header const& h)
        {
            if (h.cas_start == 0)
            {
                return;
            }
            // A Thumb function's address carries bit 0, a stacked PC never does.
            uintptr_t start = h.cas_start;
#if defined(__thumb__)
            start = start & ~static_cast<uintptr_t>(1);
#endif
            if (start < g_kickos_ucas_span[0])
            {
                g_kickos_ucas_span[0] = start;
            }
            if (h.cas_store > g_kickos_ucas_span[1])
            {
                g_kickos_ucas_span[1] = h.cas_store;
            }
        }
#endif

        // Per instance: the multi-instance sim boots its kernels at once over one image. Where
        // no backend translates, the kernel reads each header where the link's table names it;
        // where one does, through the headers kept in the pools' storage.
        struct Records
        {
            size_t count;
            size_t decls; // every image's declarations together
        };
        constinit InstanceLocal<Records> g_records = {};


        uintptr_t addr(void const* p)
        {
            return reinterpret_cast<uintptr_t>(p);
        }

#if KICKOS_IMAGE_CHECK_SPANS and KICKOS_HAVE_MPU and not KICKOS_HAVE_ASPACE \
    and not KICKOS_ARCH_SIM
        bool window_encodable(uintptr_t base, uintptr_t size)
        {
            return arch_mpu_region_encodable(base, static_cast<size_t>(size));
        }
#endif

        void const* image_view(size_t image, uintptr_t a)
        {
            void* const alias = aspace_image_alias(image, reinterpret_cast<void const*>(a));
            if (alias != nullptr)
            {
                return alias;
            }
            return reinterpret_cast<void const*>(a);
        }

#if KICKOS_IMAGE_CHECK_SPANS
        bool line_ok(uint16_t line)
        {
            return irq_line_admit(line) == 0;
        }

        bool device_ok(uintptr_t base, uintptr_t size)
        {
            return grant_device_window_admit(base, static_cast<size_t>(size)) == 0;
        }

#if KICKOS_ARCH_HAS_PORTS
        bool ports_ok(uintptr_t base, uintptr_t count)
        {
            return count != 0 and base <= 0x10000u and count <= 0x10000u - base
                   and port_aperture_ok(base, static_cast<size_t>(count));
        }
#endif
#endif

        ImageBounds bounds(size_t image)
        {
            ImageBounds b{};
            b.image = image;
            b.decls_max = static_cast<uintptr_t>(kickos_kernel_pools.task_count);
#if KICKOS_IMAGE_CHECK_SPANS
            b.prio_min = KICKOS_PRIO_MIN;
            b.prio_max = KICKOS_PRIO_MAX;
            b.cores = KICKOS_CORE_SET_ALL;
            b.authority = KOS_AUTH_ALL;
            b.stack_align = KICKOS_STACK_ALIGN;
            b.windows_max = KICKOS_MAX_THREAD_WINDOWS;
            b.lines_max = KICKOS_MAX_SPAWN_GRANTS;
            b.line_ok = line_ok;
            b.view = image_view;
            b.device_ok = device_ok;
#if KICKOS_ARCH_HAS_PORTS
            b.ports_ok = ports_ok;
#endif
#endif
#if KICKOS_ARCH_SIM
            b.spans = false;
#else
            b.build_id = addr(g_build_id);
            b.spans = true;
#if not KICKOS_HAVE_ASPACE
            b.data_in_place = addr(g_data_in_place) != 0;
#endif
#if KICKOS_HAVE_ASPACE
            b.task_start = true;
#endif
#if KICKOS_IMAGE_CHECK_SPANS
            b.tls_align_max = KICKOS_STACK_ALIGN;
#if KICKOS_HAVE_ASPACE
            b.data_in_place = true;
            aspace_image_spans(image, &b.code, &b.ram, &b.tls);
#else
            b.tls = {addr(g_tls_start), addr(g_tls_end)};
            unsigned char* const volatile w[] = {
                __kickos_codemem_start,      __kickos_codemem_end,
                __kickos_kernel_code_start,  __kickos_kernel_code_end,
                __kickos_rammem_start,       __kickos_rammem_end,
                __kickos_kernel_data_start,  __kickos_kernel_data_end,
                __kickos_kernel_stack_start, __kickos_kernel_stack_end,
                __kickos_ram_start,          __kickos_ram_end,
            };
            b.code = {addr(w[0]), addr(w[1])};
            b.kernel_code = {addr(w[2]), addr(w[3])};
            b.ram = {addr(w[4]), addr(w[5])};
            b.kernel_data = {addr(w[6]), addr(w[7])};
            b.kernel_stacks = {addr(w[8]), addr(w[9])};
            b.arena = {addr(w[10]), addr(w[11])};
#if KICKOS_HAVE_MPU
            b.window_encodable = window_encodable;
#endif
#endif
#endif
#endif
            return b;
        }

        // The kernel's own view of a header image `image`'s view names.
        kos_image_header const* kernel_view(size_t image, kos_image_header const* h)
        {
            void* const alias = aspace_image_alias(image, h);
            if (alias != nullptr)
            {
                return static_cast<kos_image_header const*>(alias);
            }
            return h;
        }

        kos_image_header const* header_of(size_t i)
        {
#if KICKOS_HAVE_ASPACE
            return pool_local(kickos_kernel_pools.image_headers)[i];
#else
            return g_table[i];
#endif
        }

        // The headers already checked, in the kernel's view.
        kos_image_header const* const* checked_headers()
        {
#if KICKOS_HAVE_ASPACE
            return pool_local(kickos_kernel_pools.image_headers);
#else
            return g_table;
#endif
        }
    }

    void image_records_publish(void)
    {
        size_t const n = static_cast<size_t>(addr(g_table_end) - addr(g_table))
                         / sizeof(kos_image_header const*);
        if (n == 0 or n > images_max())
        {
            kpanic("image table");
        }
#if KICKOS_HAVE_ASPACE
        if (aspace_image_frames_count() != n)
        {
            kpanic("image frames");
        }
#endif
        Records& r = g_records.get();
        for (size_t i = 0; i < n; i++)
        {
            ImageBounds const b = bounds(i);
            kos_image_header const* const h = kernel_view(i, g_table[i]);
            // Where a backend translates, every image sits at the same addresses in spaces of
            // its own, so no image's spans can meet another's.
            size_t met = r.count;
#if KICKOS_HAVE_ASPACE
            met = 0;
#endif
            ImageCheck const c = image_header_check(*h, b, checked_headers(), met);
            if (c != ImageCheck::OK)
            {
#if KICKOS_IMAGE_CHECK_SPANS
                kprintf("image %u header refused at %s\n", static_cast<unsigned>(i),
                        image_check_message(c));
                kpanic("image header refused");
#else
                kpanic_refuse_unlocked_hex("image header refused at check ",
                                           static_cast<unsigned>(c));
#endif
            }
#if KICKOS_HAVE_ASPACE
            pool_local(kickos_kernel_pools.image_headers)[r.count] = h;
#else
            // Where the loader placed .data where it runs, its template is filled here, before
            // anything writes it, for the image's start stub to copy back at every start.
            if (b.data_in_place and h->data_load != h->data_start)
            {
                kmemcpy(reinterpret_cast<void*>(h->data_load),
                        reinterpret_cast<void const*>(h->data_start),
                        static_cast<size_t>(h->data_end - h->data_start));
            }
#endif
#if KICKOS_IMAGE_CAS
            ucas_span_add(*h);
#endif
            r.count++;
            r.decls = r.decls + h->decl_count;
        }
        kickos_image_thread_return
            = reinterpret_cast<void (*)(void)>(header_of(0)->thread_return);
    }

    size_t image_count(void)
    {
        return g_records.get().count;
    }

    kos_image_header const& image_record(size_t i)
    {
        return *header_of(i);
    }

    void image_decls_of(int image, size_t* first, size_t* count)
    {
        Records const& r = g_records.get();
        *first = 0;
        *count = 0;
        for (size_t k = 0; k < r.count; k++)
        {
            if (static_cast<size_t>(image) == k)
            {
                *count = header_of(k)->decl_count;
                return;
            }
            *first = *first + header_of(k)->decl_count;
        }
    }

    namespace
    {
        // The image holding declaration i, and i's place in it; r.count past the last.
        size_t decl_image(size_t i, size_t* local)
        {
            Records const& r = g_records.get();
            size_t k = 0;
            while (k < r.count and i >= header_of(k)->decl_count)
            {
                i = i - header_of(k)->decl_count;
                k++;
            }
            *local = i;
            return k;
        }

        // The image whose declaration array holds `d`.
        size_t decl_owner(kos_task_decl const* d)
        {
            Records const& r = g_records.get();
            for (size_t k = 0; k + 1 < r.count; k++)
            {
                kos_task_decl const* const first
                    = static_cast<kos_task_decl const*>(image_view(k, header_of(k)->decls));
                if (d >= first and d < first + header_of(k)->decl_count)
                {
                    return k;
                }
            }
            return r.count - 1;
        }
    }

    kos_task_decl const* image_decl(size_t i)
    {
        Records const& r = g_records.get();
        size_t local = 0;
        size_t const k = decl_image(i, &local);
        if (k >= r.count)
        {
            return nullptr;
        }
        return static_cast<kos_task_decl const*>(image_view(k, header_of(k)->decls)) + local;
    }

    size_t image_decl_count(void)
    {
        return g_records.get().decls;
    }

    size_t image_of_decl(size_t i)
    {
        size_t local = 0;
        return decl_image(i, &local);
    }

    bool image_same_program(size_t a, size_t b)
    {
        char const* const x = image_program_name(static_cast<int>(a));
        char const* const y = image_program_name(static_cast<int>(b));
        size_t n = 0;
        while (x[n] == y[n])
        {
            if (x[n] == '\0')
            {
                return true;
            }
            n++;
        }
        return false;
    }

    uintptr_t image_decl_start(size_t i)
    {
        size_t local = 0;
        return header_of(decl_image(i, &local))->decl_start;
    }

    char const* image_decl_name(kos_task_decl const* d)
    {
        size_t const k = decl_owner(d);
        return static_cast<char const*>(image_view(k, header_of(k)->decls + d->name));
    }

    kos_decl_grant const* image_decl_grants(kos_task_decl const* d)
    {
        size_t const k = decl_owner(d);
        return static_cast<kos_decl_grant const*>(image_view(k, header_of(k)->decls + d->grants));
    }

    kos_image_header const* image_program(int i)
    {
        Records const& r = g_records.get();
        if (i < 0 or static_cast<size_t>(i) >= r.count)
        {
            return nullptr;
        }
        return header_of(static_cast<size_t>(i));
    }

    void image_thread_return_select(size_t i)
    {
        kickos_image_thread_return
            = reinterpret_cast<void (*)(void)>(header_of(i)->thread_return);
    }

    char const* image_program_name(int i)
    {
        size_t const k = static_cast<size_t>(i);
        return static_cast<char const*>(image_view(k, header_of(k)->program));
    }
}

void (*kickos_image_thread_return)(void) = nullptr;
