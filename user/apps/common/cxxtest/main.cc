// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Full-C++ opt-in smoke test: proves the toolchain's libstdc++/libsupc++
// over newlib actually EXECUTES on the MCU: exceptions (throw/catch, EH
// unwind), STL (std::vector, std::string over the heap), and RTTI
// (dynamic_cast + typeid). Built ONLY under the kickos_cxx opt-in (see
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
#include <kickos/libc/fmt.h>

#include <exception>
#include <stdexcept>
#include <string>
#include <typeinfo>
#include <vector>

namespace
{
    int g_fails = 0;

    void report(char const* what, bool ok)
    {
        char b[64];
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

#if KICKOS_HAVE_ASPACE
    // Read through a volatile, so each space constructs the word at its startup.
    volatile int g_ctor_seed = 41;
    int const g_ctor_word = g_ctor_seed + 1;

    enum
    {
        TK_CTOR = 0,
        TK_CAUGHT = 1,
        TK_OUT = 16 // words, ahead of the least stack in the same block
    };
    constexpr uint32_t TK_FILL = 0x5AA5C33Cu;
    constexpr uint32_t TK_STACK_MAX = KICKOS_MIN_STACK_SIZE + 1024u;
    constexpr size_t TK_BLOCK = TK_OUT * sizeof(int) + TK_STACK_MAX;

    void task_worker(void* arg)
    {
        auto const out = static_cast<int volatile*>(arg);
        out[TK_CTOR] = g_ctor_word;
        int caught = 0;
        try
        {
            throw MyError(7);
        }
        catch (MyError const& e)
        {
            caught = e.code_;
        }
        out[TK_CAUGHT] = caught;
    }

    void least_worker(void* arg)
    {
        static_cast<int volatile*>(arg)[TK_CTOR] = g_ctor_word;
    }

    // Each task a spawn builds starts from the image's pristine data and runs the image's
    // startup on its own stack: the unwind tables and the constructors, before its entry.
    void test_task_start()
    {
        void* const blk = kos_ram_alloc(TK_BLOCK);
        if (blk == nullptr or kos_mem_self_grant(blk, TK_BLOCK, 0) != 0)
        {
            report("a block for the task checks", false);
            return;
        }
        auto const out = static_cast<int volatile*>(blk);
        out[TK_CTOR] = 0;
        out[TK_CAUGHT] = 0;
        auto h = kos::thread::create(task_worker, blk, "cxxtask", 10, KOS_POLICY_FIFO, 0, blk,
                                     TK_BLOCK);
        bool const ran = h.valid() and h.join(KOS_TIMEOUT_NONE) == 0;
        report("a task runs in a space of its own", ran);
        report("a task's constructors ran in its space", out[TK_CTOR] == 42);
        report("a task throws and catches in its space", out[TK_CAUGHT] == 7);

        // The least stack the spawn admits: the floor plus the image's thread-local carve.
        auto const stk = static_cast<unsigned char*>(blk) + TK_OUT * sizeof(int);
        uint32_t size = KICKOS_MIN_STACK_SIZE;
        int rc = -KOS_EINVAL;
        while (rc == -KOS_EINVAL and size <= TK_STACK_MAX)
        {
            for (uint32_t i = 0; i < TK_STACK_MAX; i += 4u)
            {
                *reinterpret_cast<uint32_t volatile*>(stk + i) = TK_FILL;
            }
            out[TK_CTOR] = 0;
            h = kos::thread::create(least_worker, blk, "cxxleast", 10, KOS_POLICY_FIFO, 0, blk,
                                    TK_BLOCK, stk, size);
            rc = h.error();
            if (rc == -KOS_EINVAL)
            {
                size += 16u;
            }
        }
        bool const least_ran = rc == 0 and h.join(KOS_TIMEOUT_NONE) == 0;
        // The carve sits at the low end; the scan starts above it.
        uintptr_t p = reinterpret_cast<uintptr_t>(stk) + size - KICKOS_MIN_STACK_SIZE;
        uintptr_t const lo = p;
        uintptr_t const top = reinterpret_cast<uintptr_t>(stk) + size;
        while (p < top and *reinterpret_cast<uint32_t volatile const*>(p) == TK_FILL)
        {
            p += 4u;
        }
        char b[112];
        ksnprintf(b, sizeof(b),
                  "task start: least stack %u, deepest %u of the %u above the carve\n",
                  static_cast<unsigned>(size), static_cast<unsigned>(top - p),
                  static_cast<unsigned>(KICKOS_MIN_STACK_SIZE));
        kos::print(b);
        report("a task starts on the least stack a spawn admits",
               least_ran and out[TK_CTOR] == 42);
        report("its startup left the least stack's low end unwritten", p > lo);
    }
#endif

    kos::Semaphore g_done;

    // UNPRIVILEGED: the whole full-C++ body runs here, under the MPU.
    void cxx_worker(void*)
    {
        test_exceptions();
        test_stl();
        test_rtti();

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
