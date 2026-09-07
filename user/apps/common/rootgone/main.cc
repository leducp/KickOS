// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ROOT MAY DIE, AND ITS AUTHORITY IS NOT INHERITABLE.
//
// Root's ORDINARY exit ends the system (KOS_SYS_EXIT reads ThreadPool::is_root), but three
// kernel-internal routes reach sched::exit_current for root all the same: a fault, a group
// cancel, and wild-stack containment. This image takes the first of them, the same
// cross-domain write `rootfault` uses, and then asks whether the NEXT thread the pool hands
// out becomes root. ThreadPool::alloc retires root's slot rather than reclaiming it, so it
// must not: a grandchild landing on root's slot would answer is_root on every core, so its
// kos_wait_last must be refused and its own ordinary exit must end only itself.
//
// The two markers are the arm: `wait_last refused` says the identity did not move, and
// ROOTGONE PASS says the system carried on with root gone AND that the grandchild's exit was
// an ordinary one.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>
#include <kickos/sys/emit.h>   // emit: kos_print is dropped once the console is published

using kickos::emit;

namespace
{
    // Root's delegated semaphore lands at the child's table index 1 (B1 default).
    constexpr int CH_READY = 1;

    // Root faults immediately after the child's post, so this is a margin and not a
    // synchronisation: what actually establishes that root died is the kernel's own
    // thread-fault line, which the gate requires.
    constexpr uint64_t ROOT_DEATH_MARGIN_NS = 200ull * 1000ull * 1000ull;

    void grandchild(void*)
    {
        // ROOT ONLY, and the whole point: this thread holds root's vacated slot, so a grant
        // here would mean the identity was inherited.
        int const rc = kos_wait_last();
        if (rc == -KOS_EPERM)
        {
            emit("[rootgone] grandchild: wait_last refused\n");
        }
        else
        {
            char msg[80];
            ksnprintf(msg, sizeof(msg),
                      "[rootgone] grandchild: wait_last GRANTED rc=%d (inherited root)\n", rc);
            emit(msg);
        }
        kos_exit(0);
    }

    void survivor(void*)
    {
        emit("[rootgone] child: alive, releasing root\n");
        kos_sem_post(CH_READY);
        kos_sleep_ns(ROOT_DEATH_MARGIN_NS);
        emit("[rootgone] child: outlived root\n");

        // A task of its OWN, so the grandchild's own death cannot reach this thread and the
        // question stays about the POOL SLOT rather than about group membership.
        kos_task_t group = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &group) != 0)
        {
            emit("[rootgone] ERROR: no task slot for the grandchild\n");
            kos_exit(1);
        }
        auto const g = kos::thread::create(grandchild, nullptr, "grand", 10,
                                           KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                           nullptr, 0, nullptr, 0, nullptr, 0,
                                           nullptr, 0, 0, nullptr, group);
        if (not g.valid())
        {
            emit("[rootgone] ERROR: grandchild spawn refused\n");
            kos_exit(1);
        }
        if (g.join(KOS_TIMEOUT_NONE) != 0)
        {
            emit("[rootgone] ERROR: the grandchild never came back\n");
            kos_exit(1);
        }
        (void)kos_task_kill(group);
        emit("[rootgone] ROOTGONE PASS\n");
        (void)kos_shutdown(0);
        emit("[rootgone] ERROR: shutdown refused\n");
        kos_exit(1);
    }
}

int main(int, char**)
{
    void* const region = kos_ram_alloc(4096);
    kos_cap_t ready = KOS_CAP_NONE;
    int const ready_rc = kos_sem_create(0, &ready);
    if (region == nullptr or ready_rc != 0)
    {
        emit("[rootgone] ERROR: ram_alloc / sem_create refused (authority seat?)\n");
        return 1;
    }

    // The region below is what gives the child a DOMAIN, and therefore a task, of its own:
    // root's fault ends the faulting thread's whole task, so a child in root's group would
    // die with it. A NAMED task is not the way to do it here, a member being allowed no
    // mem_base of its own (kos_task_create), and the region has to be the child's for
    // root's write into it to be a cross-domain one.
    // The child outlives root and finishes the run, so it needs the two authorities root
    // spends here: a task slot for the grandchild, and the shutdown.
    kos_cap_grant caps[] = {
        { ready, KOS_CAP_WAIT | KOS_CAP_SIGNAL | KOS_CAP_TRANSFER },
    };
    auto const child = kos::thread::create_caps(survivor, nullptr, "survivor", 10,
                                                caps, 1, KOS_POLICY_FIFO, 0,
                                                /*privileged=*/false, region, 4096,
                                                KOS_AUTH_MEMORY | KOS_AUTH_SYSTEM);
    if (not child.valid())
    {
        emit("[rootgone] ERROR: child spawn refused\n");
        return 1;
    }
    kos_sem_wait(ready);

    // Announced WITH the address, as rootfault's is: the gate cross-checks this against the
    // address the kernel's fault report carries, so a trap taken anywhere else is not
    // mistaken for this one.
    char msg[96];
    ksnprintf(msg, sizeof(msg),
              "[rootgone] root: writing the child's granted region at %p (expect fault)\n",
              region);
    emit(msg);
    *static_cast<volatile int*>(region) = 0x3333;

    // Reached only where nothing is enforced, which is why the gate is registered on
    // enforcing builds only.
    emit("[rootgone] cross-domain write completed: root is NOT confined "
         "(no enforcement)\n");
    return 0;
}
