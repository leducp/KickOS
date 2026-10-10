// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The address-space arms: the map editor, frame capabilities, processes, and the stack as
// frames.

#include "selftest.h"

namespace selftest
{
#if KICKOS_HAVE_ASPACE && defined(KICKOS_ENABLE_SELFTEST)
    constexpr uint32_t MEM_UNREAD = 0xFFFFFFFFu;

    // kos_mem_count's value, or MEM_UNREAD where the read was refused.
    uint32_t mem_count(uint32_t which)
    {
        uint32_t v = MEM_UNREAD;
        if (kos_mem_count(which, &v) != 0)
        {
            return MEM_UNREAD;
        }
        return v;
    }

    // A minted run and the caller's own space capability.
    struct Seed
    {
        kos_cap_t frame = KOS_CAP_NONE;
        kos_cap_t space = KOS_CAP_NONE;
    };

    void seed_close(Seed* s)
    {
        if (s->frame != KOS_CAP_NONE)
        {
            (void)kos_handle_close(s->frame);
            s->frame = KOS_CAP_NONE;
        }
        if (s->space != KOS_CAP_NONE)
        {
            (void)kos_handle_close(s->space);
            s->space = KOS_CAP_NONE;
        }
    }

    bool seed_mint(size_t bytes, Seed* s)
    {
        if (kos_frame_create(bytes, &s->frame) != 0 or kos_aspace_self(&s->space) != 0)
        {
            seed_close(s);
            return false;
        }
        return true;
    }

    // The run's own address, where a map at 0 puts it, left unmapped; 0 if it would not map.
    uintptr_t seed_va(Seed const& s)
    {
        uintptr_t va = 0;
        if (kos_frame_map(s.frame, s.space, &va, 0) != 0)
        {
            return 0;
        }
        if (kos_frame_unmap(s.frame, s.space, va) != 0)
        {
            return 0;
        }
        return va;
    }

    void reached(kos_cap_t ep)
    {
        char const mark = 0;
        (void)kos_send(ep, &mark, sizeof(mark));
    }

    bool heard_reached(kos_cap_t ep)
    {
        char mark = 1;
        return report_await(ep, &mark, sizeof(mark));
    }

    void park_witness(void*) {}

    // True once every thread pinned to TAP_PIN_CORE above TAP_PRIO_AFTER has parked or ended: a
    // witness below them on that core runs only then. Call it once the subject is runnable.
    bool await_pinned_park()
    {
        kos::thread::Handle w = kos::thread::create_caps(
            park_witness, nullptr, "parkw", TAP_PRIO_AFTER, nullptr, 0, KOS_POLICY_FIFO, 0, false,
            nullptr, 0, 0, nullptr, KOS_TASK_NONE, nullptr, 0, TAP_PIN_CORE);
        if (not w.valid())
        {
            return false;
        }
        if (thread_end(&w) == 0)
        {
            return true;
        }
        (void)w.slay(ARM_SLAY_US);
        return false;
    }

    // Take `want` once every earlier arm's tasks are slain: a late end lowers the count past it.
    constexpr uint64_t SPACES_POLL_NS = 100000ull;
    bool spaces_back(uint32_t want)
    {
        uint64_t const deadline = kos_clock_now() + uint64_t{STALL_TOLERANT_US} * 1000u;
        while (mem_count(KOS_MEM_SPACES_HELD) > want)
        {
            if (kos_clock_now() >= deadline)
            {
                return false;
            }
            kos_sleep_ns(SPACES_POLL_NS);
        }
        return true;
    }

#if defined(__x86_64__)
    // The tables the processor itself names to ring 3, the kernel leaving UMIP off: this core's
    // GDT, inside its per-core block, and the IDT.
    struct __attribute__((packed)) DescTable
    {
        uint16_t limit;
        uint64_t base;
    };
    constexpr unsigned KERNEL_WORDS = 2;
    // The kernel image sits below the app window this function is linked into.
    bool in_kernel_half(uintptr_t addr)
    {
        return addr != 0 and addr < reinterpret_cast<uintptr_t>(&in_kernel_half);
    }
    uintptr_t kernel_word(unsigned which)
    {
        DescTable t = {};
        if (which == 0)
        {
            __asm__ volatile("sgdt %0" : "=m"(t));
        }
        else
        {
            __asm__ volatile("sidt %0" : "=m"(t));
        }
        return static_cast<uintptr_t>(t.base);
    }
    char const* kernel_word_name(unsigned which)
    {
        if (which == 0)
        {
            return "this core's GDT";
        }
        return "the IDT";
    }
#else
    extern "C" char _sbss[];
    extern "C" char __kickos_ram_end[];
    extern "C" char __kickos_rom_start[];
    extern "C" char _estack[];
    // Words of the kernel's half from the link: its .bss and the lowest word of the boot core's
    // trap stack, and the bounds of the kernel's image and RAM. Held in data, so each is an
    // absolute word and not a PC-relative reference the app's code model cannot reach.
    char* volatile g_kernel_words[] = {_sbss, __kickos_ram_end};
    char* volatile g_kernel_bounds[] = {__kickos_rom_start, _estack};
    constexpr unsigned KERNEL_WORDS = 2;
    uintptr_t kernel_word(unsigned which)
    {
        return reinterpret_cast<uintptr_t>(g_kernel_words[which]);
    }
    bool in_kernel_half(uintptr_t addr)
    {
        uintptr_t const lo = reinterpret_cast<uintptr_t>(g_kernel_bounds[0]);
        uintptr_t const hi = reinterpret_cast<uintptr_t>(g_kernel_bounds[1]);
        return (lo >> (8u * sizeof(uintptr_t) - 1u)) != 0 and addr >= lo and addr < hi;
    }
    char const* kernel_word_name(unsigned which)
    {
        if (which == 0)
        {
            return "the kernel's .bss";
        }
        return "the boot core's trap stack";
    }
#endif

    void t_cap_map()
    {
        Seed s;
        Seed other;
        uintptr_t va = 0;
        ArmHold hold;
        TAP_HOLD(hold.mapped(&s.frame, &s.space, &va) and hold.cap(&other.frame)
                  and hold.cap(&other.space));
        TAP_CHECK(seed_mint(ASPACE_GRANULE, &s));
        va = seed_va(s);
        TAP_CHECK(va != 0);

        // Permission comes from the AUTHORITY word, not a rights bit: the entry's rights field is
        // full.
        TAP_CHECK(map_at(s.frame, s.space, va, 0) == 0);
        volatile uint32_t* p = reinterpret_cast<volatile uint32_t*>(va);
        *p = 0xC2C2C2C2u;
        TAP_CHECK(*p == 0xC2C2C2C2u);

        TAP_CHECK(map_at(s.frame, s.space, va, 0) != 0);
        TAP_CHECK(map_at(s.frame, s.space, va + 1u, 0) != 0);
        TAP_CHECK(map_at(s.frame, s.space, va, 0xFFu) != 0);

        // A revoke matches the RUN and not a shape: a second run of the same length must not
        // revoke the first's mapping.
        // Asserted, never skipped: a skip here leaves the identity check untested.
        TAP_CHECK(seed_mint(ASPACE_GRANULE, &other));
        TAP_CHECK(kos_frame_unmap(other.frame, s.space, va) != 0);
        // An address inside a range and not its base is refused.
        uintptr_t const inside_text =
            reinterpret_cast<uintptr_t>(&t_cap_map) & ~(ASPACE_GRANULE - 1u);
        TAP_CHECK(kos_frame_unmap(s.frame, s.space, inside_text) != 0);

        TAP_CHECK(kos_frame_unmap(s.frame, s.space, va) == 0);
        // Re-mapping is what says the unmap RELEASED the range. A second unmap refusing does
        // not: the backend refuses an already-unmapped range either way.
        TAP_CHECK(map_at(s.frame, s.space, va, 0) == 0);
        TAP_CHECK(kos_frame_unmap(s.frame, s.space, va) == 0);
        va = 0;
    }

    // Its task has a space of its own and NO reservation: the frame reaches it as a delegated
    // capability and nothing else.
    constexpr int CH_SHARE_FRAME = 1;
    constexpr uint32_t SHARE_A = 0xC3A11CE0u; // written by main, read by the child
    constexpr uint32_t SHARE_B = 0xC3B00B1Eu; // written by the child, read by main
    // `arg` is where it maps the frame: a DIFFERENT address from main's, the holder choosing.
    void share_child(void* arg) // caps: frame@1
    {
        kos_cap_t space = KOS_CAP_NONE;
        (void)kos_aspace_self(&space);
        uintptr_t at = reinterpret_cast<uintptr_t>(arg);
        if (kos_frame_map(CH_SHARE_FRAME, space, &at, 0) == 0)
        {
            volatile uint32_t* p = reinterpret_cast<volatile uint32_t*>(at);
            if (p[0] == SHARE_A)
            {
                p[1] = SHARE_B; // seen: answer through the same frame
            }
            // Reported through the FRAME and not a global: its task holds a space of its own.
            p[2] = static_cast<uint32_t>(at & 0xFFFFFFFFu);
            p[3] = static_cast<uint32_t>(static_cast<uint64_t>(at) >> 32);
        }
    }

    constexpr int CH_PIN_FRAME = 2;
    constexpr int CH_PIN_GATE = 3; // never posted
    void pin_child(void*) // caps: E(SIGNAL)@1, frame@2, gate@3
    {
        kos_cap_t space = KOS_CAP_NONE;
        uintptr_t va = 0;
        int32_t rc = kos_aspace_self(&space);
        if (rc == 0)
        {
            rc = kos_frame_map(CH_PIN_FRAME, space, &va, 0);
        }
        kos_handle_close(CH_PIN_FRAME); // the mapping is now this task's only hold
        (void)kos_send(1, &rc, sizeof(rc));
        // Alive until slain: the space gives the run's mapping back at its task's death.
        kos_sem_wait(CH_PIN_GATE, KOS_TIMEOUT_NONE);
    }

    void ssr_worker(void*) // caps: gate@1
    {
        kos_sem_wait(1, KOS_TIMEOUT_NONE);
    }

    constexpr unsigned SSR_CYCLES = 8;

    // A thread stack takes a range slot, so a slot leaked at thread exit exhausts the list.
    void t_stack_slot_returns()
    {
        TAP_ASK(.workers = 1, .sems = 1);
        uint32_t const free0 = mem_count(KOS_MEM_RANGES_FREE);
        TAP_CHECK(free0 != MEM_UNREAD and free0 != 0);
        kos_cap_t gate = KOS_CAP_NONE;
        kos::thread::Handle w;
        ArmHold hold;
        TAP_HOLD(hold.cap(&gate) and hold.thread(&w));
        TAP_CHECK(kos_sem_create(0, &gate) == 0);
        kos_cap_grant const caps[] = {{gate, KOS_CAP_WAIT}};
        uint32_t live_low = free0;
        for (unsigned i = 0; i < SSR_CYCLES; i++)
        {
            w = kos::thread::create_caps(ssr_worker, nullptr, "ssr", 10, caps, 1);
            TAP_CHECK(w.valid());
            uint32_t const live = mem_count(KOS_MEM_RANGES_FREE);
            if (live < live_low)
            {
                live_low = live;
            }
            kos_sem_post(gate);
            TAP_CHECK(thread_end(&w) == 0);
        }
        TAP_CHECK(hold.close(&gate) == 0);
        uint32_t const after = mem_count(KOS_MEM_RANGES_FREE);
        tap::diag("stack slots: %u free, %u seen live over %u thread(s), %u after",
                  static_cast<unsigned>(free0), static_cast<unsigned>(live_low), SSR_CYCLES,
                  static_cast<unsigned>(after));
        // Positive control: a live stack takes a slot.
        TAP_CHECK(live_low < free0);
        TAP_CHECK(after == free0);
    }

    // Reject frame mappings over live stacks; replacement would overwrite thread locals.
    void t_cap_map_over_stack()
    {
        Seed s;
        ArmHold hold;
        TAP_HOLD(hold.cap(&s.frame) and hold.cap(&s.space));
        TAP_CHECK(seed_mint(ASPACE_GRANULE, &s));
        // A local, so the page under it is this thread's own stack whatever the board's layout.
        volatile uint32_t canary = 0x5A17C0DEu;
        uintptr_t const page = reinterpret_cast<uintptr_t>(&canary) & ~(ASPACE_GRANULE - 1u);
        TAP_CHECK(map_at(s.frame, s.space, page, 0) != 0);
        TAP_CHECK(canary == 0x5A17C0DEu);
        // Control: a free address maps.
        TAP_CHECK(seed_va(s) != 0);
        // The reference the map takes BEFORE the record refuses it must come back, or a run
        // nobody mapped is held for good by a call that failed: its last capability's close
        // gives its frame back.
        uint32_t const held = mem_count(KOS_MEM_FRAMES_FREE);
        seed_close(&s);
        uint32_t const closed = mem_count(KOS_MEM_FRAMES_FREE);
        tap::diag("map over the stack: %u frames free with the run, %u once closed",
                  static_cast<unsigned>(held), static_cast<unsigned>(closed));
        TAP_CHECK(held != MEM_UNREAD and closed == held + 1u);
    }

    // The stack-admission arms run on a stack the kernel placed with its guard below it, which
    // main's own stack is not: the init reserved it and handed it to main's task. Each runs in
    // a task of its own and ends by reading that guard, which ends the task.
    enum class PoolStackBody
    {
        GRANT_REFUSED,
        HANDOFF_REFUSED
    };
    constexpr uint32_t SNP_BLK = 256;
    constexpr int CH_PS_EP = 1;
    constexpr int PS_WORDS = 8;
    constexpr int32_t PS_UNSET = -99;
    struct PsReport
    {
        uintptr_t guard;
        int32_t w[PS_WORDS];
    };

    void stack_grant_refused_body(uintptr_t guard, PsReport* rep)
    {
        uintptr_t const g = ASPACE_GRANULE;
        // A local, so the page under it is this thread's own stack whatever the board's layout.
        volatile uint32_t canary = 0x51AC0DE5u;
        uintptr_t const page = reinterpret_cast<uintptr_t>(&canary) & ~(g - 1u);
        // The guard address names the base of the entire stack range.
        rep->w[0] = kos_mem_self_grant(reinterpret_cast<void*>(guard), g, 0);
        rep->w[1] = kos_mem_self_grant(reinterpret_cast<void*>(guard), 2u * g, 0);
        // A different memory type reaches admission; plain R|W takes the already-accessible
        // shortcut.
        rep->w[2] = kos_mem_self_grant(reinterpret_cast<void*>(page), g, KOS_MEM_NOCACHE);
        rep->w[3] = static_cast<int32_t>(canary == 0x51AC0DE5u);
        // Control: the same calls accept a caller-owned reservation.
        void* const mine = st_ram_own(SNP_BLK);
        if (mine != nullptr)
        {
            rep->w[4] = kos_mem_self_grant(mine, SNP_BLK, 0);
            rep->w[5] = kos_mem_self_grant(mine, SNP_BLK, KOS_MEM_NOCACHE);
            rep->w[6] = kos_mem_self_grant(mine, SNP_BLK, 0);
        }
        // Still refused after a success on the same path, so it is not a one-shot state.
        rep->w[7] = kos_mem_self_grant(reinterpret_cast<void*>(guard), g, 0);
    }

    // Reject donation of the live stack: its frames are freed when the donor exits.
    void stack_handoff_refused_body(uintptr_t guard, PsReport* rep)
    {
        uintptr_t const g = ASPACE_GRANULE;
        volatile uint32_t canary = 0x4A0FF5EDu;
        uint32_t const frames = mem_count(KOS_MEM_FRAMES_FREE);
        // Every extent: handoff requires an exact base and page count.
        int32_t admitted = 0;
        int32_t last = 0;
        for (unsigned pages = 1; pages <= 32u; pages++)
        {
            kos_task_t t = KOS_TASK_NONE;
            last = kos_task_create(reinterpret_cast<void*>(guard),
                                   static_cast<size_t>(pages) * g, 0, &t);
            if (last == 0)
            {
                admitted++;
                (void)kos_task_slay(t, STALL_TOLERANT_US);
            }
        }
        rep->w[0] = admitted;
        rep->w[1] = last;
        rep->w[2] = static_cast<int32_t>(canary == 0x4A0FF5EDu);
        // A refused handoff frees the space it half-built, so no frame is spent by the sweep.
        rep->w[3] = static_cast<int32_t>(frames != MEM_UNREAD
                                         and mem_count(KOS_MEM_FRAMES_FREE) == frames);
        // Control: accept handoff of a caller-owned reservation.
        void* const mine = st_ram_own(SNP_BLK);
        kos_task_t ok = KOS_TASK_NONE;
        if (mine != nullptr)
        {
            rep->w[4] = kos_task_create(mine, SNP_BLK, 0, &ok);
            rep->w[5] = kos_task_kill(ok);
        }
    }

    void pool_stack_worker(void* arg) // caps: E(SIGNAL)@1
    {
        // The stack's top granule, and the run below it of KICKOS_USER_STACK_SIZE rounded up
        // to whole granules; the guard is the granule under that.
        volatile uint32_t top_mark = 0;
        uintptr_t const g = ASPACE_GRANULE;
        uintptr_t const top = (reinterpret_cast<uintptr_t>(&top_mark) & ~(g - 1u)) + g;
        uintptr_t const pages = (KICKOS_SELFTEST_USER_STACK_SIZE + g - 1u) / g;
        PsReport rep = {};
        rep.guard = top - (pages + 1u) * g;
        for (int i = 0; i < PS_WORDS; i++)
        {
            rep.w[i] = PS_UNSET;
        }
        switch (static_cast<PoolStackBody>(reinterpret_cast<uintptr_t>(arg)))
        {
            case PoolStackBody::GRANT_REFUSED:
            {
                stack_grant_refused_body(rep.guard, &rep);
                break;
            }
            case PoolStackBody::HANDOFF_REFUSED:
            {
                stack_handoff_refused_body(rep.guard, &rep);
                break;
            }
        }
        (void)kos_send(CH_PS_EP, &rep, sizeof(rep));
        uint32_t step = *reinterpret_cast<volatile uint32_t*>(rep.guard + g);
        (void)kos_send(CH_PS_EP, &step, sizeof(step));
        step = *reinterpret_cast<volatile uint32_t*>(rep.guard);
        (void)kos_send(CH_PS_EP, &step, sizeof(step)); // unreachable: the guard has no frame
    }

    struct PsRun
    {
        bool spawned = false;
        bool reported = false;
        bool low_read = false;
        int joined = PS_UNSET;
        int status_rc = PS_UNSET;
        int status = PS_UNSET;
        PsReport rep = {};
    };

