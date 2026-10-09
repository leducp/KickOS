// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// See host_aspace.h.

#include "host_aspace.h"

#include <kickos/arch/arch.h>

#include "host_frame_pool.h"

using kickos::testfix::HOST_POOL_GRANULE;

namespace
{
    constexpr size_t G = HOST_POOL_GRANULE;
    constexpr size_t SPACES = 48u;
    constexpr size_t LEAVES = 256u;
    constexpr size_t TABLES = 8u;
    constexpr unsigned L1_SHIFT = 30u;
    constexpr unsigned L2_SHIFT = 21u;

    bool g_tables_from_pool = false;
    bool g_map_refused = false;

    uintptr_t g_refuse_page = 0;
    uint32_t g_refuse_left = 0;
    uint32_t g_refuse_after = 0;
    uintptr_t g_watch_page = 0;

    uintptr_t page_of(uintptr_t va)
    {
        return va & ~static_cast<uintptr_t>(G - 1u);
    }

    // The root space maps the image's own data where it lies, outside the pool.
    unsigned char* frame_bytes(arch_phys_addr_t pa)
    {
        if (kickos::testfix::host_pool_owns(pa))
        {
            return kickos::testfix::host_pool_bytes(pa, 1);
        }
        return reinterpret_cast<unsigned char*>(static_cast<uintptr_t>(pa));
    }
}

struct arch_aspace
{
    struct Leaf
    {
        uintptr_t va;
        arch_phys_addr_t pa;
        uint8_t type;
        bool used;
    };
    struct Table
    {
        unsigned shift;
        uintptr_t index;
        arch_phys_addr_t frame;
    };
    bool live;
    arch_phys_addr_t root;
    Table tables[TABLES];
    Leaf leaves[LEAVES];

    Leaf* leaf(uintptr_t va)
    {
        uintptr_t const page = page_of(va);
        for (Leaf& l : leaves)
        {
            if (l.used and l.va == page)
            {
                return &l;
            }
        }
        return nullptr;
    }

    bool table(unsigned shift, uintptr_t va)
    {
        uintptr_t const index = va >> shift;
        for (Table const& t : tables)
        {
            if (t.frame != 0 and t.shift == shift and t.index == index)
            {
                return true;
            }
        }
        for (Table& t : tables)
        {
            if (t.frame == 0)
            {
                t = {shift, index, kickos_frame_alloc()};
                return t.frame != 0;
            }
        }
        return false;
    }
};

namespace
{
    arch_aspace g_spaces[SPACES] = {};
    arch_aspace g_boot = {};
}

namespace kickos
{
    namespace testfix
    {
        uint32_t g_unmaps = 0;
        size_t g_unmapped_pages = 0;
        size_t g_aspaces_live = 0;
        size_t g_aspace_double_destroys = 0;
        bool g_watched_at_destroy = false;

        void host_aspace_tables_from_pool(bool on)
        {
            g_tables_from_pool = on;
        }

        void refuse_acquire(uintptr_t va, uint32_t count, uint32_t after)
        {
            g_refuse_page = page_of(va);
            g_refuse_left = count;
            g_refuse_after = after;
        }

        void refuse_map(bool on)
        {
            g_map_refused = on;
        }

        void watch_destroy(uintptr_t va)
        {
            g_watch_page = page_of(va);
            g_watched_at_destroy = false;
        }

        void host_aspace_reset()
        {
            g_unmaps = 0;
            g_unmapped_pages = 0;
            g_aspace_double_destroys = 0;
            refuse_acquire(0, 0);
            refuse_map(false);
            watch_destroy(0);
        }
    }
}

