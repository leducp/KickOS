// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/domain.h>

#include <kickos/image.h>

#include <kickos/aspace.h> // aspace_image_seed / aspace_release
#include <kickos/budget.h>
#include <kickos/grant.h> // grant_nocache_admissible
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/debug.h>  // KICKOS_DEBUG_ASSERT
#include <kickos/kernel.h> // KICKOS_ASSERT
#include <kickos/klink.h>
#include <kickos/ramown.h> // ram_grant_extent
#include <kickos/slotpool.h> // handle_pack / handle_index / handle_gen
#include <kickos/task.h>     // task_domains_head

#include <kickos/sys/errno.h>

#include <stdint.h> // UINT16_MAX

namespace kickos
{
    namespace
    {
        // The two immortal domains are pinned to the first pool slots; free_slot() never
        // returns them.
        enum { KDOM_KERNEL_INDEX = 0, KDOM_DEFAULT_USER_INDEX = 1 };

        // A claimed domain whose last reference went, or which was released unreferenced, heads
        // the free list, once. O(1).
        void free_put(Domain* d)
        {
            Kernel& k = kernel();
            if (d->immortal or d->refcount != 0 or k.domain_free.holds(&d->free_node))
            {
                return;
            }
#if KICKOS_HAVE_ASPACE
            domain_unpay(d);
#endif
            k.domain_free.push(&d->free_node);
        }

        // The slot freed last, else a never-claimed one. O(1).
        Domain* free_slot()
        {
            Kernel& k = kernel();
            ListNode* const n = k.domain_free.pop();
            if (n != nullptr)
            {
                return KICKOS_CONTAINER_OF(n, Domain, free_node);
            }
            int const fresh = KDOM_DEFAULT_USER_INDEX + 1 + k.domain_fresh;
            if (fresh >= k.domains().size())
            {
                return nullptr;
            }
            k.domain_fresh++;
            return &k.domains()[fresh];
        }

#if KICKOS_HAVE_ASPACE
        // The base of the handoff `d` borrows, or 0.
        uintptr_t handoff_base(Domain const* d)
        {
            for (size_t i = 0; i < VirtualRanges::capacity(); i++)
            {
                VirtualRange const* const e = d->ranges.at(i);
                if (aspace_range_handed_off(e))
                {
                    return e->base;
                }
            }
            return 0;
        }

        // The edge is returned to the caller; a release here would recurse down a chain of
        // borrowers. The donor's lend ends once this space maps its frames no more.
        Domain* drop_space(Domain* d)
        {
            Domain* const donor = d->borrowed_from;
            uintptr_t const lent = handoff_base(d);
            d->borrowed_from = nullptr;
            if (d->space != nullptr)
            {
                aspace_release(d->space, &d->ranges);
                d->space = nullptr;
                // A spaceless domain naming ranges is a shed one.
                d->ranges = VirtualRanges{};
            }
            else
            {
                // A shed space: what it still names is only its lent reservations.
                for (size_t i = 0; i < VirtualRanges::capacity(); i++)
                {
                    aspace_shed_free(&d->ranges, i);
                }
            }
            if (donor != nullptr)
            {
                domain_unlend(donor, lent);
            }
            return donor;
        }
#endif

        // A fresh unprivileged domain, with the address space it needs where a backend
        // translates, seeded with image `image` and charged to task slot `payer`, or to none
        // where it is negative. On refusal the slot is left reinitialised and *err written.
        Domain* claim_slot(size_t image, int payer, int* err)
        {
#if not KICKOS_HAVE_ASPACE
            (void)image;
            (void)payer;
#endif
            Domain* d = free_slot();
            if (d == nullptr)
            {
                *err = KOS_ENOMEM;
                return nullptr;
            }
#if KICKOS_HAVE_ASPACE
            // A free slot can still hold a space: reusing one still carrying a root would drop
            // the only handle to it, its tables and the donor edge.
            Domain* const stale_donor = drop_space(d);
#endif
            // Survives the reinitialisation and advances with it, or a capability naming this
            // slot's previous occupant resolves to the new one.
            uint32_t const gen = d->generation + 1u;
            *d = Domain{};
            d->generation = gen;
#if KICKOS_HAVE_ASPACE
            // After the reinitialisation, so a cascade that frees further slots cannot find
            // this one half written.
            domain_release(stale_donor);
            Task* t = nullptr;
            if (payer >= 0 and payer < kernel().tasks().size())
            {
                t = &kernel().tasks()[payer];
                domain_pay(d, t);
            }
            if (not task_admit(t, BudgetKind::PAGES,
                               static_cast<uint32_t>(arch_aspace_create_pages())))
            {
                free_put(d);
                *err = KOS_EAGAIN;
                return nullptr;
            }
            d->space = arch_aspace_create();
            if (d->space == nullptr)
            {
                free_put(d);
                *err = KOS_ENOMEM;
                return nullptr;
            }
            int const src = aspace_image_seed(t, d->space, &d->ranges, image);
            if (src != 0)
            {
                (void)drop_space(d); // a space with no handoff yet: the edge is null
                free_put(d);
                *err = -src;
                return nullptr;
            }
#endif
            return d;
        }
    }

