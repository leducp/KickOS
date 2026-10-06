// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A client of each driver: it calls until the driver is gone for good, printing each answer, the
// count of requests the instance answering it has served, followed by the endpoint it called.

#include <kickos/sys.h>
#include <kickos/sys/errno.h>
#include <kickos/sys/table.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

namespace
{
    constexpr uint32_t CALL_TIMEOUT_US = 100000u;
    constexpr uint64_t RETRY_NS = 10000000ull; // 10 ms
    constexpr uint32_t LINE_RAISES = 200u;

    bool same(char const* a, char const* b)
    {
        size_t i = 0;
        while (a[i] == b[i] and a[i] != '\0')
        {
            i++;
        }
        return a[i] == b[i];
    }

    // The number the table gives line role `role` of task `task`, or -1 where it names none.
    int table_line(char const* task, char const* role)
    {
        kos_table_header const* const header = kickos_table;
        auto const tasks = reinterpret_cast<kos_table_task const*>(header + 1);
        auto const grants = reinterpret_cast<kos_table_grant const*>(tasks + header->task_count);
        auto const refs = reinterpret_cast<kos_table_ref const*>(grants + header->grant_count);
        auto const privs = reinterpret_cast<kos_table_priv const*>(refs + header->ref_count);
        auto const regions = reinterpret_cast<kos_table_region const*>(privs + header->priv_count);
        auto const strings = reinterpret_cast<char const*>(regions + header->region_count);
        for (uint16_t t = 0; t < header->task_count; t++)
        {
            if (not same(&strings[tasks[t].name], task))
            {
                continue;
            }
            for (uint16_t k = 0; k < tasks[t].grant_count; k++)
            {
                kos_table_grant const& g = grants[tasks[t].first_grant + k];
                if (g.kind == KOS_GRANT_LINE and same(&strings[g.name], role))
                {
                    return g.line;
                }
            }
        }
        return -1;
    }

    void call_until_refused(kos_self_t const* self, char const* path, char const* who)
    {
        kos_cap_t const ep = kos_grant_endpoint(self, path);
        while (true)
        {
            uint32_t served = 0;
            int32_t const n = kos_call_timed(ep, &served, 0u, sizeof(served), CALL_TIMEOUT_US);
            if (n == -KOS_ECONNREFUSED)
            {
                printf("%s: refused\n", who);
                return;
            }
            if (n == static_cast<int32_t>(sizeof(served)))
            {
                printf("%s: served %lu by %s\n", who, static_cast<unsigned long>(served), path);
                continue;
            }
            kos_sleep_ns(RETRY_NS);
        }
    }
}

extern "C" void exits_client_main(kos_self_t const* self)
{
    call_until_refused(self, "/svc/exits", "exits");
}

extern "C" void traps_client_main(kos_self_t const* self)
{
    call_until_refused(self, "/svc/traps", "traps");
}

// Never started: the init starts a task once the servers it uses serve, and testfails never does.
extern "C" void fails_client_main(kos_self_t const* self)
{
    call_until_refused(self, "/svc/fails", "fails");
}

// Once testline answers its first call, raises the line the table gives its line 0, which a
// first wait discards until it arms it. Its end ends the system.
extern "C" void line_client_main(kos_self_t const* self)
{
    kos_cap_t const ep = kos_grant_endpoint(self, "/svc/line");
    uint32_t served = 0;
    int32_t const answered = static_cast<int32_t>(sizeof(served));
    while (kos_call_timed(ep, &served, 0u, sizeof(served), CALL_TIMEOUT_US) != answered)
    {
        kos_sleep_ns(RETRY_NS);
    }
    int const line = table_line("line", "routed");
    if (line < 0)
    {
        printf("line: the table gives testline no line 0\n");
        exit(1);
    }
    printf("line: raising line %d\n", line);
    for (uint32_t raises = 0; raises < LINE_RAISES; raises++)
    {
        int const raised = kos_irq_inject(line);
        if (raised != 0)
        {
            printf("line: raising line %d answered %d\n", line, raised);
            exit(1);
        }
        kos_sleep_ns(RETRY_NS);
    }
}