    void on_pool_stack(PoolStackBody body, PsRun* run)
    {
        kos_cap_t ep = KOS_CAP_NONE;
        kos_task_t t = KOS_TASK_NONE;
        ArmHold hold;
        TAP_HOLD(hold.cap(&ep) and hold.task(&t));
        TAP_CHECK(kos_endpoint_create(&ep) == 0);
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &t) == 0);
        kos_cap_grant const caps[] = {{ep, KOS_CAP_SIGNAL}};
        auto const w = kos::thread::create_caps(
            pool_stack_worker, reinterpret_cast<void*>(static_cast<uintptr_t>(body)), "pstk", 10,
            caps, 1, KOS_POLICY_FIFO, 0, /*privileged=*/false, nullptr, 0,
            KOS_AUTH_MEMORY | KOS_AUTH_TASKS, nullptr, t);
        TAP_CHECK(w.valid());
        run->spawned = true;
        run->reported = report_await(ep, &run->rep, sizeof(run->rep));
        uint32_t step = 0;
        run->low_read = run->reported and report_await(ep, &step, sizeof(step));
        run->joined = w.join(STALL_TOLERANT_US);
        run->status_rc = kos_task_exit_status(t, &run->status);
        TAP_CHECK(task_end(&t) == 0);
    }

    // The task ended on the guard read alone: the run's lowest granule read, and the one
    // below it faulted.
    bool ps_guard_faulted(PsRun const& run)
    {
        tap::diag("stack guard at 0x%lx: report %d, lowest granule read %d, joined %d, exit %d"
                  " (%d)",
                  static_cast<unsigned long>(run.rep.guard), static_cast<int>(run.reported),
                  static_cast<int>(run.low_read), run.joined, run.status, run.status_rc);
        return run.reported and run.low_read and run.joined == 0 and run.status_rc == 0
               and run.status == KOS_EXIT_FAULT;
    }

    void t_stack_grant_refused()
    {
        TAP_ASK(.workers = 1, .tasks = 1, .endpoints = 1);
        PsRun run;
        on_pool_stack(PoolStackBody::GRANT_REFUSED, &run);
        TAP_CHECK(run.spawned);
        int32_t const* const w = run.rep.w;
        tap::diag("guard grants %d %d, stack retype %d, again %d; controls %d %d %d",
                  static_cast<int>(w[0]), static_cast<int>(w[1]), static_cast<int>(w[2]),
                  static_cast<int>(w[7]), static_cast<int>(w[4]), static_cast<int>(w[5]),
                  static_cast<int>(w[6]));
        TAP_CHECK(ps_guard_faulted(run));
        TAP_CHECK(w[0] == -KOS_EPERM and w[1] == -KOS_EPERM and w[2] == -KOS_EPERM);
        TAP_CHECK(w[3] == 1);
        TAP_CHECK(w[4] == 0 and w[5] == 0 and w[6] == 0);
        TAP_CHECK(w[7] == -KOS_EPERM);
    }

    void t_stack_handoff_refused()
    {
        // The worker's control task beside its own.
        TAP_ASK(.workers = 1, .tasks = 2, .endpoints = 1);
        PsRun run;
        on_pool_stack(PoolStackBody::HANDOFF_REFUSED, &run);
        TAP_CHECK(run.spawned);
        int32_t const* const w = run.rep.w;
        tap::diag("stack handoff: %d of 32 extents admitted, last code %d, frames kept %d;"
                  " control %d, kill %d",
                  static_cast<int>(w[0]), static_cast<int>(w[1]), static_cast<int>(w[3]),
                  static_cast<int>(w[4]), static_cast<int>(w[5]));
        TAP_CHECK(ps_guard_faulted(run));
        TAP_CHECK(w[0] == 0 and w[1] != 0);
        TAP_CHECK(w[2] == 1);
        TAP_CHECK(w[3] == 1);
        TAP_CHECK(w[4] == 0 and w[5] == 0);
    }

    // A MAPPING IS A HOLDER. Without that the last capability's drop frees frames a live leaf
    // still points at, and reading the page does not say so.
    void t_cap_map_pins_run()
    {
        TAP_ASK(.workers = 1, .tasks = 1, .sems = 1, .endpoints = 1);
        uint32_t const free0 = mem_count(KOS_MEM_FRAMES_FREE);
        uint32_t const spaces0 = mem_count(KOS_MEM_SPACES_HELD);
        TAP_CHECK(free0 != MEM_UNREAD and spaces0 != MEM_UNREAD);
        kos_cap_t fcap = KOS_CAP_NONE;
        kos_cap_t gate = KOS_CAP_NONE;
        kos_cap_t ep = KOS_CAP_NONE;
        kos_task_t t = KOS_TASK_NONE;
        ArmHold hold;
        TAP_HOLD(hold.cap(&fcap) and hold.cap(&gate) and hold.cap(&ep) and hold.task(&t));
        TAP_CHECK(kos_frame_create(ASPACE_GRANULE, &fcap) == 0);
        TAP_CHECK(mem_count(KOS_MEM_FRAMES_FREE) == free0 - 1u);
        TAP_CHECK(kos_sem_create(0, &gate) == 0 and kos_endpoint_create(&ep) == 0);
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &t) == 0);
        kos_cap_grant caps[] = {{ep, KOS_CAP_SIGNAL}, {fcap, KOS_CAP_TRANSFER}, {gate, CH_FULL}};
        TAP_CHECK(kos::thread::create_caps(pin_child, nullptr, "pin", 10, caps, 3,
                                           KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                           KOS_AUTH_MEMORY, nullptr, t)
                      .valid());
        int32_t child_mapped = -1;
        TAP_CHECK(report_await(ep, &child_mapped, sizeof(child_mapped)));
        TAP_CHECK(child_mapped == 0);
        // main drops its capability too: from here NO capability names the run, and only the
        // child's MAPPING holds it.
        uint32_t const held = mem_count(KOS_MEM_FRAMES_FREE);
        TAP_CHECK(hold.close(&fcap) == 0);
        uint32_t const closed = mem_count(KOS_MEM_FRAMES_FREE);
        TAP_CHECK(task_end(&t) == 0);
        uint32_t const dead = mem_count(KOS_MEM_FRAMES_FREE);
        tap::diag("cap pin: %u frames free with the child mapping, %u once main closed, %u at"
                  " its death (%u before)",
                  static_cast<unsigned>(held), static_cast<unsigned>(closed),
                  static_cast<unsigned>(dead), static_cast<unsigned>(free0));
        TAP_CHECK(closed == held);
        TAP_CHECK(mem_count(KOS_MEM_SPACES_HELD) == spaces0);
        TAP_CHECK(dead == free0);
    }

    void t_cap_share()
    {
        TAP_ASK(.workers = 1, .tasks = 1);
        uint32_t const spaces0 = mem_count(KOS_MEM_SPACES_HELD);
        TAP_CHECK(spaces0 != MEM_UNREAD);
        Seed s;
        Seed elsewhere;
        uintptr_t va = 0;
        kos_task_t t = KOS_TASK_NONE;
        kos_task_t t2 = KOS_TASK_NONE;
        ArmHold hold;
        TAP_HOLD(hold.mapped(&s.frame, &s.space, &va) and hold.cap(&elsewhere.frame)
                  and hold.cap(&elsewhere.space) and hold.task(&t) and hold.task(&t2));
        TAP_CHECK(seed_mint(ASPACE_GRANULE, &s) and seed_mint(ASPACE_GRANULE, &elsewhere));
        // Held to the end, so its own address is free in every space the children make.
        void* const theirs_at = reinterpret_cast<void*>(seed_va(elsewhere));
        TAP_CHECK(theirs_at != nullptr);
        TAP_CHECK(kos_frame_map(s.frame, s.space, &va, 0) == 0 and va != 0);
        volatile uint32_t* const mine = reinterpret_cast<volatile uint32_t*>(va);
        mine[0] = SHARE_A;
        mine[1] = 0;
        mine[2] = 0;
        mine[3] = 0;
        uint32_t const mapped = mem_count(KOS_MEM_FRAMES_FREE);

        // A task of its OWN, carrying no grant: a null mem_base skips the handoff.
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &t) == 0);
        // Only TRANSFER is valid; neither capability type carries access rights.
        kos_cap_grant caps[] = {{s.frame, KOS_CAP_TRANSFER}};
        // The negative control below omits this authority.
        auto const child = kos::thread::create_caps(share_child, theirs_at, "shr", 10, caps, 1,
                                                    KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                                    KOS_AUTH_MEMORY, nullptr, t);
        TAP_CHECK(child.valid() and child.join(STALL_TOLERANT_US) == 0);
        uintptr_t const theirs = static_cast<uintptr_t>(mine[2])
                                 | (static_cast<uintptr_t>(mine[3]) << 32);
        tap::diag("cap share: main 0x%lx, peer 0x%lx", static_cast<unsigned long>(va),
                  static_cast<unsigned long>(theirs));
        TAP_CHECK(theirs == reinterpret_cast<uintptr_t>(theirs_at) and theirs != va);
        TAP_CHECK(mine[1] == SHARE_B);

        // The borrower dies while main still maps the run: the run belongs to the CAPABILITY,
        // so the space that mapped it owned nothing to free. Everything else the borrower took
        // comes back; only the pool count sees an early free, a stale mapping still reading
        // back.
        TAP_CHECK(task_end(&t) == 0);
        TAP_CHECK(mem_count(KOS_MEM_SPACES_HELD) == spaces0);
        TAP_CHECK(mem_count(KOS_MEM_FRAMES_FREE) == mapped);
        mine[0] = SHARE_A + 1u;
        TAP_CHECK(mine[0] == SHARE_A + 1u);

        // An identical child with NO authority cannot map the same delegated capability:
        // possession of the frame is not permission to map it.
        mine[2] = 0;
        mine[3] = 0;
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &t2) == 0);
        auto const unauth = kos::thread::create_caps(share_child, theirs_at, "shrN", 10, caps, 1,
                                                     KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                                     /*authority=*/0, nullptr, t2);
        TAP_CHECK(unauth.valid() and unauth.join(STALL_TOLERANT_US) == 0);
        TAP_CHECK(mine[2] == 0); // it reached the map and was refused
        TAP_CHECK(task_end(&t2) == 0);
        TAP_CHECK(mem_count(KOS_MEM_SPACES_HELD) == spaces0);

        TAP_CHECK(kos_frame_unmap(s.frame, s.space, va) == 0);
        va = 0;
        seed_close(&s);
        // The frames come back only once the LAST capability naming the run is gone.
        TAP_CHECK(mem_count(KOS_MEM_FRAMES_FREE) == mapped + 1u);
        seed_close(&elsewhere);
    }

    // --- A minted run, mapped where the kernel chooses, balances the memory counts ----
    constexpr size_t MINT_BYTES = 4096;
    constexpr uint32_t MINT_PATTERN = 0x5EEDF00Du;

    void t_frame_mint_census()
    {
        uint32_t free0 = 0;
        uint32_t ranges0 = 0;
        TAP_CHECK(kos_mem_count(KOS_MEM_FRAMES_FREE, &free0) == 0);
        TAP_CHECK(kos_mem_count(KOS_MEM_RANGES_FREE, &ranges0) == 0);
        uintptr_t va = 0;
        uint32_t free1 = 0;
        uint32_t ranges1 = 0;
        bool zero = true;
        bool kept = true;
        int ranges1_rc = -1;
        int unmapped = -1;
        {
            auto run = kos::Frame::create(MINT_BYTES);
            auto space = kos::Space::self();
            TAP_CHECK(run.valid() and space.valid());
            TAP_CHECK(kos_mem_count(KOS_MEM_FRAMES_FREE, &free1) == 0);
            TAP_CHECK(run.map(space.handle(), &va) == 0);
            // No return until the unmap: the run's close would leave the mapping holding it.
            ranges1_rc = kos_mem_count(KOS_MEM_RANGES_FREE, &ranges1);
            volatile uint32_t* const words = reinterpret_cast<volatile uint32_t*>(va);
            for (uint32_t i = 0; i < MINT_BYTES / sizeof(uint32_t); i++)
            {
                if (words[i] != 0u)
                {
                    zero = false;
                }
                words[i] = MINT_PATTERN ^ i;
            }
            for (uint32_t i = 0; i < MINT_BYTES / sizeof(uint32_t); i++)
            {
                if (words[i] != (MINT_PATTERN ^ i))
                {
                    kept = false;
                }
            }
            unmapped = run.unmap(space.handle(), va);
        }
        uint32_t free2 = 0;
        uint32_t ranges2 = 0;
        TAP_CHECK(kos_mem_count(KOS_MEM_FRAMES_FREE, &free2) == 0);
        TAP_CHECK(kos_mem_count(KOS_MEM_RANGES_FREE, &ranges2) == 0);
        tap::diag("frame mint: %u frames free, %u with the run, mapped at 0x%lx",
                  static_cast<unsigned>(free0), static_cast<unsigned>(free1),
                  static_cast<unsigned long>(va));
        TAP_CHECK(unmapped == 0 and ranges1_rc == 0);
        TAP_CHECK(va != 0 and va % MINT_BYTES == 0);
        TAP_CHECK(free1 < free0);
        TAP_CHECK(zero);
        TAP_CHECK(kept);
        TAP_CHECK(ranges1 == ranges0 - 1u);
        TAP_CHECK(free2 == free0);
        TAP_CHECK(ranges2 == ranges0);
    }

    // --- A space capability names nothing once its task has ended --------------------
    // The child, entry of a task main created, hands its own space to a grandchild in a task
    // of the child's making, which outlives it and maps through that space before and after.
    // Its task outlives the one that made it, so nobody can slay it: main posts its gate on every
    // path once it may exist, and its other waits bound themselves.
    void sd_grandchild(void*) // caps: report E(SIGNAL)@1, gate@2, run@3, space@4, held@5
    {
        uintptr_t at = 0;
        int32_t rep = kos_frame_map(3, 4, &at, 0);
        if (rep == 0)
        {
            rep = kos_frame_unmap(3, 4, at);
        }
        (void)kos_sem_post(5);
        (void)kos_send_timed(1, &rep, sizeof(rep), STALL_TOLERANT_US);
        (void)kos_sem_wait(2, KOS_TIMEOUT_NONE);
        at = 0;
        rep = kos_frame_map(3, 4, &at, 0);
        (void)kos_handle_close(3);
        (void)kos_handle_close(4);
        // A call, so main counts the run's frames while this thread is parked and not exiting.
        (void)kos_call_timed(1, &rep, sizeof(rep), sizeof(rep), STALL_TOLERANT_US);
    }

    void sd_child(void*) // caps: report E(SIGNAL)@1, gate@2, run@3
    {
        kos_cap_t space = KOS_CAP_NONE;
        kos_cap_t held = KOS_CAP_NONE;
        kos_task_t t = KOS_TASK_NONE;
        if (kos_aspace_self(&space) != 0 or kos_sem_create(0, &held) != 0
            or kos_task_create(nullptr, 0, 0, &t) != 0)
        {
            return;
        }
        kos_cap_grant caps[] = {{1, KOS_CAP_SIGNAL},
                                {2, CH_FULL},
                                {3, KOS_CAP_TRANSFER},
                                {space, KOS_CAP_TRANSFER},
                                {held, CH_FULL}};
        if (kos::thread::create_caps(sd_grandchild, nullptr, "sdg", 10, caps, 5,
                                     KOS_POLICY_FIFO, 0, false, nullptr, 0, KOS_AUTH_MEMORY,
                                     nullptr, t)
                .valid())
        {
            (void)kos_sem_wait(held, KOS_TIMEOUT_NONE);
        }
    }

    // Woken by the creator's watch of `t`, which is raised at its end and again at its death.
    bool task_dead(kos_task_t t, kos_cap_t note)
    {
        uint64_t const deadline = kos_clock_now() + uint64_t{STALL_TOLERANT_US} * 1000u;
        while (true)
        {
            int const state = kos_task_state(t);
            if (state >= 0 and (state & KOS_TASK_DEAD) != 0)
            {
                return true;
            }
            uint64_t const now = kos_clock_now();
            if (state < 0 or now >= deadline)
            {
                return false;
            }
            uint32_t bits = 0;
            (void)kos_notify_wait(note, ~0u, static_cast<uint32_t>((deadline - now) / 1000u),
                                  &bits);
        }
    }

    void t_space_cap_dies_with_task()
    {
        TAP_ASK(.workers = 2, .tasks = 2, .sems = 1, .endpoints = 1, .notifies = 1);
        uint32_t const spaces0 = mem_count(KOS_MEM_SPACES_HELD);
        kos_cap_t ep = KOS_CAP_NONE;
        kos_cap_t gate = KOS_CAP_NONE;
        kos_cap_t run = KOS_CAP_NONE;
        kos_cap_t note = KOS_CAP_NONE;
        kos_task_t t = KOS_TASK_NONE;
        ArmHold hold;
        TAP_HOLD(hold.cap(&ep) and hold.cap(&gate) and hold.cap(&run) and hold.bound(&note)
                  and hold.task(&t));
        TAP_CHECK(kos_endpoint_create(&ep) == 0);
        TAP_CHECK(kos_sem_create(0, &gate) == 0);
        TAP_CHECK(kos_frame_create(MINT_BYTES, &run) == 0);
        TAP_CHECK(kos_notify_create(&note) == 0 and kos_notify_bind(note) == 0);
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &t) == 0);
        TAP_CHECK(kos_task_watch(t, note, KOS_CAP_NONE) == 0);
        kos_cap_grant caps[] = {{ep, KOS_CAP_SIGNAL | KOS_CAP_TRANSFER},
                                {gate, CH_FULL},
                                {run, KOS_CAP_TRANSFER}};
        TAP_CHECK(kos::thread::create_caps(sd_child, nullptr, "sdc", 10, caps, 3,
                                           KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                           KOS_AUTH_MEMORY | KOS_AUTH_TASKS, nullptr, t)
                      .valid());
        // From here no return until the grandchild is released.
        int32_t alive = 1;
        int32_t dead = 1;
        bool const heard_alive = report_await(ep, &alive, sizeof(alive));
        // The task stays held by main, so its domain outlives its death.
        bool const ended = heard_alive and task_dead(t, note);
        (void)kos_sem_post(gate);
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, ep, 0, STALL_TOLERANT_US);
        bool const heard_dead =
            ended
            and kos_reply_recv(KOS_CAP_NONE, &dead, kos_call_lens_pack(0, sizeof(dead)), &opts)
                    == static_cast<int32_t>(sizeof(dead));
        // main's capability is the run's last holder while the grandchild waits in its call.
        uint32_t const held = mem_count(KOS_MEM_FRAMES_FREE);
        (void)hold.close(&run);
        uint32_t const closed = mem_count(KOS_MEM_FRAMES_FREE);
        if (heard_dead)
        {
            (void)kos_reply(opts.info.reply_cap, &dead, sizeof(dead));
        }
        TAP_CHECK(task_end(&t) == 0);
        // The grandchild's task, and the dead task's domain it held, go once it has its answer.
        TAP_CHECK(spaces_back(spaces0));
        tap::diag("space capability: %d while its task lived, %d once it was dead",
                  static_cast<int>(alive), static_cast<int>(dead));
        TAP_CHECK(heard_alive and alive == 0);
        TAP_CHECK(ended and heard_dead);
        TAP_CHECK(dead == -KOS_EBADF);
        TAP_CHECK(held != MEM_UNREAD and closed == held + MINT_BYTES / ASPACE_GRANULE);
    }

    // The build's granule is the map's: a run of one granule takes one frame and a byte more
    // takes two. A self-grant honours the non-cacheable type and refuses a type with no name.
    void t_aspace_seam()
    {
        static_assert((ASPACE_GRANULE & (ASPACE_GRANULE - 1u)) == 0, "a granule is a power of two");
        uint32_t const f0 = mem_count(KOS_MEM_FRAMES_FREE);
        kos_cap_t one = KOS_CAP_NONE;
        kos_cap_t two = KOS_CAP_NONE;
        ArmHold hold;
        TAP_HOLD(hold.cap(&one) and hold.cap(&two));
        TAP_CHECK(kos_frame_create(ASPACE_GRANULE, &one) == 0);
        uint32_t const f1 = mem_count(KOS_MEM_FRAMES_FREE);
        TAP_CHECK(kos_frame_create(ASPACE_GRANULE + 1u, &two) == 0);
        uint32_t const f2 = mem_count(KOS_MEM_FRAMES_FREE);
        (void)hold.close(&one);
        (void)hold.close(&two);
        uint32_t const f3 = mem_count(KOS_MEM_FRAMES_FREE);
        tap::diag("aspace: granule %u, frames free %u, %u with one granule, %u with one more"
                  " byte, %u after",
                  static_cast<unsigned>(ASPACE_GRANULE), static_cast<unsigned>(f0),
                  static_cast<unsigned>(f1), static_cast<unsigned>(f2),
                  static_cast<unsigned>(f3));
        TAP_CHECK(f0 != MEM_UNREAD and f0 != 0);
        TAP_CHECK(f0 - f1 == 1u and f1 - f2 == 2u and f3 == f0);
        void* const blk = st_ram<SEAM_RAM, 0>();
        TAP_CHECK(blk != nullptr);
        TAP_CHECK(kos_mem_self_grant(blk, SNP_BLK, 0) == 0);
        TAP_CHECK(kos_mem_self_grant(blk, SNP_BLK, KOS_MEM_NOCACHE) == 0);
        TAP_CHECK(kos_mem_self_grant(blk, SNP_BLK, KOS_MEM_NOCACHE << 1) == -KOS_EINVAL);
        TAP_CHECK(kos_mem_self_grant(blk, SNP_BLK, 0) == 0);
    }

    // One pool balance across task creation and teardown.
    // Minimum expected cost: one root, two image tables and one private data page.
    constexpr uint32_t CHURN_MIN_FRAMES = 4;

    void churn_member(void*)
    {
        kos_exit(0);
    }

    // `held` is read with the space ALIVE and before the member starts, so it is the space's own
    // cost and races with nothing.
    bool churn_cycle(uint32_t* held)
    {
        kos_task_t t = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &t) != 0)
        {
            return false;
        }
        if (held != nullptr)
        {
            *held = mem_count(KOS_MEM_FRAMES_FREE);
        }
        auto const m = kos::thread::create_caps(churn_member, nullptr, "chrn", 10, nullptr, 0,
                                               KOS_POLICY_FIFO, 0, false, nullptr, 0, 0,
                                               nullptr, t);
        bool const joined = m.valid() and m.join(STALL_TOLERANT_US) == 0;
        // Drop the creator hold before checking reclamation; thread exit leaves it live.
        bool const reaped = task_end(&t) == 0;
        return joined and reaped;
    }

    void t_aspace_churn()
    {
        TAP_ASK(.workers = 1, .tasks = 1);
        // Warm up first: initial mappings allocate intermediate tables retained for reuse.
        TAP_CHECK(churn_cycle(nullptr));
        uint32_t const frames0 = mem_count(KOS_MEM_FRAMES_FREE);
        uint32_t const roots0 = mem_count(KOS_MEM_SPACES_HELD);
        uint32_t low = frames0;
        for (int i = 0; i < 4; i++)
        {
            uint32_t held = frames0;
            TAP_CHECK(churn_cycle(&held));
            if (held < low)
            {
                low = held;
            }
        }
        uint32_t const frames1 = mem_count(KOS_MEM_FRAMES_FREE);
        uint32_t const roots1 = mem_count(KOS_MEM_SPACES_HELD);
        tap::diag("churn: %u frames free, %u with a process live, %u after; roots %u then %u",
                  static_cast<unsigned>(frames0), static_cast<unsigned>(low),
                  static_cast<unsigned>(frames1), static_cast<unsigned>(roots0),
                  static_cast<unsigned>(roots1));
        TAP_CHECK(frames0 != MEM_UNREAD and roots0 != MEM_UNREAD);
        // Require a live-process allocation to exclude a trivially balanced empty cycle.
        TAP_CHECK(low + CHURN_MIN_FRAMES <= frames0);
        TAP_CHECK(frames1 == frames0);
        TAP_CHECK(roots1 == roots0);
    }

    void sframe_worker(void*) // caps: E(SIGNAL)@1, gate@2
    {
        reached(1);
        kos_sem_wait(2, KOS_TIMEOUT_NONE); // held alive while main reads the pool
    }
    void t_stack_is_frames()
    {
        TAP_ASK(.workers = 1, .sems = 1, .endpoints = 1);
        kos_cap_t ep = KOS_CAP_NONE;
        kos_cap_t gate = KOS_CAP_NONE;
        kos::thread::Handle w;
        ArmHold hold;
        TAP_HOLD(hold.cap(&ep) and hold.cap(&gate) and hold.thread(&w));
        TAP_CHECK(kos_endpoint_create(&ep) == 0 and kos_sem_create(0, &gate) == 0);
        kos_cap_grant caps[] = {{ep, KOS_CAP_SIGNAL}, {gate, CH_FULL}};
        uint32_t before = 0;
        uint32_t live = 0;
        // The first cycle warms up: intermediate tables are retained after the first mapping.
        for (int i = 0; i < 2; i++)
        {
            before = mem_count(KOS_MEM_FRAMES_FREE);
            w = kos::thread::create_caps(sframe_worker, nullptr, "sfram", 10, caps, 2);
            TAP_CHECK(w.valid());
            TAP_CHECK(heard_reached(ep));
            live = mem_count(KOS_MEM_FRAMES_FREE);
            kos_sem_post(gate);
            TAP_CHECK(thread_end(&w) == 0);
        }
        TAP_CHECK(before != MEM_UNREAD);
        // At least the stack and the unmapped page below it.
        TAP_CHECK(before >= live + 2u);
        TAP_CHECK(mem_count(KOS_MEM_FRAMES_FREE) == before);
        tap::diag("stack frames: %u held by one live thread",
                  static_cast<unsigned>(before - live));
    }

    // Two threads given one grant, created before either runs, each write the same global and
    // read it back once both have: a shared space would hand one of them the other's value.
    enum
    {
        SP_SEED = 0,  // main's, read by the worker: the value only that worker writes
        SP_READ = 1,  // what the worker read back
        SP_WORDS = 2
    };
    volatile uint32_t g_sp_word = 0u;
    constexpr uint32_t SP_MAIN = 0x5BACEu;
    constexpr uint32_t SP_A = 0xA11CEu;
    constexpr uint32_t SP_B = 0xB0B0Bu;
    void space_word_worker(void* arg) // caps: E(SIGNAL)@1, gate@2
    {
        volatile uint32_t* const slot = static_cast<volatile uint32_t*>(arg);
        g_sp_word = slot[SP_SEED];
        reached(1);
        kos_sem_wait(2, KOS_TIMEOUT_NONE); // both have written by now
        slot[SP_READ] = g_sp_word;
    }

    void t_aspace_two_spaces_same_grant()
    {
        // A spawn carrying a grant builds a task of its own.
        TAP_ASK(.workers = 2, .tasks = 2, .sems = 1, .endpoints = 1);
        constexpr uint32_t SP_BLK = SP_RAM.size[0];
        void* const shared = st_ram<SP_RAM, 0>();
        TAP_CHECK(shared != nullptr);
        // main maps the block too: the pair answers through it.
        TAP_CHECK(kos_mem_self_grant(shared, SP_BLK, 0) == 0);
        volatile uint32_t* const slot = static_cast<volatile uint32_t*>(shared);
        slot[SP_SEED] = SP_A;
        slot[SP_READ] = 0;
        slot[SP_WORDS + SP_SEED] = SP_B;
        slot[SP_WORDS + SP_READ] = 0;
        g_sp_word = SP_MAIN;
        kos_cap_t ep = KOS_CAP_NONE;
        kos_cap_t gate = KOS_CAP_NONE;
        kos::thread::Handle w[2];
        ArmHold hold;
        TAP_HOLD(hold.cap(&ep) and hold.cap(&gate) and hold.thread(&w[0])
                  and hold.thread(&w[1]));
        TAP_CHECK(kos_endpoint_create(&ep) == 0 and kos_sem_create(0, &gate) == 0);
        kos_cap_grant caps[] = {{ep, KOS_CAP_SIGNAL}, {gate, CH_FULL}};
        for (int i = 0; i < 2; i++)
        {
            w[i] = kos::thread::create_caps(space_word_worker,
                                            const_cast<uint32_t*>(&slot[i * SP_WORDS]), "spw",
                                            10, caps, 2, KOS_POLICY_FIFO, 0, false, shared,
                                            SP_BLK);
            TAP_CHECK(w[i].valid());
        }
        TAP_CHECK(heard_reached(ep) and heard_reached(ep));
        kos_sem_post(gate);
        kos_sem_post(gate);
        TAP_CHECK(thread_end(&w[0]) == 0 and thread_end(&w[1]) == 0);
        tap::diag("one grant, two spaces: read back 0x%x and 0x%x, main 0x%x",
                  static_cast<unsigned>(slot[SP_READ]),
                  static_cast<unsigned>(slot[SP_WORDS + SP_READ]),
                  static_cast<unsigned>(g_sp_word));
        TAP_CHECK(slot[SP_READ] == SP_A and slot[SP_WORDS + SP_READ] == SP_B);
        TAP_CHECK(g_sp_word == SP_MAIN);
    }

    // One virtual address, a private copy behind it in each process. Each worker reports
    // through its own block; private globals cannot report to main.
    enum
    {
        PW_ADDR = 0, // the member's own &g_pw_word
        PW_READBACK = 1,
        PW_SEED = 2, // main's, read by the member: the value only that member writes
        PW_WORDS = 3
    };
    volatile uint32_t g_pw_word = 0u;
    void pw_member(void* arg) // caps: E(SIGNAL)@1, gate@2
    {
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(arg);
        uint32_t const mine = static_cast<uint32_t>(out[PW_SEED]);
        out[PW_ADDR] = reinterpret_cast<uintptr_t>(&g_pw_word);
        g_pw_word = mine;
        reached(1);
        kos_sem_wait(2, KOS_TIMEOUT_NONE); // both members have written by now
        out[PW_READBACK] = g_pw_word;
    }
    void t_process_private_data()
    {
        TAP_ASK(.workers = 2, .tasks = 2, .sems = 1, .endpoints = 1);
        constexpr uint32_t PW_BLK = PW_RAM.size[0];
        constexpr uint32_t PW_A = 0xA5A50F0Fu;
        constexpr uint32_t PW_B = 0x5A5AF0F0u;
        void* const ba = st_ram<PW_RAM, 0>();
        void* const bb = st_ram<PW_RAM, 1>();
        TAP_CHECK(ba != nullptr and bb != nullptr);
        TAP_CHECK(kos_mem_self_grant(ba, PW_BLK, 0) == 0);
        TAP_CHECK(kos_mem_self_grant(bb, PW_BLK, 0) == 0);
        volatile uint64_t* const oa = static_cast<volatile uint64_t*>(ba);
        volatile uint64_t* const ob = static_cast<volatile uint64_t*>(bb);
        for (int i = 0; i < PW_WORDS; i++)
        {
            oa[i] = 0;
            ob[i] = 0;
        }
        oa[PW_SEED] = PW_A;
        ob[PW_SEED] = PW_B;
        g_pw_word = 0x600Du;
        kos_cap_t ep = KOS_CAP_NONE;
        kos_cap_t gate = KOS_CAP_NONE;
        kos_task_t ta = KOS_TASK_NONE;
        kos_task_t tb = KOS_TASK_NONE;
        ArmHold hold;
        TAP_HOLD(hold.cap(&ep) and hold.cap(&gate) and hold.task(&ta) and hold.task(&tb));
        TAP_CHECK(kos_endpoint_create(&ep) == 0 and kos_sem_create(0, &gate) == 0);
        TAP_CHECK(kos_task_create(ba, PW_BLK, 0, &ta) == 0
                  and kos_task_create(bb, PW_BLK, 0, &tb) == 0);
        kos_cap_grant caps[] = {{ep, KOS_CAP_SIGNAL}, {gate, CH_FULL}};
        auto const pa = kos::thread::create_caps(pw_member, ba, "pwA", 10, caps, 2,
                                                 KOS_POLICY_FIFO, 0, false, nullptr, 0, 0,
                                                 nullptr, ta);
        TAP_CHECK(pa.valid());
        auto const pb = kos::thread::create_caps(pw_member, bb, "pwB", 10, caps, 2,
                                                 KOS_POLICY_FIFO, 0, false, nullptr, 0, 0,
                                                 nullptr, tb);
        TAP_CHECK(pb.valid());
        TAP_CHECK(heard_reached(ep) and heard_reached(ep)); // both have written their own copy
        kos_sem_post(gate);
        kos_sem_post(gate);
        // Both have read it back.
        TAP_CHECK(pa.join(STALL_TOLERANT_US) == 0 and pb.join(STALL_TOLERANT_US) == 0);
        TAP_CHECK(task_end(&ta) == 0 and task_end(&tb) == 0);
        uintptr_t const own = reinterpret_cast<uintptr_t>(&g_pw_word);
        tap::diag("process: read back 0x%x and 0x%x at 0x%lx, main 0x%x",
                  static_cast<unsigned>(oa[PW_READBACK]), static_cast<unsigned>(ob[PW_READBACK]),
                  static_cast<unsigned long>(own), static_cast<unsigned>(g_pw_word));
        TAP_CHECK(oa[PW_ADDR] == own and ob[PW_ADDR] == own);
        // Each read back its OWN write, after the other had written the same address.
        TAP_CHECK(oa[PW_READBACK] == PW_A and ob[PW_READBACK] == PW_B);
        TAP_CHECK(g_pw_word == 0x600Du); // and main's copy was untouched by either
    }

    // Two task siblings share one image. The writer, the task's entry, holds the task open
    // until the reader has read: its exit ends the task.
    volatile uint32_t g_sib_word = 0u;
    void sib_writer(void* arg) // caps: E(SIGNAL)@1, park@2
    {
        g_sib_word = 0xBEEFu;
        static_cast<volatile uint64_t*>(arg)[0] = 1u;
        reached(1);
        kos_sem_wait(2, KOS_TIMEOUT_NONE);
    }
    void sib_reader(void* arg)
    {
        static_cast<volatile uint64_t*>(arg)[1] = g_sib_word;
    }
    void t_task_siblings_share()
    {
        TAP_ASK(.workers = 2, .tasks = 1, .sems = 1, .endpoints = 1);
        constexpr uint32_t SIB_BLK = SIB_RAM.size[0];
        void* const blk = st_ram<SIB_RAM, 0>();
        TAP_CHECK(blk != nullptr);
        TAP_CHECK(kos_mem_self_grant(blk, SIB_BLK, 0) == 0);
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(blk);
        out[0] = 0;
        out[1] = 0;
        g_sib_word = 0u;
        kos_task_t t = KOS_TASK_NONE;
        kos_cap_t park = KOS_CAP_NONE;
        kos_cap_t ep = KOS_CAP_NONE;
        ArmHold hold;
        TAP_HOLD(hold.task(&t) and hold.cap(&park) and hold.cap(&ep));
        TAP_CHECK(kos_task_create(blk, SIB_BLK, 0, &t) == 0);
        TAP_CHECK(kos_sem_create(0, &park) == 0 and kos_endpoint_create(&ep) == 0);
        kos_cap_grant caps[] = {{ep, KOS_CAP_SIGNAL}, {park, KOS_CAP_WAIT}};
        auto const writer = kos::thread::create_caps(sib_writer, blk, "sibW", 10, caps, 2,
                                                     KOS_POLICY_FIFO, 0, false, nullptr, 0, 0,
                                                     nullptr, t);
        TAP_CHECK(writer.valid());
        // The reader starts once the writer has written.
        TAP_CHECK(heard_reached(ep));
        auto const reader = kos::thread::create_caps(sib_reader, blk, "sibR", 10, nullptr, 0,
                                                     KOS_POLICY_FIFO, 0, false, nullptr, 0, 0,
                                                     nullptr, t);
        TAP_CHECK(reader.valid() and reader.join(STALL_TOLERANT_US) == 0);
        kos_sem_post(park);
        TAP_CHECK(writer.join(STALL_TOLERANT_US) == 0);
        TAP_CHECK(task_end(&t) == 0);
        TAP_CHECK(out[0] == 1u);          // the writer ran
        TAP_CHECK(out[1] == 0xBEEFu);     // and its sibling saw the store: one image, one group
        TAP_CHECK(g_sib_word == 0u);      // main did not: a different group is a different copy
    }

    // Handoff at the same address, through both task creation and a grant-carrying spawn: the
    // borrower reads what main wrote and its echo reaches main through the same bytes, so one
    // frame sits under both spaces.
    enum
    {
        HO_SEEN = 0,
        HO_ADDR = 1,
        HO_WORDS = 2
    };
    constexpr uint64_t HO_SENTINEL = 0x0D15EA5Eu;
    void ho_reader(void* arg)
    {
        volatile uint64_t* const blk = static_cast<volatile uint64_t*>(arg);
        uint64_t const seen = blk[HO_SEEN];
        blk[HO_ADDR] = reinterpret_cast<uintptr_t>(arg);
        blk[HO_SEEN] = seen + 1u; // the readback, echoed back through the same bytes
    }
    void t_task_handoff_readback()
    {
        TAP_ASK(.workers = 1, .tasks = 1);
        constexpr uint32_t HO_BLK = HO_RAM.size[0];
        void* const blk = st_ram<HO_RAM, 0>();
        TAP_CHECK(blk != nullptr);
        // Reach it before writing it: allocation grants nothing.
        TAP_CHECK(kos_mem_self_grant(blk, HO_BLK, 0) == 0);
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(blk);
        out[HO_SEEN] = HO_SENTINEL;
        out[HO_ADDR] = 0;
        kos_task_t t = KOS_TASK_NONE;
        kos_task_t tt = KOS_TASK_NONE;
        kos::thread::Handle spawned;
        ArmHold hold;
        TAP_HOLD(hold.task(&t) and hold.task(&tt) and hold.thread(&spawned));
        // Path one: an explicit task create.
        TAP_CHECK(kos_task_create(blk, HO_BLK, 0, &t) == 0);
        auto const first = kos::thread::create_caps(ho_reader, blk, "hoT", 10, nullptr, 0,
                                                    KOS_POLICY_FIFO, 0, false, nullptr, 0, 0,
                                                    nullptr, t);
        TAP_CHECK(first.valid() and first.join(STALL_TOLERANT_US) == 0);
        TAP_CHECK(task_end(&t) == 0);
        uint64_t const first_seen = out[HO_SEEN];
        uint64_t const first_addr = out[HO_ADDR];
        out[HO_SEEN] = HO_SENTINEL;
        out[HO_ADDR] = 0;
        // Path two: the grant-carrying spawn, same range.
        spawned = kos::thread::create_caps(ho_reader, blk, "hoS", 10, nullptr, 0,
                                           KOS_POLICY_FIFO, 0, false, blk, HO_BLK);
        TAP_CHECK(spawned.valid());
        TAP_CHECK(thread_end(&spawned) == 0);
        TAP_CHECK(first_seen == HO_SENTINEL + 1u);   // the child read what main wrote
        TAP_CHECK(first_addr == reinterpret_cast<uintptr_t>(blk)); // at the address main named
        TAP_CHECK(out[HO_SEEN] == HO_SENTINEL + 1u);
        TAP_CHECK(out[HO_ADDR] == reinterpret_cast<uintptr_t>(blk));
        // A nondefault type on a separate block. Task creation only; the spawn grant ABI has no
        // memory-type field and passes zero.
        void* const typed = st_ram<HO_RAM, 1>();
        TAP_CHECK(kos_mem_self_grant(typed, HO_BLK, KOS_MEM_NOCACHE) == 0);
        volatile uint64_t* const tout = static_cast<volatile uint64_t*>(typed);
        tout[HO_SEEN] = HO_SENTINEL;
        tout[HO_ADDR] = 0;
        TAP_CHECK(kos_task_create(typed, HO_BLK, KOS_MEM_NOCACHE, &tt) == 0);
        auto const third = kos::thread::create_caps(ho_reader, typed, "hoN", 10, nullptr, 0,
                                                    KOS_POLICY_FIFO, 0, false, nullptr, 0, 0,
                                                    nullptr, tt);
        TAP_CHECK(third.valid() and third.join(STALL_TOLERANT_US) == 0);
        TAP_CHECK(task_end(&tt) == 0);
        TAP_CHECK(tout[HO_SEEN] == HO_SENTINEL + 1u);
        TAP_CHECK(tout[HO_ADDR] == reinterpret_cast<uintptr_t>(typed));
    }

    // An interior base in a two-page reservation is refused: accepting the whole reservation
    // would expose a donor page the caller did not name.
    enum
    {
        SL_GRAN = 0, // the granule, so the borrower can name the page below its own
        SL_ECHO = 1  // where the borrower reports what it read there
    };
    constexpr uint64_t SL_TOKEN = 0x5A17ED10u;

    // Reached only where the refusal did not hold: reads the page below the one handed over
    // and echoes it into the page that was.
    void sl_reader(void* arg)
    {
        volatile uint64_t* const mine = static_cast<volatile uint64_t*>(arg);
        uintptr_t const below =
            reinterpret_cast<uintptr_t>(arg) - static_cast<uintptr_t>(mine[SL_GRAN]);
        mine[SL_ECHO] = *reinterpret_cast<volatile uint64_t const*>(below);
    }

    void t_task_handoff_slice()
    {
        TAP_ASK(.tasks = 1);
        uint64_t const g = ASPACE_GRANULE;
        uint32_t const two = static_cast<uint32_t>(2u * g);
        void* const blk = st_ram<SL_RAM, 0>();
        TAP_CHECK(blk != nullptr);
        TAP_CHECK(kos_mem_self_grant(blk, two, 0) == 0);
        volatile uint64_t* const lower = static_cast<volatile uint64_t*>(blk);
        lower[0] = SL_TOKEN;
        void* const upper = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(blk) + g);
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(upper);
        out[SL_GRAN] = g;
        out[SL_ECHO] = 0;
        kos_task_t t = KOS_TASK_NONE;
        kos_task_t whole = KOS_TASK_NONE;
        ArmHold hold;
        TAP_HOLD(hold.task(&t) and hold.task(&whole));
        int const rc = kos_task_create(upper, static_cast<uint32_t>(g), 0, &t);
        if (rc == 0)
        {
            auto const rd = kos::thread::create(sl_reader, upper, "hsl", 10, KOS_POLICY_FIFO,
                                                0, false, nullptr, 0, nullptr, 0, nullptr, 0,
                                                nullptr, 0, 0, nullptr, t);
            if (rd.valid())
            {
                (void)rd.join(STALL_TOLERANT_US);
            }
            (void)task_end(&t);
            tap::diag("interior handoff admitted; the borrower read 0x%x below its page",
                      static_cast<unsigned>(out[SL_ECHO]));
        }
        TAP_CHECK(rc == -KOS_EPERM);
        // Control: accept the same reservation at its actual base.
        TAP_CHECK(kos_task_create(blk, two, 0, &whole) == 0);
        TAP_CHECK(task_end(&whole) == 0);
    }

    // The donor task exits before its borrower, then a churn task allocates and writes frames:
    // the borrower's data must survive on the domain lifetime reference. Pool counters alone
    // cannot detect early reuse. main cannot serve as the donor; it joins it before the churn.
    enum
    {
        DX_STATUS = 0, // 1 once the donor seated a borrower into its own block
        DX_ADDR = 1,   // the donor's address for that block
        DX_HELD = 2,   // spaces held while the donor was still alive
        DX_WORDS = 3
    };
    // A reservation sits at its frames' own address, so a churn block at the donor's address
    // landed on the donor's frame.
    enum
    {
        CX_TAKEN = 0, // blocks the churn task got
        CX_ADDR0 = 1, // one word per block: its address
        CX_MAX = 24
    };
    constexpr uint32_t DX_BLK = DX_RAM.size[0];
    constexpr uint32_t DX_CBLK = DX_RAM.size[1];
    constexpr uint64_t DX_DONOR_WORD = 0xD0D0D0D0D0D0D0D0ull;
    constexpr uint64_t DX_BORROW_WORD = 0xB0B0B0B0B0B0B0B0ull;
    constexpr uint64_t DX_CHURN_WORD = 0xC0C0C0C0C0C0C0C0ull;
    constexpr int DX_EP = 1; // the donor's and the borrower's endpoint index
    // Main's four bounded steps between the borrower's seating and its call (the donor's join
    // and slay, the churn's join and slay), and one more for the calls between them.
    constexpr uint32_t DX_BORROW_US = 5u * STALL_TOLERANT_US;
    struct DxReport
    {
        uint64_t seen;     // what the borrower found in the block
        uint64_t readback; // what it read after writing its own word over it
        uint64_t addr;
    };

    // Park until main has joined the donor and exhausted available frames with churn. Its task
    // is the donor's, which main cannot slay, so its wait bounds itself.
    void dx_borrower(void* arg) // caps: ep(WAIT)@1
    {
        volatile uint64_t* const blk = static_cast<volatile uint64_t*>(arg);
        char msg[sizeof(DxReport)] = {};
        struct kos_recv_info info = {0u, KOS_CAP_NONE};
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, DX_EP, 0, DX_BORROW_US);
        int32_t const got =
            kos_reply_recv(KOS_CAP_NONE, msg, kos_call_lens_pack(0, sizeof(msg)), &opts);
        info = opts.info;
        DxReport r = {};
        r.seen = blk[0];
        blk[0] = DX_BORROW_WORD; // and WRITES it: a stale mapping is writable, not only readable
        r.readback = blk[0];
        r.addr = reinterpret_cast<uintptr_t>(arg);
        if (got >= 0)
        {
            memcpy(msg, &r, sizeof(r));
            (void)kos_reply(info.reply_cap, msg, sizeof(msg));
        }
    }

    // main joins the donor instead of waiting for a post.
    void dx_donor(void* arg) // caps: ep(WAIT|TRANSFER)@1; arg is main's report block
    {
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(arg);
        void* const blk = st_ram_own(DX_BLK);
        if (blk == nullptr or kos_mem_self_grant(blk, DX_BLK, 0) != 0)
        {
            return;
        }
        volatile uint64_t* const mine = static_cast<volatile uint64_t*>(blk);
        mine[0] = DX_DONOR_WORD;
        out[DX_ADDR] = reinterpret_cast<uintptr_t>(blk);
        kos_task_t tb = KOS_TASK_NONE;
        if (kos_task_create(blk, DX_BLK, 0, &tb) != 0)
        {
            return;
        }
        kos_cap_grant caps[] = {{DX_EP, KOS_CAP_WAIT}};
        if (not kos::thread::create_caps(dx_borrower, blk, "dxB", 10, caps, 1, KOS_POLICY_FIFO,
                                         0, false, nullptr, 0, 0, nullptr, tb)
                    .valid())
        {
            // An empty task outlives its creator.
            (void)kos_task_kill(tb);
            return;
        }
        // Read last: main compares it after the join with no space allocated or freed between.
        out[DX_HELD] = mem_count(KOS_MEM_SPACES_HELD);
        out[DX_STATUS] = 1;
    }

    void dx_churn(void* arg)
    {
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(arg);
        size_t const g = ASPACE_GRANULE;
        uint64_t taken = 0;
        while (taken < static_cast<uint64_t>(CX_MAX))
        {
            void* const p = st_ram_own(g);
            if (p == nullptr or kos_mem_self_grant(p, static_cast<uint32_t>(g), 0) != 0)
            {
                break;
            }
            *static_cast<volatile uint64_t*>(p) = DX_CHURN_WORD;
            out[CX_ADDR0 + taken] = reinterpret_cast<uintptr_t>(p);
            taken++;
        }
        out[CX_TAKEN] = taken;
    }

    void t_task_handoff_donor_exits()
    {
        TAP_ASK(.workers = 2, .tasks = 2, .endpoints = 1);
        void* const dblk = st_ram<DX_RAM, 0>();
        void* const cblk = st_ram<DX_RAM, 1>();
        TAP_CHECK(dblk != nullptr and cblk != nullptr);
        if (kos_mem_self_grant(dblk, DX_BLK, 0) != 0
            or kos_mem_self_grant(cblk, DX_CBLK, 0) != 0)
        {
            tap::skip("the report blocks are not reachable");
            return;
        }
        volatile uint64_t* const dout = static_cast<volatile uint64_t*>(dblk);
        volatile uint64_t* const cout = static_cast<volatile uint64_t*>(cblk);
        for (int i = 0; i < DX_WORDS; i++)
        {
            dout[i] = 0;
        }
        for (int i = 0; i < CX_ADDR0 + CX_MAX; i++)
        {
            cout[i] = 0;
        }
        uint32_t const spaces0 = mem_count(KOS_MEM_SPACES_HELD);
        kos_cap_t ep = KOS_CAP_NONE;
        kos_task_t td = KOS_TASK_NONE;
        kos_task_t tc = KOS_TASK_NONE;
        ArmHold hold;
        TAP_HOLD(hold.cap(&ep) and hold.task(&td) and hold.task(&tc));
        TAP_CHECK(kos_endpoint_create(&ep) == 0);
        TAP_CHECK(kos_task_create(dblk, DX_BLK, 0, &td) == 0);
        // TRANSFER lets the donor delegate the endpoint to the borrower.
        kos_cap_grant dcaps[] = {{ep, static_cast<uint8_t>(KOS_CAP_WAIT | KOS_CAP_TRANSFER)}};
        // The donor seats its borrower in a task of its own, which is the task authority.
        auto const donor = kos::thread::create_caps(dx_donor, dblk, "dxD", 10, dcaps, 1,
                                                    KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                                    KOS_AUTH_MEMORY | KOS_AUTH_TASKS, nullptr,
                                                    td);
        TAP_CHECK(donor.valid());
        // The donor has exited before any frame churn begins. From here no return until the
        // borrower has been called: its task is no handle of main's.
        bool const donor_gone = donor.join(STALL_TOLERANT_US) == 0;
        // Drop main's creator hold before the churn, or it hides a missing borrower reference.
        bool const donor_reaped = task_end(&td) == 0;
        bool const seated = dout[DX_STATUS] == 1u;
        uint64_t const held_after = mem_count(KOS_MEM_SPACES_HELD);

        bool churn_ran = kos_task_create(cblk, DX_CBLK, 0, &tc) == 0;
        if (churn_ran)
        {
            auto const churn = kos::thread::create_caps(dx_churn, cblk, "dxC", 10, nullptr, 0,
                                                        KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                                        KOS_AUTH_MEMORY, nullptr, tc);
            churn_ran = churn.valid() and churn.join(STALL_TOLERANT_US) == 0;
        }
        bool const churn_reaped = task_end(&tc) == 0;
        uint64_t const churned = cout[CX_TAKEN];

        // Release and receive in one call.
        char msg[sizeof(DxReport)] = {};
        int32_t const n = kos_call_timed(ep, msg, sizeof(msg), sizeof(msg), STALL_TOLERANT_US);
        DxReport r = {};
        if (n == static_cast<int32_t>(sizeof(r)))
        {
            memcpy(&r, msg, sizeof(r));
        }
        (void)hold.close(&ep);
        // The borrower's task, and with it the donor's domain, go once it has answered.
        bool const released = spaces_back(spaces0);

        // No frame the pool handed the churn task may be the one the borrower still maps.
        bool handed_out = false;
        uint64_t clo = 0;
        uint64_t chi = 0;
        for (uint64_t i = 0; i < churned and i < static_cast<uint64_t>(CX_MAX); i++)
        {
            uint64_t const tok = cout[CX_ADDR0 + i];
            if (tok == dout[DX_ADDR])
            {
                handed_out = true;
            }
            if (clo == 0 or tok < clo)
            {
                clo = tok;
            }
            if (tok > chi)
            {
                chi = tok;
            }
        }
        // Report the churn range: blocks outside it do not test frame reuse.
        tap::diag("donor-exits: donor block 0x%lx, borrower at 0x%lx, spaces held %u -> %u",
                  static_cast<unsigned long>(dout[DX_ADDR]), static_cast<unsigned long>(r.addr),
                  static_cast<unsigned>(dout[DX_HELD]), static_cast<unsigned>(held_after));
        tap::diag("donor-exits: borrower read 0x%lx, churn took %u blocks over 0x%lx..0x%lx",
                  static_cast<unsigned long>(r.seen), static_cast<unsigned>(churned),
                  static_cast<unsigned long>(clo), static_cast<unsigned long>(chi));
        TAP_CHECK(donor_gone and donor_reaped and seated);
        TAP_CHECK(churn_ran and churn_reaped);
        // The donor's task has exited, but the borrower must still hold its domain.
        TAP_CHECK(held_after == dout[DX_HELD]);
        TAP_CHECK(n == static_cast<int32_t>(sizeof(r))); // the borrower answered at all
        TAP_CHECK(r.seen == DX_DONOR_WORD);
        TAP_CHECK(r.readback == DX_BORROW_WORD);
        TAP_CHECK(not handed_out);
        TAP_CHECK(released);
    }

    // An ungranted reservation has no leaves, so only aspace_release returns its frames. The
    // first cycle is a warm-up.
    enum
    {
        RT_ADDR = 0, // the reservation the member took and never mapped
        RT_FREE = 1, // frames free with it out, read INSIDE the live process
        RT_WORDS = 2
    };
    constexpr uintptr_t RT_PAGES = 3;
    void rt_member(void*) // caps: E(SIGNAL)@1
    {
        uint64_t rep[RT_WORDS] = {0, 0};
        uintptr_t const g = ASPACE_GRANULE;
        // Leave the reservation unmapped for teardown.
        void* const blk = st_ram_own(static_cast<size_t>(RT_PAGES * g));
        rep[RT_ADDR] = reinterpret_cast<uintptr_t>(blk);
        rep[RT_FREE] = mem_count(KOS_MEM_FRAMES_FREE);
        (void)kos_send(1, rep, sizeof(rep));
    }
    bool rt_cycle(kos_cap_t ep, uint64_t* rep)
    {
        kos_task_t t = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &t) != 0)
        {
            return false;
        }
        kos_cap_grant caps[] = {{ep, KOS_CAP_SIGNAL}};
        auto const m = kos::thread::create_caps(rt_member, nullptr, "rtres", 10, caps, 1,
                                                KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                                KOS_AUTH_MEMORY, nullptr, t);
        // The member parks in its send until this receive arrives, so the report is read
        // while the reservation is still out.
        bool const heard = m.valid() and report_await(ep, rep, sizeof(uint64_t) * RT_WORDS);
        bool const joined = m.valid() and m.join(STALL_TOLERANT_US) == 0;
        // main's creator hold keeps the empty task's space alive.
        bool const reaped = task_end(&t) == 0;
        return heard and joined and reaped;
    }
    void t_reservation_teardown()
    {
        TAP_ASK(.workers = 1, .tasks = 1, .endpoints = 1);
        kos_cap_t ep = KOS_CAP_NONE;
        ArmHold hold;
        TAP_HOLD(hold.cap(&ep));
        TAP_CHECK(kos_endpoint_create(&ep) == 0);
        uint64_t rep[RT_WORDS] = {0, 0};
        TAP_CHECK(rt_cycle(ep, rep));
        uint64_t const before = mem_count(KOS_MEM_FRAMES_FREE);
        bool const ran = rt_cycle(ep, rep);
        uint64_t const after = mem_count(KOS_MEM_FRAMES_FREE);
        TAP_CHECK(ran);
        TAP_CHECK(rep[RT_ADDR] != 0);
        tap::diag("reservation teardown: %u frames free, %u inside the live process, %u after",
                  static_cast<unsigned>(before), static_cast<unsigned>(rep[RT_FREE]),
                  static_cast<unsigned>(after));
        TAP_CHECK(before != MEM_UNREAD);
        // Pool usage must change, or an empty reservation passes.
        TAP_CHECK(rep[RT_FREE] + RT_PAGES <= before);
        TAP_CHECK(after == before);
    }

    // Frames a dead process filled must read zeroed when reallocated. The warm-up cycle
    // equalizes retained table costs. A reservation sits at its frames' own address, so one
    // address in two processes is one frame.
    enum
    {
        FS_FRAME = 0, // the reservation's address
        FS_HITS = 1,  // token words found BEFORE this process wrote any
        FS_WORDS = 2
    };
    constexpr uintptr_t FS_PAGES = 2;
    constexpr uint64_t FS_TOKEN = 0x5CB0BE5CB0BE5CB0ull;
    void fs_member(void*) // caps: E(SIGNAL)@1
    {
        uint64_t rep[FS_WORDS] = {0, 0};
        uintptr_t const g = ASPACE_GRANULE;
        size_t const bytes = static_cast<size_t>(FS_PAGES * g);
        void* const blk = st_ram_own(bytes);
        if (blk != nullptr and kos_mem_self_grant(blk, bytes, 0) == 0)
        {
            rep[FS_FRAME] = reinterpret_cast<uintptr_t>(blk);
            volatile uint64_t* const w = static_cast<volatile uint64_t*>(blk);
            size_t const words = bytes / sizeof(uint64_t);
            // READ BEFORE WRITE: what the previous holder of these frames left behind.
            for (size_t i = 0; i < words; i++)
            {
                if (w[i] == FS_TOKEN)
                {
                    rep[FS_HITS]++;
                }
            }
            for (size_t i = 0; i < words; i++)
            {
                w[i] = FS_TOKEN;
            }
        }
        (void)kos_send(1, rep, sizeof(rep));
    }
    bool fs_cycle(kos_cap_t ep, uint64_t* rep)
    {
        kos_task_t t = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &t) != 0)
        {
            return false;
        }
        kos_cap_grant caps[] = {{ep, KOS_CAP_SIGNAL}};
        auto const m = kos::thread::create_caps(fs_member, nullptr, "fscrb", 10, caps, 1,
                                                KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                                KOS_AUTH_MEMORY, nullptr, t);
        // The member parks in its send until this receive arrives, and the frames go back
        // only once the group is reaped below.
        bool const heard = m.valid() and report_await(ep, rep, sizeof(uint64_t) * FS_WORDS);
        bool const joined = m.valid() and m.join(STALL_TOLERANT_US) == 0;
        bool const reaped = task_end(&t) == 0;
        return heard and joined and reaped;
    }
    void t_frame_scrub_cross_task()
    {
        TAP_ASK(.workers = 1, .tasks = 1, .endpoints = 1);
        kos_cap_t ep = KOS_CAP_NONE;
        ArmHold hold;
        TAP_HOLD(hold.cap(&ep));
        TAP_CHECK(kos_endpoint_create(&ep) == 0);
        uint64_t warm[FS_WORDS] = {0, 0};
        uint64_t first[FS_WORDS] = {0, 0};
        uint64_t second[FS_WORDS] = {0, 0};
        TAP_CHECK(fs_cycle(ep, warm) and fs_cycle(ep, first) and fs_cycle(ep, second));
        tap::diag("frame scrub: blocks 0x%lx then 0x%lx, token words read %u then %u",
                  static_cast<unsigned long>(first[FS_FRAME]),
                  static_cast<unsigned long>(second[FS_FRAME]),
                  static_cast<unsigned>(first[FS_HITS]),
                  static_cast<unsigned>(second[FS_HITS]));
        TAP_CHECK(first[FS_FRAME] != 0);
        // Reuse of the first process's frames, or stale data would not be visible.
        TAP_CHECK(second[FS_FRAME] == first[FS_FRAME]);
        TAP_CHECK(second[FS_HITS] == 0);
    }

    // A spawn refused for thread slots after task/domain/space creation must release those
    // objects and the donor reference. A separate donor task, so its lifetime ends in this arm.
    constexpr int LR_PARK_CAP = 24;
    enum
    {
        LR_RC = 0,      // what the refused spawn answered, negated
        LR_PARKED = 1,  // workers seated before the pool refused
        LR_SPACES0 = 2, // spaces held and frames free, read either side of the refusal
        LR_SPACES1 = 3,
        LR_FRAMES0 = 4,
        LR_FRAMES1 = 5,
        LR_WORDS = 6
    };
    void lr_parked(void*) // caps: gate@1
    {
        kos_sem_wait(1, KOS_TIMEOUT_NONE);
    }
    void lr_member(void*) // caps: E(SIGNAL)@1
    {
        uint64_t rep[LR_WORDS] = {0, 0, 0, 0, 0, 0};
        uintptr_t const g = ASPACE_GRANULE;
        uint32_t const bytes = static_cast<uint32_t>(2u * g);
        void* const blk = st_ram_own(bytes);
        kos_cap_t gate = KOS_CAP_NONE;
        if (blk != nullptr and kos_mem_self_grant(blk, bytes, 0) == 0
            and kos_sem_create(0, &gate) == 0)
        {
            kos_thread_t held[LR_PARK_CAP];
            kos_cap_grant gcaps[] = {{gate, CH_FULL}};
            int n = 0;
            // Plain spawns consume thread slots while sharing the member's task and space.
            while (n < LR_PARK_CAP)
            {
                auto h = kos::thread::create_caps(lr_parked, nullptr, "lrprk", 10, gcaps, 1);
                if (not h.valid())
                {
                    break;
                }
                held[n] = h.id();
                n++;
            }
            rep[LR_PARKED] = static_cast<uint64_t>(n);
            rep[LR_SPACES0] = mem_count(KOS_MEM_SPACES_HELD);
            rep[LR_FRAMES0] = mem_count(KOS_MEM_FRAMES_FREE);
            int const rc = kos::thread::create(lr_parked, nullptr, "lrbad", 10,
                                               KOS_POLICY_FIFO, 0, false, blk, bytes).error();
            rep[LR_RC] = static_cast<uint64_t>(static_cast<uint32_t>(-rc));
            rep[LR_SPACES1] = mem_count(KOS_MEM_SPACES_HELD);
            rep[LR_FRAMES1] = mem_count(KOS_MEM_FRAMES_FREE);
            for (int i = 0; i < n; i++)
            {
                (void)kos_sem_post(gate);
            }
            for (int i = 0; i < n; i++)
            {
                (void)kos_thread_join(held[i], STALL_TOLERANT_US);
            }
            (void)kos_handle_close(gate);
        }
        (void)kos_send(1, rep, sizeof(rep));
    }
    bool lr_cycle(kos_cap_t ep, uint64_t* rep)
    {
        kos_task_t t = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &t) != 0)
        {
            return false;
        }
        kos_cap_grant caps[] = {{ep, KOS_CAP_SIGNAL}};
        // The member's spawn builds a task, so it holds the task authority and is refused
        // further down, where this arm looks.
        auto const m = kos::thread::create_caps(lr_member, nullptr, "lrful", 10, caps, 1,
                                                KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                                KOS_AUTH_MEMORY | KOS_AUTH_TASKS, nullptr, t);
        bool const heard = m.valid() and report_await(ep, rep, sizeof(uint64_t) * LR_WORDS);
        bool const joined = m.valid() and m.join(STALL_TOLERANT_US) == 0;
        bool const reaped = task_end(&t) == 0;
        return heard and joined and reaped;
    }
    void t_spawn_refusal_frees_task()
    {
        // The refused spawn builds a task of its own before the thread pool refuses it.
        TAP_ASK(.workers = 1, .tasks = 2, .endpoints = 1);
        kos_cap_t ep = KOS_CAP_NONE;
        ArmHold hold;
        TAP_HOLD(hold.cap(&ep));
        TAP_CHECK(kos_endpoint_create(&ep) == 0);
        uint64_t rep[LR_WORDS] = {0, 0, 0, 0, 0, 0};
        TAP_CHECK(lr_cycle(ep, rep));
        tap::diag("thread-pool refusal: %u parked, rc %u, spaces %u->%u, frames %u->%u",
                  static_cast<unsigned>(rep[LR_PARKED]), static_cast<unsigned>(rep[LR_RC]),
                  static_cast<unsigned>(rep[LR_SPACES0]),
                  static_cast<unsigned>(rep[LR_SPACES1]),
                  static_cast<unsigned>(rep[LR_FRAMES0]),
                  static_cast<unsigned>(rep[LR_FRAMES1]));
        TAP_CHECK(rep[LR_PARKED] != 0);
        if (rep[LR_PARKED] == LR_PARK_CAP)
        {
            // At the cap the pool outran this arm, so the refusal it measures never happened
            // and the two equalities below would hold vacuously.
            tap::skip("thread pool not exhausted by this arm");
            return;
        }
        TAP_CHECK(rep[LR_RC] == KOS_ENOMEM);
        TAP_CHECK(rep[LR_SPACES0] != MEM_UNREAD and rep[LR_FRAMES0] != MEM_UNREAD);
        TAP_CHECK(rep[LR_SPACES1] == rep[LR_SPACES0]);
        TAP_CHECK(rep[LR_FRAMES1] == rep[LR_FRAMES0]);
    }

    // A failed spawn must release both the borrower space and its donor reference. The member's
    // own task is the donor; no domain is allocated across the reap, which could hide a stale
    // reference.
    enum
    {
        LD_RC = 0,     // the refused spawn's own answer, negated
        LD_SPACES = 1, // spaces held, read inside the member AFTER the refusal
        LD_FRAMES = 2,
        LD_WORDS = 3
    };
    void ld_never(void*)
    {
        kos_exit(0);
    }
    void ld_member(void*) // caps: E(SIGNAL)@1, done@2
    {
        uint64_t rep[LD_WORDS] = {0, 0, 0};
        uintptr_t const g = ASPACE_GRANULE;
        uint32_t const bytes = static_cast<uint32_t>(2u * g);
        void* const blk = st_ram_own(bytes);
        int rc = 0;
        if (blk != nullptr and kos_mem_self_grant(blk, bytes, 0) == 0)
        {
            kos_cap_grant caps[] = {{CH_LOCK, CH_FULL}};
            uint16_t const dest = 0x0FFFu; // past any child capability run
            rc = kos::thread::create_caps(ld_never, nullptr, "ldbad", 10, caps, 1,
                                          KOS_POLICY_FIFO, 0, false, blk, bytes, 0,
                                          &dest).error();
        }
        rep[LD_RC] = static_cast<uint64_t>(static_cast<uint32_t>(-rc));
        rep[LD_SPACES] = mem_count(KOS_MEM_SPACES_HELD);
        rep[LD_FRAMES] = mem_count(KOS_MEM_FRAMES_FREE);
        (void)kos_send(1, rep, sizeof(rep));
    }
    void t_spawn_refusal_frees_donor()
    {
        // The refused spawn builds a task of its own before its late refusal.
        TAP_ASK(.workers = 1, .tasks = 2, .endpoints = 1);
        kos_cap_t ep = KOS_CAP_NONE;
        kos_task_t t = KOS_TASK_NONE;
        ArmHold hold;
        TAP_HOLD(hold.cap(&ep) and hold.task(&t));
        TAP_CHECK(kos_endpoint_create(&ep) == 0);
        uint64_t rep[LD_WORDS] = {0, 0, 0};
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &t) == 0);
        // g_done is only a capability for the member to delegate.
        kos_cap_grant caps[] = {{ep, KOS_CAP_SIGNAL}, {g_done, CH_FULL}};
        // The member's spawn builds a task, so it holds the task authority and is refused
        // further down, where this arm looks.
        auto const m = kos::thread::create_caps(ld_member, nullptr, "lddon", 10, caps, 2,
                                                KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                                KOS_AUTH_MEMORY | KOS_AUTH_TASKS, nullptr, t);
        TAP_CHECK(m.valid());
        bool const heard = report_await(ep, rep, sizeof(uint64_t) * LD_WORDS);
        bool const joined = m.join(STALL_TOLERANT_US) == 0;
        // The member's group dies here, and its domain with it unless something still
        // holds a reference on it.
        bool const reaped = task_end(&t) == 0;
        uint64_t const spaces_end = mem_count(KOS_MEM_SPACES_HELD);
        uint64_t const frames_end = mem_count(KOS_MEM_FRAMES_FREE);
        tap::diag("late refusal in a donor: rc %u, spaces %u->%u, frames %u->%u",
                  static_cast<unsigned>(rep[LD_RC]),
                  static_cast<unsigned>(rep[LD_SPACES]),
                  static_cast<unsigned>(spaces_end),
                  static_cast<unsigned>(rep[LD_FRAMES]),
                  static_cast<unsigned>(frames_end));
        TAP_CHECK(heard and joined and reaped);
        // The refusal really was the late one, and not an early argument check.
        TAP_CHECK(rep[LD_RC] == KOS_EINVAL);
        TAP_CHECK(rep[LD_SPACES] != MEM_UNREAD and rep[LD_FRAMES] != MEM_UNREAD);
        // EXACTLY ONE space fewer: the donor's, which the reap can only release if the
        // refused handoff gave its reference back.
        TAP_CHECK(spaces_end + 1u == rep[LD_SPACES]);
        TAP_CHECK(frames_end > rep[LD_FRAMES]);
    }

    // A task sibling overwrites the victim's user stack with privileged state before it runs;
    // the victim must still reach its own entry and fail kos_shutdown with EPERM. The whole
    // frame window is filled so a layout change cannot hide the attack, and the sibling stays
    // alive until the victim finishes.
    constexpr uint64_t HOSTILE_EL1H = 0x205u; // M[3:0] = EL1h, plus the debug mask
    constexpr uint32_t HOSTILE_WINDOW = 1024; // spans the whole armv8a exception frame
    constexpr int CH_HPARK = 2; // delegated SECOND to the sibling
    // The victim has private globals, so report through the block shared with main.
    void hostile_victim(void* arg)
    {
        *static_cast<volatile int32_t*>(arg) = kos_shutdown(0);
        kos_exit(0);
    }
    void hostile_sibling(void* arg) // caps: E(SIGNAL)@1, park@2
    {
        uintptr_t const top = reinterpret_cast<uintptr_t>(arg);
        volatile uint64_t* const w = reinterpret_cast<volatile uint64_t*>(top - HOSTILE_WINDOW);
        for (uint32_t i = 0; i < HOSTILE_WINDOW / sizeof(uint64_t); i++)
        {
            w[i] = HOSTILE_EL1H;
        }
        reached(1);
        kos_sem_wait(CH_HPARK, KOS_TIMEOUT_NONE);
        kos_exit(1); // unreachable: nothing posts that semaphore
    }
    void parked_frame_hostile_pinned()
    {
        TAP_ASK(.workers = 2, .tasks = 1, .sems = 1, .endpoints = 1);
#if defined(KICKOS_TLS) && KICKOS_TLS
        // The TLS seat admits a caller stack of exactly one stride, stride-aligned.
        constexpr uint32_t VSTK = KICKOS_TLS_STRIDE;
#else
        constexpr uint32_t VSTK = 8192;
#endif
        static_assert(PFH_RAM.size[0] == 3u * VSTK);
        void* const raw = st_ram<PFH_RAM, 0>();
        TAP_CHECK(raw != nullptr);
        // Map main's reservation so it can read the shared verdict.
        TAP_CHECK(kos_mem_self_grant(raw, 3u * VSTK, 0) == 0);
        uintptr_t const vbase = (reinterpret_cast<uintptr_t>(raw) + (VSTK - 1u))
            & ~static_cast<uintptr_t>(VSTK - 1u);
        volatile int32_t* const verdict =
            reinterpret_cast<volatile int32_t*>(reinterpret_cast<uintptr_t>(raw) + 2u * VSTK);
        *verdict = -99;
        kos_cap_t park = KOS_CAP_NONE;
        kos_cap_t ep = KOS_CAP_NONE;
        kos_task_t task = KOS_TASK_NONE;
        ArmHold hold;
        TAP_HOLD(hold.cap(&park) and hold.cap(&ep) and hold.task(&task));
        TAP_CHECK(kos_sem_create(0, &park) == 0 and kos_endpoint_create(&ep) == 0);
        TAP_CHECK(kos_task_create(raw, 3u * VSTK, 0, &task) == 0);
        // Below main on main's core, the victim stays parked until main joins. A caller-owned
        // stack lets its sibling locate and overwrite the saved frame.
        auto const victim = kos::thread::create(hostile_victim,
                                                const_cast<int32_t*>(verdict), "hvic",
                                                TAP_PRIO_AFTER, KOS_POLICY_FIFO, 0, false,
                                                nullptr, 0, reinterpret_cast<void*>(vbase), VSTK,
                                                nullptr, 0, nullptr, 0, 0, nullptr, task,
                                                TAP_PIN_CORE);
        TAP_CHECK(victim.valid());
        kos_cap_grant const caps[2] = {{ep, KOS_CAP_SIGNAL}, {park, KOS_CAP_WAIT}};
        auto const sibling = kos::thread::create(hostile_sibling,
                                               reinterpret_cast<void*>(vbase + VSTK), "hsib",
                                               TAP_PRIO_PARKS, KOS_POLICY_FIFO, 0, false,
                                               nullptr, 0, nullptr, 0, nullptr, 0, caps, 2, 0,
                                               nullptr, task, TAP_PIN_CORE);
        TAP_CHECK(sibling.valid());
        TAP_CHECK(heard_reached(ep)); // the scribble is COMPLETE before the victim is let go
        int const jrc = victim.join(STALL_TOLERANT_US);
        int const rc = *verdict;
        TAP_CHECK(task_end(&task) == 0);
        TAP_CHECK(jrc == 0);
        TAP_CHECK(rc == -KOS_EPERM);
    }

    // main holds the victim's core from its creation to the join: idle, that core would run it
    // before the scribble.
    void t_parked_frame_hostile()
    {
#if KICKOS_KERNEL_CORES > 1
        main_pinned<parked_frame_hostile_pinned>();
#else
        parked_frame_hostile_pinned();
#endif
    }

    // Processes whose static buffers have equal virtual addresses but different frames. The
    // sender's receive-info guard detects a copy to the current space instead of the parked
    // receiver's.
    enum
    {
        PI_ADDR = 0,      // the member's own &g_pi_msg[0]
        PI_INFO_ADDR = 1, // its own &g_pi_info
        PI_N = 2,         // what its own IPC call returned
        PI_SEEN = 3,      // payload bytes matching the pattern its role expects
        PI_BADGE = 4,
        PI_RCAP = 5,
        PI_REPLY_RC = 6, // the call arm's server only
        PI_WORDS = 7
    };
    constexpr int PI_MSG = 12;
    constexpr uint32_t PI_GUARD = 0xDEADBEEFu;
    constexpr uint32_t PI_BLK = PI_RAM.size[0];
    // Volatile detects writes by the other process or kernel through an incorrect alias.
    volatile char g_pi_msg[PI_MSG] = {};
    volatile kos_recv_info g_pi_info = {};

    void pi_mine(volatile uint64_t* out, char first)
    {
        for (int i = 0; i < PI_MSG; i++)
        {
            g_pi_msg[i] = static_cast<char>(first + i);
        }
        g_pi_info.badge = PI_GUARD;
        g_pi_info.reply_cap = PI_GUARD;
        out[PI_ADDR] = reinterpret_cast<uintptr_t>(&g_pi_msg[0]);
        out[PI_INFO_ADDR] = reinterpret_cast<uintptr_t>(&g_pi_info);
    }

    void pi_report(volatile uint64_t* out, int32_t n, char first)
    {
        out[PI_N] = static_cast<uint64_t>(static_cast<int64_t>(n));
        uint64_t seen = 0;
        for (int i = 0; i < PI_MSG; i++)
        {
            if (g_pi_msg[i] == static_cast<char>(first + i))
            {
                seen++;
            }
        }
        out[PI_SEEN] = seen;
        out[PI_BADGE] = g_pi_info.badge;
        out[PI_RCAP] = g_pi_info.reply_cap;
    }

    void pi_server(void* arg) // caps: E(WAIT)@1
    {
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(arg);
        pi_mine(out, '\0');
        struct kos_reply_recv_opts o;
        kos_reply_recv_opts_init(&o, 1, 0, KOS_TIMEOUT_NONE);
        int32_t const n = kos_reply_recv(KOS_CAP_NONE, const_cast<char*>(&g_pi_msg[0]),
                                         kos_call_lens_pack(0, PI_MSG), &o);
        *const_cast<kos_recv_info*>(&g_pi_info) = o.info;
        pi_report(out, n, 'A');
    }

    void pi_client(void* arg) // caps: E(SIGNAL)@1
    {
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(arg);
        pi_mine(out, 'A');
        int32_t const n = kos_send(1, const_cast<char*>(&g_pi_msg[0]), PI_MSG);
        pi_report(out, n, 'A');
    }

    void pi_call_server(void* arg) // caps: E(WAIT)@1
    {
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(arg);
        pi_mine(out, '\0');
        struct kos_reply_recv_opts o;
        kos_reply_recv_opts_init(&o, 1, 0, KOS_TIMEOUT_NONE);
        int32_t const n = kos_reply_recv(KOS_CAP_NONE, const_cast<char*>(&g_pi_msg[0]),
                                         kos_call_lens_pack(0, PI_MSG), &o);
        *const_cast<kos_recv_info*>(&g_pi_info) = o.info;
        pi_report(out, n, 'A');
        // The reply leaves the server's OWN copy of the buffer and must land in the parked
        // caller's copy, at the same number.
        for (int i = 0; i < PI_MSG; i++)
        {
            g_pi_msg[i] = static_cast<char>('a' + i);
        }
        out[PI_REPLY_RC] = static_cast<uint64_t>(static_cast<int64_t>(
            kos_reply(g_pi_info.reply_cap, const_cast<char*>(&g_pi_msg[0]), PI_MSG)));
    }

    void pi_call_client(void* arg) // caps: E(SIGNAL)@1
    {
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(arg);
        pi_mine(out, 'A');
        int32_t const n = kos_call(1, const_cast<char*>(&g_pi_msg[0]), PI_MSG, PI_MSG);
        pi_report(out, n, 'a'); // the REPLY, in its own copy of the one address
    }

    struct PiRun
    {
        volatile uint64_t* oa = nullptr;
        volatile uint64_t* ob = nullptr;
        bool blocks = false;
        bool ran = false;
    };

    // The server, pinned, has parked before the client exists.
    void pi_two_processes(void (*server)(void*), void (*client)(void*), PiRun* run)
    {
        void* const ba = st_ram<PI_RAM, 0>();
        void* const bb = st_ram<PI_RAM, 1>();
        if (ba == nullptr or bb == nullptr)
        {
            return;
        }
        run->blocks = true;
        TAP_CHECK(kos_mem_self_grant(ba, PI_BLK, 0) == 0
                  and kos_mem_self_grant(bb, PI_BLK, 0) == 0);
        volatile uint64_t* const sa = static_cast<volatile uint64_t*>(ba);
        volatile uint64_t* const sb = static_cast<volatile uint64_t*>(bb);
        for (int i = 0; i < PI_WORDS; i++)
        {
            sa[i] = 0;
            sb[i] = 0;
        }
        kos_cap_t ep = KOS_CAP_NONE;
        kos_task_t ta = KOS_TASK_NONE;
        kos_task_t tb = KOS_TASK_NONE;
        ArmHold hold;
        TAP_HOLD(hold.cap(&ep) and hold.task(&ta) and hold.task(&tb));
        TAP_CHECK(kos_endpoint_create(&ep) == 0);
        TAP_CHECK(kos_task_create(ba, PI_BLK, 0, &ta) == 0
                  and kos_task_create(bb, PI_BLK, 0, &tb) == 0);
        kos_cap_grant const scaps[1] = {{ep, KOS_CAP_WAIT}};
        kos_cap_grant const ccaps[1] = {{ep, KOS_CAP_SIGNAL}};
        auto const s = kos::thread::create_caps(server, ba, "piS", 12, scaps, 1, KOS_POLICY_FIFO,
                                                0, false, nullptr, 0, 0, nullptr, ta, nullptr, 0,
                                                TAP_PIN_CORE);
        TAP_CHECK(s.valid());
        TAP_CHECK(await_pinned_park());
        auto const c = kos::thread::create_caps(client, bb, "piC", 11, ccaps, 1, KOS_POLICY_FIFO,
                                                0, false, nullptr, 0, 0, nullptr, tb);
        TAP_CHECK(c.valid());
        TAP_CHECK(s.join(STALL_TOLERANT_US) == 0 and c.join(STALL_TOLERANT_US) == 0);
        TAP_CHECK(task_end(&ta) == 0 and task_end(&tb) == 0);
        run->oa = sa;
        run->ob = sb;
        run->ran = true;
    }

    void pi_root_seed()
    {
        for (int i = 0; i < PI_MSG; i++)
        {
            g_pi_msg[i] = static_cast<char>('R' + i);
        }
        g_pi_info.badge = PI_GUARD;
        g_pi_info.reply_cap = PI_GUARD;
    }

    // main's own copies of both, which no member's IPC may reach.
    bool pi_root_intact()
    {
        bool ok = g_pi_info.badge == PI_GUARD and g_pi_info.reply_cap == PI_GUARD;
        for (int i = 0; i < PI_MSG; i++)
        {
            ok = ok and g_pi_msg[i] == static_cast<char>('R' + i);
        }
        return ok;
    }

    void t_process_ipc_same_addr()
    {
        TAP_ASK(.workers = 2, .tasks = 2, .endpoints = 1);
        PiRun run;
        pi_root_seed();
        pi_two_processes(pi_server, pi_client, &run);
        TAP_CHECK(run.blocks);
        TAP_CHECK(run.ran);
        volatile uint64_t* const oa = run.oa;
        volatile uint64_t* const ob = run.ob;
        uintptr_t const own = reinterpret_cast<uintptr_t>(&g_pi_msg[0]);
        uintptr_t const own_info = reinterpret_cast<uintptr_t>(&g_pi_info);
        tap::diag("same-address send: n %d/%d",
                  static_cast<int>(static_cast<int32_t>(oa[PI_N])),
                  static_cast<int>(static_cast<int32_t>(ob[PI_N])));
        TAP_CHECK(oa[PI_ADDR] == own and ob[PI_ADDR] == own);
        TAP_CHECK(oa[PI_INFO_ADDR] == own_info and ob[PI_INFO_ADDR] == own_info);
        TAP_CHECK(static_cast<int32_t>(oa[PI_N]) == PI_MSG);
        TAP_CHECK(oa[PI_SEEN] == PI_MSG);
        // A plain send: badge 0 and no reply cap.
        TAP_CHECK(oa[PI_BADGE] == 0);
        TAP_CHECK(static_cast<uint32_t>(oa[PI_RCAP]) == KOS_CAP_NONE);
        // The sender's same-address buffer and receive-info must remain untouched.
        TAP_CHECK(static_cast<int32_t>(ob[PI_N]) == PI_MSG);
        TAP_CHECK(ob[PI_SEEN] == PI_MSG);
        TAP_CHECK(ob[PI_BADGE] == PI_GUARD);
        TAP_CHECK(static_cast<uint32_t>(ob[PI_RCAP]) == PI_GUARD);
        TAP_CHECK(pi_root_intact());
    }

    // With CALL: the request and reply cap land in the server's space, then the reply in the
    // parked caller's.
    void t_process_call_reply()
    {
        TAP_ASK(.workers = 2, .tasks = 2, .endpoints = 1);
        PiRun run;
        pi_root_seed();
        pi_two_processes(pi_call_server, pi_call_client, &run);
        TAP_CHECK(run.blocks);
        TAP_CHECK(run.ran);
        volatile uint64_t* const oa = run.oa;
        volatile uint64_t* const ob = run.ob;
        uintptr_t const own = reinterpret_cast<uintptr_t>(&g_pi_msg[0]);
        tap::diag("same-address call: reply rc %d, n %d",
                  static_cast<int>(static_cast<int32_t>(oa[PI_REPLY_RC])),
                  static_cast<int>(static_cast<int32_t>(ob[PI_N])));
        TAP_CHECK(oa[PI_ADDR] == own and ob[PI_ADDR] == own);
        // Server side: the whole request and a reply cap, in its own space.
        TAP_CHECK(static_cast<int32_t>(oa[PI_N]) == PI_MSG);
        TAP_CHECK(oa[PI_SEEN] == PI_MSG);
        TAP_CHECK(oa[PI_BADGE] == 0);
        TAP_CHECK(static_cast<uint32_t>(oa[PI_RCAP]) != KOS_CAP_NONE
                  and static_cast<uint32_t>(oa[PI_RCAP]) != PI_GUARD);
        TAP_CHECK(static_cast<int32_t>(oa[PI_REPLY_RC]) == 0);
        // Caller side: the reply landed in ITS copy of the one address, and its own
        // out-pointer was never a target.
        TAP_CHECK(static_cast<int32_t>(ob[PI_N]) == PI_MSG);
        TAP_CHECK(ob[PI_SEEN] == PI_MSG);
        TAP_CHECK(ob[PI_BADGE] == PI_GUARD);
        TAP_CHECK(static_cast<uint32_t>(ob[PI_RCAP]) == PI_GUARD);
        TAP_CHECK(pi_root_intact());
    }

    // Access-denial tests.

