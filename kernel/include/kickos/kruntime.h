// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The runtime the KERNEL calls. Spell these names in kernel code and the call is correct on
// every backend.
//
// WHERE A TRANSLATING BACKEND SPLITS THE IMAGE IN TWO, they are the kernel's OWN copies and
// the ordinary names belong to the app: app text is EL0-reachable, so it carries
// privileged-execute-never, and a per-process root need not map it at all.
// tests/static/check_kernel_runtime.sh refuses an ordinary name in an archive holding
// kernel text there. A reference the COMPILER emitted, an aggregate copy or the default
// construction of a struct with default member initialisers, is rewritten to these names
// after `ar` by kickos_privatise_runtime, so only an EXPLICIT call has to be spelled this
// way.
//
// A REGION BACKEND HAS NO SUCH SPLIT and never will: one text mapping serves both
// privilege levels, so a second copy would be flash spent on nothing. The two 64 KB parts
// in the fleet have under 400 bytes of headroom at their selftest preset, which is less
// than the duplicate costs. So the names alias the app's there, and every kernel call site
// reads the same either way.

#ifndef KICKOS_KRUNTIME_H
#define KICKOS_KRUNTIME_H

#include <stdarg.h>
#include <stddef.h>

#if KICKOS_HAVE_ASPACE

extern "C"
{
    void* kmemcpy(void* dst, void const* src, size_t n);
    void* kmemset(void* dst, int c, size_t n);
    void* kmemmove(void* dst, void const* src, size_t n);
    int kmemcmp(void const* a, void const* b, size_t n);
    size_t kstrlen(char const* s);
    size_t kstrnlen(char const* s, size_t maxlen);

    // kvsnprintf/ksnprintf under the private name: same formatter, same limits
    // (lib/include/kickos/libc/fmt.h).
    int kfmt_vsnprintf(char* buf, size_t size, char const* fmt, va_list ap);
    int kfmt_snprintf(char* buf, size_t size, char const* fmt, ...)
        __attribute__((format(printf, 3, 4)));
}

#else

#include <kickos/libc/fmt.h>
#include <kickos/libc/string.h>

#define kmemcpy memcpy
#define kmemset memset
#define kmemmove memmove
#define kmemcmp memcmp
#define kstrlen strlen
#define kstrnlen strnlen
#define kfmt_vsnprintf kvsnprintf
#define kfmt_snprintf ksnprintf

#endif

#endif
