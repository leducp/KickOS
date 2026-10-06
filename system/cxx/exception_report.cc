// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Pulled in only through --wrap=__cxa_allocate_exception: nothing may reference it directly.

#include <cstddef>
#include <exception>
#include <typeinfo>
#include <cxxabi.h> // abi::__cxa_current_exception_type
#include <kickos/kos.h>

#include "exception_report.h"

extern "C" void* __real___cxa_allocate_exception(std::size_t size) noexcept;

extern "C" void* __wrap___cxa_allocate_exception(std::size_t size) noexcept
{
    return __real___cxa_allocate_exception(size);
}

namespace
{
    void report()
    {
        std::type_info* t = abi::__cxa_current_exception_type();
        if (t == nullptr)
        {
            return;
        }
        kos::print(" [");
        kos::print(t->name());
        kos::print("]");
        // Only safe to re-throw when an exception is actually active; a bare `throw;` otherwise
        // recurses back into terminate.
        try
        {
            throw;
        }
        catch (std::exception const& e)
        {
            kos::print(": ");
            kos::print(e.what());
        }
        catch (...)
        {
        }
    }
}

#if KICKOS_HAVE_ASPACE && !KICKOS_LINKER_WEAK_UNDEF
// A PE32+ image holds no undefined weak reference, so user/src/root_entry.cc registers no unwind
// tables there; this constructor does, first of the app's, ahead of any that throws.
extern "C" unsigned char __eh_frame_start[];
extern "C" void __register_frame(void*);
#endif

__attribute__((constructor(101))) static void kickos_exception_report_install()
{
#if KICKOS_HAVE_ASPACE && !KICKOS_LINKER_WEAK_UNDEF
    __register_frame(__eh_frame_start);
#endif
    kickos::cxx::g_exception_report = report;
}