#if KICKOS_FAULT_ISOLATION
    // Park on a semaphore that is never posted; only task-wide death can release it.
    void fault_sibling(void*) // caps: park@1
    {
        kos_sem_wait(KOS_SPAWN_DELEGATED_CAP0, KOS_TIMEOUT_NONE);
        kos_exit(1);
    }

    // Reads a kernel structure ring 3 must not reach; the read ends this thread's whole task.
    void kernel_state_reader(void* arg)
    {
        (void)*static_cast<unsigned char const volatile*>(arg);
        kos_exit(1);
    }

    // Kernel state the trap path uses on every entry. A ring-3 read of either word must end
    // the reading task, and only task-wide death releases its parked sibling, so a read that
    // returned fails the join.
    void t_kernel_state_unreachable()
    {
        TAP_ASK(.workers = 2, .tasks = 1, .sems = 1);
        kos_cap_t park = KOS_CAP_NONE;
        kos_task_t task = KOS_TASK_NONE;
        ArmHold hold;
        TAP_HOLD(hold.cap(&park) and hold.task(&task));
        for (unsigned which = 0; which < KERNEL_WORDS; ++which)
        {
            uintptr_t const addr = kernel_word(which);
            tap::diag("%s at 0x%lx, read from ring 3", kernel_word_name(which),
                      static_cast<unsigned long>(addr));
            TAP_CHECK(in_kernel_half(addr));
            TAP_CHECK(kos_sem_create(0, &park) == 0);
            TAP_CHECK(kos_task_create(nullptr, 0, 0, &task) == 0);
            kos_cap_grant const caps[1] = {{park, KOS_CAP_WAIT}};
            auto const sibling = kos::thread::create(fault_sibling, nullptr, "ksib", 10,
                                                    KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                                    nullptr, 0, nullptr, 0, nullptr, 0, caps, 1,
                                                    /*authority=*/0, /*cap_dest=*/nullptr, task);
            TAP_CHECK(sibling.valid());
            void* const target = reinterpret_cast<void*>(addr);
            auto const victim = kos::thread::create(kernel_state_reader, target, "kvic", 10,
                                                   KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                                   nullptr, 0, nullptr, 0, nullptr, 0, nullptr,
                                                   0, /*authority=*/0, /*cap_dest=*/nullptr,
                                                   task);
            TAP_CHECK(victim.valid());
            TAP_CHECK(victim.join(STALL_TOLERANT_US) == 0);
            TAP_CHECK(sibling.join(STALL_TOLERANT_US) == 0);
            TAP_CHECK(task_end(&task) == 0);
            TAP_CHECK(hold.close(&park) == 0);
        }
    }

