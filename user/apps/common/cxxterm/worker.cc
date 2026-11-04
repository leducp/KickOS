// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// An exception nothing catches, thrown in a worker of a task this image creates (peer.cc): the
// terminate handler names it, and the whole task ends with 128 + SIGABRT, its sibling and its
// main stopped, which this creator reads (tests/integration/check_qemu_cxxterm.sh --worker).

#include <signal.h>

#include <kickos/apps/peer_run.h>
#include <kickos/kos.h>
#include <kickos/libc/fmt.h>
#include <kickos/sys.h>

int main(int, char**)
{
    constexpr int POLLS = 30;
    constexpr uint64_t POLL_NS = 100000000ull;
    kos_program_t prog = KOS_CAP_NONE;
    if (kos_program_find("cxxterm_worker_peer", &prog) != 0)
    {
        kos::print("cxxterm: no peer program\n");
        return 1;
    }
    kos_task_params params = {};
    kos_task_t peer = KOS_TASK_NONE;
    if (kickos::apps::create_peer(prog, params, &peer) != 0)
    {
        kos::print("cxxterm: no task slot for the peer\n");
        return 1;
    }
    int status = 0;
    int state = 0;
    for (int i = 0; i < POLLS; i++)
    {
        state = kos_task_state(peer, &status);
        if (state < 0)
        {
            kos::print("cxxterm: the peer task's state is unreadable\n");
            return 1;
        }
        if ((state & KOS_TASK_DEAD) != 0)
        {
            break;
        }
        kos::sleep_ns(POLL_NS);
    }
    if ((state & KOS_TASK_DEAD) == 0)
    {
        kos::print("cxxterm: the peer task still runs after its worker's uncaught throw\n");
        return 1;
    }
    char line[64];
    ksnprintf(line, sizeof(line), "cxxterm: the peer task is dead, status %d\n", status);
    kos::print(line);
    (void)kos_task_slay(peer, 0);
    (void)kos_handle_close(prog);
    if (status != 128 + SIGABRT)
    {
        return 1;
    }
    return 0;
}
