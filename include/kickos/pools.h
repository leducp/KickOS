// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Where the kernel's pools live. Their storage is not in the kernel archives: the composer
// generates it per system into a TU the system's final link carries (<kickos/pool_storage.h>),
// and the kernel reads each pool's base and count from the one const descriptor that TU defines.
// Only the type ceilings of <kickos/config/pool_abi.h> are fixed when the kernel is compiled.
//
// On the tree-wide include root because the arch layer reads its own pools here, and arch may
// not include a kernel header: every pooled type is only named.

#ifndef KICKOS_POOLS_H
#define KICKOS_POOLS_H

#include <stddef.h>
#include <stdint.h>

#include <kickos/config/pool_abi.h>
#include <kickos/instance_local.h> // kickos_instance_index, KICKOS_MAX_INSTANCES, arch.h
#include <kickos/sys/budget.h>     // KOS_BUDGET_KIND_COUNT

struct kos_image_header;

namespace kickos
{
    struct CapEntry;
    struct Domain;
    struct Endpoint;
    struct FrameRun;
    struct IrqBinding;
    struct Notification;
    struct PresyncRecord;
    struct RamBlock;
    struct Task;
    struct Thread;
    struct WordRecord;
    struct ListNode;
    struct TaskLinks;

    namespace aspace
    {
        struct ResidencyRow;
    }

    // A thread-pool slot index plus one, 0 for none: wide enough for every slot up to the
    // thread-slot ceiling.
#if KICKOS_THREAD_SLOT_CEILING < 255
    using ThreadLink = uint8_t;
#else
    using ThreadLink = uint16_t;
#endif

    // The first task slot one thread-pool slot's occupant holds as creator, plus one, 0 for none.
    struct ThreadLinks
    {
        uint8_t created_head;
    };

    // One generational pool's side arrays (slotpool.h), whatever it holds: the generations, the
    // object-side refcount, and the node each slot has on the pool's free list.
    struct SlotArrays
    {
        uint32_t* gen;
        uint8_t* refs;
        ListNode* nodes;
        int count;
    };

    template <class T>
    struct SlotStore
    {
        SlotArrays arrays;
        T* slots;
    };

    // Every pool of the system. A base names instance 0's storage; instance i's lies i * stride
    // bytes past it.
    struct KernelPools
    {
        size_t stride;

        // The thread pool, and what each of its slots owns.
        Thread* threads;
        uint16_t* thread_gen;
        uint16_t* thread_gen_hi;
        ThreadLinks* thread_links;
        WordRecord* word_records;
        // thread_slots rows of cap_dir_length entries.
        CapEntry** cap_dirs;
#if KICKOS_HAVE_ASPACE
        // Null where no system call syncs a frame outside the kernel lock.
        PresyncRecord* presync;
#endif
#if KICKOS_KERNEL_STACKS
        unsigned char* kstacks;
#endif
#if KICKOS_KERNEL_CORES > 1
        // [producer][consumer][sched_ring_depth] pool slot indices.
        uint16_t* sched_rings;
        uint32_t sched_ring_depth;
#endif
        int thread_slots;

        Task* tasks;
        TaskLinks* task_links;
        int task_count;
        Domain* domains;
        int domain_count;

        SlotStore<Endpoint> endpoints;
        SlotStore<Notification> notifies;
        SlotStore<IrqBinding> irq_bindings;
#if KICKOS_HAVE_ASPACE
        SlotStore<FrameRun> frame_runs;
        // One per machine: a row per domain slot, and the identifiers' bitmap, a bit per row.
        aspace::ResidencyRow* residency_rows;
        uint32_t* residency_taken;
        // Each image's header as the kernel reads it, the init's and one per task slot.
        kos_image_header const** image_headers;
#endif
#if not KICKOS_HAVE_ASPACE
        RamBlock* ram_owners;
        int ram_owner_count;
#endif

        // The capability slab, in chunks of KCAP_CHUNK_SLOTS entries.
        CapEntry* cap_chunks;
        uint32_t cap_chunk_count;
        uint32_t cap_root_width;
        uint32_t cap_dir_length;

        // The init's budgets, by enum kos_budget_kind: root's task's (<kickos/budget.h>).
        uint32_t init_budgets[KOS_BUDGET_KIND_COUNT];
    };

    // Defined by the system's generated pools TU, under the C name the system's link requires
    // (cmake/compose.cmake).
    extern "C" KernelPools const kickos_kernel_pools;

    // The running instance's copy of the storage `base` names for instance 0.
    template <class T>
    inline T* pool_local(T* base)
    {
#if KICKOS_MAX_INSTANCES == 1
        return base;
#else
        uintptr_t const at = reinterpret_cast<uintptr_t>(base)
                             + kickos_instance_index() * kickos_kernel_pools.stride;
        return reinterpret_cast<T*>(at);
#endif
    }

    // A pool's slots, as the running instance holds them.
    template <class T>
    struct PoolSpan
    {
        T* base;
        int count;

        T& operator[](int i) const
        {
            return base[i];
        }
        T* begin() const
        {
            return base;
        }
        T* end() const
        {
            return base + count;
        }
        int size() const
        {
            return count;
        }
    };

    template <class T>
    inline PoolSpan<T> pool_span(T* base, int count)
    {
        return PoolSpan<T>{pool_local(base), count};
    }

    // The link-time witness that the kernel and the system's pools TU agree on every pooled
    // type and every ceiling: each side names the specialisation of its own fingerprint, and the
    // pools TU defines only its own.
    template <uint32_t Layout>
    struct PoolLayout
    {
        static char const tag;
    };
}

#endif