extern "C"
{
    size_t arch_aspace_granule(void)
    {
        return G;
    }

    uintptr_t arch_aspace_user_offset(void)
    {
        return 0;
    }

    bool arch_aspace_memtype_support(enum arch_map_memtype)
    {
        return true;
    }

    struct arch_aspace* arch_aspace_create(void)
    {
        for (arch_aspace& s : g_spaces)
        {
            if (s.live)
            {
                continue;
            }
            arch_phys_addr_t root = 0;
            if (g_tables_from_pool)
            {
                root = kickos_frame_alloc();
                if (root == 0)
                {
                    return nullptr;
                }
            }
            s = arch_aspace{};
            s.live = true;
            s.root = root;
            kickos::testfix::g_aspaces_live++;
            return &s;
        }
        return nullptr;
    }

    // A backend's walk frees every frame a leaf still names, and its tables.
    void arch_aspace_destroy(struct arch_aspace* space)
    {
        if (space == nullptr or space == &g_boot)
        {
            return;
        }
        if (not space->live)
        {
            kickos::testfix::g_aspace_double_destroys++;
            return;
        }
        if (g_watch_page != 0)
        {
            kickos::testfix::g_watched_at_destroy = space->leaf(g_watch_page) != nullptr;
        }
        for (arch_aspace::Leaf const& l : space->leaves)
        {
            if (l.used)
            {
                kickos_frame_free(l.pa);
            }
        }
        for (arch_aspace::Table const& t : space->tables)
        {
            if (t.frame != 0)
            {
                kickos_frame_free(t.frame);
            }
        }
        if (space->root != 0)
        {
            kickos_frame_free(space->root);
        }
        *space = arch_aspace{};
        kickos::testfix::g_aspaces_live--;
    }

    struct arch_aspace* arch_aspace_boot(void)
    {
        return &g_boot;
    }

    void arch_aspace_activate(struct arch_aspace*) {}

    enum arch_aspace_result arch_aspace_map(struct arch_aspace* space, uintptr_t va,
                                            arch_phys_addr_t pa, size_t pages, uint32_t,
                                            enum arch_map_memtype type)
    {
        if (space == nullptr or pages == 0 or (va % G) != 0 or (pa % G) != 0)
        {
            return ARCH_ASPACE_EINVAL;
        }
        size_t mapped = 0;
        for (size_t i = 0; i < pages; i++)
        {
            if (space->leaf(va + i * G) != nullptr)
            {
                mapped++;
            }
        }
        if (mapped != 0 and mapped != pages)
        {
            return ARCH_ASPACE_EINVAL;
        }
        if (g_map_refused)
        {
            return ARCH_ASPACE_ENOMEM;
        }
        for (size_t i = 0; g_tables_from_pool and i < pages; i++)
        {
            uintptr_t const at = va + i * G;
            if (not space->table(L1_SHIFT, at) or not space->table(L2_SHIFT, at))
            {
                return ARCH_ASPACE_ENOMEM;
            }
        }
        for (size_t i = 0; i < pages; i++)
        {
            uintptr_t const at = va + i * G;
            arch_aspace::Leaf* l = space->leaf(at);
            for (size_t k = 0; l == nullptr and k < LEAVES; k++)
            {
                if (not space->leaves[k].used)
                {
                    l = &space->leaves[k];
                }
            }
            if (l == nullptr)
            {
                return ARCH_ASPACE_ECAPACITY;
            }
            *l = {at, pa + static_cast<arch_phys_addr_t>(i * G), static_cast<uint8_t>(type),
                  true};
        }
        return ARCH_ASPACE_OK;
    }

    enum arch_aspace_result arch_aspace_unmap(struct arch_aspace* space, uintptr_t va,
                                              size_t pages)
    {
        if (space == nullptr or pages == 0)
        {
            return ARCH_ASPACE_EINVAL;
        }
        for (size_t i = 0; i < pages; i++)
        {
            if (space->leaf(va + i * G) == nullptr)
            {
                return ARCH_ASPACE_EINVAL;
            }
        }
        for (size_t i = 0; i < pages; i++)
        {
            space->leaf(va + i * G)->used = false;
        }
        kickos::testfix::g_unmaps++;
        kickos::testfix::g_unmapped_pages += pages;
        return ARCH_ASPACE_OK;
    }

    void* arch_aspace_acquire(struct arch_aspace* space, uintptr_t va, bool* uncached)
    {
        if (space == nullptr)
        {
            return nullptr;
        }
        arch_aspace::Leaf const* const l = space->leaf(va);
        if (l == nullptr)
        {
            return nullptr;
        }
        if (g_refuse_left != 0 and page_of(va) == g_refuse_page)
        {
            if (g_refuse_after != 0)
            {
                g_refuse_after--;
            }
            else
            {
                g_refuse_left--;
                return nullptr;
            }
        }
        if (uncached != nullptr)
        {
            *uncached = l->type == ARCH_MAP_NOCACHE;
        }
        return frame_bytes(l->pa) + (va - l->va);
    }

    void arch_aspace_release(struct arch_aspace*, uintptr_t) {}

    arch_phys_addr_t arch_aspace_frame_at(struct arch_aspace* space, uintptr_t va)
    {
        if (space == nullptr)
        {
            return 0;
        }
        arch_aspace::Leaf const* const l = space->leaf(va);
        if (l == nullptr)
        {
            return 0;
        }
        return l->pa;
    }
}
