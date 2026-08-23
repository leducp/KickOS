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

    // Only ever called once the sections are known non-empty; see tls_block_size().
    size_t tls_payload_bytes()
    {
        return static_cast<size_t>(__kickos_tbss_end - __kickos_tdata_start);
    }

}

size_t tls_block_size()
{
    // EMPTINESS IS DECIDED BY THE TWO SECTIONS, NOT BY THE SPAN, and the span is signed.
    // With no thread_local anywhere both sections are empty, and ld still assigns them
    // addresses: on every ARM preset the empty .tdata's ALIGN(8) advances the location
    // counter past where the empty .tbss lands, so tbss_end is FOUR BYTES BELOW
    // tdata_start. Read as size_t that is 0xFFFFFFFC, and every thread takes a carve the
    // size of the address space.
    size_t const tdata = tdata_bytes();
    size_t const tbss = static_cast<size_t>(__kickos_tbss_end - __kickos_tbss_start);
    if (tdata == 0 and tbss == 0)
    {
        return 0;
    }
    // THE SPAN AND NOT THE SUM, once there IS content. The linker may align .tbss above the
    // end of .tdata, and the offsets the compiler computed are relative to the layout it
    // produced, gap included. Adding the two sizes would size the block short by exactly
    // that gap and put every .tbss object past its end. The macro emits the two adjacent
    // and ld refuses them otherwise, so the span can only be the sum or more.
    // No assert that the span covers both sections: ld HARD-ERRORS on a non-adjacent
    // .tdata/.tbss pair, so a script cannot produce the case, and the check cost eight
    // bytes on the two deepest measured chains in the fleet.
    size_t const payload = tls_payload_bytes();
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
    // THE STRIDE IS THE WHOLE CONTRACT, and the block must BE one, not merely fit inside
    // one. The thread pointer is SP masked down to KICKOS_TLS_STRIDE, so two blocks smaller
    // than a stride can sit inside the SAME one and mask to the same base: an app handing
    // kos_thread_spawn two 1024-byte stacks 1024 apart would give both threads one
    // thread_local. Arena blocks cannot do that, being strided by the allocator, but a
    // caller-supplied pointer is not drawn from the arena.
    if ((base & (KICKOS_TLS_STRIDE - 1u)) != 0)
    {
        return false;
    }
    if (size != KICKOS_TLS_STRIDE)
    {
        return false;
    }
    return size > tls_block_size();
}

void tls_seat(void* base)
{
    size_t const block = tls_block_size();
    if (block == 0)
    {
        return;
    }
    unsigned char* const p = static_cast<unsigned char*>(base) + KICKOS_ARCH_TLS_TCB;
    size_t const initialised = tdata_bytes();
    size_t const payload = tls_payload_bytes();
    memcpy(p, __kickos_tdata_start, initialised);
    // From the end of the template to the end of the span: any alignment gap the linker
    // inserted, then .tbss. Both must read as zero.
    memset(p + initialised, 0, payload - initialised);
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

void tls_seat(void*)
{
}

#endif

} // namespace kickos
