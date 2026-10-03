// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include "lookup_seam.h"

#include <kickos/sys/errno.h>

#include <gtest/gtest.h>

#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

namespace seam
{
    std::vector<kos_window> windows;
    kos_window stale = {};
    std::vector<uint32_t> asked;
    std::vector<uint64_t> sleeps;
    std::function<void()> on_sleep;
    kos_self_t const* entered = nullptr;
    char const* entry_print = nullptr;
    int entry_exit = -1;

    void reset()
    {
        windows.clear();
        stale = kos_window{};
        asked.clear();
        sleeps.clear();
        on_sleep = nullptr;
        entered = nullptr;
        entry_print = nullptr;
        entry_exit = -1;
    }

    kos_window window(uintptr_t base, uint32_t size, uint8_t kind, uint8_t flags)
    {
        kos_window w = {};
        w.base = base;
        w.size = size;
        w.kind = kind;
        w.flags = flags;
        return w;
    }

    uint32_t index_of(char const* name)
    {
        auto const tasks = reinterpret_cast<kos_table_task const*>(kickos_table + 1);
        auto const grants = reinterpret_cast<kos_table_grant const*>(tasks + kickos_table->task_count);
        auto const refs = reinterpret_cast<kos_table_ref const*>(grants + kickos_table->grant_count);
        auto const privs = reinterpret_cast<kos_table_priv const*>(refs + kickos_table->ref_count);
        auto const regions = reinterpret_cast<kos_table_region const*>(privs + kickos_table->priv_count);
        auto const strings = reinterpret_cast<char const*>(regions + kickos_table->region_count);
        for (uint32_t i = 0; i < kickos_table->task_count; i++)
        {
            if (strcmp(&strings[tasks[i].name], name) == 0)
            {
                return i;
            }
        }
        ADD_FAILURE() << "no task named " << name << " in the table";
        return 0;
    }

    uint32_t watched_at(char const* watcher, char const* name)
    {
        auto const tasks = reinterpret_cast<kos_table_task const*>(kickos_table + 1);
        auto const grants = reinterpret_cast<kos_table_grant const*>(tasks + kickos_table->task_count);
        auto const refs = reinterpret_cast<kos_table_ref const*>(grants + kickos_table->grant_count);
        kos_table_task const& w = tasks[index_of(watcher)];
        uint32_t const watched = index_of(name);
        for (uint32_t k = 0; k < w.watch_count; k++)
        {
            if (refs[w.first_watch + k].task == watched)
            {
                return k;
            }
        }
        ADD_FAILURE() << watcher << " does not watch " << name;
        return 0;
    }

    kos_self_t const* task(char const* name)
    {
        return reinterpret_cast<kos_table_task const*>(kickos_table + 1) + index_of(name);
    }

    StatusBlock::StatusBlock(uint32_t size)
        : page_{mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)}
        , size_{size}
    {
        if (page_ == MAP_FAILED)
        {
            ADD_FAILURE() << "no status block";
            page_ = nullptr;
        }
    }

    StatusBlock::~StatusBlock()
    {
        if (page_ != nullptr)
        {
            munmap(page_, size_);
        }
    }

    kickos::init::StatusRecord* StatusBlock::record(uint32_t task)
    {
        return static_cast<kickos::init::StatusRecord*>(page_) + task;
    }

    uintptr_t StatusBlock::base() const
    {
        return reinterpret_cast<uintptr_t>(page_);
    }

    uint32_t StatusBlock::size() const
    {
        return size_;
    }
}

namespace
{
    void enter(kos_self_t const* self)
    {
        seam::entered = self;
        if (seam::entry_print != nullptr)
        {
            printf("%s", seam::entry_print);
        }
        if (seam::entry_exit >= 0)
        {
            kos_exit(seam::entry_exit);
        }
    }
}

extern "C"
{

int kos_window_get(uint32_t index, struct kos_window* out)
{
    seam::asked.push_back(index);
    if (out == nullptr)
    {
        return -KOS_EINVAL;
    }
    if (index >= seam::windows.size())
    {
        *out = seam::stale;
        return -KOS_EINVAL;
    }
    *out = seam::windows[index];
    return 0;
}

void kos_sleep_ns(uint64_t ns)
{
    seam::sleeps.push_back(ns);
    if (seam::on_sleep)
    {
        seam::on_sleep();
    }
}

void kos_exit(int code)
{
    throw seam::Exit{code};
}

void sensor_main(kos_self_t const* self)
{
    enter(self);
}

void app_main(kos_self_t const* self)
{
    enter(self);
}

void health_main(kos_self_t const* self)
{
    enter(self);
}

}