#if defined(__x86_64__)
    // --- The x86 port grant --------------------------------------------------------------
    // A holder of the CMOS pair has the kernel write the index, which the chip keeps closed to
    // it, and reads the data port itself; a value with the NMI-mask bit is refused, and so is a
    // port it does not hold. A second holder and the PIC are refused, and COM1 is granted, or
    // refused as held where a console driver holds it. A COM2 holder
    // alternates with it on one core, each reading its own port, then reaches for the CMOS
    // data port and faults; the CMOS holder moves to another core where there is one and reads
    // again. A holder writing the CMOS index itself faults. Reports travel over an endpoint.
    constexpr uint16_t PW_CMOS = 0x70u;
    constexpr uint16_t PW_COM2 = 0x2f8u;
    constexpr int PW_ROUNDS = 4;
    constexpr uint64_t PW_IOPL = 0x3000u;
    constexpr uint64_t PW_IF = 0x200u;
    uint8_t pw_inb(uint16_t port)
    {
        uint8_t v = 0;
        __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port) : "memory");
        return v;
    }
    void pw_outb(uint16_t port, uint8_t v)
    {
        __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port) : "memory");
    }
    struct PwSeen
    {
        int32_t index;
        int32_t nmi;
        int32_t unheld;
        int32_t rounds;
        int32_t moved; // this thread's own move to core 1, where there is one
    };
    void pw_cmos(void*) // caps: E(SIGNAL)@1, mine@2, theirs@3
    {
        PwSeen seen = {-99, -99, -99, 0, 0};
        seen.index = kos_port_reg_write(PW_CMOS, 0, 0x0au);
        seen.nmi = kos_port_reg_write(PW_CMOS, 0, 0x8au);
        seen.unheld = kos_port_reg_write(PW_COM2, 0, 0);
        for (int i = 0; i < PW_ROUNDS; i++)
        {
            kos_sem_wait(2, KOS_TIMEOUT_NONE);
            (void)pw_inb(PW_CMOS + 1u);
            seen.rounds++;
            kos_sem_post(3);
        }
        kos_sem_wait(2, KOS_TIMEOUT_NONE);
#if KICKOS_KERNEL_CORES > 1
        seen.moved = kos::thread::pin(kos_thread_self(), 1);
#endif
        (void)pw_inb(PW_CMOS + 1u);
        seen.rounds++;
        (void)kos_send(1, &seen, sizeof(seen));
        kos_exit(0);
    }
    void pw_com2(void*) // caps: E(SIGNAL)@1, mine@2, theirs@3
    {
        int32_t rounds = 0;
        for (int i = 0; i < PW_ROUNDS; i++)
        {
            kos_sem_wait(2, KOS_TIMEOUT_NONE);
            (void)pw_inb(PW_COM2);
            rounds++;
            if (i + 1 < PW_ROUNDS)
            {
                kos_sem_post(3);
            }
        }
        (void)kos_send(1, &rounds, sizeof(rounds));
        (void)pw_inb(PW_CMOS + 1u);
        (void)kos_send(1, &rounds, sizeof(rounds)); // unreachable: that port is not its own
        kos_exit(0);
    }
    void pw_index(void*) // caps: E(SIGNAL)@1
    {
        pw_outb(PW_CMOS, 0x0au);
        char const x = 1;
        (void)kos_send(1, &x, 1); // unreachable: the chip keeps the index closed
        kos_exit(0);
    }
    void pw_noop(void*) {}
    int32_t pw_recv(kos_cap_t ep, void* buf, size_t len, uint32_t timeout_us)
    {
        struct kos_reply_recv_opts o;
        kos_reply_recv_opts_init(&o, ep, KOS_RECV_NO_INFO, timeout_us);
        return kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, len), &o);
    }
    uint64_t pw_flags_after_syscall()
    {
        uintptr_t nr = KOS_SYS_YIELD;
        uint64_t flags = 0;
        __asm__ volatile("xorl %%esi, %%esi\n\t"
                         "xorl %%edx, %%edx\n\t"
                         "xorl %%r10d, %%r10d\n\t"
                         "xorl %%r8d, %%r8d\n\t"
                         "syscall\n\t"
                         "pushfq\n\t"
                         "popq %1"
                         : "+D"(nr), "=r"(flags)
                         :
                         : "rax", "rcx", "rdx", "rsi", "r8", "r10", "r11", "memory", "cc");
        return flags;
    }
    void t_port_window()
    {
        TAP_ASK(.workers = 2, .tasks = 4, .sems = 2, .endpoints = 1);
        kos_cap_t ep = KOS_CAP_NONE;
        kos_cap_t sa = KOS_CAP_NONE;
        kos_cap_t sb = KOS_CAP_NONE;
        kos_task_t tc = KOS_TASK_NONE;
        kos_task_t tb = KOS_TASK_NONE;
        kos_task_t ti = KOS_TASK_NONE;
        kos_task_t tk = KOS_TASK_NONE;
        ArmHold hold;
        TAP_HOLD(hold.cap(&ep) and hold.cap(&sa) and hold.cap(&sb) and hold.task(&tc)
                  and hold.task(&tb) and hold.task(&ti) and hold.task(&tk));
        TAP_CHECK(kos_endpoint_create(&ep) == 0 and kos_sem_create(0, &sa) == 0
                  and kos_sem_create(0, &sb) == 0);
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &tc) == 0
                  and kos_task_create(nullptr, 0, 0, &tb) == 0
                  and kos_task_create(nullptr, 0, 0, &ti) == 0);
        kos_window const cmos = {PW_CMOS, 2u, KOS_WINDOW_PORTS, 0};
        kos_window const com2 = {PW_COM2, 8u, KOS_WINDOW_PORTS, 0};
        kos_window const pic = {0x20u, 2u, KOS_WINDOW_PORTS, 0};
        kos_cap_grant const ccaps[] = {{ep, KOS_CAP_SIGNAL}, {sa, CH_FULL}, {sb, CH_FULL}};
        kos_cap_grant const bcaps[] = {{ep, KOS_CAP_SIGNAL}, {sb, CH_FULL}, {sa, CH_FULL}};
        auto const holder = kos::thread::create(pw_cmos, nullptr, "pwc", 10, KOS_POLICY_FIFO,
                                                0, false, nullptr, 0, nullptr, 0, &cmos, 1,
                                                ccaps, 3, 0, nullptr, tc, 1u);
        TAP_CHECK(holder.valid());
        TAP_CHECK(kos::thread::create(pw_noop, nullptr, "pwd", 10, KOS_POLICY_FIFO, 0, false,
                                      nullptr, 0, nullptr, 0, &cmos, 1, nullptr, 0, 0, nullptr,
                                      ti)
                      .error()
                  == -KOS_EBUSY);
        TAP_CHECK(kos::thread::create(pw_noop, nullptr, "pwp", 10, KOS_POLICY_FIFO, 0, false,
                                      nullptr, 0, nullptr, 0, &pic, 1, nullptr, 0, 0, nullptr,
                                      ti)
                      .error()
                  == -KOS_EINVAL);
        auto const other = kos::thread::create(pw_com2, nullptr, "pwb", 10, KOS_POLICY_FIFO, 0,
                                               false, nullptr, 0, nullptr, 0, &com2, 1, bcaps,
                                               3, 0, nullptr, tb, 1u);
        TAP_CHECK(other.valid());
        kos_sem_post(sa);
        int32_t rounds = -1;
        TAP_CHECK(pw_recv(ep, &rounds, sizeof(rounds), STALL_TOLERANT_US)
                  == static_cast<int32_t>(sizeof(rounds)));
        TAP_CHECK(other.join(STALL_TOLERANT_US) == 0);
        // An absence after the join, not an order: nothing is left to send.
        TAP_CHECK(pw_recv(ep, &rounds, sizeof(rounds), 20000) == -KOS_ETIMEDOUT);
        kos_sem_post(sa);
        PwSeen seen = {-99, -99, -99, -1, -1};
        TAP_CHECK(pw_recv(ep, &seen, sizeof(seen), STALL_TOLERANT_US)
                  == static_cast<int32_t>(sizeof(seen)));
        TAP_CHECK(holder.join(STALL_TOLERANT_US) == 0);
        TAP_CHECK(rounds == PW_ROUNDS);
        TAP_CHECK(seen.index == 0 and seen.nmi == -KOS_EINVAL and seen.unheld == -KOS_EPERM);
        TAP_CHECK(seen.rounds == PW_ROUNDS + 1 and seen.moved == 0);
        // The CMOS pair is free again, and its index stays closed to the next holder.
        kos_cap_grant const icaps[] = {{ep, KOS_CAP_SIGNAL}};
        auto const writer = kos::thread::create(pw_index, nullptr, "pwi", 10, KOS_POLICY_FIFO,
                                                0, false, nullptr, 0, nullptr, 0, &cmos, 1,
                                                icaps, 1, 0, nullptr, ti);
        TAP_CHECK(writer.valid());
        TAP_CHECK(writer.join(STALL_TOLERANT_US) == 0);
        // An absence after the join, not an order: nothing is left to send.
        char x = 0;
        TAP_CHECK(pw_recv(ep, &x, 1, 20000) == -KOS_ETIMEDOUT);
        // A task of its own: an ended task takes no member, and ti's last one has exited.
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &tk) == 0);
        kos_window const com1 = {0x3f8u, 8u, KOS_WINDOW_PORTS, 0};
        auto const console = kos::thread::create(pw_noop, nullptr, "pwk", 10, KOS_POLICY_FIFO, 0,
                                                 false, nullptr, 0, nullptr, 0, &com1, 1, nullptr,
                                                 0, 0, nullptr, tk);
