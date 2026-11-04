// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Full-C++ opt-in smoke test: proves the toolchain's libstdc++/libsupc++
// over newlib actually EXECUTES on the MCU: exceptions (throw/catch, EH
// unwind), STL (std::vector, std::string over the heap), RTTI (dynamic_cast +
// typeid), and the runtime's state shared by threads (threads.cc). Built ONLY under the kickos_cxx opt-in (see
// CMakeLists.txt). Prints one PASS/FAIL line per check then returns (clean
// exit -> QEMU SYS_EXIT).
//
// The checks run in a spawned UNPRIVILEGED worker, not inline in main: main's grant
// is composed at boot from the whole app image, so an inline throw would prove
// nothing about a thread whose grant is composed at spawn from what this app asks
// for. Under enforcement the worker's throw/catch drives the DWARF/EHABI/SjLj
// unwinder over eh_globals + the FDE registry, and every new/vector/string allocation
// goes through the app-side arena, so all of it must lie in the worker's reachable
// grant. That is the real gate: full-C++ inside MPU confinement (the RISC-V
// gp-in-appdata layout).

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/image.h>
#include <kickos/libc/fmt.h>

#if KICKOS_HAVE_ASPACE
#include <kickos/apps/peer_run.h>

#include "peer.h"
#endif

#include "threads.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <exception>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <typeinfo>
#include <vector>

namespace
{
    int g_fails = 0;

    void report(char const* what, bool ok)
    {
        char b[96];
        char const* tag = "PASS";
        if (not ok)
        {
            tag = "FAIL";
            ++g_fails;
        }
        ksnprintf(b, sizeof(b), "%s: %s\n", tag, what);
        kos::print(b);
    }

    // A custom exception carrying a payload, so the catch can prove the RIGHT
    // handler ran (not merely that some unwind happened).
    struct MyError : std::runtime_error
    {
        explicit MyError(int code)
            : std::runtime_error("my error")
            , code_(code)
        {
        }
        int code_;
    };

    struct Base
    {
        virtual ~Base() = default;
        virtual int tag() const
        {
            return 1;
        }
    };
    struct Derived : Base
    {
        int tag() const override
        {
            return 2;
        }
    };

    void test_exceptions()
    {
        bool right_handler = false;
        int seen_code = 0;
        try
        {
            throw MyError(42);
        }
        catch (std::logic_error const&)
        {
            right_handler = false; // wrong sibling: must NOT be chosen
        }
        catch (MyError const& e)
        {
            right_handler = true;
            seen_code = e.code_;
        }
        catch (...)
        {
            right_handler = false;
        }
        report("exception caught by exact type", right_handler and seen_code == 42);

        bool caught_by_base = false;
        try
        {
            throw MyError(7);
        }
        catch (std::exception const& e)
        {
            caught_by_base = std::string(e.what()) == "my error";
        }
        report("exception caught by std::exception base", caught_by_base);

        bool unwound = false;
        try
        {
            std::string local("scope-local");
            throw std::runtime_error("boom");
        }
        catch (std::runtime_error const&)
        {
            unwound = true;
        }
        report("stack unwind runs local dtors", unwound);
    }

    void test_stl()
    {
        std::vector<int> v;
        for (int i = 1; i <= 100; ++i)
        {
            v.push_back(i);
        }
        int sum = 0;
        for (int x : v)
        {
            sum += x;
        }
        report("std::vector<int> push_back + sum == 5050", v.size() == 100 and sum == 5050);

        std::string s = "Kick";
        s += "OS";
        s.append("/full-c++");
        report("std::string concat == KickOS/full-c++", s == "KickOS/full-c++");
    }

    void test_rtti()
    {
        Derived d;
        Base* b = &d;

        report("virtual dispatch picks override", b->tag() == 2);

        Derived* dd = dynamic_cast<Derived*>(b);
        report("dynamic_cast to real type succeeds", dd != nullptr);

        Base base_only;
        Base* bp = &base_only;
        Derived* bad = dynamic_cast<Derived*>(bp);
        report("dynamic_cast to wrong type yields nullptr", bad == nullptr);

        bool typeid_ok = typeid(*b) == typeid(Derived) and typeid(*bp) == typeid(Base);
        report("typeid reflects dynamic type", typeid_ok);
    }

