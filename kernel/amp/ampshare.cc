// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/ampshare.h>

#if KICKOS_AMP_SHARE

#include <kickos/arch/amp_shared.h>
#include <kickos/aspace.h>
#include <kickos/domain.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/kruntime.h>
#include <kickos/ramown.h>
#include <kickos/task.h>
#include <kickos/thread.h>
#include <kickos/sys/errno.h>

#include <amp_partition.ld.h>

static_assert(KICKOS_AMP_USER_SHARE_ADDR == KICKOS_AMP_USER_SHARE_BASE,
              "cmake/amp_partition.cmake and arch/common/amp_partition.ld.h place the partition's "
              "user share apart");

namespace kickos
{
    unsigned char* amp_share_kernel_view(void)
    {
        // The chip script holds .amp_shared at the region's base.
        return __kickos_amp_shared_start + KICKOS_AMP_KERNEL_SHARED_SIZE;
    }

    void amp_share_clear(void)
    {
        if (KICKOS_AMP_NODE_ID != 0)
        {
            return;
        }
        kmemset(amp_share_kernel_view(), 0, AMP_SHARE_SIZE);
#if KICKOS_ARCH_ARENA_DCACHE
        arch_dcache_invalidate(amp_share_kernel_view(), AMP_SHARE_SIZE);
#endif
    }

    void amp_share_seat(Thread* root)
    {
        IrqLock lock;
        bool seated = false;
#if KICKOS_HAVE_ASPACE
        size_t const g = arch_aspace_granule();
        VirtualRanges* const r = domain_ranges_mut(thread_domain(root));
        // The share's user address is its physical one, which is what <kickos/amp.h> states.
        seated = r != nullptr and aspace_user_va(AMP_SHARE_BASE) == AMP_SHARE_BASE
                 and (AMP_SHARE_BASE % g) == 0 and (AMP_SHARE_SIZE % g) == 0
                 and r->reserve(AMP_SHARE_BASE, AMP_SHARE_SIZE / g, VR_SHARE);
#elif KICKOS_HAVE_MPU
        seated = ram_owner_seat(root->task, AMP_SHARE_BASE, AMP_SHARE_SIZE);
#else
        (void)root;
        seated = true;
#endif
        if (not seated)
        {
            kpanic(diag::kBootAmpShare);
        }
    }

#if KICKOS_HAVE_ASPACE
    bool amp_share_window(VirtualRanges const* own, kos_window const& w, int* rc)
    {
        size_t const g = arch_aspace_granule();
        if (own == nullptr or (w.base % g) != 0 or w.size > SIZE_MAX - (g - 1u))
        {
            return false;
        }
        size_t const pages = (w.size + g - 1u) / g;
        VirtualRange const* const e = own->find(w.base, pages * g);
        if (e == nullptr or (e->flags & VR_SHARE) == 0)
        {
            return false;
        }
        uint8_t type = ARCH_MAP_NORMAL;
        if ((w.flags & KOS_WINDOW_UNCACHED) != 0)
        {
            if (not arch_aspace_memtype_support(ARCH_MAP_NOCACHE))
            {
                *rc = -KOS_ENOTSUP;
                return true;
            }
            type = ARCH_MAP_NOCACHE;
        }
        *rc = 0;
        if ((w.size % g) != 0)
        {
            *rc = -KOS_EINVAL;
        }
        else if (not aspace_frames_type_ok(aspace_frame_of(w.base), pages, type, nullptr))
        {
            *rc = -KOS_EBUSY;
        }
        return true;
    }
#endif

#if defined(KICKOS_ENABLE_SELFTEST)
    bool amp_share_seated(uintptr_t* base, size_t* size)
    {
        Task const* const root = kernel().threads.slots[ThreadPool::ROOT_INDEX].task;
        if (root == nullptr)
        {
            return false;
        }
#if KICKOS_HAVE_ASPACE
        VirtualRanges const* const r = domain_ranges(task_domain(root));
        for (size_t i = 0; r != nullptr and i < VirtualRanges::capacity(); i++)
        {
            VirtualRange const* const e = r->at(i);
            if (e != nullptr and (e->flags & VR_SHARE) != 0)
            {
                *base = e->base;
                *size = static_cast<size_t>(e->pages) * arch_aspace_granule();
                return true;
            }
        }
        return false;
#elif KICKOS_HAVE_MPU
        return ram_owner_extent(root, AMP_SHARE_BASE, base, size);
#else
        (void)base;
        (void)size;
        return false;
#endif
    }
#endif
}

#endif