#if defined(KICKOS_SELFTEST_CONSOLE_DRIVER)
        TAP_CHECK(console.error() == -KOS_EBUSY);
#else
        TAP_CHECK(console.error() == 0);
        TAP_CHECK(console.join(STALL_TOLERANT_US) == 0);
#endif
        uint64_t const back = pw_flags_after_syscall();
        TAP_CHECK((back & PW_IOPL) == 0 and (back & PW_IF) != 0);
    }
#endif

#if defined(KICKOS_SELFTEST_SPARE_DEV)
    // --- A device window at the address the kernel chose -------------------------------
    // The holder, in a task of its own, reads a device the kernel never drives through the
    // address kos_window_get answers, which is not the physical one, and is answered no window
    // past its list; it reports over an endpoint, its task's data being its own.
    // While it lives a second task is refused the device; once it has exited, its sibling in
    // the task faults on the address it left, and the next instance is granted the device and
    // reaches it again.
    constexpr uintptr_t WA_DEV = KICKOS_SELFTEST_SPARE_DEV;
    constexpr uint32_t WA_SIZE = 0x1000u;
    struct WaSeen
    {
        int32_t rc;
        int32_t other;
        uint32_t value;
        uint32_t moved;
    };
    uintptr_t g_wa_addr = 0; // the holder's task's copy, which its sibling reads
    void wa_holder(void*) // caps: E(SIGNAL)@1, hold@2
    {
        kos_window at = {};
        WaSeen seen = {-99, -99, 0, 0};
        seen.rc = kos_window_get(0, &at);
        g_wa_addr = at.base;
        if (seen.rc == 0)
        {
            if (g_wa_addr != WA_DEV)
            {
                seen.moved = 1u;
            }
            seen.value = *reinterpret_cast<volatile uint32_t*>(at.base);
        }
        kos_window other = {};
        seen.other = kos_window_get(1, &other);
        (void)kos_send(1, &seen, sizeof(seen));
        kos_sem_wait(2, KOS_TIMEOUT_NONE);
        kos_exit(0);
    }
    void wa_sibling(void*) // caps: go@1, E(SIGNAL)@2
    {
        kos_sem_wait(1, KOS_TIMEOUT_NONE);
        (void)*reinterpret_cast<volatile uint32_t*>(g_wa_addr);
        (void)kos_send(2, "x", 1); // unreachable: the holder's exit took the mapping with it
        kos_exit(0);
    }
    void wa_noop(void*) {}
