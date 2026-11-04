// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/tls.h>

#include <kickos/arch/arch.h>
#include <kickos/aspace.h>
#include <kickos/config/system.h>
#include <kickos/image.h>
#include <kickos/instance.h>
#include <kickos/kernel.h>
#include <kickos/kruntime.h>
#include <kickos/ustack.h>

namespace kickos
{

#if KICKOS_TLS

namespace
{
    // Image `image`'s part of the link's one thread-local segment, from its published header:
    // the bytes its template initialises and the span its offsets reach, its own .tbss and any
    // gap the linker put before it included. The span is zero where the image declares no
    // thread_local.
    kos_image_header const& tls_of(size_t image)
    {
        return image_record(image);
    }

    unsigned char const* template_of(kos_image_header const& h, size_t image)
    {
        void const* const own = reinterpret_cast<void const*>(h.tls_template);
        void const* const alias = aspace_image_alias(image, own);
        if (alias != nullptr)
        {
            return static_cast<unsigned char const*>(alias);
        }
        return static_cast<unsigned char const*>(own);
    }
}

size_t tls_block_size(size_t image)
{
    kos_image_header const& h = tls_of(image);
    if (h.tls_total == 0)
    {
        return 0;
    }
    size_t const block = KICKOS_ARCH_TLS_TCB + static_cast<size_t>(h.tls_total);
    return (block + (KICKOS_STACK_ALIGN - 1u)) & ~static_cast<size_t>(KICKOS_STACK_ALIGN - 1u);
}

uintptr_t tls_thread_pointer(size_t image, uintptr_t block)
{
    return block - static_cast<uintptr_t>(tls_of(image).tls_offset);
}

void tls_seat(void* block, size_t image)
{
    if (tls_block_size(image) == 0)
    {
        return;
    }
    kos_image_header const& h = tls_of(image);
    unsigned char* const p = static_cast<unsigned char*>(block) + KICKOS_ARCH_TLS_TCB;
    size_t const initialised = static_cast<size_t>(h.tls_init);
    kmemcpy(p, template_of(h, image), initialised);
    // From the end of the template to the end of the span: any alignment gap the linker
    // inserted, then .tbss. Both must read as zero.
    kmemset(p + initialised, 0, static_cast<size_t>(h.tls_total) - initialised);
}

#else

size_t tls_block_size(size_t)
{
    return 0;
}

uintptr_t tls_thread_pointer(size_t, uintptr_t block)
{
    return block;
}

void tls_seat(void*, size_t)
{
}

#endif

#if KICKOS_TLS or KICKOS_LIBC_REENT

size_t tls_libc_carve(size_t image)
{
    return static_cast<size_t>(image_record(image).reent_carve);
}

namespace
{
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

    bool carve_fits(uintptr_t base, size_t size, size_t carve, size_t image)
    {
        (void)image;
        if (carve == 0)
        {
            return true;
        }
#if KICKOS_TLS and KICKOS_TLS_FROM_SP
        // WHERE THE THREAD POINTER IS SP MASKED down to KICKOS_TLS_STRIDE and a thread_local is
        // declared, the block must BE one stride and sit on one: two blocks smaller than a
        // stride mask to the same base, so one thread would read another's thread-local
        // storage. A caller-supplied pointer is not strided by the allocator.
        if (tls_block_size(image) != 0)
        {
            if ((base & (KICKOS_TLS_STRIDE - 1u)) != 0)
            {
                return false;
            }
            if (size != KICKOS_TLS_STRIDE)
            {
                return false;
            }
        }
#endif
        if ((base & (KICKOS_STACK_ALIGN - 1u)) != 0)
        {
            return false;
        }
        // A block no larger than the carve leaves the thread no stack.
        return size > carve;
    }
}

size_t tls_pool_carve_size(size_t image)
{
    return tls_libc_carve(image) + tls_block_size(image);
}

size_t tls_carve_size(Thread const* t)
{
    size_t const image = task_image(t->task);
    if (is_idle_tcb(t))
    {
        return tls_block_size(image);
    }
    return tls_pool_carve_size(image);
}

bool tls_stack_admissible(uintptr_t base, size_t size, size_t image)
{
    return carve_fits(base, size, tls_pool_carve_size(image), image);
}

void* tls_carve(Thread const* t, void* stack_base, size_t stack_size)
{
    size_t const carve = tls_carve_size(t);
    if (carve == 0)
    {
        return nullptr;
    }
    size_t const image = task_image(t->task);
    // Keyed on identity and never on size: a caller-supplied stack that skipped the carve would
    // leave the thread pointer answering for a block nobody seated.
    bool const admissible
        = carve_fits(reinterpret_cast<uintptr_t>(stack_base), stack_size, carve, image);
    KICKOS_ASSERT(admissible or is_idle_tcb(t));
    if (not admissible)
    {
        return nullptr;
    }
    void* home = stack_base;
#if KICKOS_HAVE_ASPACE
    // A user stack is named in the CHILD's space, not the running one, and ring 0 never writes
    // through a user address: spawn admission refuses a stack the pool gives no alias.
    if (not t->privileged)
    {
        home = ustack_kptr(reinterpret_cast<uintptr_t>(stack_base));
        KICKOS_ASSERT(home != nullptr);
    }
#endif
    tls_seat(static_cast<unsigned char*>(home) + (carve - tls_block_size(image)), image);
    return home;
}

#else

size_t tls_libc_carve(size_t)
{
    return 0;
}

size_t tls_pool_carve_size(size_t)
{
    return 0;
}

size_t tls_carve_size(Thread const*)
{
    return 0;
}

bool tls_stack_admissible(uintptr_t, size_t, size_t)
{
    return true;
}

void* tls_carve(Thread const*, void*, size_t)
{
    return nullptr;
}

#endif

}