    // Not a multiple of the 8-byte RDSEED word.
    constexpr size_t ENTROPY_PROBE = 37;

    // A source's checks, kept out of test_entropy's frame for the boards that have none.
    __attribute__((noinline)) void test_entropy_source(unsigned char const* first)
    {
        unsigned char second[ENTROPY_PROBE] = {};
        bool const differ = getentropy(second, sizeof(second)) == 0
                            and memcmp(first, second, sizeof(second)) != 0;
        report("getentropy fills two requests with different bytes", differ);

        unsigned char most[257] = {};
        errno = 0;
        bool const bounded = getentropy(most, 256) == 0 and getentropy(most, 257) == -1
                             and errno == EINVAL;
        report("getentropy serves 256 bytes and refuses 257 with EINVAL", bounded);

        bool drew = false;
        try
        {
            std::random_device device;
            (void)device();
            (void)device();
            drew = true;
        }
        catch (std::exception const&)
        {
        }
        report("random_device constructs and draws from getentropy", drew);
    }

    // The answer picks the branch; the judge requires the lines of the one the board owes.
    void test_entropy()
    {
        unsigned char first[ENTROPY_PROBE] = {};
        errno = 0;
        int const rc = getentropy(first, sizeof(first));
        if (not (rc == -1 and errno == ENOSYS))
        {
            report("getentropy answers a request from its source", rc == 0);
            test_entropy_source(first);
            return;
        }
        report("getentropy refuses with ENOSYS", true);
        bool refused = false;
        try
        {
            std::random_device device;
            (void)device();
        }
        catch (std::system_error const& e)
        {
            refused = e.code().value() == ENOSYS;
        }
        report("random_device refuses to construct with no source", refused);
    }

#if KICKOS_HAVE_ASPACE
    constexpr uint32_t TK_FILL = 0x5AA5C33Cu;
    constexpr uint32_t TK_STACK_MAX = KICKOS_MIN_STACK_SIZE + 1024u;

    // One incarnation of the peer; its exit status, or a value no status takes.
    int run_peer(kos_program_t prog, kos_task_params const& p)
    {
        kos_task_t t = KOS_TASK_NONE;
        int const rc = kickos::apps::create_peer(prog, p, &t);
        if (rc != 0)
        {
            return rc;
        }
        int status = -1;
        int const wait = kickos::apps::wait_dead(t, &status, 5000u);
        if (wait != 0)
        {
            return wait;
        }
        return status;
    }

