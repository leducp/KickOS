// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The init's walk over the emitted table (docs/design-m10-target.md, section 1): what it holds
// from boot, the start of each task once the servers it uses are ready, each death and readiness
// the kernel reports, and the end of the system.

#ifndef KICKOS_SYSTEM_INIT_COMPOSE_WALK_H
#define KICKOS_SYSTEM_INIT_COMPOSE_WALK_H

#include <kickos/board_config.h> // KICKOS_MAX_SPAWN_GRANTS, KICKOS_MAX_THREAD_WINDOWS
#include <kickos/sys.h>
#include <kickos/sys/init_geometry.h>
#include <kickos/sys/init_status.h>
#include <kickos/sys/table.h>

#include <stddef.h>
#include <stdint.h>

namespace kickos::init
{
    // What the walk reads of the kernel build it runs on.
    struct Build
    {
        bool translating; // each task in a space of its own, its stack block its data region
        bool sp_masked;   // every thread stack one stride, which the kernel hands out
        bool multicore;   // more than one kernel core: a line's claimer pins itself to its core
    };

    // How long a slay waits for the group to empty (1.4).
    constexpr uint32_t SLAY_TIMEOUT_US = 1000u;
    // How often, and how far apart, a claim answered -KOS_EAGAIN is tried again (1.6).
    constexpr uint32_t CLAIM_RETRIES = 100u;
    constexpr uint64_t CLAIM_RETRY_NS = 1000000ull;

    enum class Phase : uint8_t
    {
        WAITING, // never started
        RUNNING, // `handle` names its instance
        PENDING, // dead, with a restart to come
        GONE,    // dead for good
        DOWN     // dependency-down: a server it uses is gone, so it never starts
    };

    // One task's record in the private block.
    struct TaskRecord
    {
        kos_task_t handle;  // the current instance, or KOS_TASK_NONE
        kos_task_t ready;   // the instance that last became ready and has not ended since
        kos_cap_t endpoint; // the endpoint it serves, kept with HANDOUT
        kos_cap_t notify;   // a watcher's notification
        void* block;        // its reserved stack or a packaged driver's ring block, or null
        StatusRecord* status; // a watcher's status block, a record per task it watches, or null
        uint16_t deaths;
        uint8_t restarts_left;
        Phase phase;
    };

    // What a watcher reads of the task `record` keeps.
    StatusFields status_of(TaskRecord const& record);

    // The private block: a record per task in table order, then one per shared region holding
    // where the region was reserved.
    union PrivateRecord
    {
        TaskRecord task;
        void* region;
        uint8_t bytes[KICKOS_INIT_PRIVATE_RECORD_SIZE];
    };
    static_assert(sizeof(PrivateRecord) == KICKOS_INIT_PRIVATE_RECORD_SIZE,
                  "a private record is the size cmake/init_geometry.cmake declares");

    class Walk
    {
    public:
        Walk(kos_table_header const* table, Build const& build);

        // Boots, then starts, waits and handles what each wake reports until the system ends.
        [[noreturn]] void run();

        // Task `i`'s table entry.
        kos_table_task const& task(uint16_t i) const;
        // Task `i`'s name, from the table's strings.
        char const* name(uint16_t i) const;
        // The table's task count.
        uint16_t task_count() const;
        // Task `i`'s k-th grant.
        kos_table_grant const& grant(uint16_t i, uint16_t k) const;
        // Task `i`'s record in the private block, which boot reserves.
        TaskRecord& record(uint16_t i);
        // The init's notification, whose bit i%32 a watch on task i raises.
        kos_cap_t notify() const;

        // Panics naming the kernel call, the task (KOS_TABLE_NONE for none) and its answer.
        [[noreturn]] void refused(char const* step, uint16_t task, char const* answer) const;
        // Refuses with `rc` unless it is 0.
        void check(int rc, char const* step, uint16_t task) const;
        // Reserves `size` bytes with kos_ram_alloc, refusing a null answer.
        void* reserve(uint32_t size, char const* step, uint16_t task) const;

    private:
        // A diagnostic line, printed once the deaths and starts of the pass that made it are
        // handled (1.6).
        struct Report
        {
            enum class Kind : uint8_t
            {
                FAILED,   // a failed start
                RELEASED, // a death released
                KEEPS     // members left past its slay
            };
            Kind kind;
            uint16_t task;
            int32_t rc;
            char const* step;
        };
        static constexpr uint16_t REPORTS = 8;

        struct Start
        {
            uint16_t task;
            kos_task_t handle;
            kos_cap_t copy;
            kos_cap_t lines[KICKOS_MAX_SPAWN_GRANTS];
            uint16_t line_count;
            bool pinned;
            char const* step; // the step that failed
        };

        void boot();
        void check_task(uint16_t i);
        void scan();
        bool startable(uint16_t i);
        bool serving(uint16_t i);
        void start(uint16_t i);
        int take_lines(Start& s);
        int claim(uint16_t line, kos_cap_t* out);
        int spawn(Start& s);
        void release(Start& s);
        void failed(uint16_t i, kos_task_t handle, char const* step, int rc);
        void wake();
        void death(uint16_t i);
        void tell(uint16_t i);
        void write_status(uint16_t i);
        void report(Report::Kind kind, uint16_t task, char const* step, int rc);
        void flush();
        void drop(uint16_t i);
        void propagate();
        void end_if_cancelled();
        [[noreturn]] void end(int status);
        void close(kos_cap_t cap, char const* step, uint16_t task);
        kos_table_grant const* grant_of(uint16_t i, uint8_t kind) const;

        kos_table_header const* header_;
        kos_table_task const* tasks_;
        kos_table_grant const* grants_;
        kos_table_ref const* refs_;
        kos_table_region const* regions_;
        char const* strings_;
        Build build_;
        uint16_t ends_;
        kos_cap_t notify_;
        PrivateRecord* private_;
        Report reports_[REPORTS];
        uint16_t report_count_;
    };
}

#endif
