// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Replaces libsupc++'s eh_globals.o, linked as an object ahead of libsupc++ in every image, so
// the linker never extracts it: each thread's exception state is its own, in its cell. Nothing
// here may reference the heap, or every image links it.

#include <cxxabi.h>
#include <unwind.h> // __ARM_EABI_UNWINDER__
#include <kickos/reent.h>

namespace
{
    // libsupc++'s __cxa_eh_globals (unwind-cxx.h), which no installed header defines.
    struct EhGlobals
    {
        void* caught;
        unsigned int uncaught;
#ifdef __ARM_EABI_UNWINDER__
        void* propagating;
#endif
    };
    static_assert(sizeof(EhGlobals) == sizeof(KickosThreadWords::eh_globals),
                  "the cell's exception state is not this runtime's __cxa_eh_globals");
    static_assert(alignof(EhGlobals) <= alignof(void*), "the cell's words align it");
}

extern "C" __cxxabiv1::__cxa_eh_globals* __cxxabiv1::__cxa_get_globals_fast() noexcept
{
    return reinterpret_cast<__cxa_eh_globals*>(kickos_thread_words()->eh_globals);
}

extern "C" __cxxabiv1::__cxa_eh_globals* __cxxabiv1::__cxa_get_globals() noexcept
{
    return reinterpret_cast<__cxa_eh_globals*>(kickos_thread_words()->eh_globals);
}
