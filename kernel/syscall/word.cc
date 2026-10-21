// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Whether a thread may name a word of user memory, and what it names.

#include <kickos/ampshare.h>
#include <kickos/arch/arch.h>
#include <kickos/aspace.h>
#include <kickos/domain.h>
#include <kickos/mpuset.h>
#include <kickos/thread.h>
#include <kickos/vrange.h>
#include <kickos/word.h>

#include <kickos/sys/errno.h>

#include <stddef.h>
#include <stdint.h>

#include "syscall_internal.h"

namespace kickos
{
    namespace
    {
#if KICKOS_HAVE_ASPACE
        int word_key(Thread const* c, uintptr_t va, arch_phys_addr_t* key)
        {
            Domain const* const d = thread_domain(c);
            struct arch_aspace* const space = domain_space(d);
            VirtualRanges const* const ranges = domain_ranges(d);
            if (space == nullptr or ranges == nullptr)
            {
                return -KOS_EFAULT;
            }
            size_t const g = arch_aspace_granule();
            VirtualRange const* const e = ranges->find(va, sizeof(uint32_t));
            bool writable = thread_writable_found(c, va, sizeof(uint32_t), e);
            // Stacks are mapped task-wide and never granted; the range's first granule is the
            // guard.
            if (not writable and e != nullptr and (e->flags & VR_USTACK) != 0
                and va - e->base >= g)
            {
                writable = true;
            }
            if (not writable or e == nullptr)
            {
                return -KOS_EFAULT;
            }
            uintptr_t const in = va & static_cast<uintptr_t>(g - 1u);
            arch_phys_addr_t const frame = arch_aspace_frame_at(space, va - in);
            if (frame == 0)
            {
                return -KOS_EFAULT;
            }
            if (e->memtype != static_cast<uint8_t>(ARCH_MAP_NORMAL))
            {
                return -KOS_EINVAL;
            }
            *key = frame + in;
            return 0;
        }
#else
        bool word_writable(Thread const* t, uintptr_t va)
        {
#if KICKOS_ARCH_SIM
            // The host's own heap and the host blocks standing in for small stacks lie outside
            // the arena and outside the image, where every thread writes.
            uintptr_t const arena = arch_ram_base();
            if (va < arena or va - arena >= arch_ram_size())
            {
                return true;
            }
#endif
            return thread_writable_ok(t, va, sizeof(uint32_t));
        }

        // No two memory types share a byte, so any region meeting the word decides its type.
        bool word_region_typed(Thread const* t, uintptr_t va)
        {
            for (arch_mpu_region const& r : t->mpu)
            {
                if ((r.attr & (ARCH_MPU_NOCACHE | ARCH_MPU_DEV)) != 0 and r.size != 0
                    and va <= r.base + (r.size - 1u) and r.base <= va + (sizeof(uint32_t) - 1u))
                {
                    return true;
                }
            }
            return false;
        }

        int word_key(Thread const* c, uintptr_t va, arch_phys_addr_t* key)
        {
            if (not word_writable(c, va))
            {
                return -KOS_EFAULT;
            }
            if (word_region_typed(c, va))
            {
                return -KOS_EINVAL;
            }
            *key = va;
            return 0;
        }
#endif
    }

    int word_ref(Thread const* c, uintptr_t va, WordRef* out)
    {
        if ((va & (sizeof(uint32_t) - 1u)) != 0)
        {
            return -KOS_EINVAL;
        }
        if (c == nullptr)
        {
            return -KOS_EFAULT;
        }
        arch_phys_addr_t key = 0;
        int const rc = word_key(c, va, &key);
        if (rc != 0)
        {
            return rc;
        }
#if KICKOS_AMP_SHARE
        if (amp_share_meets(key, key + sizeof(uint32_t)))
        {
            return -KOS_EINVAL;
        }
#endif
        out->key = key;
        out->space = user_space_of(c);
        out->va = va;
        return 0;
    }

    WordRef word_ref_parked(Thread const* w, arch_phys_addr_t key)
    {
        WordRef r;
        r.key = key;
        r.space = user_space_of(w);
        r.va = w->wait_va;
        return r;
    }

    bool word_could_hold(Thread const* t, WordRef const& w)
    {
        if (t == nullptr)
        {
            return false;
        }
#if KICKOS_HAVE_ASPACE
        Domain const* const d = thread_domain(t);
        struct arch_aspace* const space = domain_space(d);
        if (space == nullptr)
        {
            return false;
        }
        if (space == w.space)
        {
            return true;
        }
        return aspace_maps_writable(space, domain_ranges(d), w.key);
#elif KICKOS_MEMORY_ENFORCED
        // Never narrower than the hardware: a set is applied only when every overlap decides as
        // read_only_overlaps does (mpu_overlap_expressible, at spawn, boot and self-grant).
        return word_writable(t, w.va);
#else
        (void)w;
        return true;
#endif
    }
}
