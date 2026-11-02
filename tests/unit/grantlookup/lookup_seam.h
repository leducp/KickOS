// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The kernel the lookups ask, faked: kos_window_get answers `windows`, kos_sleep_ns records and
// runs `on_sleep`, kos_exit throws Exit. Each table's user entries are defined here too.
//
// HOST-ONLY. These are public kos_* names, which a target image linking this TU would take
// from the executable instead of the syscall stubs.

#ifndef KICKOS_TESTS_UNIT_GRANTLOOKUP_LOOKUP_SEAM_H
#define KICKOS_TESTS_UNIT_GRANTLOOKUP_LOOKUP_SEAM_H

#include <kickos/sys.h>
#include <kickos/sys/init_status.h>
#include <kickos/sys/table.h>

#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <vector>

namespace seam
{
    // The calling thread's spawn windows, in place order.
    extern std::vector<kos_window> windows;
    // What a refused kos_window_get leaves in *out, which the kernel never writes.
    extern kos_window stale;
    // Every index kos_window_get was asked, in order.
    extern std::vector<uint32_t> asked;
    // Every kos_sleep_ns duration, in order.
    extern std::vector<uint64_t> sleeps;
    // Run after each sleep is recorded.
    extern std::function<void()> on_sleep;

    // What a user entry saw, what it prints first, and the code it exits with itself, or -1 to
    // return.
    extern kos_self_t const* entered;
    // The record an entry saw, copied, which outlives an entry frame its exit unwinds.
    extern kos_table_task entered_record;
    extern char const* entry_print;
    // Run as the entry is entered, with its `self`.
    extern std::function<void(kos_self_t const*)> entry_seen;
    extern int entry_exit;

    struct Exit
    {
        int code;
    };

    // What an entry takes as `self`: a copy of its table record, and the argv its task started with.
    struct TaskSelf
    {
        kos_table_task task;
        int argc;
        char** argv;
    };

    void reset();

    kos_window window(uintptr_t base, uint32_t size, uint8_t kind, uint8_t flags);

    // The table record named `name`, which must exist.
    kos_self_t const* task(char const* name);
    // The lookups of <kickos/sys.h> as the task `self` runs them, none for a null `self`.
    kos_cap_t grant_endpoint(kos_self_t const* self, char const* name);
    kos_cap_t grant_notify(kos_self_t const* self, char const* name);
    kos_window_t grant_mmio(kos_self_t const* self, char const* name);
    kos_window_t grant_mem(kos_self_t const* self, char const* name);
    kos_window_t grant_ports(kos_self_t const* self, char const* name);
    kos_line_t grant_irq(kos_self_t const* self, char const* name);
    int task_status(kos_self_t const* self, uint32_t i, struct kos_task_status* out);
    // A task's place in the table.
    uint32_t index_of(char const* name);
    // Where task `name` sits among `watcher`'s watches, which indexes the watcher's status block.
    uint32_t watched_at(char const* watcher, char const* name);

    // A page-aligned status block of `size` bytes, zeroed, unmapped at destruction.
    class StatusBlock
    {
    public:
        explicit StatusBlock(uint32_t size);
        ~StatusBlock();
        StatusBlock(StatusBlock const&) = delete;
        StatusBlock& operator=(StatusBlock const&) = delete;

        kickos::init::StatusRecord* record(uint32_t task);
        uintptr_t base() const;
        uint32_t size() const;

    private:
        void* page_;
        uint32_t size_;
    };
}

#endif