#if defined(KICKOS_SELFTEST_BUS_MASTER_DEV)
    struct WaBusMaster
    {
        int32_t spare;
        int32_t bus_master;
        int32_t slain[2];
    };
    // [0] holds AUTH_MEMORY, AUTH_TASKS and AUTH_SYSTEM; [1] AUTH_MEMORY, AUTH_TASKS and
    // AUTH_BUS_MASTER.
    WaBusMaster g_wa_bm[2] = {{-99, -99, {-99, -99}}, {-99, -99, {-99, -99}}};
    kos_window const g_wa_bm_dev = {KICKOS_SELFTEST_BUS_MASTER_DEV, WA_SIZE, KOS_WINDOW_DEVICE, 0};
    void wa_bus_master_spawner(void* arg)
    {
        WaBusMaster* const out = static_cast<WaBusMaster*>(arg);
        kos_task_t t = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &t) == 0)
        {
            kos_window const spare = {WA_DEV, WA_SIZE, KOS_WINDOW_DEVICE, 0};
            auto const h = kos::thread::create(wa_noop, nullptr, "wbs", 10, KOS_POLICY_FIFO, 0,
                                               false, nullptr, 0, nullptr, 0, &spare, 1,
                                               nullptr, 0, 0, nullptr, t);
            out->spare = h.error();
            out->slain[0] = kos_task_slay(t, STALL_TOLERANT_US);
        }
        // A task of its own: the one above ended with its entry.
        if (kos_task_create(nullptr, 0, 0, &t) == 0)
        {
            auto const b = kos::thread::create(wa_noop, nullptr, "wbm", 10, KOS_POLICY_FIFO, 0,
                                               false, nullptr, 0, nullptr, 0, &g_wa_bm_dev, 1,
                                               nullptr, 0, 0, nullptr, t);
            out->bus_master = b.error();
            out->slain[1] = kos_task_slay(t, STALL_TOLERANT_US);
        }
    }