    void domain_init(void)
    {
        Kernel& k = kernel();
        for (Domain& d : k.domains())
        {
            d = Domain{};
        }
        Domain* const kdom = &k.domains()[KDOM_KERNEL_INDEX];
        kdom->privileged = true;
        kdom->immortal = true;
        size_t size = arch_ram_size();
        if (size != 0)
        {
            kdom->regions[0].base = arch_ram_base();
            kdom->regions[0].size = size;
            kdom->regions[0].attr = ARCH_MPU_R | ARCH_MPU_W;
            kdom->region_count = 1;
        }
        k.domains()[KDOM_DEFAULT_USER_INDEX].immortal = true;
        k.domain_fresh = 0;
        k.domain_free = HeadList{};
    }

    // Only domain_init and domain_for may touch regions[] directly; every reader outside this
    // file comes through these accessors.
    size_t domain_region_count(Domain const* d)
    {
        if (d == nullptr)
        {
            return 0;
        }
        return d->region_count;
    }

    arch_mpu_region const* domain_region_at(Domain const* d, size_t i)
    {
        KICKOS_DEBUG_ASSERT(i < KICKOS_DOMAIN_REGIONS);
        return &d->regions[i];
    }

    Domain* domain_kernel(void)
    {
        return &kernel().domains()[KDOM_KERNEL_INDEX];
    }

    Domain* domain_default_user(void)
    {
        return &kernel().domains()[KDOM_DEFAULT_USER_INDEX];
    }

    // SlotPool's handle codec. An aged handle is negative, so no decoder may test its sign; -1,
    // the null domain's handle, is refused by its all-ones index, which the pool never reaches
    // (<kickos/pool_storage.h>).

    int domain_handle(Domain const* d)
    {
        if (d == nullptr)
        {
            return -1;
        }
        size_t const idx = static_cast<size_t>(d - &kernel().domains()[0]);
        return static_cast<int>(
            handle_pack(static_cast<uint16_t>(d->generation), static_cast<uint32_t>(idx)));
    }

    Domain* domain_resolve(int handle)
    {
        uint32_t const raw = static_cast<uint32_t>(handle);
        size_t const idx = handle_index(raw);
        if (idx >= static_cast<size_t>(kernel().domains().size()))
        {
            return nullptr;
        }
        Domain* d = &kernel().domains()[idx];
        if (static_cast<uint16_t>(d->generation) != handle_gen(raw))
        {
            return nullptr; // the slot has been reclaimed since this handle was minted
        }
        if (d->refcount == 0 and not d->immortal)
        {
            return nullptr; // free slot: a live handle to one cannot exist
        }
        return d;
    }

    struct arch_aspace* domain_space(Domain const* d)
    {
#if KICKOS_HAVE_ASPACE
        if (d == nullptr)
        {
            return nullptr;
        }
        return d->space;
#else
        (void)d;
        return nullptr;
#endif
    }

#if KICKOS_HAVE_ASPACE
    VirtualRanges const* domain_ranges(Domain const* d)
    {
        if (d == nullptr or d->space == nullptr)
        {
            return nullptr;
        }
        return &d->ranges;
    }

    VirtualRanges* domain_ranges_mut(Domain* d)
    {
        return const_cast<VirtualRanges*>(domain_ranges(d));
    }

    void domain_retire_handles(Domain* d)
    {
        if (d != nullptr and not d->immortal)
        {
            d->generation++;
        }
    }

    void domain_retire_space(Domain* d)
    {
        if (d != nullptr)
        {
            aspace_release_runs(d->space, &d->ranges);
        }
    }

