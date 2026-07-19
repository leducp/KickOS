// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Full-C++ opt-in smoke test (Stage B of docs/design-kickcat-k64f.md): proves the
// toolchain's libstdc++/libsupc++ over newlib actually EXECUTES on the MCU --
// exceptions (throw/catch, EH unwind), STL (std::vector, std::string over the heap),
// and RTTI (dynamic_cast + typeid). Built ONLY under the FULL_CXX opt-in (see
// CMakeLists.txt); a freestanding app cannot use any of this. Prints one PASS/FAIL
// line per check then returns (clean exit -> QEMU SYS_EXIT).
//
// The checks run in a spawned UNPRIVILEGED worker, not inline in main: main is the
// privileged root thread, which bypasses the MPU, so an inline throw would never
// touch the app grant. Under enforcement the worker's throw/catch drives the DWARF/
// EHABI/SjLj unwinder over eh_globals + the FDE registry, and every new/vector/string
// allocation goes through the app-side arena -- all of which must lie in the worker's
// reachable grant. That is the real gate: full-C++ inside MPU confinement (the RISC-V
// gp-in-appdata layout, docs/design-riscv-gp-split.md).

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

    // Polymorphic hierarchy for the RTTI checks.
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
        // Custom type caught by its concrete type -- the right handler, with payload.
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

        // Catch by std base -- proves the libsupc++ type-match walks the hierarchy.
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

        // Unwinding a scope with a non-trivial local (std::string dtor must run).
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

        // Virtual dispatch through the base pointer.
        report("virtual dispatch picks override", b->tag() == 2);

        // dynamic_cast down the real dynamic type must succeed...
        Derived* dd = dynamic_cast<Derived*>(b);
        report("dynamic_cast to real type succeeds", dd != nullptr);

        // ...and a cast to an unrelated sibling type must fail (return nullptr).
        Base base_only;
        Base* bp = &base_only;
        Derived* bad = dynamic_cast<Derived*>(bp);
        report("dynamic_cast to wrong type yields nullptr", bad == nullptr);

        // typeid reflects the dynamic type through a base reference.
        bool typeid_ok = typeid(*b) == typeid(Derived) and typeid(*bp) == typeid(Base);
        report("typeid reflects dynamic type", typeid_ok);
    }

    int g_done = -1; // worker -> main handoff (MAIN's cap; delegated to the worker)
    // B1: fresh child table => handle == index; the worker's delegated g_done is at index 1.
    constexpr int CH_DONE = 1;

    // UNPRIVILEGED: the whole full-C++ body runs here, under the MPU. Exceptions
    // unwind, RTTI matches, and STL/new allocate all through the worker's own grant.
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
        kos_sem_post(CH_DONE); // g_done (delegated from main)
    }
}

int main(int, char**)
{
    kos::print("KickOS full-C++ opt-in test\n");

    g_done = kos_sem_create(0);
    // Default spawn => UNPRIVILEGED (privileged=false). prio 10 sits above root
    // (KICKOS_PRIO_MIN+1), so once main blocks on g_done the worker runs to completion
    // and root wakes as the last live thread (the selftest orchestration shape). Stack:
    // the kernel default KICKOS_USER_STACK_SIZE (8 KB on every gated board), demand-
    // allocated from the app arena -- headroom for the EH unwind + libstdc++ working set,
    // and it fits each board's window (a static KOS_STACK_DEFINE buffer would have to be a
    // pow2 region inside C6's 4 KB .appdata, which 8 KB cannot).
    kos_cap_grant caps[] = {{g_done, KOS_CAP_WAIT | KOS_CAP_SIGNAL | KOS_CAP_TRANSFER}}; // g_done@1
    int w = kos::thread::spawn_caps(cxx_worker, nullptr, "cxxwork", 10, caps, 1);
    if (w < 0)
    {
        kos::print("SOME FAILED\n"); // spawn failure: fail loud, do not fall back to privileged
        return 1;
    }
    kos_sem_wait(g_done);
    kos_sem_destroy(g_done);

    if (g_fails == 0)
    {
        return 0;
    }
    return 1;
}