#endif
    // Spawns a holder of the device in `task` and receives its report.
    kos::thread::Handle wa_spawn(kos_task_t task, kos_cap_t ep, kos_cap_t hold, WaSeen* seen)
    {
        kos_window const dev = {WA_DEV, WA_SIZE, KOS_WINDOW_DEVICE, 0};
        kos_cap_grant const caps[] = {{ep, KOS_CAP_SIGNAL}, {hold, KOS_CAP_WAIT}};
        *seen = {-99, -99, 0, 0};
        auto h = kos::thread::create(wa_holder, nullptr, "wah", 10, KOS_POLICY_FIFO, 0, false,
                                     nullptr, 0, nullptr, 0, &dev, 1, caps, 2, 0, nullptr, task);
        if (h.valid())
        {
            struct kos_reply_recv_opts o;
            kos_reply_recv_opts_init(&o, ep, KOS_RECV_NO_INFO, STALL_TOLERANT_US);
            (void)kos_reply_recv(KOS_CAP_NONE, seen, kos_call_lens_pack(0, sizeof(*seen)), &o);
        }
        return h;
    }
    void t_window_addr()
    {
        TAP_ASK(.workers = 2, .tasks = 2, .sems = 2, .endpoints = 1);
        kos_cap_t ep = KOS_CAP_NONE;
        kos_cap_t park = KOS_CAP_NONE;
        kos_cap_t go = KOS_CAP_NONE;
        kos_task_t t = KOS_TASK_NONE;
        kos_task_t other = KOS_TASK_NONE;
#if defined(KICKOS_SELFTEST_BUS_MASTER_DEV)
        kos_task_t bt = KOS_TASK_NONE;
        kos::thread::Handle spawner;
#endif
        ArmHold hold;
        TAP_HOLD(hold.cap(&ep) and hold.cap(&park) and hold.cap(&go) and hold.task(&t)
                  and hold.task(&other));
#if defined(KICKOS_SELFTEST_BUS_MASTER_DEV)
        TAP_HOLD(hold.task(&bt) and hold.thread(&spawner));
#endif
        TAP_CHECK(kos_endpoint_create(&ep) == 0 and kos_sem_create(0, &park) == 0
                  and kos_sem_create(0, &go) == 0);
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &t) == 0
                  and kos_task_create(nullptr, 0, 0, &other) == 0);
        WaSeen first = {-99, -99, 0, 0};
        auto const holder = wa_spawn(t, ep, park, &first);
        kos_cap_grant const scaps[] = {{go, KOS_CAP_WAIT}, {ep, KOS_CAP_SIGNAL}};
        auto const sibling = kos::thread::create(wa_sibling, nullptr, "was", 10,
                                                 KOS_POLICY_FIFO, 0, false, nullptr, 0, nullptr,
                                                 0, nullptr, 0, scaps, 2, 0, nullptr, t);
        TAP_CHECK(holder.valid() and sibling.valid());
        kos_window const dev = {WA_DEV, WA_SIZE, KOS_WINDOW_DEVICE, 0};
        TAP_CHECK(kos::thread::create(wa_noop, nullptr, "wab", 10, KOS_POLICY_FIFO, 0, false,
                                      nullptr, 0, nullptr, 0, &dev, 1, nullptr, 0, 0, nullptr,
                                      other)
                      .error()
                  == -KOS_EBUSY);
        // A megabyte past the spare device is outside every aperture this chip states.
        kos_window const outside = {WA_DEV + 0x100000u, WA_SIZE, KOS_WINDOW_DEVICE, 0};
        TAP_CHECK(kos::thread::create(wa_noop, nullptr, "wao", 10, KOS_POLICY_FIFO, 0, false,
                                      nullptr, 0, nullptr, 0, &outside, 1, nullptr, 0, 0,
                                      nullptr, other)
                      .error()
                  == -KOS_EINVAL);
        kos_sem_post(park);
        TAP_CHECK(holder.join(STALL_TOLERANT_US) == 0);
        kos_sem_post(go);
        TAP_CHECK(sibling.join(STALL_TOLERANT_US) == 0);
        // An absence after the join, not an order: nothing is left to send.
        char late = 0;
        struct kos_reply_recv_opts o;
        kos_reply_recv_opts_init(&o, ep, KOS_RECV_NO_INFO, 20000);
        TAP_CHECK(kos_reply_recv(KOS_CAP_NONE, &late, kos_call_lens_pack(0, 1), &o)
                  == -KOS_ETIMEDOUT);
        TAP_CHECK(task_end(&t) == 0);
        TAP_CHECK(first.rc == 0 and first.moved == 1u and first.value != 0);
        TAP_CHECK(first.other == -KOS_EINVAL);
        // The next instance, in the other task, maps the device again.
        WaSeen again = {-99, -99, 0, 0};
        kos_sem_post(park);
        auto const next = wa_spawn(other, ep, park, &again);
        TAP_CHECK(next.valid() and next.join(STALL_TOLERANT_US) == 0);
        TAP_CHECK(again.rc == 0 and again.value != 0);
        TAP_CHECK(task_end(&other) == 0);
#if defined(KICKOS_SELFTEST_BUS_MASTER_DEV)
        // A bus master's window takes AUTH_BUS_MASTER of its spawner; AUTH_SYSTEM is not it.
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &bt) == 0);
        auto const bm = kos::thread::create(wa_noop, nullptr, "wbm", 10, KOS_POLICY_FIFO, 0, false,
                                            nullptr, 0, nullptr, 0, &g_wa_bm_dev, 1, nullptr, 0,
                                            0, nullptr, bt);
        TAP_CHECK(bm.valid() and bm.join(STALL_TOLERANT_US) == 0);
        TAP_CHECK(task_end(&bt) == 0);
        uint32_t const bauth[2] = {KOS_AUTH_MEMORY | KOS_AUTH_TASKS | KOS_AUTH_SYSTEM,
                                   KOS_AUTH_MEMORY | KOS_AUTH_TASKS | KOS_AUTH_BUS_MASTER};
        for (int i = 0; i < 2; i++)
        {
            g_wa_bm[i] = {-99, -99, {-99, -99}};
            spawner = kos::thread::create_caps(wa_bus_master_spawner, &g_wa_bm[i], "wbp", 10,
                                               nullptr, 0, KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                               bauth[i]);
            TAP_CHECK(spawner.valid());
            TAP_CHECK(thread_end(&spawner) == 0);
        }
        tap::diag("bus master window: memory and system %ld, bus_master %ld",
                  static_cast<long>(g_wa_bm[0].bus_master),
                  static_cast<long>(g_wa_bm[1].bus_master));
        for (int i = 0; i < 2; i++)
        {
            TAP_CHECK(g_wa_bm[i].slain[0] == 0 and g_wa_bm[i].slain[1] == 0);
        }
        TAP_CHECK(g_wa_bm[0].spare == 0 and g_wa_bm[1].spare == 0);
        TAP_CHECK(g_wa_bm[0].bus_master == -KOS_EPERM);
        TAP_CHECK(g_wa_bm[1].bus_master == 0);
#endif
    }
