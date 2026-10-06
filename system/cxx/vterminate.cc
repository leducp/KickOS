// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Replaces libstdc++'s vterminate.o on the kickos_cxx link path, so the linker never
// extracts it: demangling here would pull __cxa_demangle back in, and the newlib float dtoa
// with it (~65K flash). Nothing here may reference the exception runtime, or every full-C++
// image links it.

#include <cstdlib>
#include <kickos/kos.h>

#include "exception_report.h"

namespace kickos::cxx
{
    void (*g_exception_report)() = nullptr;
}

// GCC 15's <cxxabi.h> no longer declares this symbol; self-declare it so our
// strong definition still overrides the archive member.
namespace __gnu_cxx
{
    void __verbose_terminate_handler();
}

namespace __gnu_cxx
{
    void __verbose_terminate_handler()
    {
        static bool terminating = false;
        if (terminating)
        {
            kos::print("terminate: recursive, aborting\n");
            std::abort();
        }
        terminating = true;

        kos::print("terminate: uncaught exception");
        if (kickos::cxx::g_exception_report != nullptr)
        {
            kickos::cxx::g_exception_report();
        }
        kos::print("\n");
        std::abort();
    }
}