    Domain* domain_retire_handoff(Domain* d)
    {
        if (d == nullptr or d->borrowed_from == nullptr)
        {
            return nullptr;
        }
        Domain* const donor = d->borrowed_from;
        uintptr_t const lent = handoff_base(d);
        aspace_release_handoff(d->space, &d->ranges);
        d->borrowed_from = nullptr;
        domain_unlend(donor, lent);
        return donor;
    }

    void domain_unlend(Domain* d, uintptr_t base)
    {
        if (d == nullptr or base == 0)
        {
            return;
        }
        uint16_t const left = d->ranges.lend(base, false);
        if (left == 0 and d->space == nullptr)
        {
            aspace_shed_free(&d->ranges, d->ranges.index_at(base));
        }
    }

    void domain_shed(Domain* d)
    {
        if (d == nullptr or d->immortal or d->space == nullptr)
        {
            return;
        }
        aspace_shed(d->space, &d->ranges);
        d->space = nullptr;
    }

    Task* domain_payer(Domain const* d)
    {
        if (d == nullptr or d->payer == 0)
        {
            return nullptr;
        }
        return &kernel().tasks()[d->payer - 1u];
    }

    void domain_pay(Domain* d, Task* t)
    {
        HeadList* const domains = task_domains_head(t);
        if (d == nullptr or d->immortal or d->payer != 0 or domains == nullptr)
        {
            return;
        }
        d->payer = static_cast<uint8_t>(t - kernel().tasks().base + 1);
        domains->push(&d->payer_node);
    }

    // O(1).
    void domain_unpay(Domain* d)
    {
        Task* const t = domain_payer(d);
        if (t == nullptr)
        {
            return;
        }
        task_domains_head(t)->unlink(&d->payer_node);
        d->payer = 0;
    }

    uint32_t domain_pages_paid(Task const* t)
    {
        HeadList const* const domains = task_domains_head(t);
        if (domains == nullptr)
        {
            return 0;
        }
        uint32_t pages = 0;
        for (ListNode* n = domains->head; n != nullptr; n = n->next)
        {
            Domain const* const d = KICKOS_CONTAINER_OF(n, Domain, payer_node);
            if (d->space == nullptr)
            {
                pages = pages + aspace_shed_pages(&d->ranges);
            }
            else
            {
                pages = pages + aspace_task_usage(d, BudgetKind::PAGES);
            }
        }
        return pages;
    }

    size_t domain_spaces_held(void)
    {
        size_t held = 0;
        for (Domain const& d : kernel().domains())
        {
            if (d.space != nullptr)
            {
                held++;
            }
        }
        return held;
    }
#endif

    Domain* domain_for(uint32_t caller, void* mem_base, size_t mem_size, uint32_t mem_attr,
                       Domain* donor, int* err)
    {
#if not KICKOS_HAVE_ASPACE
        (void)donor;
#endif
        *err = 0;
        if ((caller & DOM_CALLER_PRIVILEGED) != 0)
        {
            return domain_kernel();
        }
        size_t const image = (caller >> DOM_CALLER_IMAGE_SHIFT) & 0xFFu;
        int const payer = static_cast<int>(caller >> DOM_CALLER_PAYER_SHIFT) - 1;
        if (mem_base == nullptr or mem_size == 0)
        {
#if KICKOS_HAVE_ASPACE
            return claim_slot(image, payer, err);
#else
            return domain_default_user();
#endif
        }
        uintptr_t const base = reinterpret_cast<uintptr_t>(mem_base);
        // Access is the grant's own; only the memory-type bits come from the caller.
        uint32_t const attr = ARCH_MPU_R | ARCH_MPU_W | (mem_attr & ARCH_MPU_NOCACHE);
        if (not grant_nocache_admissible(attr))
        {
            *err = KOS_ENOTSUP;
            return nullptr;
        }
        Domain* d = claim_slot(image, payer, err);
        if (d == nullptr)
        {
            return nullptr;
        }
#if KICKOS_HAVE_ASPACE
        // On a translating backend admission is this handoff: the range must be one the donor
        // reserved, which no MMIO block and no kernel address can be. A refusal leaves no
        // half-built domain: the release below frees the space and the slot.
        enum arch_map_memtype mtype = ARCH_MAP_NORMAL;
        if ((mem_attr & ARCH_MPU_NOCACHE) != 0)
        {
            mtype = ARCH_MAP_NOCACHE;
        }
        int const hrc = aspace_handoff(domain_payer(d), domain_ranges(donor), d->space,
                                       &d->ranges, base, mem_size, mtype);
        if (hrc != 0)
        {
            domain_release(d);
            *err = -hrc;
            return nullptr;
        }
        // The lifetime edge, and the only place one is made. Without it the donor's last task
        // can exit, destroy its space and return every borrowed frame to the pool under a live
        // mapping. Taken only on success, so aspace_handoff's unwind arms surrender nothing.
        // A handoff passed on names the domain that reserved the frames, so a borrower holds
        // only that one, never the domain it was passed through.
        Domain* owner = donor;
        VirtualRanges const* const lent = domain_ranges(donor);
        if (lent != nullptr and aspace_range_handed_off(lent->at_base(base))
            and donor->borrowed_from != nullptr)
        {
            owner = donor->borrowed_from;
        }
        KICKOS_DEBUG_ASSERT(d->borrowed_from == nullptr and d != owner);
        d->borrowed_from = owner;
        domain_ref(owner);
        (void)owner->ranges.lend(base, true);
        // The range list carries the handoff with the exact extent, and is what the entry path
        // answers from on this backend.
#else
        d->regions[0].base = base;
        d->regions[0].size = ram_grant_extent(mem_size);
        d->regions[0].attr = attr;
        d->region_count = 1;
#endif
        return d;
    }