#endif
#endif

    // main has memory authority but must not grant an unreserved kernel address. Do not reject
    // all high addresses: the user arena is high-half on this board.
    void t_grant_kernel_word_refused()
    {
        void* const kword = reinterpret_cast<void*>(kernel_word(0));
        void* const mine = st_ram<KW_RAM, 0>();
        TAP_CHECK(mine != nullptr);
        tap::diag("self-granting %s 0x%lx against own range 0x%lx", kernel_word_name(0),
                  static_cast<unsigned long>(reinterpret_cast<uintptr_t>(kword)),
                  static_cast<unsigned long>(reinterpret_cast<uintptr_t>(mine)));
        TAP_CHECK(in_kernel_half(reinterpret_cast<uintptr_t>(kword)));
        TAP_CHECK(kos_mem_self_grant(kword, sizeof(uint32_t), 0) == -KOS_EPERM);
        // Control: a caller-owned reservation, same syscall.
        TAP_CHECK(kos_mem_self_grant(mine, 64, 0) == 0);
        // Still refused after a success on the same path, so the refusal is not a one-shot
        // state the first call left behind.
        TAP_CHECK(kos_mem_self_grant(kword, sizeof(uint32_t), 0) == -KOS_EPERM);
    }

    // A non-cacheable mapping reads what the kernel wrote through its cacheable view, and the
    // kernel reads what was written through it: the clear of a fresh run, IPC into and out of a
    // non-cacheable buffer, and a retype back to cacheable after the kernel's reads. No emulator
    // models a data cache, so only silicon can fail these compares.
    constexpr size_t UA_LEN = 16;
    constexpr uint32_t UA_BLK = UA_RAM.size[0];
    constexpr int UA_ROUNDS = 3;
    constexpr int CH_UA_EP = 1;
    constexpr uint32_t UA_RUN_PAGES = 4;
    constexpr uint32_t UA_WIDE_PAGES = 32;
    constexpr uint32_t UA_FILL = 0xCAC4ED00u;
    unsigned char* g_ua_dst[UA_ROUNDS] = {};
    Atomic<int32_t, Order::RELAXED> g_ua_got[UA_ROUNDS];
    void ua_receiver(void*) // caps: E(WAIT)@1
    {
        for (int r = 0; r < UA_ROUNDS; r++)
        {
            struct kos_reply_recv_opts o;
            kos_reply_recv_opts_init(&o, CH_UA_EP, KOS_RECV_NO_INFO, STALL_TOLERANT_US);
            g_ua_got[r] = kos_reply_recv(KOS_CAP_NONE, g_ua_dst[r],
                                         kos_call_lens_pack(0, UA_LEN), &o);
        }
    }
    void* g_ua_unauth_blk = nullptr;
    int32_t g_ua_unauth = 0;
    void ua_unauthorised(void*)
    {
        g_ua_unauth = kos_mem_self_grant(g_ua_unauth_blk, UA_BLK, KOS_MEM_NOCACHE);
    }

    uint32_t ua_mismatches(uintptr_t p, size_t bytes, uint32_t base)
    {
        volatile uint32_t const* const w = reinterpret_cast<volatile uint32_t const*>(p);
        uint32_t bad = 0;
        for (uint32_t i = 0; i < bytes / sizeof(uint32_t); i++)
        {
            if (w[i] != (base ^ i))
            {
                bad++;
            }
        }
        return bad;
    }
    uint32_t ua_nonzero(uintptr_t p, size_t bytes)
    {
        volatile uint32_t const* const w = reinterpret_cast<volatile uint32_t const*>(p);
        uint32_t bad = 0;
        for (uint32_t i = 0; i < bytes / sizeof(uint32_t); i++)
        {
            if (w[i] != 0u)
            {
                bad++;
            }
        }
        return bad;
    }
    void ua_fill(uintptr_t p, size_t bytes, uint32_t base)
    {
        volatile uint32_t* const w = reinterpret_cast<volatile uint32_t*>(p);
        for (uint32_t i = 0; i < bytes / sizeof(uint32_t); i++)
        {
            w[i] = base ^ i;
        }
    }

    // A run filled through a cacheable mapping and closed, then a fresh run read through a
    // non-cacheable one: the kernel's clear must have reached memory. Written there, it must
    // read back through the next cacheable mapping. The run is one type at a time.
    void ua_run()
    {
        size_t const bytes = UA_RUN_PAGES * ASPACE_GRANULE;
        Seed first;
        Seed s;
        Seed scratch;
        uintptr_t first_at = 0;
        uintptr_t at = 0;
        ArmHold hold;
        TAP_HOLD(hold.mapped(&first.frame, &first.space, &first_at)
                  and hold.mapped(&s.frame, &s.space, &at) and hold.cap(&scratch.frame)
                  and hold.cap(&scratch.space));
        TAP_CHECK(seed_mint(bytes, &first));
        TAP_CHECK(kos_frame_map(first.frame, first.space, &first_at, 0) == 0);
        uintptr_t const first_va = first_at;
        ua_fill(first_va, bytes, UA_FILL);
        TAP_CHECK(kos_frame_unmap(first.frame, first.space, first_va) == 0);
        first_at = 0;
        seed_close(&first);
        TAP_CHECK(seed_mint(bytes, &s));
        TAP_CHECK(kos_frame_map(s.frame, s.space, &at, KOS_MEM_NOCACHE) == 0);
        uintptr_t const va = at;
        uint32_t const cleared = ua_nonzero(va, bytes);
        // One run, two types at once, is incoherent.
        TAP_CHECK(seed_mint(ASPACE_GRANULE, &scratch));
        uintptr_t const elsewhere = seed_va(scratch);
        seed_close(&scratch);
        int32_t const mixed = map_at(s.frame, s.space, elsewhere, 0);
        if (mixed == 0)
        {
            (void)kos_frame_unmap(s.frame, s.space, elsewhere);
        }
        ua_fill(va, bytes, ~UA_FILL);
        TAP_CHECK(kos_frame_unmap(s.frame, s.space, va) == 0);
        at = 0;
        TAP_CHECK(map_at(s.frame, s.space, va, 0) == 0);
        at = va;
        uint32_t const kept = ua_mismatches(va, bytes, ~UA_FILL);
        TAP_CHECK(kos_frame_unmap(s.frame, s.space, va) == 0);
        at = 0;
        seed_close(&s);
        tap::diag("run at 0x%lx (the closed one at 0x%lx): %u words not cleared through the"
                  " non-cacheable map, %u lost by the cacheable one after it; mixed type %ld",
                  static_cast<unsigned long>(va), static_cast<unsigned long>(first_va),
                  static_cast<unsigned>(cleared), static_cast<unsigned>(kept),
                  static_cast<long>(mixed));
        // The frames the closed run left dirty, or the clear is not what was read.
        TAP_CHECK(va == first_va);
        TAP_CHECK(cleared == 0);
        TAP_CHECK(mixed == -KOS_EBUSY);
        TAP_CHECK(kept == 0);
    }

    void t_uncached_alias_sync()
    {
        TAP_ASK(.workers = 1, .endpoints = 1);
        unsigned char* const unc = static_cast<unsigned char*>(st_ram<UA_RAM, 0>());
        unsigned char* const cac = static_cast<unsigned char*>(st_ram<UA_RAM, 1>());
        size_t const wide_bytes = UA_WIDE_PAGES * ASPACE_GRANULE;
        void* const wide = st_ram<UA_RAM, 2>();
        TAP_CHECK(unc != nullptr and cac != nullptr and wide != nullptr);
        kos_cap_t ep = KOS_CAP_NONE;
        kos::thread::Handle unauth;
        kos::thread::Handle receiver;
        ArmHold hold;
        TAP_HOLD(hold.cap(&ep) and hold.thread(&unauth) and hold.thread(&receiver));
        TAP_CHECK(kos_endpoint_create(&ep) == 0);
        TAP_CHECK(kos_mem_self_grant(cac, UA_BLK, 0) == 0);
        TAP_CHECK(kos_mem_self_grant(unc, UA_BLK, KOS_MEM_NOCACHE) == 0);
        // A wide reservation the kernel cleared, read through a non-cacheable grant.
        TAP_CHECK(kos_mem_self_grant(wide, wide_bytes, KOS_MEM_NOCACHE) == 0);
        uint32_t const wide_cleared =
            ua_nonzero(reinterpret_cast<uintptr_t>(wide), wide_bytes);
        tap::diag("%u words of %lu granules not cleared through the non-cacheable grant",
                  static_cast<unsigned>(wide_cleared),
                  static_cast<unsigned long>(UA_WIDE_PAGES));
        TAP_CHECK(wide_cleared == 0);
        // Refused, or mapped so already: the call answers before it maps anything.
        g_ua_unauth_blk = cac;
        g_ua_unauth = 99;
        unauth = kos::thread::create_caps(ua_unauthorised, nullptr, "uanA", 10, nullptr, 0,
                                          KOS_POLICY_FIFO, 0, false, nullptr, 0, 0);
        TAP_CHECK(unauth.valid());
        TAP_CHECK(thread_end(&unauth) == 0);
        TAP_CHECK(g_ua_unauth == -KOS_EPERM);
        TAP_CHECK(kos_mem_self_grant(unc, UA_BLK, KOS_MEM_NOCACHE) == 0);
        for (size_t i = 0; i < UA_LEN; i++)
        {
            cac[UA_LEN * 2 + i] = static_cast<unsigned char>(0x40u + i);
            unc[UA_LEN * 2 + i] = static_cast<unsigned char>(0x80u + i);
        }
        unsigned char const* const src[UA_ROUNDS] = {cac + UA_LEN * 2, unc + UA_LEN * 2,
                                                     cac + UA_LEN * 2};
        g_ua_dst[0] = unc;
        g_ua_dst[1] = cac;
        g_ua_dst[2] = cac + UA_LEN * 4;
        for (int r = 0; r < UA_ROUNDS; r++)
        {
            g_ua_got[r] = 99;
        }
        kos_cap_grant const caps[] = {{ep, KOS_CAP_WAIT}};
        receiver = kos::thread::create_caps(ua_receiver, nullptr, "uarcv", TAP_PRIO_PARKS, caps, 1,
                                            KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr,
                                            KOS_TASK_NONE, nullptr, 0, TAP_PIN_CORE);
        TAP_CHECK(receiver.valid());
        // Each send meets the receiver's next receive, which follows its record of the last.
        int32_t sent[UA_ROUNDS];
        for (int r = 0; r < UA_ROUNDS; r++)
        {
            sent[r] = kos_send_timed(ep, src[r], UA_LEN, STALL_TOLERANT_US);
        }
        TAP_CHECK(thread_end(&receiver) == 0);
        for (int r = 0; r < UA_ROUNDS; r++)
        {
            tap::diag("round %d: sent %ld, received %ld", r, static_cast<long>(sent[r]),
                      static_cast<long>(g_ua_got[r].load()));
            TAP_CHECK(sent[r] == static_cast<int32_t>(UA_LEN));
            TAP_CHECK(g_ua_got[r].load() == static_cast<int32_t>(UA_LEN));
            TAP_CHECK(memcmp(g_ua_dst[r], src[r], UA_LEN) == 0);
        }
        TAP_CHECK(hold.close(&ep) == 0);
        // Back to cacheable: the lines the kernel's reads left are stale to the new mapping.
        TAP_CHECK(kos_mem_self_grant(unc, UA_BLK, 0) == 0);
        bool same = true;
        for (size_t i = 0; i < UA_LEN; i++)
        {
            if (unc[i] != src[0][i] or unc[UA_LEN * 2 + i] != static_cast<unsigned char>(0x80u + i))
            {
                same = false;
            }
        }
        TAP_CHECK(same);
        ua_run();
    }

    // New processes copy the saved startup globals, never a live process's mutable data.
    volatile uint64_t g_dt_word = 0xC0FFEEull;
    constexpr uint64_t DT_A = 0xC0FFEEull;
    constexpr uint64_t DT_B = 0xBADBADull;
    enum
    {
        DT_VALUE = 0,
        DT_WORDS = 1
    };
    void dt_reader(void* arg)
    {
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(arg);
        out[DT_VALUE] = g_dt_word;
    }
    // A receiver parks, a sibling unmaps its buffer and sends: both must get EFAULT. The sender
    // buffer is separate to exclude overlap errors. Both are pinned and the receiver outranks
    // the sibling, so validation precedes the unmap. Sender EFAULT proves delivery reached a
    // parked receiver; a boundary rejection would leave the sender to time out.
    constexpr size_t RU_LEN = 32;
    constexpr uint32_t RU_SEND_US = 200000;
    constexpr int CH_RU_EP = 1;
    constexpr int CH_RU_FRAME = 2;
    constexpr int CH_RU_SPACE = 3;
    char g_ru_src[RU_LEN];
    Atomic<int32_t, Order::RELAXED> g_ru_unmap{1};
    Atomic<int32_t, Order::RELAXED> g_ru_sent{1};
    Atomic<int32_t, Order::RELAXED> g_ru_got{99};
    void ru_receiver(void* arg) // caps: E(WAIT)@1
    {
        struct kos_reply_recv_opts opts = {};
        opts.timeout_us = STALL_TOLERANT_US;
        opts.info.reply_cap = KOS_CAP_NONE;
        opts.ep = CH_RU_EP;
        g_ru_got = kos_reply_recv(KOS_CAP_NONE, arg, kos_call_lens_pack(0, RU_LEN), &opts);
    }
    void ru_puller(void* arg) // caps: E(SIGNAL)@1, frame@2, space@3
    {
        g_ru_unmap = kos_frame_unmap(CH_RU_FRAME, CH_RU_SPACE,
                                     reinterpret_cast<uintptr_t>(arg));
        g_ru_sent = kos_send_timed(CH_RU_EP, g_ru_src, RU_LEN, RU_SEND_US);
    }
    void t_recv_buf_unmapped()
    {
        TAP_ASK(.workers = 2, .endpoints = 1);
        Seed s;
        uintptr_t mapped_at = 0;
        kos_cap_t ep = KOS_CAP_NONE;
        kos::thread::Handle receiver;
        kos::thread::Handle puller;
        ArmHold hold;
        TAP_HOLD(hold.mapped(&s.frame, &s.space, &mapped_at) and hold.cap(&ep)
                  and hold.thread(&receiver) and hold.thread(&puller));
        TAP_CHECK(seed_mint(ASPACE_GRANULE, &s));
        uintptr_t const va = seed_va(s);
        TAP_CHECK(va != 0);
        TAP_CHECK(kos_endpoint_create(&ep) == 0);
        TAP_CHECK(map_at(s.frame, s.space, va, 0) == 0);
        mapped_at = va;
        for (size_t i = 0; i < RU_LEN; i++)
        {
            g_ru_src[i] = static_cast<char>('A' + (i & 15u));
        }
        g_ru_unmap = 1;
        g_ru_sent = 1;
        g_ru_got = 99;
        // No task named, so both are threads of main's task and the page the puller takes is
        // the receiver's own.
        kos_cap_grant rcaps[] = {{ep, KOS_CAP_WAIT}};
        kos_cap_grant pcaps[] = {{ep, KOS_CAP_SIGNAL}, {s.frame, KOS_CAP_TRANSFER},
                                 {s.space, KOS_CAP_TRANSFER}};
        receiver = kos::thread::create_caps(ru_receiver, reinterpret_cast<void*>(va), "rurcv",
                                            TAP_PRIO_PARKS, rcaps, 1, KOS_POLICY_FIFO, 0, false,
                                            nullptr, 0, 0, nullptr, KOS_TASK_NONE, nullptr, 0,
                                            TAP_PIN_CORE);
        TAP_CHECK(receiver.valid());
        puller = kos::thread::create_caps(ru_puller, reinterpret_cast<void*>(va), "rupul",
                                          TAP_PRIO_AFTER, pcaps, 3, KOS_POLICY_FIFO, 0, false,
                                          nullptr, 0, KOS_AUTH_MEMORY, nullptr, KOS_TASK_NONE,
                                          nullptr, 0, TAP_PIN_CORE);
        TAP_CHECK(puller.valid());
        TAP_CHECK(thread_end(&receiver) == 0 and thread_end(&puller) == 0);
        if (g_ru_unmap.load() == 0)
        {
            mapped_at = 0;
        }
        tap::diag("recv buffer unmapped: recv %ld, unmap %ld, send %ld",
                  static_cast<long>(g_ru_got.load()), static_cast<long>(g_ru_unmap.load()),
                  static_cast<long>(g_ru_sent.load()));
        TAP_CHECK(g_ru_unmap.load() == 0);
        TAP_CHECK(g_ru_got.load() == -KOS_EFAULT);
        TAP_CHECK(g_ru_sent.load() == -KOS_EFAULT);
    }

    // Frame-run creation after slot reuse, when a raw index no longer resolves as a generational
    // handle: more create/close cycles than the task's budget holds at once force the reuse.
    constexpr uint32_t FR_HOLD_MAX = 24;
    constexpr uint32_t FR_CYCLES = FR_HOLD_MAX;
    kos_cap_t g_fr_f[FR_HOLD_MAX];

    void t_frame_run_slot_recycle()
    {
        kos_cap_t space = KOS_CAP_NONE;
        if (kos_aspace_self(&space) != 0)
        {
            tap::fail("the caller's own space capability was refused");
            return;
        }
        // Phase one: hold until a mint is refused.
        uint32_t held = 0;
        int32_t refused = 0;
        while (held < FR_HOLD_MAX)
        {
            refused = kos_frame_create(ASPACE_GRANULE, &g_fr_f[held]);
            if (refused != 0)
            {
                break;
            }
            held++;
        }
        for (uint32_t i = 0; i < held; i++)
        {
            // Closing the last capability frees frames and advances the slot generation.
            (void)kos_handle_close(g_fr_f[i]);
        }

        // Phase two: more cycles than phase one could hold at once.
        uint32_t minted = 0;
        uint32_t mapped = 0;
        for (uint32_t i = 0; i < FR_CYCLES; i++)
        {
            kos_cap_t fcap = KOS_CAP_NONE;
            if (kos_frame_create(ASPACE_GRANULE, &fcap) != 0)
            {
                break;
            }
            minted++;
            // Mapping must resolve the run through its current generational handle.
            uintptr_t va = 0;
            if (kos_frame_map(fcap, space, &va, 0) == 0)
            {
                mapped++;
                (void)kos_frame_unmap(fcap, space, va);
            }
            (void)kos_handle_close(fcap);
        }
        (void)kos_handle_close(space);
        tap::diag("frame run recycle: %u held at once (then %ld), then %u of %u cycle(s), %u"
                  " mapped",
                  static_cast<unsigned>(held), static_cast<long>(refused),
                  static_cast<unsigned>(minted), static_cast<unsigned>(FR_CYCLES),
                  static_cast<unsigned>(mapped));
        // Exhaustion, then enough cycles to prove slot reuse.
        TAP_CHECK(held > 0 and held < FR_HOLD_MAX);
        TAP_CHECK(refused == -KOS_EAGAIN or refused == -KOS_ENOMEM or refused == -KOS_EMFILE);
        TAP_CHECK(FR_CYCLES > held);
        TAP_CHECK(minted == FR_CYCLES);
        TAP_CHECK(mapped == minted);
    }

    // A failed reply-cap write-back revokes the cap and returns EFAULT to both ends. Site A uses
    // an already-unmapped opts page (boundary validation); site B unmaps after parking (rollback
    // after a successful validation). KICKOS_CAP_REPLY_MAX failures precede the control, so it
    // sees both the slot and the reply-cap budget recovered.
    constexpr size_t LU_LEN = 8;
    constexpr int CH_LU_EP = 1;
    constexpr int CH_LU_GATE = 2;
    char g_lu_req[LU_LEN];
    char g_lu_rx[LU_LEN];
    uintptr_t g_lu_info = 0;
    Atomic<int32_t, Order::RELAXED> g_lu_call{99};
    Atomic<int32_t, Order::RELAXED> g_lu_reply{-1};
    Atomic<uint32_t, Order::RELAXED> g_lu_rounds{0};
    int32_t g_lu_got[KICKOS_CAP_REPLY_MAX + 1];

    // Parked on send_waiters before main receives, which reaches the receiver scan instead of
    // the fastpath.
    void lu_caller(void*) // caps: E(SIGNAL)@1
    {
        g_lu_call = kos_call_timed(CH_LU_EP, g_lu_req, LU_LEN, LU_LEN, STALL_TOLERANT_US);
    }

    // One receiver across the failures and the control, so they spend one cap budget. main maps
    // opts before each gate and unmaps it only once the receiver parked, so the failure is at
    // delivery and not at syscall entry.
    void lu_receiver(void*) // caps: E(WAIT)@1, gate@2
    {
        for (uint32_t i = 0; i <= KICKOS_CAP_REPLY_MAX; i++)
        {
            kos_sem_wait(CH_LU_GATE, KOS_TIMEOUT_NONE);
            // Place opts on the mapped page so unmapping invalidates the reply-cap output.
            struct kos_reply_recv_opts* const o =
                reinterpret_cast<struct kos_reply_recv_opts*>(g_lu_info);
            kos_reply_recv_opts_init(o, CH_LU_EP, 0, KOS_TIMEOUT_NONE);
            int32_t const got =
                kos_reply_recv(KOS_CAP_NONE, g_lu_rx, kos_call_lens_pack(0, LU_LEN), o);
            g_lu_got[i] = got;
            g_lu_rounds = i + 1u;
            // Only in the round main left mapped, and never decided by got: a wrong success
            // would fault.
            if (i == KICKOS_CAP_REPLY_MAX)
            {
                g_lu_reply = kos_reply(o->info.reply_cap, g_lu_rx, LU_LEN);
            }
        }
    }

    void t_call_reply_undisclosed()
    {
        // A witness beside each of the two parties.
        TAP_ASK(.workers = 2, .sems = 1, .endpoints = 1);
        Seed s;
        uintptr_t mapped_at = 0;
        kos_cap_t ep = KOS_CAP_NONE;
        kos_cap_t gate = KOS_CAP_NONE;
        kos::thread::Handle caller;
        kos::thread::Handle receiver;
        ArmHold hold;
        TAP_HOLD(hold.mapped(&s.frame, &s.space, &mapped_at) and hold.cap(&ep)
                  and hold.cap(&gate) and hold.thread(&caller) and hold.thread(&receiver));
        TAP_CHECK(seed_mint(ASPACE_GRANULE, &s));
        uintptr_t const va = seed_va(s);
        TAP_CHECK(va != 0);
        TAP_CHECK(kos_endpoint_create(&ep) == 0 and kos_sem_create(0, &gate) == 0);
        // The page must map and unmap before its unmapped state means anything.
        TAP_CHECK(map_at(s.frame, s.space, va, 0) == 0);
        TAP_CHECK(kos_frame_unmap(s.frame, s.space, va) == 0);
        g_lu_info = va;
        for (size_t i = 0; i < LU_LEN; i++)
        {
            g_lu_req[i] = static_cast<char>('a' + i);
        }

        // Site A validates and writes the output under one lock without parking, so only boundary
        // validation is reachable; site B and tests/unit/capprobe cover capability rollback.
        int32_t const a_entry =
            kos_reply_recv(KOS_CAP_NONE, g_lu_rx, kos_call_lens_pack(0, LU_LEN),
                           reinterpret_cast<struct kos_reply_recv_opts*>(va));

        // Control: the same receiver scan with a valid output page.
        g_lu_call = 99;
        kos_cap_grant ccaps[] = {{ep, KOS_CAP_SIGNAL}};
        caller = kos::thread::create_caps(lu_caller, nullptr, "luctl", TAP_PRIO_PARKS, ccaps, 1,
                                          KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr,
                                          KOS_TASK_NONE, nullptr, 0, TAP_PIN_CORE);
        TAP_CHECK(caller.valid());
        TAP_CHECK(await_pinned_park());
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, ep, 0, STALL_TOLERANT_US);
        int32_t const a_ctl =
            kos_reply_recv(KOS_CAP_NONE, g_lu_rx, kos_call_lens_pack(0, LU_LEN), &opts);
        struct kos_recv_info const a_info = opts.info;
        int a_reply = -1;
        if (a_info.reply_cap != KOS_CAP_NONE)
        {
            a_reply = kos_reply(a_info.reply_cap, g_lu_rx, LU_LEN);
        }
        TAP_CHECK(thread_end(&caller) == 0);

        // Site B: endpoint_call's fastpath, minting into a PARKED RECEIVER's table.
        g_lu_rounds = 0;
        g_lu_reply = -1;
        for (uint32_t i = 0; i <= KICKOS_CAP_REPLY_MAX; i++)
        {
            g_lu_got[i] = 99;
        }
        bool b_call_faulted = true;
        int32_t b_call_saw = 77;
        kos_cap_grant rcaps[] = {{ep, KOS_CAP_WAIT}, {gate, CH_FULL}};
        receiver = kos::thread::create_caps(lu_receiver, nullptr, "lurcv", TAP_PRIO_PARKS, rcaps,
                                            2, KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr,
                                            KOS_TASK_NONE, nullptr, 0, TAP_PIN_CORE);
        TAP_CHECK(receiver.valid());
        for (uint32_t i = 0; i < KICKOS_CAP_REPLY_MAX; i++)
        {
            TAP_CHECK(map_at(s.frame, s.space, va, 0) == 0);
            mapped_at = va;
            kos_sem_post(gate);
            TAP_CHECK(await_pinned_park());
            TAP_CHECK(kos_frame_unmap(s.frame, s.space, va) == 0);
            mapped_at = 0;
            b_call_saw = kos_call_timed(ep, g_lu_req, LU_LEN, LU_LEN, STALL_TOLERANT_US);
            if (b_call_saw != -KOS_EFAULT)
            {
                b_call_faulted = false;
            }
            // Parked on the gate again, it has recorded this round.
            TAP_CHECK(await_pinned_park());
            TAP_CHECK(g_lu_rounds.load() == i + 1u);
        }
        // Mapped this time: the control needs the budget the failed mints returned.
        TAP_CHECK(map_at(s.frame, s.space, va, 0) == 0);
        mapped_at = va;
        kos_sem_post(gate);
        TAP_CHECK(await_pinned_park());
        int32_t const b_ctl = kos_call_timed(ep, g_lu_req, LU_LEN, LU_LEN, STALL_TOLERANT_US);
        // The receiver stores its reply's result after that reply has released main.
        TAP_CHECK(thread_end(&receiver) == 0);
        TAP_CHECK(kos_frame_unmap(s.frame, s.space, va) == 0);
        mapped_at = 0;
        bool b_recv_faulted = true;
        for (uint32_t i = 0; i < KICKOS_CAP_REPLY_MAX; i++)
        {
            if (g_lu_got[i] != -KOS_EFAULT)
            {
                b_recv_faulted = false;
            }
        }
        tap::diag("local undisclosed: recv boundary %ld, recv control %ld replied %d; %u "
                  "fastpath refusal(s), call control %ld, receiver round(s) %u last %ld "
                  "replied %ld, call saw %ld", static_cast<long>(a_entry),
                  static_cast<long>(a_ctl), a_reply,
                  static_cast<unsigned>(KICKOS_CAP_REPLY_MAX), static_cast<long>(b_ctl),
                  static_cast<unsigned>(g_lu_rounds.load()),
                  static_cast<long>(g_lu_got[KICKOS_CAP_REPLY_MAX]),
                  static_cast<long>(g_lu_reply.load()), static_cast<long>(b_call_saw));
        // Site A: an unmapped out-pointer never reaches the mint, and the ordinary
        // rendezvous through that same arm still lands.
        TAP_CHECK(a_entry == -KOS_EFAULT);
        TAP_CHECK(a_ctl == static_cast<int32_t>(LU_LEN));
        TAP_CHECK(a_info.reply_cap != KOS_CAP_NONE);
        TAP_CHECK(a_reply == 0);
        TAP_CHECK(g_lu_call.load() == static_cast<int32_t>(LU_LEN));
        // Site B: the same, with the mint on the other table.
        TAP_CHECK(g_lu_rounds.load() == KICKOS_CAP_REPLY_MAX + 1u);
        TAP_CHECK(b_recv_faulted);
        TAP_CHECK(b_call_faulted);
        TAP_CHECK(g_lu_got[KICKOS_CAP_REPLY_MAX] == static_cast<int32_t>(LU_LEN));
        TAP_CHECK(g_lu_reply.load() == 0);
        TAP_CHECK(b_ctl == static_cast<int32_t>(LU_LEN));
    }

    // A task's static data is the image's as root held it at the first explicit task, the init
    // creating main's, never a global main wrote since; the task after the write and its restart
    // both read the image's value.
    void t_process_data_from_image()
    {
        TAP_ASK(.workers = 1, .tasks = 1);
        constexpr uint32_t FI_BLK = 8u * DT_WORDS;
        static_assert(FI_RAM.size[0] == FI_BLK);
        void* const blk = st_ram<FI_RAM, 0>();
        TAP_CHECK(blk != nullptr);
        TAP_CHECK(kos_mem_self_grant(blk, FI_BLK, 0) == 0);
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(blk);
        kos_task_t t = KOS_TASK_NONE;
        ArmHold hold;
        TAP_HOLD(hold.task(&t));
        TAP_CHECK(kos_task_create(blk, FI_BLK, 0, &t) == 0);
        TAP_CHECK(task_end(&t) == 0);
        g_dt_word = DT_B;
        uint64_t seen[2] = {0, 0};
        for (int run = 0; run < 2; run++)
        {
            out[DT_VALUE] = 0;
            TAP_CHECK(kos_task_create(blk, FI_BLK, 0, &t) == 0);
            auto const reader = kos::thread::create_caps(dt_reader, blk, "fiR", 10, nullptr, 0,
                                                         KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                                         0, nullptr, t);
            TAP_CHECK(reader.valid() and reader.join(STALL_TOLERANT_US) == 0);
            TAP_CHECK(task_end(&t) == 0);
            seen[run] = out[DT_VALUE];
        }
        g_dt_word = DT_A;
        tap::diag("data from the image: first %u, restart %u",
                  static_cast<unsigned>(seen[0]), static_cast<unsigned>(seen[1]));
        TAP_CHECK(seen[0] == DT_A); // not main's DT_B
        TAP_CHECK(seen[1] == DT_A);
    }

    // Absolute words the link stores in the app's own data. Volatile, so each is read from
    // memory and not folded from its initializer.
    uint32_t g_reloc_target = 0x5eedu;
    uint32_t* volatile g_reloc_data_ptr = &g_reloc_target;
    __attribute__((noinline)) uint32_t reloc_text_target()
    {
        return g_reloc_target + 1u;
    }
    uint32_t (*volatile g_reloc_text_ptr)() = &reloc_text_target;
    uint32_t volatile g_ctor_ran = 0;
    struct CtorWitness
    {
        CtorWitness()
        {
            g_ctor_ran = 1;
        }
    };
    CtorWitness g_ctor_witness;

    // The app's initialized pointers and constructor table hold link-time addresses the load
    // relocated. Each must equal the address the running code computes for the same object,
    // which on x86_64 is the app window's alias and not the kernel's view of it.
    void t_app_pointers_relocated()
    {
        uint32_t* const data = g_reloc_data_ptr;
        uint32_t (*const text)() = g_reloc_text_ptr;
        tap::diag("data word 0x%lx, text word 0x%lx",
                  static_cast<unsigned long>(reinterpret_cast<uintptr_t>(data)),
                  static_cast<unsigned long>(reinterpret_cast<uintptr_t>(text)));
        TAP_CHECK(g_ctor_ran == 1u);
        TAP_CHECK(data == &g_reloc_target);
        TAP_CHECK(text == &reloc_text_target);
        if (data == &g_reloc_target and text == &reloc_text_target)
        {
            TAP_CHECK(*data == 0x5eedu);
            TAP_CHECK(text() == 0x5eeeu);
        }
    }
#endif
}
