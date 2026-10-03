// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// One run of the init's walk over an emitted system against the scripted kernel.

#ifndef KICKOS_TESTS_UNIT_INITWALK_HARNESS_H
#define KICKOS_TESTS_UNIT_INITWALK_HARNESS_H

#include "fake_kernel.h"
#include "systems.h"

#include <kickos/sys/init_status.h>

#include <stdint.h>

#include <functional>
#include <string>
#include <vector>

namespace harness
{
    // How a run of the walk ended.
    struct Outcome
    {
        enum class Kind
        {
            PANIC,
            SHUTDOWN,
            IDLE
        };
        Kind kind;
        std::string message; // a panic's
        int status;          // a shutdown's
    };

    // The scripted kernel's configuration for `system`.
    fake::Config config(systems::System const& system);

    // Runs the walk over `system` against the kernel as reset, refused and scripted.
    Outcome run(systems::System const& system);

    // The table index of task `name` in `system`.
    uint16_t index_of(systems::System const& system, char const* name);

    // A writable copy of `system`'s table, which run() walks in its place.
    class Patched
    {
    public:
        explicit Patched(systems::System const& system);

        systems::System const& system() const;
        kos_table_header& header();
        kos_table_task& task(char const* name);
        // Task `name`'s k-th grant.
        kos_table_grant& grant(char const* name, uint16_t k);

    private:
        std::vector<uint64_t> bytes_;
        kos_table_header const* header_;
        systems::System system_;
    };

    // Task `task`'s record in the init's private block, the block the walk reserved first.
    kickos::init::TaskRecord const& record(uint16_t task);

    // What the init keeps of task `task` in the last run, checked against the copy each watcher of
    // it reads from its own status block.
    kickos::init::StatusFields status(uint16_t task);
}

// The scripted kernel's steps the tests share, each run before one kos_notify_wait reads its bits.
namespace step
{
    using Step = std::function<void()>;

    Step ready_all();
    Step ready(char const* name);
    Step die(char const* name);
    Step end(char const* name, int status);
    Step raise(uint32_t bit);
    Step nothing();
}

#endif