    uint16_t domain_refcount(Domain const* d)
    {
        if (d == nullptr)
        {
            return 0;
        }
        return d->refcount;
    }

    void domain_ref(Domain* d)
    {
        // An immortal domain's refcount is not tracked: an unbounded, transient set of tasks
        // references it and the counter would wrap.
        if (d != nullptr and not d->immortal)
        {
            KICKOS_DEBUG_ASSERT(d->refcount < UINT16_MAX);
            d->refcount++;
        }
    }

    void domain_release(Domain* d)
    {
        // Iterative: freeing a borrower surrenders its donor's reference, which can free the
        // donor in turn, and the chain runs as long as the domain pool, with a tree walk a link.
        while (d != nullptr and not d->immortal)
        {
            if (d->refcount > 0)
            {
                d->refcount--;
            }
            if (d->refcount != 0)
            {
                return;
            }
            free_put(d);
#if KICKOS_HAVE_ASPACE
            // drop_space hands back the donor whose reference the next turn of the loop drops;
            // reusing the slot without it would strand the root and its tables.
            d = drop_space(d);
#else
            return;
#endif
        }
    }
}

namespace
{
    // end > start, never start != 0: a zero start is valid on flash-at-zero targets.
    void add_static(struct arch_mpu_region* out, size_t* n, size_t max, unsigned char const* start,
                    unsigned char const* end, uint32_t attr)
    {
        uintptr_t const lo = reinterpret_cast<uintptr_t>(start);
        uintptr_t const hi = reinterpret_cast<uintptr_t>(end);
        if (hi > lo and *n < max)
        {
            out[*n].base = lo;
            out[*n].size = static_cast<size_t>(hi - lo);
            out[*n].attr = attr;
            (*n)++;
        }
    }
}

extern "C"
{
    extern unsigned char __kickos_code_start[] KICKOS_LINK_BOUND;
    extern unsigned char __kickos_code_end[] KICKOS_LINK_BOUND;
    extern unsigned char __kickos_appdata_start[] KICKOS_LINK_BOUND;
    extern unsigned char __kickos_appdata_end[] KICKOS_LINK_BOUND;

    // The data region is image `image`'s window, where the script carves the app one a window.
    size_t arch_domain_static_regions(struct arch_mpu_region* out, size_t max, size_t image)
    {
        size_t n = 0;
        // KERNEL TEXT INCLUDED: a syscall return preempted at kickos_armv7m_svc_drop or
        // kickos_armv6m_svc_drop is saved demoted and resumes UNPRIVILEGED on those instructions.
        add_static(out, &n, max, __kickos_code_start, __kickos_code_end, ARCH_MPU_R | ARCH_MPU_X);
        if (reinterpret_cast<uintptr_t>(__kickos_appdata_end)
            > reinterpret_cast<uintptr_t>(__kickos_appdata_start))
        {
            kos_image_header const& h = kickos::image_record(image);
            unsigned char const* const start
                = reinterpret_cast<unsigned char const*>(h.window_start);
            add_static(out, &n, max, start, start + h.window_size, ARCH_MPU_R | ARCH_MPU_W);
        }
        return n;
    }
}
