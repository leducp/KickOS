// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/tls.h>

#include <kickos/arch/arch.h>
#include <kickos/config/system.h>
#include <kickos/debug.h>
#include <kickos/libc/string.h>

namespace kickos
{

#if defined(KICKOS_TLS) && KICKOS_TLS

extern "C"
{
    // sections.ld.h. .tbss immediately follows .tdata, and both are empty when the image
    // declares no thread_local.
    extern unsigned char __kickos_tdata_start[];
    extern unsigned char __kickos_tdata_end[];
    extern unsigned char __kickos_tbss_start[];
    extern unsigned char __kickos_tbss_end[];
}

namespace
{
    size_t tdata_bytes()
    {
        return static_cast<size_t>(__kickos_tdata_end - __kickos_tdata_start);
    }

    size_t tbss_bytes()
    {
        return static_cast<size_t>(__kickos_tbss_end - __kickos_tbss_start);
    }
}

size_t tls_block_size()
{
    size_t const payload = tdata_bytes() + tbss_bytes();
    if (payload == 0)
    {
        return 0;
    }
    // The ABI reserve sits BELOW the thread pointer on a variant 1 arch and is zero on a
    // variant 2 one; arch/<arch>/include/kickos/arch/context.h states which.
    size_t const block = KICKOS_ARCH_TLS_TCB + payload;
    return (block + (KICKOS_STACK_ALIGN - 1u)) & ~static_cast<size_t>(KICKOS_STACK_ALIGN - 1u);
}

bool tls_stack_admissible(uintptr_t base, size_t size)
{
    if (tls_block_size() == 0)
    {
        return true;
    }
    // THE STRIDE IS THE WHOLE CONTRACT. The thread pointer is SP masked down to
    // KICKOS_TLS_STRIDE, so a block that does not start on a multiple of it, or that spans
    // more than one of them, masks into a neighbour's TLS.
    if ((base & (KICKOS_TLS_STRIDE - 1u)) != 0)
    {
        return false;
    }
    if (size > KICKOS_TLS_STRIDE)
    {
        return false;
    }
    return size > tls_block_size();
}

bool tls_stack_below_stride(size_t size)
{
    if (tls_block_size() == 0)
    {
        return false;
    }
    return size < KICKOS_TLS_STRIDE;
}

void tls_seat(void* base)
{
    size_t const block = tls_block_size();
    if (block == 0)
    {
        return;
    }
    unsigned char* const p = static_cast<unsigned char*>(base) + KICKOS_ARCH_TLS_TCB;
    memcpy(p, __kickos_tdata_start, tdata_bytes());
    memset(p + tdata_bytes(), 0, tbss_bytes());
}

#else

size_t tls_block_size()
{
    return 0;
}

bool tls_stack_admissible(uintptr_t, size_t)
{
    return true;
}

bool tls_stack_below_stride(size_t)
{
    return false;
}

void tls_seat(void*)
{
}

#endif

} // namespace kickos