    // Each task a create builds starts from the image's pristine data and runs the image's
    // startup on its own stack: the unwind tables and the constructors, before its entry.
    void test_task_start()
    {
        kos_program_t prog = KOS_CAP_NONE;
        if (kos_program_find("cxxtest_peer", &prog) != 0)
        {
            report("the peer program is found", false);
            return;
        }
        kos_task_params p = {};
        int status = run_peer(prog, p);
        report("a task runs in a space of its own", status >= 0);
        report("a task's constructors ran in its space",
               status >= 0 and (status & CXXTEST_PEER_CTOR_BAD) == 0);
        report("a task throws and catches in its space",
               status >= 0 and (status & CXXTEST_PEER_CAUGHT_BAD) == 0);

        // The least stack the create admits: the floor plus the image's thread-local carve.
        static char const* const least_argv[] = {"peer", "least", nullptr};
        void* const blk = kos_ram_alloc(TK_STACK_MAX);
        if (blk == nullptr or kos_mem_self_grant(blk, TK_STACK_MAX, 0) != 0)
        {
            report("a block for the least stack", false);
            (void)kos_handle_close(prog);
            return;
        }
        auto const stk = static_cast<unsigned char*>(blk);
        uint32_t size = KICKOS_MIN_STACK_SIZE;
        status = -KOS_EINVAL;
        while (status == -KOS_EINVAL and size <= TK_STACK_MAX)
        {
            for (uint32_t i = 0; i < TK_STACK_MAX; i += 4u)
            {
                *reinterpret_cast<uint32_t volatile*>(stk + i) = TK_FILL;
            }
            kos_task_params q = {};
            q.argv = least_argv;
            q.block = blk;
            q.size = TK_STACK_MAX;
            q.stack = stk;
            q.stack_size = size;
            status = run_peer(prog, q);
            if (status == -KOS_EINVAL)
            {
                size += 16u;
            }
        }
        (void)kos_handle_close(prog);
        bool const least_ran = status == 0;
        // The carve sits at the low end; the scan starts above it.
        uintptr_t p0 = reinterpret_cast<uintptr_t>(stk) + size - KICKOS_MIN_STACK_SIZE;
        uintptr_t const lo = p0;
        uintptr_t const top = reinterpret_cast<uintptr_t>(stk) + size;
        while (p0 < top and *reinterpret_cast<uint32_t volatile const*>(p0) == TK_FILL)
        {
            p0 += 4u;
        }
        char b[112];
        ksnprintf(b, sizeof(b),
                  "task start: least stack %u, deepest %u of the %u above the carve\n",
                  static_cast<unsigned>(size), static_cast<unsigned>(top - p0),
                  static_cast<unsigned>(KICKOS_MIN_STACK_SIZE));
        kos::print(b);
        report("a task starts on the least stack a create admits", least_ran);
        report("its startup left the least stack's low end unwritten", p0 > lo);
    }
#endif

    // Root's startup is the init's alone, and the app's constructors run in the entry thread of
    // the app's first task, which here is main's: the thread this word names. The host runs a sim
    // image's before the kernel boots.
#if not KICKOS_ARCH_SIM
#define CXXTEST_OWN_STARTUP 1
    kos_thread_t g_ctor_thread = KOS_THREAD_NONE;

    struct CtorWitness
    {
        CtorWitness()
        {
            g_ctor_thread = kos_thread_self();
        }
    };
    CtorWitness g_ctor_witness;

    void test_own_startup()
    {
        report("app constructors ran in main's thread", g_ctor_thread == kos_thread_self());
        uintptr_t const at = reinterpret_cast<uintptr_t>(&g_ctor_thread);
        uintptr_t const base = kickos_image_header.window_start;
        report("app statics in the app image's window",
               at >= base and at - base < kickos_image_header.window_size);
    }
#endif

    kos::Semaphore g_done;

    // UNPRIVILEGED: the whole full-C++ body runs here, under the MPU.
    void cxx_worker(void*)
    {
        cxxtest_first_throws(report);
        test_exceptions();
        test_stl();
        test_rtti();
        test_entropy();
        cxxtest_threads(report);

        char const* verdict = "SOME FAILED\n";
        if (g_fails == 0)
        {
            verdict = "ALL PASS\n";
        }
        kos::print(verdict);
        (void)g_done.post();
    }
}

int main(int, char**)
{
    kos::print("KickOS full-C++ opt-in test\n");
#if CXXTEST_OWN_STARTUP
    test_own_startup();
#endif
#if KICKOS_HAVE_ASPACE
    test_task_start();
#endif

    // Default spawn => UNPRIVILEGED (privileged=false). prio 10 sits above main's 2, so the
    // worker runs to completion once main blocks on g_done. The stack is the board's own
    // KICKOS_USER_STACK_SIZE, demand-allocated from the app arena, so the EH unwind and the
    // libstdc++ working set are charged to that arena.
    auto w = kos::thread::create(cxx_worker, nullptr, "cxxwork", 10);
    if (not w.valid())
    {
        kos::print("SOME FAILED\n"); // spawn failure: fail loud, do not fall back to privileged
        return 1;
    }
    (void)g_done.wait();

    if (g_fails == 0)
    {
        return 0;
    }
    return 1;
}
