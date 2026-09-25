// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A TASK LOSES ITS LAST MEMBER WHILE A PEER CORE IS STILL INSIDE ITS OWN DEATH.
//
// Two members of one task, one pinned per core, both leaving. The first one to go runs its
// capability sweep, which drops the kernel lock between chunks, so it is preemptible and still
// running; the second one's departure is what takes the task's refcount to zero and frees the
// address space's tables.
//
// The hazard this shape exercises: the release's peer sweep clears another core's BOOKKEEPING
// cell but cannot reach that core's translation register, so a core still walking tables at
// that moment must have already left the space before the sweep runs. On rv64 the kernel's own
// top-level entries live in the root page that gets freed, so a miss here is a kernel crash
// rather than a stray user mapping.
//
// WHAT THIS IMAGE ASSERTS is the positive statement: KOS_ASPACE_OP_RELEASE_PEER_HITS, the
// count of peer cores a destroy has found still holding the space it was destroying, must
// read 0, the destroy's peer sweep being bookkeeping and never a repair. Beside it
// KOS_ASPACE_OP_RELEASE_RUNS, without which the 0 is equally the answer for a run in which no
// destroy happened at all. Plus the run surviving, which is the crash above.
//
// THIS IS A CRASH DETECTOR AND NOT A NEGATIVE CONTROL: reverting the fix leaves it PASSING.
// Whether the second member's release lands inside the first one's sweep is a race this image
// cannot force: nothing in userspace observes when a peer is inside its own sweep, and
// nothing it can mint widens that sweep either, cap_teardown's chunk count being fixed by the
// child's table WIDTH (KICKOS_CAP_CHILD_WIDTH), which userspace cannot move. The
// deterministic arm for the ORDER is the host gate (tests/unit/deathspace, LeaveSpace).

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/abi_probe.h>
#include <kickos/libc/fmt.h>
#include <kickos/sys/emit.h>

using kickos::emit;

namespace
{
    // Root's semaphore, delegated to both members at their table index 1.
    constexpr int SIG = 1;

    constexpr int ROUNDS = 8;

    void leaver(void*)
    {
        // Releases the peer, then dies: the peer's own departure is what empties the group,
        // and it must land while this thread is still inside its own teardown.
        kos_sem_post(SIG);
        kos_exit(0);
    }

    void closer(void*)
    {
        kos_sem_wait(SIG);
        kos_exit(0);
    }
}

int main(int, char**)
{
    for (int round = 0; round < ROUNDS; round++)
    {
        kos_task_t group = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &group) != 0)
        {
            emit("[taskleave] ERROR: no task slot\n");
            return 1;
        }
        kos_cap_t sig = KOS_CAP_NONE;
        if (kos_sem_create(0, &sig) != 0)
        {
            emit("[taskleave] ERROR: sem_create refused\n");
            return 1;
        }
        kos_cap_grant caps[] = {
            { sig, KOS_CAP_WAIT | KOS_CAP_SIGNAL | KOS_CAP_TRANSFER },
        };

        // The closer first: the leaver runs on its own core and may be gone before a later
        // spawn, which would reclaim its slot and leave its handle stale for the join below.
        auto const b = kos::thread::create_caps(closer, nullptr, "closer", 10, caps, 1,
                                                KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                                nullptr, 0, 0, nullptr, group,
                                                nullptr, 0, 1u << 1);
        auto const a = kos::thread::create_caps(leaver, nullptr, "leaver", 10, caps, 1,
                                                KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                                nullptr, 0, KOS_AUTH_MEMORY, nullptr, group,
                                                nullptr, 0, 1u << 0);
        if (not a.valid() or not b.valid())
        {
            emit("[taskleave] ERROR: member spawn refused\n");
            return 1;
        }
        // Both, and unbounded: a bound here would turn the hazard's own symptom, a core
        // walking freed tables, into a timeout report instead of a hang the gate names.
        if (a.join(KOS_TIMEOUT_NONE) != 0 or b.join(KOS_TIMEOUT_NONE) != 0)
        {
            emit("[taskleave] ERROR: a member never came back\n");
            return 1;
        }
        // The group is empty, so this drops root's creator hold and releases the slot.
        (void)kos_task_kill(group);
        (void)kos_handle_close(sig);
    }

    uintptr_t const hits = kos_aspace_probe(KOS_ASPACE_OP_RELEASE_PEER_HITS, 0);
    uintptr_t const runs = kos_aspace_probe(KOS_ASPACE_OP_RELEASE_RUNS, 0);
    char msg[96];
    ksnprintf(msg, sizeof(msg), "[taskleave] release peer hits: %llu\n",
              static_cast<unsigned long long>(hits));
    emit(msg);
    ksnprintf(msg, sizeof(msg), "[taskleave] space destroys: %llu\n",
              static_cast<unsigned long long>(runs));
    emit(msg);
    if (hits != 0u)
    {
        emit("[taskleave] TASKLEAVE FAIL: a destroy found a peer core holding the space\n");
        return 1;
    }
    if (runs < static_cast<uintptr_t>(ROUNDS))
    {
        emit("[taskleave] TASKLEAVE FAIL: destroys ran for fewer rounds than this image had,\n"
             "so the peer-hit count above answered for a destroy that never ran\n");
        return 1;
    }
    emit("[taskleave] TASKLEAVE PASS\n");
    return 0;
}
