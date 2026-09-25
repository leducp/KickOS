// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/tls.h>

#include <kickos/arch/arch.h>
#include <kickos/config/system.h>
#include <kickos/instance.h>
#include <kickos/kernel.h>
#include <kickos/kruntime.h>
#include <kickos/ustack.h>

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

    // Valid only once the sections are known non-empty.
    size_t tls_payload_bytes()
    {
        return static_cast<size_t>(__kickos_tbss_end - __kickos_tdata_start);
    }

    // By storage: thread_create runs before the scheduler knows any idle thread.
    bool is_idle_tcb(Thread const* t)
    {
        Kernel& k = kernel();
        if (t == &k.idle_tcb)
        {
            return true;
        }
#if KICKOS_KERNEL_CORES > 1
        for (Thread const& peer : k.idle_tcb_peer)
        {
            if (t == &peer)
            {
                return true;
            }
        }
#endif
        return false;
    }
}

size_t tls_block_size()
{
    // EMPTINESS IS DECIDED BY THE TWO SECTIONS AND NOT BY THE SPAN: with no thread_local the
    // empty .tdata's ALIGN(8) leaves tbss_end below tdata_start and the span reads 0xFFFFFFFC.
    size_t const tdata = tdata_bytes();
    size_t const tbss = static_cast<size_t>(__kickos_tbss_end - __kickos_tbss_start);
    if (tdata == 0 and tbss == 0)
    {
#if KICKOS_REENT_IN_TCB
        // The control block at the thread pointer holds libc's reentrant-state pointer.
        return (KICKOS_ARCH_TLS_TCB + (KICKOS_STACK_ALIGN - 1u))
            & ~static_cast<size_t>(KICKOS_STACK_ALIGN - 1u);
#else
        return 0;
#endif
    }
    // THE SPAN AND NOT THE SUM: the compiler's offsets include any gap the linker puts between
    // .tdata and .tbss.
    size_t const payload = tls_payload_bytes();
    // The ABI reserve at the thread pointer; zero on a variant 2 arch.
    size_t const block = KICKOS_ARCH_TLS_TCB + payload;
    return (block + (KICKOS_STACK_ALIGN - 1u)) & ~static_cast<size_t>(KICKOS_STACK_ALIGN - 1u);
}

bool tls_stack_admissible(uintptr_t base, size_t size)
{
    if (tls_block_size() == 0)
    {
        return true;
    }
#if KICKOS_TLS_FROM_SP
    // WHERE THE THREAD POINTER IS SP MASKED down to KICKOS_TLS_STRIDE the block must BE one
    // stride and sit on one: two blocks smaller than a stride mask to the same base, so one
    // thread would read another's thread-local storage. A caller-supplied pointer is not
    // strided by the allocator.
    if ((base & (KICKOS_TLS_STRIDE - 1u)) != 0)
    {
        return false;
    }
    if (size != KICKOS_TLS_STRIDE)
    {
        return false;
    }
#else
    if ((base & (KICKOS_STACK_ALIGN - 1u)) != 0)
    {
        return false;
    }
#endif
    // A block no larger than the carve leaves the thread no stack.
    return size > tls_block_size();
}

void tls_seat(void* base)
{
    size_t const block = tls_block_size();
    if (block == 0)
    {
        return;
    }
#if KICKOS_REENT_IN_TCB
    // A control block alone: over an empty template the span reads as a huge length.
    if (tdata_bytes() == 0 and __kickos_tbss_end - __kickos_tbss_start == 0)
    {
        return;
    }
#endif
    unsigned char* const p = static_cast<unsigned char*>(base) + KICKOS_ARCH_TLS_TCB;
    size_t const initialised = tdata_bytes();
    size_t const payload = tls_payload_bytes();
    kmemcpy(p, __kickos_tdata_start, initialised);
    // From the end of the template to the end of the span: any alignment gap the linker
    // inserted, then .tbss. Both must read as zero.
    kmemset(p + initialised, 0, payload - initialised);
}

void* tls_carve(Thread const* t, void* stack_base, size_t stack_size)
{
    if (tls_block_size() == 0)
    {
        return nullptr;
    }
    // Keyed on identity and never on size: a caller-supplied stack that skipped the carve would
    // leave the thread pointer answering for a block nobody seated.
    bool const admissible =
        tls_stack_admissible(reinterpret_cast<uintptr_t>(stack_base), stack_size);
    KICKOS_ASSERT(admissible or is_idle_tcb(t));
    if (not admissible)
    {
        return nullptr;
    }
    // A mapped stack is named by a virtual address in the CHILD's space, which is not the
    // running one at spawn, so the block is reached through the physical map.
    void* seat = stack_base;
    void* const alias = ustack_kptr(reinterpret_cast<uintptr_t>(stack_base));
    if (alias != nullptr)
    {
        seat = alias;
    }
    tls_seat(seat);
    return seat;
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

void* tls_carve(Thread const*, void*, size_t)
{
    return nullptr;
}

#endif

}
