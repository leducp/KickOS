// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Replaces libstdc++'s vterminate.o on the FULL_CXX link path, so the linker never
// extracts it. Demangling t->name() here would pull __cxa_demangle back in, and the
// newlib float dtoa with it (~65K flash); the name stays mangled.

#include <exception>
#include <cstdlib>
#include <typeinfo>
#include <cxxabi.h> // abi::__cxa_current_exception_type
#include <kickos/kos.h>

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
        std::type_info* t = abi::__cxa_current_exception_type();
        if (t != nullptr)
        {
            kos::print(" [");
            kos::print(t->name());
            kos::print("]");
            // Only safe to re-throw when an exception is actually active; a bare
            // `throw;` otherwise recurses back into terminate.
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
        kos::print("\n");
        std::abort();
    }
}
