// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The address-space arms: the map editor, frame capabilities, processes, and the stack as
// frames.

#include "selftest.h"

#include <malloc.h>
#include <stdlib.h>

#include <kickos/config.h> // KICKOS_MAX_SPAWN_GRANTS

#if KICKOS_HAVE_ASPACE
#include <kickos/sys/image.h>
#endif
#if KICKOS_HAVE_ASPACE and defined(__x86_64__)
#include <kickos/arch/program_base.h>
#endif

namespace selftest
{
#if KICKOS_HAVE_ASPACE
    constexpr uint32_t MEM_UNREAD = 0xFFFFFFFFu;

    // A semaphore word in the last bytes of a block both ends map: one frame, one key.
    kos_sem_t* block_sem(void* blk, uint32_t bytes)
    {
        return reinterpret_cast<kos_sem_t*>(static_cast<char*>(blk) + bytes - sizeof(kos_sem_t));
    }

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
        (void)kos_send(ep, &mark, sizeof(mark), KOS_TIMEOUT_NONE);
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
        kos::thread::Handle w = kos::thread::create_caps(park_witness, nullptr, "parkw",
                                                         TAP_PRIO_AFTER, nullptr, 0,
                                                         KOS_POLICY_FIFO, 0, 0, nullptr, nullptr,
                                                         0, TAP_PIN_CORE);
        if (not w.valid())
        {
            return false;
        }
        if (thread_end(&w) == 0)
        {
            return true;
        }
        (void)w.cancel(KOS_CANCEL_SLAY, ARM_SLAY_US);
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
    extern "C" char _sdata[];
    extern "C" char __kickos_ram_end[];
    extern "C" char __kickos_rom_start[];
    extern "C" char _estack[];
    // Words of the kernel's half from the board's layout: the first word of its writable state
    // and the lowest word of the boot core's trap stack, and the bounds of the kernel's image
    // and RAM. Held in data, so each is an absolute word and not a PC-relative reference the
    // app's code model cannot reach.
    char* volatile g_kernel_words[] = {_sdata, __kickos_ram_end};
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
            return "the kernel's .data";
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
    constexpr int CH_SHARE_FRAME = KOS_SPAWN_DELEGATED_CAP0;
    constexpr uint32_t SHARE_A = 0xC3A11CE0u; // written by main, read by the child
    constexpr uint32_t SHARE_B = 0xC3B00B1Eu; // written by the child, read by main
    // `arg` is where it maps the frame: a DIFFERENT address from main's, the holder choosing.
    void share_child(void* arg) // caps: frame@0
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

    constexpr int CH_PIN_FRAME = KOS_SPAWN_DELEGATED_CAP0 + 1;
    // Never posted: a task waits on its own copy.
    kos_sem_t g_as_never = KOS_SEM_INIT(0);
    void pin_child(void*) // caps: E(SIGNAL)@0, frame@1
    {
        kos_cap_t space = KOS_CAP_NONE;
        uintptr_t va = 0;
        int32_t rc = kos_aspace_self(&space);
        if (rc == 0)
        {
            rc = kos_frame_map(CH_PIN_FRAME, space, &va, 0);
        }
        kos_handle_close(CH_PIN_FRAME); // the mapping is now this task's only hold
        (void)kos_send(child_cap(0), &rc, sizeof(rc), KOS_TIMEOUT_NONE);
        // Alive until slain: the space gives the run's mapping back at its task's death.
        kos_sem_wait(&g_as_never, KOS_TIMEOUT_NONE);
    }

    kos_sem_t g_ssr_gate = KOS_SEM_INIT(0);
    void ssr_worker(void*)
    {
        kos_sem_wait(&g_ssr_gate, KOS_TIMEOUT_NONE);
    }

    constexpr unsigned SSR_CYCLES = 8;

    // A thread stack takes a range slot, so a slot leaked at thread exit exhausts the list.
    void t_stack_slot_returns()
    {
        TAP_ASK(.workers = 1);
        uint32_t const free0 = mem_count(KOS_MEM_RANGES_FREE);
        TAP_CHECK(free0 != MEM_UNREAD and free0 != 0);
        kos::thread::Handle w;
        ArmHold hold;
        TAP_HOLD(hold.thread(&w));
        TAP_CHECK(kos_sem_init(&g_ssr_gate, 0) == 0);
        uint32_t live_low = free0;
        for (unsigned i = 0; i < SSR_CYCLES; i++)
        {
            w = kos::thread::create_caps(ssr_worker, nullptr, "ssr", 10, nullptr, 0);
            TAP_CHECK(w.valid());
            uint32_t const live = mem_count(KOS_MEM_RANGES_FREE);
            if (live < live_low)
            {
                live_low = live;
            }
            kos_sem_post(&g_ssr_gate);
            TAP_CHECK(thread_end(&w) == 0);
        }
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
    constexpr int CH_PS_EP = KOS_SPAWN_DELEGATED_CAP0;
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
        kos_program_t prog = KOS_CAP_NONE;
        (void)kos_program_find("stpeer", &prog);
        char const* nop[] = {"peer", "nop", nullptr};
        for (unsigned pages = 1; pages <= 32u; pages++)
        {
            kos_task_t t = KOS_TASK_NONE;
            kos_task_params p{};
            p.argv = nop;
            p.block = reinterpret_cast<void*>(guard);
            p.size = static_cast<uint32_t>(pages * g);
            last = kos_task_create(prog, &p, 0, &t);
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
            kos_task_params p{};
            p.argv = nop;
            p.block = mine;
            p.size = SNP_BLK;
            rep->w[4] = kos_task_create(prog, &p, 0, &ok);
            rep->w[5] = kos_task_slay(ok, STALL_TOLERANT_US);
        }
        (void)kos_handle_close(prog);
    }

    void pool_stack_worker(void* arg) // caps: E(SIGNAL)@0
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
        (void)kos_send(CH_PS_EP, &rep, sizeof(rep), KOS_TIMEOUT_NONE);
        uint32_t step = *reinterpret_cast<volatile uint32_t*>(rep.guard + g);
        (void)kos_send(CH_PS_EP, &step, sizeof(step), KOS_TIMEOUT_NONE);
        step = *reinterpret_cast<volatile uint32_t*>(rep.guard);
        // unreachable: the guard has no frame
        (void)kos_send(CH_PS_EP, &step, sizeof(step), KOS_TIMEOUT_NONE);
    }
#if KICKOS_SELFTEST_PEER
    // The worker on a stack the kernel placed, which the peer's entry's argv block is not: a
    // thread of the peer's task, holding the task authority its control creates need.
    int pool_stack_task(char const* body) // caps: E(SIGNAL | TRANSFER)@0
    {
        kos_cap_grant const caps[] = {{child_cap(0), KOS_CAP_SIGNAL}};
        auto const w = kos::thread::create_caps(pool_stack_worker,
                                                reinterpret_cast<void*>(peer_word(body)), "pstk",
                                                10, caps, 1, KOS_POLICY_FIFO, 0,
                                                KOS_AUTH_MEMORY | KOS_AUTH_TASKS | KOS_AUTH_IRQ);
        if (not w.valid())
        {
            return 1;
        }
        return w.join(STALL_TOLERANT_US);
    }
#endif

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
        kos_cap_grant const caps[] = {{ep, KOS_CAP_SIGNAL | KOS_CAP_TRANSFER}};
        PeerHex const which = peer_hex(static_cast<uintptr_t>(body));
        TAP_CHECK(PeerRun("pstk", which.s).caps(caps, 1).start(&t) == 0);
        run->spawned = true;
        run->reported = report_await(ep, &run->rep, sizeof(run->rep));
        uint32_t step = 0;
        run->low_read = run->reported and report_await(ep, &step, sizeof(step));
        run->joined = 0;
        if (peer_dead_status(t) == PEER_UNDEAD)
        {
            run->joined = -KOS_ETIMEDOUT;
        }
        run->status_rc = kos_task_state(t, &run->status);
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
        return run.reported and run.low_read and run.joined == 0 and state_ended(run.status_rc)
               and run.status == KOS_EXIT_FAULT;
    }

    void t_stack_grant_refused()
    {
        TAP_ASK(.tasks = 1, .endpoints = 1, .peer_workers = 1);
        PsRun run;
        on_pool_stack(PoolStackBody::GRANT_REFUSED, &run);
        TAP_CHECK(run.spawned);
        int32_t const* const w = run.rep.w;
        tap::diag("guard grants %d %d, stack retype %d, again %d; controls %d %d %d",
                  static_cast<int>(w[0]), static_cast<int>(w[1]), static_cast<int>(w[2]),
                  static_cast<int>(w[7]), static_cast<int>(w[4]), static_cast<int>(w[5]),
                  static_cast<int>(w[6]));
        TAP_CHECK(ps_guard_faulted(run));
        TAP_CHECK(w[0] == -KOS_EPERM and w[1] == -KOS_EPERM);
        TAP_CHECK(w[3] == 1);
        TAP_CHECK(w[4] == 0 and w[6] == 0);
        TAP_CHECK(w[7] == -KOS_EPERM);
        // A board whose RAM takes no uncached type refuses the typed retype by its type first.
        if (w[5] == -KOS_ENOTSUP)
        {
            TAP_CHECK(w[2] == -KOS_ENOTSUP);
            tap::partial("typed stack retype refused by its type (no uncached RAM)");
            return;
        }
        TAP_CHECK(w[2] == -KOS_EPERM and w[5] == 0);
    }

    void t_stack_handoff_refused()
    {
        // The worker's control task beside its own.
        TAP_ASK(.tasks = 2, .endpoints = 1, .peer_workers = 1);
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
        TAP_ASK(.workers = 1, .tasks = 1, .endpoints = 1);
        uint32_t const free0 = mem_count(KOS_MEM_FRAMES_FREE);
        uint32_t const spaces0 = mem_count(KOS_MEM_SPACES_HELD);
        TAP_CHECK(free0 != MEM_UNREAD and spaces0 != MEM_UNREAD);
        kos_cap_t fcap = KOS_CAP_NONE;
        kos_cap_t ep = KOS_CAP_NONE;
        kos_task_t t = KOS_TASK_NONE;
        ArmHold hold;
        TAP_HOLD(hold.cap(&fcap) and hold.cap(&ep) and hold.task(&t));
        TAP_CHECK(kos_frame_create(ASPACE_GRANULE, &fcap) == 0);
        TAP_CHECK(mem_count(KOS_MEM_FRAMES_FREE) == free0 - 1u);
        TAP_CHECK(kos_endpoint_create(&ep) == 0);
        kos_cap_grant caps[] = {{ep, KOS_CAP_SIGNAL}, {fcap, KOS_CAP_TRANSFER}};
        TAP_CHECK(PeerRun("pin").caps(caps, 2).start(&t) == 0);
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
        TAP_ASK(.tasks = 1, .peer_workers = 1);
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
        uint32_t const unmapped = mem_count(KOS_MEM_FRAMES_FREE);
        TAP_CHECK(kos_frame_map(s.frame, s.space, &va, 0) == 0 and va != 0);
        volatile uint32_t* const mine = reinterpret_cast<volatile uint32_t*>(va);
        mine[0] = SHARE_A;
        mine[1] = 0;
        mine[2] = 0;
        mine[3] = 0;
        uint32_t const mapped = mem_count(KOS_MEM_FRAMES_FREE);

        // A task of its OWN, carrying no block.
        // Only TRANSFER is valid; neither capability type carries access rights.
        kos_cap_grant caps[] = {{s.frame, KOS_CAP_TRANSFER}};
        PeerHex const at = peer_hex(reinterpret_cast<uintptr_t>(theirs_at));
        // The negative control below omits this authority.
        TAP_CHECK(PeerRun("shr", at.s).caps(caps, 1).start(&t) == 0);
        TAP_CHECK(peer_dead_status(t) == 0);
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
        TAP_CHECK(PeerRun("shrN", at.s).caps(caps, 1).start(&t2) == 0);
        TAP_CHECK(peer_dead_status(t2) == 0);
        TAP_CHECK(mine[2] == 0); // it reached the map and was refused
        TAP_CHECK(task_end(&t2) == 0);
        TAP_CHECK(mem_count(KOS_MEM_SPACES_HELD) == spaces0);

        TAP_CHECK(kos_frame_unmap(s.frame, s.space, va) == 0);
        va = 0;
        seed_close(&s);
        // The frames come back only once the LAST capability naming the run is gone.
        TAP_CHECK(mem_count(KOS_MEM_FRAMES_FREE) == unmapped + 1u);
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
    // Its task outlives the one that made it, so nobody can slay it: main signals its gate on
    // every path once it may exist, and its other waits bound themselves. The gate and `held`
    // are notifications: they cross three tasks.
    void sd_grandchild(void*) // caps: report E(SIGNAL)@0, gate@1, run@2, space@3, held@4
    {
        (void)kos_notify_bind(child_cap(1));
        uintptr_t at = 0;
        int32_t rep = kos_frame_map(child_cap(2), child_cap(3), &at, 0);
        if (rep == 0)
        {
            rep = kos_frame_unmap(child_cap(2), child_cap(3), at);
        }
        (void)kos_notify(child_cap(4));
        (void)kos_send(child_cap(0), &rep, sizeof(rep), STALL_TOLERANT_US);
        uint32_t bits = 0;
        (void)kos_notify_wait(child_cap(1), ~0u, KOS_TIMEOUT_NONE, &bits);
        at = 0;
        rep = kos_frame_map(child_cap(2), child_cap(3), &at, 0);
        (void)kos_handle_close(child_cap(2));
        (void)kos_handle_close(child_cap(3));
        // A call, so main counts the run's frames while this thread is parked and not exiting.
        (void)kos_call(child_cap(0), &rep, sizeof(rep), sizeof(rep), STALL_TOLERANT_US);
    }

    // The grandchild is a peer of the child's making.
    void sd_child(void*) // caps: report E(SIGNAL)@0, gate@1, run@2
    {
        kos_cap_t space = KOS_CAP_NONE;
        kos_cap_t held = KOS_CAP_NONE;
        kos_program_t prog = KOS_CAP_NONE;
        if (kos_aspace_self(&space) != 0 or kos_notify_create(&held) != 0
            or kos_notify_bind(held) != 0 or kos_program_find("stpeer", &prog) != 0)
        {
            return;
        }
        kos_cap_grant caps[] = {{child_cap(0), KOS_CAP_SIGNAL},
                                {child_cap(1), KOS_CAP_WAIT},
                                {child_cap(2), KOS_CAP_TRANSFER},
                                {space, KOS_CAP_TRANSFER},
                                {held, KOS_CAP_SIGNAL}};
        char const* argv[] = {"peer", "sdg", nullptr};
        kos_task_params p{};
        p.argv = argv;
        p.caps = caps;
        p.cap_count = 5;
        kos_task_t t = KOS_TASK_NONE;
        if (kos_task_create(prog, &p, 0, &t) == 0)
        {
            uint32_t bits = 0;
            (void)kos_notify_wait(held, ~0u, KOS_TIMEOUT_NONE, &bits);
        }
    }

    // Woken by the creator's watch of `t`, which is raised at its end and again at its death.
    bool task_dead(kos_task_t t, kos_cap_t note)
    {
        uint64_t const deadline = kos_clock_now() + uint64_t{STALL_TOLERANT_US} * 1000u;
        while (true)
        {
            int const state = kos_task_state(t, nullptr);
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
        TAP_ASK(.tasks = 2, .endpoints = 1, .notifies = 2);
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
        TAP_CHECK(kos_notify_create(&gate) == 0);
        TAP_CHECK(kos_frame_create(MINT_BYTES, &run) == 0);
        TAP_CHECK(kos_notify_create(&note) == 0 and kos_notify_bind(note) == 0);
        kos_cap_grant caps[] = {{ep, KOS_CAP_SIGNAL | KOS_CAP_TRANSFER},
                                {gate, KOS_CAP_WAIT | KOS_CAP_TRANSFER},
                                {run, KOS_CAP_TRANSFER}};
        TAP_CHECK(PeerRun("sdc").caps(caps, 3).start(&t) == 0);
        TAP_CHECK(kos_task_watch(t, note, KOS_CAP_NONE) == 0);
        // From here no return until the grandchild is released.
        int32_t alive = 1;
        int32_t dead = 1;
        bool const heard_alive = report_await(ep, &alive, sizeof(alive));
        // The task stays held by main, so its domain outlives its death.
        bool const ended = heard_alive and task_dead(t, note);
        (void)kos_notify(gate);
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
        int const typed = kos_mem_self_grant(blk, SNP_BLK, KOS_MEM_NOCACHE);
        TAP_CHECK(kos_mem_self_grant(blk, SNP_BLK, KOS_MEM_NOCACHE << 1) == -KOS_EINVAL);
        TAP_CHECK(kos_mem_self_grant(blk, SNP_BLK, 0) == 0);
        if (typed == -KOS_ENOTSUP)
        {
            tap::partial("no uncached retype (no uncached RAM)");
            return;
        }
        TAP_CHECK(typed == 0);
    }

    // One pool balance across task creation and teardown.
    // Minimum expected cost: one root, two image tables and one private data page.
    constexpr uint32_t CHURN_MIN_FRAMES = 4;

    // `held` is read with the space ALIVE and before its entry runs, so it is the space's own
    // cost.
    bool churn_cycle(uint32_t* held)
    {
        kos_task_t t = KOS_TASK_NONE;
        if (PeerRun("nop").start(&t) != 0)
        {
            return false;
        }
        if (held != nullptr)
        {
            *held = mem_count(KOS_MEM_FRAMES_FREE);
        }
        bool const joined = peer_dead_status(t) == 0;
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

    kos_sem_t g_sframe_gate = KOS_SEM_INIT(0);
    void sframe_worker(void*) // caps: E(SIGNAL)@0
    {
        reached(child_cap(0));
        kos_sem_wait(&g_sframe_gate, KOS_TIMEOUT_NONE); // held alive while main reads the pool
    }
    void t_stack_is_frames()
    {
        TAP_ASK(.workers = 1, .endpoints = 1);
        kos_cap_t ep = KOS_CAP_NONE;
        kos::thread::Handle w;
        ArmHold hold;
        TAP_HOLD(hold.cap(&ep) and hold.thread(&w));
        TAP_CHECK(kos_endpoint_create(&ep) == 0 and kos_sem_init(&g_sframe_gate, 0) == 0);
        kos_cap_grant caps[] = {{ep, KOS_CAP_SIGNAL}};
        uint32_t before = 0;
        uint32_t live = 0;
        // The first cycle warms up: intermediate tables are retained after the first mapping.
        for (int i = 0; i < 2; i++)
        {
            before = mem_count(KOS_MEM_FRAMES_FREE);
            w = kos::thread::create_caps(sframe_worker, nullptr, "sfram", 10, caps, 1);
            TAP_CHECK(w.valid());
            TAP_CHECK(heard_reached(ep));
            live = mem_count(KOS_MEM_FRAMES_FREE);
            kos_sem_post(&g_sframe_gate);
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
        SP_GATE = 2,  // the worker's gate, a semaphore word
        SP_WORDS = 3
    };
    volatile uint32_t g_sp_word = 0u;
    constexpr uint32_t SP_MAIN = 0x5BACEu;
    constexpr uint32_t SP_A = 0xA11CEu;
    constexpr uint32_t SP_B = 0xB0B0Bu;
    kos_sem_t* sp_gate(volatile uint32_t* slot)
    {
        return reinterpret_cast<kos_sem_t*>(const_cast<uint32_t*>(&slot[SP_GATE]));
    }
    void space_word_worker(void* arg) // caps: E(SIGNAL)@0
    {
        volatile uint32_t* const slot = static_cast<volatile uint32_t*>(arg);
        g_sp_word = slot[SP_SEED];
        reached(child_cap(0));
        kos_sem_wait(sp_gate(slot), KOS_TIMEOUT_NONE); // both have written by now
        slot[SP_READ] = g_sp_word;
    }

    void t_aspace_two_spaces_same_grant()
    {
        // Two instances of one program over one block.
        TAP_ASK(.tasks = 2, .endpoints = 1);
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
        kos_task_t w[2] = {KOS_TASK_NONE, KOS_TASK_NONE};
        ArmHold hold;
        TAP_HOLD(hold.cap(&ep) and hold.task(&w[0]) and hold.task(&w[1]));
        TAP_CHECK(kos_endpoint_create(&ep) == 0);
        TAP_CHECK(kos_sem_init(sp_gate(&slot[0]), 0) == 0
                  and kos_sem_init(sp_gate(&slot[SP_WORDS]), 0) == 0);
        kos_cap_grant caps[] = {{ep, KOS_CAP_SIGNAL}};
        for (int i = 0; i < 2; i++)
        {
            PeerHex const at = peer_hex(reinterpret_cast<uintptr_t>(&slot[i * SP_WORDS]));
            TAP_CHECK(PeerRun("spw", at.s).caps(caps, 1).block(shared, SP_BLK).start(&w[i]) == 0);
        }
        TAP_CHECK(heard_reached(ep) and heard_reached(ep));
        kos_sem_post(sp_gate(&slot[0]));
        kos_sem_post(sp_gate(&slot[SP_WORDS]));
        TAP_CHECK(peer_dead_status(w[0]) == 0 and peer_dead_status(w[1]) == 0);
        tap::diag("one grant, two spaces: read back 0x%x and 0x%x, main 0x%x",
                  static_cast<unsigned>(slot[SP_READ]),
                  static_cast<unsigned>(slot[SP_WORDS + SP_READ]),
                  static_cast<unsigned>(g_sp_word));
        TAP_CHECK(slot[SP_READ] == SP_A and slot[SP_WORDS + SP_READ] == SP_B);
        TAP_CHECK(g_sp_word == SP_MAIN);
    }

    // One virtual address, a private copy behind it in each instance of one program. Each
    // member reports through its own block; private globals cannot report to main.
    enum
    {
        PW_ADDR = 0, // the member's own &g_pw_word
        PW_READBACK = 1,
        PW_SEED = 2, // main's, read by the member: the value only that member writes
        PW_WORDS = 3
    };
    volatile uint32_t g_pw_word = 0u;
    void pw_member(void* arg) // caps: E(SIGNAL)@0; the gate ends its block
    {
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(arg);
        uint32_t const mine = static_cast<uint32_t>(out[PW_SEED]);
        out[PW_ADDR] = reinterpret_cast<uintptr_t>(&g_pw_word);
        g_pw_word = mine;
        reached(child_cap(0));
        // both members have written by now
        kos_sem_wait(block_sem(arg, PW_RAM.size[0]), KOS_TIMEOUT_NONE);
        out[PW_READBACK] = g_pw_word;
    }
    void t_process_private_data()
    {
        TAP_ASK(.tasks = 2, .endpoints = 1);
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
        kos_task_t ta = KOS_TASK_NONE;
        kos_task_t tb = KOS_TASK_NONE;
        ArmHold hold;
        TAP_HOLD(hold.cap(&ep) and hold.task(&ta) and hold.task(&tb));
        TAP_CHECK(kos_endpoint_create(&ep) == 0);
        TAP_CHECK(kos_sem_init(block_sem(ba, PW_BLK), 0) == 0
                  and kos_sem_init(block_sem(bb, PW_BLK), 0) == 0);
        kos_cap_grant caps[] = {{ep, KOS_CAP_SIGNAL}};
        PeerHex const at_a = peer_hex(reinterpret_cast<uintptr_t>(ba));
        PeerHex const at_b = peer_hex(reinterpret_cast<uintptr_t>(bb));
        TAP_CHECK(PeerRun("pw", at_a.s).caps(caps, 1).block(ba, PW_BLK).start(&ta) == 0);
        TAP_CHECK(PeerRun("pw", at_b.s).caps(caps, 1).block(bb, PW_BLK).start(&tb) == 0);
        TAP_CHECK(heard_reached(ep) and heard_reached(ep)); // both have written their own copy
        kos_sem_post(block_sem(ba, PW_BLK));
        kos_sem_post(block_sem(bb, PW_BLK));
        // Both have read it back.
        TAP_CHECK(peer_dead_status(ta) == 0 and peer_dead_status(tb) == 0);
        TAP_CHECK(task_end(&ta) == 0 and task_end(&tb) == 0);
        uintptr_t const own = reinterpret_cast<uintptr_t>(&g_pw_word);
        tap::diag("process: read back 0x%x and 0x%x at 0x%lx, main 0x%x",
                  static_cast<unsigned>(oa[PW_READBACK]), static_cast<unsigned>(ob[PW_READBACK]),
                  static_cast<unsigned long>(own), static_cast<unsigned>(g_pw_word));
        // One program twice: one address in both, another program's elsewhere.
        TAP_CHECK(oa[PW_ADDR] == ob[PW_ADDR] and oa[PW_ADDR] != own);
        // Each read back its OWN write, after the other had written the same address.
        TAP_CHECK(oa[PW_READBACK] == PW_A and ob[PW_READBACK] == PW_B);
        TAP_CHECK(g_pw_word == 0x600Du); // and main's copy was untouched by either
    }

    // Two task siblings share one image. The writer, the task's entry, starts the reader once
    // it has written and holds the task open until main releases it: its exit ends the task.
    volatile uint32_t g_sib_word = 0u;
    void sib_reader(void* arg)
    {
        static_cast<volatile uint64_t*>(arg)[1] = g_sib_word;
    }
    int sib_writer(char const* block) // caps: E(SIGNAL)@0; the park ends the task's block
    {
        void* const arg = reinterpret_cast<void*>(peer_word(block));
        g_sib_word = 0xBEEFu;
        static_cast<volatile uint64_t*>(arg)[0] = 1u;
        auto const reader = kos::thread::create(sib_reader, arg, "sibR", 10);
        if (not reader.valid() or reader.join(STALL_TOLERANT_US) != 0)
        {
            return 1;
        }
        reached(child_cap(0));
        kos_sem_wait(block_sem(arg, SIB_RAM.size[0]), KOS_TIMEOUT_NONE);
        return 0;
    }
    void t_task_siblings_share()
    {
        TAP_ASK(.tasks = 1, .endpoints = 1, .peer_workers = 1);
        constexpr uint32_t SIB_BLK = SIB_RAM.size[0];
        void* const blk = st_ram<SIB_RAM, 0>();
        TAP_CHECK(blk != nullptr);
        TAP_CHECK(kos_mem_self_grant(blk, SIB_BLK, 0) == 0);
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(blk);
        out[0] = 0;
        out[1] = 0;
        g_sib_word = 0u;
        kos_task_t t = KOS_TASK_NONE;
        kos_sem_t* const park = block_sem(blk, SIB_BLK);
        kos_cap_t ep = KOS_CAP_NONE;
        ArmHold hold;
        TAP_HOLD(hold.task(&t) and hold.cap(&ep));
        TAP_CHECK(kos_sem_init(park, 0) == 0);
        TAP_CHECK(kos_endpoint_create(&ep) == 0);
        kos_cap_grant caps[] = {{ep, KOS_CAP_SIGNAL}};
        PeerHex const at = peer_hex(reinterpret_cast<uintptr_t>(blk));
        TAP_CHECK(PeerRun("sib", at.s).caps(caps, 1).block(blk, SIB_BLK).start(&t) == 0);
        // The reader has read once the writer reports.
        TAP_CHECK(heard_reached(ep));
        kos_sem_post(park);
        TAP_CHECK(peer_dead_status(t) == 0);
        TAP_CHECK(task_end(&t) == 0);
        TAP_CHECK(out[0] == 1u);          // the writer ran
        TAP_CHECK(out[1] == 0xBEEFu);     // and its sibling saw the store: one image, one group
        TAP_CHECK(g_sib_word == 0u);      // main did not: a different group is a different copy
    }

    // Handoff at the same address through the task's block: the borrower reads what main wrote
    // and its echo reaches main through the same bytes, so one frame sits under both spaces.
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
        ArmHold hold;
        TAP_HOLD(hold.task(&t) and hold.task(&tt));
        PeerHex const at = peer_hex(reinterpret_cast<uintptr_t>(blk));
        TAP_CHECK(PeerRun("ho", at.s).block(blk, HO_BLK).start(&t) == 0);
        TAP_CHECK(peer_dead_status(t) == 0);
        TAP_CHECK(task_end(&t) == 0);
        TAP_CHECK(out[HO_SEEN] == HO_SENTINEL + 1u);   // the child read what main wrote
        TAP_CHECK(out[HO_ADDR] == reinterpret_cast<uintptr_t>(blk)); // at the address main named
        // A nondefault type on a separate block.
        void* const typed = st_ram<HO_RAM, 1>();
        int const typed_rc = kos_mem_self_grant(typed, HO_BLK, KOS_MEM_NOCACHE);
        if (typed_rc == -KOS_ENOTSUP)
        {
            // Both consumers refuse the type alike.
            TAP_CHECK(kos_mem_self_grant(typed, HO_BLK, 0) == 0);
            TAP_CHECK(PeerRun("ho", peer_hex(reinterpret_cast<uintptr_t>(typed)).s)
                          .block(typed, HO_BLK, KOS_MEM_NOCACHE)
                          .start(&tt)
                      == -KOS_ENOTSUP);
            tap::partial("typed handoff refused (no uncached RAM)");
            return;
        }
        TAP_CHECK(typed_rc == 0);
        volatile uint64_t* const tout = static_cast<volatile uint64_t*>(typed);
        tout[HO_SEEN] = HO_SENTINEL;
        tout[HO_ADDR] = 0;
        PeerHex const typed_at = peer_hex(reinterpret_cast<uintptr_t>(typed));
        TAP_CHECK(PeerRun("ho", typed_at.s).block(typed, HO_BLK, KOS_MEM_NOCACHE).start(&tt) == 0);
        TAP_CHECK(peer_dead_status(tt) == 0);
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
        PeerHex const at = peer_hex(reinterpret_cast<uintptr_t>(upper));
        int const rc = PeerRun("hsl", at.s).block(upper, static_cast<uint32_t>(g)).start(&t);
        if (rc == 0)
        {
            (void)peer_dead_status(t);
            (void)task_end(&t);
            tap::diag("interior handoff admitted; the borrower read 0x%x below its page",
                      static_cast<unsigned>(out[SL_ECHO]));
        }
        TAP_CHECK(rc == -KOS_EPERM);
        // Control: accept the same reservation at its actual base.
        TAP_CHECK(PeerRun("nop").block(blk, two).start(&whole) == 0);
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
    constexpr int DX_EP = KOS_SPAWN_DELEGATED_CAP0; // the donor's and the borrower's endpoint index
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
    void dx_borrower(void* arg) // caps: ep(WAIT)@0
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
    void dx_donor(void* arg) // caps: ep(WAIT|TRANSFER)@0; arg is main's report block
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
        // The borrower, a peer of the donor's making over the donor's own reservation.
        kos_program_t prog = KOS_CAP_NONE;
        if (kos_program_find("stpeer", &prog) != 0)
        {
            return;
        }
        PeerHex const at = peer_hex(reinterpret_cast<uintptr_t>(blk));
        char const* argv[] = {"peer", "dxB", at.s, nullptr};
        kos_cap_grant caps[] = {{DX_EP, KOS_CAP_WAIT}};
        kos_task_params p{};
        p.argv = argv;
        p.block = blk;
        p.size = DX_BLK;
        p.caps = caps;
        p.cap_count = 1;
        kos_task_t tb = KOS_TASK_NONE;
        int const created = kos_task_create(prog, &p, 0, &tb);
        (void)kos_handle_close(prog);
        if (created != 0)
        {
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
        TAP_ASK(.tasks = 2, .endpoints = 1);
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
        // TRANSFER lets the donor delegate the endpoint to the borrower.
        kos_cap_grant dcaps[] = {{ep, static_cast<uint8_t>(KOS_CAP_WAIT | KOS_CAP_TRANSFER)}};
        PeerHex const dat = peer_hex(reinterpret_cast<uintptr_t>(dblk));
        TAP_CHECK(PeerRun("dxD", dat.s).caps(dcaps, 1).block(dblk, DX_BLK).start(&td) == 0);
        // The donor has exited before any frame churn begins. From here no return until the
        // borrower has been called: its task is no handle of main's.
        bool const donor_gone = peer_dead_status(td) == 0;
        // Drop main's creator hold before the churn, or it hides a missing borrower reference.
        bool const donor_reaped = task_end(&td) == 0;
        bool const seated = dout[DX_STATUS] == 1u;
        uint64_t const held_after = mem_count(KOS_MEM_SPACES_HELD);

        PeerHex const cat = peer_hex(reinterpret_cast<uintptr_t>(cblk));
        bool churn_ran = PeerRun("dxC", cat.s).block(cblk, DX_CBLK).start(&tc) == 0;
        if (churn_ran)
        {
            churn_ran = peer_dead_status(tc) == 0;
        }
        bool const churn_reaped = task_end(&tc) == 0;
        uint64_t const churned = cout[CX_TAKEN];

        // Release and receive in one call.
        char msg[sizeof(DxReport)] = {};
        int32_t const n = kos_call(ep, msg, sizeof(msg), sizeof(msg), STALL_TOLERANT_US);
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
        // The donor's space ends with its task; the borrower keeps only the block it borrowed.
        TAP_CHECK(held_after + 1u == dout[DX_HELD]);
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
    void rt_member(void*) // caps: E(SIGNAL)@0
    {
        uint64_t rep[RT_WORDS] = {0, 0};
        uintptr_t const g = ASPACE_GRANULE;
        // Leave the reservation unmapped for teardown.
        void* const blk = st_ram_own(static_cast<size_t>(RT_PAGES * g));
        rep[RT_ADDR] = reinterpret_cast<uintptr_t>(blk);
        rep[RT_FREE] = mem_count(KOS_MEM_FRAMES_FREE);
        (void)kos_send(child_cap(0), rep, sizeof(rep), KOS_TIMEOUT_NONE);
    }
    bool rt_cycle(kos_cap_t ep, uint64_t* rep)
    {
        kos_task_t t = KOS_TASK_NONE;
        kos_cap_grant caps[] = {{ep, KOS_CAP_SIGNAL}};
        bool const started = PeerRun("rt").caps(caps, 1).start(&t) == 0;
        // The member parks in its send until this receive arrives, so the report is read
        // while the reservation is still out.
        bool const heard = started and report_await(ep, rep, sizeof(uint64_t) * RT_WORDS);
        bool const joined = started and peer_dead_status(t) == 0;
        // main's creator hold keeps the dead task's space alive.
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
    void fs_member(void*) // caps: E(SIGNAL)@0
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
        (void)kos_send(child_cap(0), rep, sizeof(rep), KOS_TIMEOUT_NONE);
    }
    bool fs_cycle(kos_cap_t ep, uint64_t* rep)
    {
        kos_task_t t = KOS_TASK_NONE;
        kos_cap_grant caps[] = {{ep, KOS_CAP_SIGNAL}};
        bool const started = PeerRun("fs").caps(caps, 1).start(&t) == 0;
        // The member parks in its send until this receive arrives, and the frames go back
        // only once the group is reaped below.
        bool const heard = started and report_await(ep, rep, sizeof(uint64_t) * FS_WORDS);
        bool const joined = started and peer_dead_status(t) == 0;
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

    // A member at its task's THREADS budget is refused with the budget's code, never with an
    // exhausted pool: the pool holds every task's budget, so a create of another task still
    // finds its entry thread's slot. That task's slay then gives back its space and its frames.
    constexpr int LR_PARK_CAP = 24;
    enum
    {
        LR_RC = 0,      // what the create beside the refusal answered, negated
        LR_PARKED = 1,  // workers seated before the budget refused
        LR_SPACES0 = 2, // spaces held and frames free, before the create and after its slay
        LR_SPACES1 = 3,
        LR_FRAMES0 = 4,
        LR_FRAMES1 = 5,
        LR_REFUSAL = 6, // what the refused spawn answered, negated
        LR_WORDS = 7
    };
    kos_sem_t g_lr_gate = KOS_SEM_INIT(0);
    void lr_parked(void*)
    {
        kos_sem_wait(&g_lr_gate, KOS_TIMEOUT_NONE);
    }
    void lr_member(void*) // caps: E(SIGNAL)@0
    {
        uint64_t rep[LR_WORDS] = {};
        uintptr_t const g = ASPACE_GRANULE;
        uint32_t const bytes = static_cast<uint32_t>(2u * g);
        void* const blk = st_ram_own(bytes);
        if (blk != nullptr and kos_mem_self_grant(blk, bytes, 0) == 0
            and kos_sem_init(&g_lr_gate, 0) == 0)
        {
            kos_thread_t held[LR_PARK_CAP];
            int n = 0;
            // Plain spawns take thread slots of the member's task, sharing its space.
            while (n < LR_PARK_CAP)
            {
                auto h = kos::thread::create_caps(lr_parked, nullptr, "lrprk", 10, nullptr, 0);
                if (not h.valid())
                {
                    rep[LR_REFUSAL] = static_cast<uint64_t>(static_cast<uint32_t>(-h.error()));
                    break;
                }
                held[n] = h.id();
                n++;
            }
            rep[LR_PARKED] = static_cast<uint64_t>(n);
            kos_program_t prog = KOS_CAP_NONE;
            (void)kos_program_find("stpeer", &prog);
            char const* nop[] = {"peer", "nop", nullptr};
            kos_task_params p{};
            p.argv = nop;
            p.block = blk;
            p.size = bytes;
            kos_task_t t = KOS_TASK_NONE;
            rep[LR_SPACES0] = mem_count(KOS_MEM_SPACES_HELD);
            rep[LR_FRAMES0] = mem_count(KOS_MEM_FRAMES_FREE);
            int rc = kos_task_create(prog, &p, 0, &t);
            rep[LR_RC] = static_cast<uint64_t>(static_cast<uint32_t>(-rc));
            if (rc == 0)
            {
                (void)kos_task_slay(t, STALL_TOLERANT_US);
            }
            rep[LR_SPACES1] = mem_count(KOS_MEM_SPACES_HELD);
            rep[LR_FRAMES1] = mem_count(KOS_MEM_FRAMES_FREE);
            (void)kos_handle_close(prog);
            for (int i = 0; i < n; i++)
            {
                (void)kos_sem_post(&g_lr_gate);
            }
            for (int i = 0; i < n; i++)
            {
                (void)kos_thread_join(held[i], STALL_TOLERANT_US);
            }
        }
        (void)kos_send(child_cap(0), rep, sizeof(rep), KOS_TIMEOUT_NONE);
    }
    bool lr_cycle(kos_cap_t ep, uint64_t* rep)
    {
        kos_task_t t = KOS_TASK_NONE;
        kos_cap_grant caps[] = {{ep, KOS_CAP_SIGNAL}};
        bool const started = PeerRun("lrful").caps(caps, 1).start(&t) == 0;
        bool const heard = started and report_await(ep, rep, sizeof(uint64_t) * LR_WORDS);
        bool const joined = started and peer_dead_status(t) == 0;
        bool const reaped = task_end(&t) == 0;
        return heard and joined and reaped;
    }
    void t_spawn_budget_spares_a_create()
    {
        TAP_ASK(.tasks = 2, .endpoints = 1, .peer_workers = 1);
        kos_cap_t ep = KOS_CAP_NONE;
        ArmHold hold;
        TAP_HOLD(hold.cap(&ep));
        TAP_CHECK(kos_endpoint_create(&ep) == 0);
        uint64_t rep[LR_WORDS] = {};
        TAP_CHECK(lr_cycle(ep, rep));
        tap::diag("thread budget: %u parked, refused %u, create %u, spaces %u->%u, frames %u->%u",
                  static_cast<unsigned>(rep[LR_PARKED]), static_cast<unsigned>(rep[LR_REFUSAL]),
                  static_cast<unsigned>(rep[LR_RC]), static_cast<unsigned>(rep[LR_SPACES0]),
                  static_cast<unsigned>(rep[LR_SPACES1]),
                  static_cast<unsigned>(rep[LR_FRAMES0]),
                  static_cast<unsigned>(rep[LR_FRAMES1]));
        TAP_CHECK(rep[LR_PARKED] != 0);
        if (rep[LR_PARKED] == LR_PARK_CAP)
        {
            // At the cap the budget outran this arm, so the refusal it measures never happened.
            tap::skip("the member's thread budget is wider than this arm parks");
            return;
        }
        TAP_CHECK(rep[LR_REFUSAL] == KOS_EAGAIN);
        TAP_CHECK(rep[LR_RC] == 0);
        TAP_CHECK(rep[LR_SPACES0] != MEM_UNREAD and rep[LR_FRAMES0] != MEM_UNREAD);
        TAP_CHECK(rep[LR_SPACES1] == rep[LR_SPACES0]);
        TAP_CHECK(rep[LR_FRAMES1] == rep[LR_FRAMES0]);
    }

    // A failed create must release both the borrower space and its donor reference. The member's
    // own task is the donor; no domain is allocated across the reap, which could hide a stale
    // reference.
    enum
    {
        LD_RC = 0,     // the refused spawn's own answer, negated
        LD_SPACES = 1, // spaces held, read inside the member AFTER the refusal
        LD_FRAMES = 2,
        LD_WORDS = 3
    };
    void ld_member(void*) // caps: E(SIGNAL|TRANSFER)@0
    {
        uint64_t rep[LD_WORDS] = {0, 0, 0};
        uintptr_t const g = ASPACE_GRANULE;
        uint32_t const bytes = static_cast<uint32_t>(2u * g);
        void* const blk = st_ram_own(bytes);
        int rc = 0;
        kos_program_t prog = KOS_CAP_NONE;
        if (blk != nullptr and kos_mem_self_grant(blk, bytes, 0) == 0
            and kos_program_find("stpeer", &prog) == 0)
        {
            kos_cap_grant caps[] = {{child_cap(0), KOS_CAP_SIGNAL}};
            uint16_t const dest = 0x0FFFu; // past any child capability run
            char const* nop[] = {"peer", "nop", nullptr};
            kos_task_params p{};
            p.argv = nop;
            p.block = blk;
            p.size = bytes;
            p.caps = caps;
            p.cap_dest = &dest;
            p.cap_count = 1;
            kos_task_t t = KOS_TASK_NONE;
            rc = kos_task_create(prog, &p, 0, &t);
            if (rc == 0)
            {
                (void)kos_task_slay(t, STALL_TOLERANT_US);
            }
            (void)kos_handle_close(prog);
        }
        rep[LD_RC] = static_cast<uint64_t>(static_cast<uint32_t>(-rc));
        rep[LD_SPACES] = mem_count(KOS_MEM_SPACES_HELD);
        rep[LD_FRAMES] = mem_count(KOS_MEM_FRAMES_FREE);
        (void)kos_send(child_cap(0), rep, sizeof(rep), KOS_TIMEOUT_NONE);
    }
    void t_spawn_refusal_frees_donor()
    {
        // The refused create builds a task of its own before its late refusal.
        TAP_ASK(.workers = 1, .tasks = 2, .endpoints = 1);
        kos_cap_t ep = KOS_CAP_NONE;
        kos_task_t t = KOS_TASK_NONE;
        ArmHold hold;
        TAP_HOLD(hold.cap(&ep) and hold.task(&t));
        TAP_CHECK(kos_endpoint_create(&ep) == 0);
        uint64_t rep[LD_WORDS] = {0, 0, 0};
        // TRANSFER is only for the member to delegate it.
        kos_cap_grant caps[] = {{ep, KOS_CAP_SIGNAL | KOS_CAP_TRANSFER}};
        TAP_CHECK(PeerRun("lddon").caps(caps, 1).start(&t) == 0);
        bool const heard = report_await(ep, rep, sizeof(uint64_t) * LD_WORDS);
        bool const joined = peer_dead_status(t) == 0;
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
#if KICKOS_TLS
    // The TLS seat admits a caller stack of exactly one stride, stride-aligned.
    constexpr uint32_t HOSTILE_STACK = KICKOS_TLS_STRIDE;
#else
    constexpr uint32_t HOSTILE_STACK = 8192;
#endif
    // The victim has private globals, so report through the block shared with main.
    void hostile_victim(void* arg) // caps: E(SIGNAL)@0
    {
        *static_cast<volatile int32_t*>(arg) = kos_shutdown(0, KOS_SHUTDOWN_HALT);
        reached(child_cap(0));
        kos_exit(0);
    }
#if KICKOS_SELFTEST_PEER
    // The sibling, the task's entry, starts the victim below main on main's core on a stack in
    // the task's block at `block`, then scribbles it before it runs.
    int hostile_sibling(char const* block) // caps: E(SIGNAL | TRANSFER)@0
    {
        peer_seat(TAP_PRIO_PARKS, TAP_PIN_CORE);
        uintptr_t const raw = peer_word(block);
        uintptr_t const vbase = (raw + (HOSTILE_STACK - 1u)) & ~uintptr_t{HOSTILE_STACK - 1u};
        auto* const verdict = reinterpret_cast<int32_t*>(raw + 2u * HOSTILE_STACK);
        kos_cap_grant const caps[1] = {{child_cap(0), KOS_CAP_SIGNAL}};
        auto const victim = kos::thread::create(hostile_victim, verdict, "hvic", TAP_PRIO_AFTER,
                                                KOS_POLICY_FIFO, 0,
                                                reinterpret_cast<void*>(vbase), HOSTILE_STACK,
                                                nullptr, 0, caps, 1, 0, nullptr, TAP_PIN_CORE);
        if (not victim.valid())
        {
            return 1;
        }
        uintptr_t const top = vbase + HOSTILE_STACK;
        volatile uint64_t* const w = reinterpret_cast<volatile uint64_t*>(top - HOSTILE_WINDOW);
        for (uint32_t i = 0; i < HOSTILE_WINDOW / sizeof(uint64_t); i++)
        {
            w[i] = HOSTILE_EL1H;
        }
        reached(child_cap(0));
        kos_sem_wait(&g_as_never, KOS_TIMEOUT_NONE);
        return 1; // unreachable: nothing posts that semaphore
    }
#endif
    void parked_frame_hostile_pinned()
    {
        TAP_ASK(.tasks = 1, .endpoints = 1, .peer_workers = 1);
        constexpr uint32_t VSTK = HOSTILE_STACK;
        static_assert(PFH_RAM.size[0] == 3u * VSTK);
        void* const raw = st_ram<PFH_RAM, 0>();
        TAP_CHECK(raw != nullptr);
        // Map main's reservation so it can read the shared verdict.
        TAP_CHECK(kos_mem_self_grant(raw, 3u * VSTK, 0) == 0);
        volatile int32_t* const verdict =
            reinterpret_cast<volatile int32_t*>(reinterpret_cast<uintptr_t>(raw) + 2u * VSTK);
        *verdict = -99;
        kos_cap_t ep = KOS_CAP_NONE;
        kos_task_t task = KOS_TASK_NONE;
        ArmHold hold;
        TAP_HOLD(hold.cap(&ep) and hold.task(&task));
        TAP_CHECK(kos_endpoint_create(&ep) == 0);
        kos_cap_grant const caps[1] = {{ep, KOS_CAP_SIGNAL | KOS_CAP_TRANSFER}};
        PeerHex const at = peer_hex(reinterpret_cast<uintptr_t>(raw));
        TAP_CHECK(PeerRun("hostile", at.s).caps(caps, 1).block(raw, 3u * VSTK).start(&task) == 0);
        TAP_CHECK(heard_reached(ep)); // the scribble is COMPLETE before the victim is let go
        // Below main on main's core, the victim runs once main waits here.
        bool const finished = heard_reached(ep);
        int const rc = *verdict;
        TAP_CHECK(task_end(&task) == 0);
        TAP_CHECK(finished);
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

    void pi_server(void* arg) // caps: E(WAIT)@0
    {
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(arg);
        pi_mine(out, '\0');
        struct kos_reply_recv_opts o;
        kos_reply_recv_opts_init(&o, child_cap(0), 0, KOS_TIMEOUT_NONE);
        int32_t const n = kos_reply_recv(KOS_CAP_NONE, const_cast<char*>(&g_pi_msg[0]),
                                         kos_call_lens_pack(0, PI_MSG), &o);
        *const_cast<kos_recv_info*>(&g_pi_info) = o.info;
        pi_report(out, n, 'A');
    }

    void pi_client(void* arg) // caps: E(SIGNAL)@0
    {
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(arg);
        pi_mine(out, 'A');
        int32_t const n =
            kos_send(child_cap(0), const_cast<char*>(&g_pi_msg[0]), PI_MSG, KOS_TIMEOUT_NONE);
        pi_report(out, n, 'A');
    }

    void pi_call_server(void* arg) // caps: E(WAIT)@0
    {
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(arg);
        pi_mine(out, '\0');
        struct kos_reply_recv_opts o;
        kos_reply_recv_opts_init(&o, child_cap(0), 0, KOS_TIMEOUT_NONE);
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

    void pi_call_client(void* arg) // caps: E(SIGNAL)@0
    {
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(arg);
        pi_mine(out, 'A');
        int32_t const n = kos_call(child_cap(0), const_cast<char*>(&g_pi_msg[0]), PI_MSG, PI_MSG,
                                   KOS_TIMEOUT_NONE);
        pi_report(out, n, 'a'); // the REPLY, in its own copy of the one address
    }

    struct PiRun
    {
        volatile uint64_t* oa = nullptr;
        volatile uint64_t* ob = nullptr;
        bool blocks = false;
        bool ran = false;
    };

#if KICKOS_SELFTEST_PEER
    // The server, pinned at 12, tells main once it is seated there, then parks in its receive.
    int pi_seated_server(void (*server)(void*), char const* block) // caps: E(WAIT)@0, N(SIGNAL)@1
    {
        peer_seat(12, TAP_PIN_CORE);
        (void)kos_notify(child_cap(1));
        server(reinterpret_cast<void*>(peer_word(block)));
        return 0;
    }
    int pi_client_at(void (*client)(void*), char const* block) // caps: E(SIGNAL)@0
    {
        peer_seat(11);
        client(reinterpret_cast<void*>(peer_word(block)));
        return 0;
    }
#endif

    // The server, pinned, has parked before the client exists. `server` and `client` name
    // their roles.
    void pi_two_processes(char const* server, char const* client, PiRun* run)
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
        kos_cap_t seated = KOS_CAP_NONE;
        kos_task_t ta = KOS_TASK_NONE;
        kos_task_t tb = KOS_TASK_NONE;
        ArmHold hold;
        TAP_HOLD(hold.cap(&ep) and hold.bound(&seated) and hold.task(&ta) and hold.task(&tb));
        TAP_CHECK(kos_endpoint_create(&ep) == 0);
        TAP_CHECK(kos_notify_create(&seated) == 0 and kos_notify_bind(seated) == 0);
        kos_cap_grant const scaps[2] = {{ep, KOS_CAP_WAIT}, {seated, KOS_CAP_SIGNAL}};
        kos_cap_grant const ccaps[1] = {{ep, KOS_CAP_SIGNAL}};
        PeerHex const at_a = peer_hex(reinterpret_cast<uintptr_t>(ba));
        PeerHex const at_b = peer_hex(reinterpret_cast<uintptr_t>(bb));
        TAP_CHECK(PeerRun(server, at_a.s).caps(scaps, 2).block(ba, PI_BLK).start(&ta) == 0);
        uint32_t bits = 0;
        TAP_CHECK(kos_notify_wait(seated, ~0u, STALL_TOLERANT_US, &bits) == 0);
        TAP_CHECK(await_pinned_park());
        TAP_CHECK(PeerRun(client, at_b.s).caps(ccaps, 1).block(bb, PI_BLK).start(&tb) == 0);
        TAP_CHECK(peer_dead_status(ta) == 0 and peer_dead_status(tb) == 0);
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
        TAP_ASK(.workers = 2, .tasks = 2, .endpoints = 1, .notifies = 1);
        PiRun run;
        pi_root_seed();
        pi_two_processes("piS", "piC", &run);
        TAP_CHECK(run.blocks);
        TAP_CHECK(run.ran);
        volatile uint64_t* const oa = run.oa;
        volatile uint64_t* const ob = run.ob;
        uintptr_t const own = reinterpret_cast<uintptr_t>(&g_pi_msg[0]);
        uintptr_t const own_info = reinterpret_cast<uintptr_t>(&g_pi_info);
        tap::diag("same-address send: n %d/%d",
                  static_cast<int>(static_cast<int32_t>(oa[PI_N])),
                  static_cast<int>(static_cast<int32_t>(ob[PI_N])));
        // One program twice: one address in both, another program's elsewhere.
        TAP_CHECK(oa[PI_ADDR] == ob[PI_ADDR] and oa[PI_ADDR] != own);
        TAP_CHECK(oa[PI_INFO_ADDR] == ob[PI_INFO_ADDR] and oa[PI_INFO_ADDR] != own_info);
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
        TAP_ASK(.workers = 2, .tasks = 2, .endpoints = 1, .notifies = 1);
        PiRun run;
        pi_root_seed();
        pi_two_processes("piCS", "piCC", &run);
        TAP_CHECK(run.blocks);
        TAP_CHECK(run.ran);
        volatile uint64_t* const oa = run.oa;
        volatile uint64_t* const ob = run.ob;
        uintptr_t const own = reinterpret_cast<uintptr_t>(&g_pi_msg[0]);
        tap::diag("same-address call: reply rc %d, n %d",
                  static_cast<int>(static_cast<int32_t>(oa[PI_REPLY_RC])),
                  static_cast<int>(static_cast<int32_t>(ob[PI_N])));
        TAP_CHECK(oa[PI_ADDR] == ob[PI_ADDR] and oa[PI_ADDR] != own);
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
    void fault_sibling(void*)
    {
        kos_sem_wait(&g_as_never, KOS_TIMEOUT_NONE);
        kos_exit(1);
    }

    // Reads a kernel structure ring 3 must not reach; the read ends this thread's whole task.
    void kernel_state_reader(void* arg)
    {
        (void)*static_cast<unsigned char const volatile*>(arg);
        kos_exit(1);
    }
#if KICKOS_SELFTEST_PEER
    // The reader beside the task's entry, parked as its sibling.
    int kernel_state_task(char const* at)
    {
        auto const victim = kos::thread::create(kernel_state_reader,
                                                reinterpret_cast<void*>(peer_word(at)), "kvic", 10);
        if (not victim.valid())
        {
            return 1;
        }
        fault_sibling(nullptr);
        return 1;
    }
#endif

    // Kernel state the trap path uses on every entry. A ring-3 read of either word must end
    // the reading task, and only task-wide death releases its parked sibling, so a read that
    // returned leaves the task alive.
    void t_kernel_state_unreachable()
    {
        TAP_ASK(.tasks = 1, .peer_workers = 1);
        kos_task_t task = KOS_TASK_NONE;
        ArmHold hold;
        TAP_HOLD(hold.task(&task));
        for (unsigned which = 0; which < KERNEL_WORDS; ++which)
        {
            uintptr_t const addr = kernel_word(which);
            tap::diag("%s at 0x%lx, read from ring 3", kernel_word_name(which),
                      static_cast<unsigned long>(addr));
            TAP_CHECK(in_kernel_half(addr));
            PeerHex const at = peer_hex(addr);
            TAP_CHECK(PeerRun("kvic", at.s).start(&task) == 0);
            TAP_CHECK(peer_dead_status(task) == KOS_EXIT_FAULT);
            TAP_CHECK(task_end(&task) == 0);
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
    // Each holder's turn, main's helper words, which the holder reaches at `arg`, its task's
    // mapping of its entry's window.
    constexpr int PW_SA = 0;
    constexpr int PW_SB = 1;
    void pw_cmos(void* arg) // caps: E(SIGNAL)@0
    {
        kos_sem_t* const mine = static_cast<kos_sem_t*>(arg) + PW_SA;
        kos_sem_t* const theirs = static_cast<kos_sem_t*>(arg) + PW_SB;
        PwSeen seen = {-99, -99, -99, 0, 0};
        seen.index = kos_port_reg_write(PW_CMOS, 0, 0x0au);
        seen.nmi = kos_port_reg_write(PW_CMOS, 0, 0x8au);
        seen.unheld = kos_port_reg_write(PW_COM2, 0, 0);
        for (int i = 0; i < PW_ROUNDS; i++)
        {
            kos_sem_wait(mine, KOS_TIMEOUT_NONE);
            (void)pw_inb(PW_CMOS + 1u);
            seen.rounds++;
            kos_sem_post(theirs);
        }
        kos_sem_wait(mine, KOS_TIMEOUT_NONE);
#if KICKOS_KERNEL_CORES > 1
        seen.moved = kos::thread::pin(kos_thread_self(), 1);
#endif
        (void)pw_inb(PW_CMOS + 1u);
        seen.rounds++;
        (void)kos_send(child_cap(0), &seen, sizeof(seen), KOS_TIMEOUT_NONE);
        kos_exit(0);
    }
    void pw_com2(void* arg) // caps: E(SIGNAL)@0
    {
        kos_sem_t* const mine = static_cast<kos_sem_t*>(arg) + PW_SB;
        kos_sem_t* const theirs = static_cast<kos_sem_t*>(arg) + PW_SA;
        int32_t rounds = 0;
        for (int i = 0; i < PW_ROUNDS; i++)
        {
            kos_sem_wait(mine, KOS_TIMEOUT_NONE);
            (void)pw_inb(PW_COM2);
            rounds++;
            if (i + 1 < PW_ROUNDS)
            {
                kos_sem_post(theirs);
            }
        }
        (void)kos_send(child_cap(0), &rounds, sizeof(rounds), KOS_TIMEOUT_NONE);
        (void)pw_inb(PW_CMOS + 1u);
        // unreachable: that port is not its own
        (void)kos_send(child_cap(0), &rounds, sizeof(rounds), KOS_TIMEOUT_NONE);
        kos_exit(0);
    }
    // Posted by the entry once main has taken its report, so a holder that faults at once
    // waits on it: its task's own copy.
    kos_sem_t g_pw_reported = KOS_SEM_INIT(0);
    void pw_index(void*) // caps: E(SIGNAL)@0
    {
        kos_sem_wait(&g_pw_reported, KOS_TIMEOUT_NONE);
        pw_outb(PW_CMOS, 0x0au);
        char const x = 1;
        // unreachable: the chip keeps the index closed
        (void)kos_send(child_cap(0), &x, 1, KOS_TIMEOUT_NONE);
        kos_exit(0);
    }
#if KICKOS_SELFTEST_PEER
    constexpr kos_window PW_CMOS_WINDOW = {PW_CMOS, 2u, KOS_WINDOW_PORTS, 0};
    // A holder of `ports`, a thread of the peer's task on core 0 named for the fault it may
    // take, joined by the entry, which reports it started. Its arg is the entry's first window
    // where it holds one.
    int pw_holder(void (*entry)(void*), char const* name, kos_window const& ports)
    // caps: E(SIGNAL | TRANSFER)@0
    {
        peer_seat(10, 1u);
        kos_window x = {};
        if (kos_window_get(0, &x) != 0)
        {
            x.base = 0;
        }
        kos_cap_grant const caps[] = {{child_cap(0), KOS_CAP_SIGNAL}};
        auto const h = kos::thread::create(entry, reinterpret_cast<void*>(x.base), name, 10,
                                           KOS_POLICY_FIFO, 0, nullptr, 0, &ports, 1, caps, 1, 0,
                                           nullptr, 1u);
        if (not h.valid())
        {
            return 1;
        }
        reached(child_cap(0));
        kos_sem_post(&g_pw_reported);
        return h.join(STALL_TOLERANT_US);
    }
#endif
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
        TAP_ASK(.tasks = 2, .endpoints = 1, .peer_workers = 1);
        kos_cap_t ep = KOS_CAP_NONE;
        kos_sem_t* const sa = xsem(PW_SA);
        kos_sem_t* const sb = xsem(PW_SB);
        kos_task_t tc = KOS_TASK_NONE;
        kos_task_t tb = KOS_TASK_NONE;
        kos_task_t ti = KOS_TASK_NONE;
        kos_task_t tk = KOS_TASK_NONE;
        ArmHold hold;
        TAP_HOLD(hold.cap(&ep) and hold.task(&tc) and hold.task(&tb) and hold.task(&ti)
                 and hold.task(&tk));
        TAP_CHECK(kos_endpoint_create(&ep) == 0 and kos_sem_init(sa, 0) == 0
                  and kos_sem_init(sb, 0) == 0);
        kos_window const cmos = {PW_CMOS, 2u, KOS_WINDOW_PORTS, 0};
        kos_window const pic = {0x20u, 2u, KOS_WINDOW_PORTS, 0};
        kos_cap_grant const ecaps[] = {{ep, KOS_CAP_SIGNAL | KOS_CAP_TRANSFER}};
        kos_window const xw = xsem_window();
        TAP_CHECK(PeerRun("pwc").caps(ecaps, 1).windows(&xw, 1).start(&tc) == 0
                  and heard_reached(ep));
        TAP_CHECK(PeerRun("nop").windows(&cmos, 1).start(&ti) == -KOS_EBUSY);
        TAP_CHECK(PeerRun("nop").windows(&pic, 1).start(&ti) == -KOS_EINVAL);
        TAP_CHECK(PeerRun("pwb").caps(ecaps, 1).windows(&xw, 1).start(&tb) == 0
                  and heard_reached(ep));
        kos_sem_post(sa);
        int32_t rounds = -1;
        TAP_CHECK(pw_recv(ep, &rounds, sizeof(rounds), STALL_TOLERANT_US)
                  == static_cast<int32_t>(sizeof(rounds)));
        TAP_CHECK(peer_dead_status(tb) == KOS_EXIT_FAULT);
        // An absence after the death, not an order: nothing is left to send.
        TAP_CHECK(pw_recv(ep, &rounds, sizeof(rounds), 20000) == -KOS_ETIMEDOUT);
        kos_sem_post(sa);
        PwSeen seen = {-99, -99, -99, -1, -1};
        TAP_CHECK(pw_recv(ep, &seen, sizeof(seen), STALL_TOLERANT_US)
                  == static_cast<int32_t>(sizeof(seen)));
        TAP_CHECK(peer_dead_status(tc) == 0);
        TAP_CHECK(rounds == PW_ROUNDS);
        TAP_CHECK(seen.index == 0 and seen.nmi == -KOS_EINVAL and seen.unheld == -KOS_EPERM);
        int32_t moved_to = 0; // one kernel core: no move is made
#if KICKOS_KERNEL_CORES > 1
        moved_to = 0x2;
#endif
        TAP_CHECK(seen.rounds == PW_ROUNDS + 1 and seen.moved == moved_to);
        // The CMOS pair is free again, and its index stays closed to the next holder.
        TAP_CHECK(task_end(&tc) == 0);
        bool const heard = PeerRun("pwi").caps(ecaps, 1).start(&ti) == 0 and heard_reached(ep);
        TAP_CHECK(heard);
        TAP_CHECK(peer_dead_status(ti) == KOS_EXIT_FAULT);
        // An absence after the death, not an order: nothing is left to send.
        char x = 0;
        TAP_CHECK(pw_recv(ep, &x, 1, 20000) == -KOS_ETIMEDOUT);
        TAP_CHECK(task_end(&ti) == 0);
        kos_window const com1 = {0x3f8u, 8u, KOS_WINDOW_PORTS, 0};
        int const console = PeerRun("nop").windows(&com1, 1).start(&tk);
#if defined(KICKOS_SELFTEST_CONSOLE_DRIVER)
        TAP_CHECK(console == -KOS_EBUSY);
#else
        TAP_CHECK(console == 0);
        TAP_CHECK(peer_dead_status(tk) == 0);
#endif
        uint64_t const back = pw_flags_after_syscall();
        TAP_CHECK((back & PW_IOPL) == 0 and (back & PW_IF) != 0);
    }
#endif

#if defined(KICKOS_SELFTEST_SPARE_DEV)
    // --- A device window at the address the kernel chose -------------------------------
    // The holder, in a task of its own, reads a device the kernel never drives through the
    // address kos_window_get answers, which is not the physical one, is answered no window past
    // its list, and is refused a console write the kernel would read from the device; it reports
    // over an endpoint, its task's data being its own.
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
        int32_t console; // the kernel asked to read the window
    };
    uintptr_t g_wa_addr = 0; // the holder's task's copy, which its sibling reads
    // A bound notification's wait: the gate main raises across tasks without a window, which
    // would change the lists the holder reads back.
    void wa_gate(kos_cap_t note)
    {
        uint32_t bits = 0;
        (void)kos_notify_bind(note);
        (void)kos_notify_wait(note, ~0u, KOS_TIMEOUT_NONE, &bits);
    }
    void wa_holder(void*) // caps: E(SIGNAL)@0, hold@1
    {
        kos_window at = {};
        WaSeen seen = {-99, -99, 0, 0, -99};
        seen.rc = kos_window_get(0, &at);
        g_wa_addr = at.base;
        if (seen.rc == 0)
        {
            if (g_wa_addr != WA_DEV)
            {
                seen.moved = 1u;
            }
            seen.value = *reinterpret_cast<volatile uint32_t*>(at.base);
            seen.console = KICKOS_KCONSOLE_MEASURED(
                ANSWER, reinterpret_cast<void const*>(at.base), sizeof(uint32_t));
        }
        kos_window other = {};
        seen.other = kos_window_get(1, &other);
        (void)kos_send(child_cap(0), &seen, sizeof(seen), KOS_TIMEOUT_NONE);
        wa_gate(child_cap(1));
        kos_exit(0);
    }
    void wa_sibling(void*) // caps: go@0, E(SIGNAL)@1
    {
        wa_gate(child_cap(0));
        (void)*reinterpret_cast<volatile uint32_t*>(g_wa_addr);
        // unreachable: the holder's exit took the mapping with it
        (void)kos_send(child_cap(1), "x", 1, KOS_TIMEOUT_NONE);
        kos_exit(0);
    }
#if KICKOS_SELFTEST_PEER
    // The holder and its sibling as threads of the peer's task, the entry telling main once the
    // holder has exited.
    int wa_task() // caps: E(SIGNAL | TRANSFER)@0, hold(WAIT | TRANSFER)@1, go(WAIT | TRANSFER)@2
    {
        kos_window const dev = {WA_DEV, WA_SIZE, KOS_WINDOW_DEVICE, 0};
        kos_cap_grant const hcaps[] = {{child_cap(0), KOS_CAP_SIGNAL}, {child_cap(1), KOS_CAP_WAIT}};
        auto const holder = kos::thread::create(wa_holder, nullptr, "wah", 10, KOS_POLICY_FIFO, 0,
                                                nullptr, 0, &dev, 1, hcaps, 2);
        kos_cap_grant const scaps[] = {{child_cap(2), KOS_CAP_WAIT}, {child_cap(0), KOS_CAP_SIGNAL}};
        auto const sibling = kos::thread::create(wa_sibling, nullptr, "was", 10, KOS_POLICY_FIFO,
                                                 0, nullptr, 0, nullptr, 0, scaps, 2);
        if (not holder.valid() or not sibling.valid())
        {
            return 1;
        }
        // -KOS_EBADF: the holder exited before the sibling's spawn, which took its slot.
        int const joined = holder.join(STALL_TOLERANT_US);
        if (joined != 0 and joined != -KOS_EBADF)
        {
            return 1;
        }
        reached(child_cap(0));
        (void)sibling.join(STALL_TOLERANT_US);
        return 0;
    }
#endif
#if defined(KICKOS_SELFTEST_BUS_MASTER_DEV)
    struct WaBusMaster
    {
        int32_t spare;
        int32_t bus_master;
        int32_t slain[2];
    };
    // [0] holds a peer's authority and AUTH_SYSTEM; [1] a peer's and AUTH_BUS_MASTER.
    WaBusMaster g_wa_bm[2] = {{-99, -99, {-99, -99}}, {-99, -99, {-99, -99}}};
    kos_window const g_wa_bm_dev = {KICKOS_SELFTEST_BUS_MASTER_DEV, WA_SIZE, KOS_WINDOW_DEVICE, 0};
    void wa_bus_master_spawner(void* arg)
    {
        WaBusMaster* const out = static_cast<WaBusMaster*>(arg);
        kos_program_t prog = KOS_CAP_NONE;
        out->spare = kos_program_find("stpeer", &prog);
        if (out->spare != 0)
        {
            return;
        }
        kos_task_t t = KOS_TASK_NONE;
        kos_window const spare = {WA_DEV, WA_SIZE, KOS_WINDOW_DEVICE, 0};
        out->spare = PeerRun("nop").windows(&spare, 1).start(&t, prog);
        out->slain[0] = 0;
        if (out->spare == 0)
        {
            out->slain[0] = kos_task_slay(t, STALL_TOLERANT_US);
        }
        out->bus_master = PeerRun("nop").windows(&g_wa_bm_dev, 1).start(&t, prog);
        out->slain[1] = 0;
        if (out->bus_master == 0)
        {
            out->slain[1] = kos_task_slay(t, STALL_TOLERANT_US);
        }
        (void)kos_handle_close(prog);
    }
#endif
    // Receives the report of a holder of the device.
    void wa_report(kos_cap_t ep, WaSeen* seen)
    {
        *seen = {-99, -99, 0, 0, -99};
        struct kos_reply_recv_opts o;
        kos_reply_recv_opts_init(&o, ep, KOS_RECV_NO_INFO, STALL_TOLERANT_US);
        (void)kos_reply_recv(KOS_CAP_NONE, seen, kos_call_lens_pack(0, sizeof(*seen)), &o);
    }
    void t_window_addr()
    {
        TAP_ASK(.workers = 2, .tasks = 2, .endpoints = 1, .notifies = 2, .peer_workers = 2);
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
        TAP_CHECK(kos_endpoint_create(&ep) == 0 and kos_notify_create(&park) == 0
                  and kos_notify_create(&go) == 0);
        kos_cap_grant const caps[] = {{ep, KOS_CAP_SIGNAL | KOS_CAP_TRANSFER},
                                      {park, KOS_CAP_WAIT | KOS_CAP_TRANSFER},
                                      {go, KOS_CAP_WAIT | KOS_CAP_TRANSFER}};
        TAP_CHECK(PeerRun("wa").caps(caps, 3).start(&t) == 0);
        WaSeen first = {-99, -99, 0, 0, -99};
        wa_report(ep, &first);
        kos_window const dev = {WA_DEV, WA_SIZE, KOS_WINDOW_DEVICE, 0};
        TAP_CHECK(PeerRun("nop").windows(&dev, 1).start(&other) == -KOS_EBUSY);
        // A megabyte past the spare device is outside every aperture this chip states.
        kos_window const outside = {WA_DEV + 0x100000u, WA_SIZE, KOS_WINDOW_DEVICE, 0};
        TAP_CHECK(PeerRun("nop").windows(&outside, 1).start(&other) == -KOS_EINVAL);
        (void)kos_notify(park);
        TAP_CHECK(heard_reached(ep));
        (void)kos_notify(go);
        TAP_CHECK(peer_dead_status(t) == KOS_EXIT_FAULT);
        // An absence after the death, not an order: nothing is left to send.
        char late = 0;
        struct kos_reply_recv_opts o;
        kos_reply_recv_opts_init(&o, ep, KOS_RECV_NO_INFO, 20000);
        TAP_CHECK(kos_reply_recv(KOS_CAP_NONE, &late, kos_call_lens_pack(0, 1), &o)
                  == -KOS_ETIMEDOUT);
        TAP_CHECK(task_end(&t) == 0);
        TAP_CHECK(first.rc == 0 and first.moved == 1u and first.value != 0);
        TAP_CHECK(first.other == -KOS_EINVAL);
        TAP_CHECK(first.console == -KOS_EFAULT);
        // The next instance, in the other task, maps the device again.
        WaSeen again = {-99, -99, 0, 0, -99};
        (void)kos_notify(park);
        kos_cap_grant const hcaps[] = {{ep, KOS_CAP_SIGNAL}, {park, KOS_CAP_WAIT}};
        TAP_CHECK(PeerRun("wah").caps(hcaps, 2).windows(&dev, 1).start(&other) == 0);
        wa_report(ep, &again);
        TAP_CHECK(peer_dead_status(other) == 0);
        TAP_CHECK(again.rc == 0 and again.value != 0);
        TAP_CHECK(task_end(&other) == 0);
#if defined(KICKOS_SELFTEST_BUS_MASTER_DEV)
        // A bus master's window takes AUTH_BUS_MASTER of its spawner; AUTH_SYSTEM is not it.
        TAP_CHECK(PeerRun("nop").windows(&g_wa_bm_dev, 1).start(&bt) == 0);
        TAP_CHECK(peer_dead_status(bt) == 0);
        TAP_CHECK(task_end(&bt) == 0);
        uint32_t const peer = KOS_AUTH_MEMORY | KOS_AUTH_TASKS | KOS_AUTH_IRQ;
        uint32_t const bauth[2] = {peer | KOS_AUTH_SYSTEM, peer | KOS_AUTH_BUS_MASTER};
        for (int i = 0; i < 2; i++)
        {
            g_wa_bm[i] = {-99, -99, {-99, -99}};
            spawner = kos::thread::create_caps(wa_bus_master_spawner, &g_wa_bm[i], "wbp", 10,
                                               nullptr, 0, KOS_POLICY_FIFO, 0, bauth[i]);
            TAP_CHECK(spawner.valid());
            TAP_CHECK(thread_end(&spawner) == 0);
        }
        tap::diag("bus master window: memory and system %ld, bus_master %ld; spare %ld, %ld",
                  static_cast<long>(g_wa_bm[0].bus_master),
                  static_cast<long>(g_wa_bm[1].bus_master),
                  static_cast<long>(g_wa_bm[0].spare), static_cast<long>(g_wa_bm[1].spare));
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
    constexpr int CH_UA_EP = KOS_SPAWN_DELEGATED_CAP0;
    constexpr uint32_t UA_RUN_PAGES = 4;
    constexpr uint32_t UA_WIDE_PAGES = 32;
    constexpr uint32_t UA_FILL = 0xCAC4ED00u;
    unsigned char* g_ua_dst[UA_ROUNDS] = {};
    Atomic<int32_t, Order::RELAXED> g_ua_got[UA_ROUNDS];
    void ua_receiver(void*) // caps: E(WAIT)@0
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

    // A frame run is refused the type as a reservation is.
    void ua_refused()
    {
        Seed s;
        uintptr_t at = 0;
        ArmHold hold;
        TAP_HOLD(hold.mapped(&s.frame, &s.space, &at));
        TAP_CHECK(seed_mint(ASPACE_GRANULE, &s));
        TAP_CHECK(kos_frame_map(s.frame, s.space, &at, KOS_MEM_NOCACHE) == -KOS_ENOTSUP);
        TAP_CHECK(at == 0);
        seed_close(&s);
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
        int const unc_rc = kos_mem_self_grant(unc, UA_BLK, KOS_MEM_NOCACHE);
        if (unc_rc == -KOS_ENOTSUP)
        {
            ua_refused();
            tap::partial("no alias to maintain (no uncached RAM)");
            return;
        }
        TAP_CHECK(unc_rc == 0);
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
                                          KOS_POLICY_FIFO, 0, 0);
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
        receiver = kos::thread::create_caps(ua_receiver, nullptr, "uarcv", TAP_PRIO_PARKS, caps,
                                            1, KOS_POLICY_FIFO, 0, 0, nullptr, nullptr, 0,
                                            TAP_PIN_CORE);
        TAP_CHECK(receiver.valid());
        // Each send meets the receiver's next receive, which follows its record of the last.
        int32_t sent[UA_ROUNDS];
        for (int r = 0; r < UA_ROUNDS; r++)
        {
            sent[r] = kos_send(ep, src[r], UA_LEN, STALL_TOLERANT_US);
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

    // Each space constructs these at its startup: the image holds a zero word and a zero count.
    volatile uint64_t g_ct_seed = 0x5EEDC7ull;
    constexpr uint64_t CT_MASK = 0xA5A5ull;
    constexpr uint64_t CT_WORD = 0x5EEDC7ull ^ CT_MASK;
    uint64_t const g_ct_word = g_ct_seed ^ CT_MASK;
    uint32_t g_ct_runs = 0;
    __attribute__((constructor)) void ct_count()
    {
        g_ct_runs++;
    }

    // task_start_dead_starter's trap door: a starter holding an endpoint at the last slot a full
    // grant list fills says it is inside the constructor walk, a send main receives, and parks
    // there until it is killed. Any other thread's slot is empty and refuses the send. The walk
    // runs before the task's own declaration is known, so the slot is the grant bound's.
    kos_cap_t ds_trigger()
    {
        return KOS_CAP_FIRST_DYNAMIC + KICKOS_MAX_SPAWN_GRANTS - 1;
    }
    __attribute__((constructor)) void ds_trap_door()
    {
        if (kos_send(ds_trigger(), "", 0, KOS_TIMEOUT_NONE) == 0)
        {
            while (true)
            {
                (void)kos_sem_wait(&g_as_never, KOS_TIMEOUT_NONE);
            }
        }
    }

    volatile uint64_t g_dt_word = 0xC0FFEEull;
    constexpr uint64_t DT_A = 0xC0FFEEull;
    constexpr uint64_t DT_B = 0xBADBADull;
    enum
    {
        DT_VALUE = 0,
        DT_HEAP = 1,
        DT_CTOR = 2,
        DT_WORDS = 3
    };
    void dt_reader(void* arg)
    {
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(arg);
        out[DT_VALUE] = g_dt_word;
        out[DT_CTOR] = g_ct_word;
        // An image with no heap answers null; what matters is that the call returns.
        free(malloc(64));
        out[DT_HEAP] = 1u;
    }
    // Holds its space's malloc lock across a write to static data until main lets it go.
    kos_sem_t g_dt_held = KOS_SEM_INIT(0);
    kos_sem_t g_dt_go = KOS_SEM_INIT(0);
    void dt_holder(void*)
    {
        __malloc_lock(_REENT);
        g_dt_word = DT_B;
        kos_sem_post(&g_dt_held);
        (void)kos_sem_wait(&g_dt_go, KOS_TIMEOUT_NONE);
        g_dt_word = DT_A;
        __malloc_unlock(_REENT);
    }
    // A receiver parks, a sibling unmaps its buffer and sends: both must get EFAULT. The sender
    // buffer is separate to exclude overlap errors. Both are pinned and the receiver outranks
    // the sibling, so validation precedes the unmap. Sender EFAULT proves delivery reached a
    // parked receiver; a boundary rejection would leave the sender to time out.
    constexpr size_t RU_LEN = 32;
    constexpr uint32_t RU_SEND_US = 200000;
    constexpr int CH_RU_EP = KOS_SPAWN_DELEGATED_CAP0;
    constexpr int CH_RU_FRAME = KOS_SPAWN_DELEGATED_CAP0 + 1;
    constexpr int CH_RU_SPACE = KOS_SPAWN_DELEGATED_CAP0 + 2;
    char g_ru_src[RU_LEN];
    Atomic<int32_t, Order::RELAXED> g_ru_unmap{1};
    Atomic<int32_t, Order::RELAXED> g_ru_sent{1};
    Atomic<int32_t, Order::RELAXED> g_ru_got{99};
    void ru_receiver(void* arg) // caps: E(WAIT)@0
    {
        struct kos_reply_recv_opts opts = {};
        opts.timeout_us = STALL_TOLERANT_US;
        opts.info.reply_cap = KOS_CAP_NONE;
        opts.ep = CH_RU_EP;
        g_ru_got = kos_reply_recv(KOS_CAP_NONE, arg, kos_call_lens_pack(0, RU_LEN), &opts);
    }
    void ru_puller(void* arg) // caps: E(SIGNAL)@0, frame@1, space@2
    {
        g_ru_unmap = kos_frame_unmap(CH_RU_FRAME, CH_RU_SPACE,
                                     reinterpret_cast<uintptr_t>(arg));
        g_ru_sent = kos_send(CH_RU_EP, g_ru_src, RU_LEN, RU_SEND_US);
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
                                            TAP_PRIO_PARKS, rcaps, 1, KOS_POLICY_FIFO, 0, 0,
                                            nullptr, nullptr, 0, TAP_PIN_CORE);
        TAP_CHECK(receiver.valid());
        puller = kos::thread::create_caps(ru_puller, reinterpret_cast<void*>(va), "rupul",
                                          TAP_PRIO_AFTER, pcaps, 3, KOS_POLICY_FIFO, 0,
                                          KOS_AUTH_MEMORY, nullptr, nullptr, 0, TAP_PIN_CORE);
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
    constexpr int CH_LU_EP = KOS_SPAWN_DELEGATED_CAP0;
    kos_sem_t g_lu_gate = KOS_SEM_INIT(0);
    char g_lu_req[LU_LEN];
    char g_lu_rx[LU_LEN];
    uintptr_t g_lu_info = 0;
    Atomic<int32_t, Order::RELAXED> g_lu_call{99};
    Atomic<int32_t, Order::RELAXED> g_lu_reply{-1};
    Atomic<uint32_t, Order::RELAXED> g_lu_rounds{0};
    int32_t g_lu_got[KICKOS_CAP_REPLY_MAX + 1];

    // Parked on send_waiters before main receives, which reaches the receiver scan instead of
    // the fastpath.
    void lu_caller(void*) // caps: E(SIGNAL)@0
    {
        g_lu_call = kos_call(CH_LU_EP, g_lu_req, LU_LEN, LU_LEN, STALL_TOLERANT_US);
    }

    // One receiver across the failures and the control, so they spend one cap budget. main maps
    // opts before each gate and unmaps it only once the receiver parked, so the failure is at
    // delivery and not at syscall entry.
    void lu_receiver(void*) // caps: E(WAIT)@0
    {
        for (uint32_t i = 0; i <= KICKOS_CAP_REPLY_MAX; i++)
        {
            kos_sem_wait(&g_lu_gate, KOS_TIMEOUT_NONE);
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
        TAP_ASK(.workers = 2, .endpoints = 1);
        Seed s;
        uintptr_t mapped_at = 0;
        kos_cap_t ep = KOS_CAP_NONE;
        kos::thread::Handle caller;
        kos::thread::Handle receiver;
        ArmHold hold;
        TAP_HOLD(hold.mapped(&s.frame, &s.space, &mapped_at) and hold.cap(&ep)
                  and hold.thread(&caller) and hold.thread(&receiver));
        TAP_CHECK(seed_mint(ASPACE_GRANULE, &s));
        uintptr_t const va = seed_va(s);
        TAP_CHECK(va != 0);
        TAP_CHECK(kos_endpoint_create(&ep) == 0 and kos_sem_init(&g_lu_gate, 0) == 0);
        // The page must map and unmap before its unmapped state means anything.
        TAP_CHECK(map_at(s.frame, s.space, va, 0) == 0);
        TAP_CHECK(kos_frame_unmap(s.frame, s.space, va) == 0);
        g_lu_info = va;
        for (size_t i = 0; i < LU_LEN; i++)
        {
            g_lu_req[i] = static_cast<char>('a' + i);
        }

        // Site A validates and writes the output under one lock without parking, so only boundary
        // validation is reachable; site B and tests/unit/capbadge cover capability rollback.
        int32_t const a_entry =
            kos_reply_recv(KOS_CAP_NONE, g_lu_rx, kos_call_lens_pack(0, LU_LEN),
                           reinterpret_cast<struct kos_reply_recv_opts*>(va));

        // Control: the same receiver scan with a valid output page.
        g_lu_call = 99;
        kos_cap_grant ccaps[] = {{ep, KOS_CAP_SIGNAL}};
        caller = kos::thread::create_caps(lu_caller, nullptr, "luctl", TAP_PRIO_PARKS, ccaps, 1,
                                          KOS_POLICY_FIFO, 0, 0, nullptr, nullptr, 0, TAP_PIN_CORE);
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
        kos_cap_grant rcaps[] = {{ep, KOS_CAP_WAIT}};
        receiver = kos::thread::create_caps(lu_receiver, nullptr, "lurcv", TAP_PRIO_PARKS, rcaps,
                                            1, KOS_POLICY_FIFO, 0, 0, nullptr, nullptr, 0,
                                            TAP_PIN_CORE);
        TAP_CHECK(receiver.valid());
        for (uint32_t i = 0; i < KICKOS_CAP_REPLY_MAX; i++)
        {
            TAP_CHECK(map_at(s.frame, s.space, va, 0) == 0);
            mapped_at = va;
            kos_sem_post(&g_lu_gate);
            TAP_CHECK(await_pinned_park());
            TAP_CHECK(kos_frame_unmap(s.frame, s.space, va) == 0);
            mapped_at = 0;
            b_call_saw = kos_call(ep, g_lu_req, LU_LEN, LU_LEN, STALL_TOLERANT_US);
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
        kos_sem_post(&g_lu_gate);
        TAP_CHECK(await_pinned_park());
        int32_t const b_ctl = kos_call(ep, g_lu_req, LU_LEN, LU_LEN, STALL_TOLERANT_US);
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

    // A task starts from its image's pristine data, never from a live space's: a task created
    // after main wrote a global, its restart, and a task created while a sibling of main holds
    // main's malloc lock across a write all read the image's value, and that last one's malloc
    // does not wait on main's lock.
    void t_process_data_from_image()
    {
        TAP_ASK(.workers = 2, .tasks = 1);
        constexpr uint32_t FI_BLK = 8u * DT_WORDS;
        static_assert(FI_RAM.size[0] == FI_BLK);
        void* const blk = st_ram<FI_RAM, 0>();
        TAP_CHECK(blk != nullptr);
        TAP_CHECK(kos_mem_self_grant(blk, FI_BLK, 0) == 0);
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(blk);
        kos_task_t t = KOS_TASK_NONE;
        kos::thread::Handle holder;
        ArmHold hold;
        TAP_HOLD(hold.task(&t) and hold.thread(&holder));
        PeerHex const at = peer_hex(reinterpret_cast<uintptr_t>(blk));
        g_dt_word = DT_B;
        uint64_t seen[2] = {0, 0};
        uint64_t built[2] = {0, 0};
        for (int run = 0; run < 2; run++)
        {
            out[DT_VALUE] = 0;
            out[DT_CTOR] = 0;
            TAP_CHECK(PeerRun("dt", at.s).block(blk, FI_BLK).start(&t) == 0);
            TAP_CHECK(peer_dead_status(t) == 0);
            TAP_CHECK(task_end(&t) == 0);
            seen[run] = out[DT_VALUE];
            built[run] = out[DT_CTOR];
        }
        g_dt_word = DT_A;

        TAP_CHECK(kos_sem_init(&g_dt_held, 0) == 0 and kos_sem_init(&g_dt_go, 0) == 0);
        holder = kos::thread::create_caps(dt_holder, nullptr, "fiH", 10, nullptr, 0);
        TAP_CHECK(holder.valid());
        TAP_CHECK(kos_sem_wait(&g_dt_held, STALL_TOLERANT_US) == 0);
        // Nothing here may allocate: the holder keeps main's malloc lock until `go`.
        out[DT_VALUE] = 0;
        out[DT_HEAP] = 0;
        out[DT_CTOR] = 0;
        bool const spawned_ran = PeerRun("dt", at.s).block(blk, FI_BLK).start(&t) == 0
                                 and peer_dead_status(t) == 0;
        uint64_t const spawned_saw = out[DT_VALUE];
        uint64_t const spawned_heap = out[DT_HEAP];
        uint64_t const spawned_built = out[DT_CTOR];
        kos_sem_post(&g_dt_go);
        TAP_CHECK(hold.joined());
        tap::diag("data from the image: first %lx, restart %lx, created beside a held lock %lx",
                  static_cast<unsigned long>(seen[0]), static_cast<unsigned long>(seen[1]),
                  static_cast<unsigned long>(spawned_saw));
        TAP_CHECK(seen[0] == DT_A); // not main's DT_B
        TAP_CHECK(seen[1] == DT_A);
        TAP_CHECK(built[0] == CT_WORD); // the image's zero: the startup never ran there
        TAP_CHECK(built[1] == CT_WORD);
        TAP_CHECK(spawned_ran);
        TAP_CHECK(spawned_saw == DT_A); // not the holder's DT_B
        TAP_CHECK(spawned_heap == 1u);
        TAP_CHECK(spawned_built == CT_WORD);
        TAP_CHECK(g_dt_word == DT_A);
    }

    enum
    {
        TS_WORD = 0,
        TS_RUNS = 1,
        TS_LOW = 0,
        TS_HIGH = 2,
        TS_WORDS = 4
    };
    void ts_reader(void* arg)
    {
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(arg);
        out[TS_WORD] = g_ct_word;
        out[TS_RUNS] = g_ct_runs;
    }
#if KICKOS_SELFTEST_PEER
    // The entry, which ran its image's start, and a thread it starts above itself.
    int ts_task(char const* block)
    {
        peer_seat(TAP_PRIO_AFTER, TAP_PIN_CORE);
        auto* const out = reinterpret_cast<uint64_t*>(peer_word(block));
        auto const high = kos::thread::create(ts_reader, out + TS_HIGH, "tsH", TAP_PRIO_PARKS,
                                              KOS_POLICY_FIFO, 0, nullptr, 0, nullptr, 0, nullptr,
                                              0, 0, nullptr, TAP_PIN_CORE);
        if (not high.valid() or high.join(STALL_TOLERANT_US) != 0)
        {
            return 1;
        }
        ts_reader(out + TS_LOW);
        return 0;
    }
#endif

    // A fresh task's entry and a thread it starts above itself both read constructed data,
    // built once.
    void t_task_start_once()
    {
        TAP_ASK(.tasks = 1, .peer_workers = 1);
        constexpr uint32_t TS_BLK = 8u * TS_WORDS;
        static_assert(TS_RAM.size[0] == TS_BLK);
        void* const blk = st_ram<TS_RAM, 0>();
        TAP_CHECK(blk != nullptr);
        TAP_CHECK(kos_mem_self_grant(blk, TS_BLK, 0) == 0);
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(blk);
        for (uint32_t i = 0; i < TS_WORDS; i++)
        {
            out[i] = 0;
        }
        kos_task_t t = KOS_TASK_NONE;
        ArmHold hold;
        TAP_HOLD(hold.task(&t));
        PeerHex const at = peer_hex(reinterpret_cast<uintptr_t>(blk));
        TAP_CHECK(PeerRun("ts", at.s).block(blk, TS_BLK).start(&t) == 0);
        TAP_CHECK(peer_dead_status(t) == 0);
        tap::diag("task start: first in read %lx after %lu runs, the task's entry %lx after %lu",
                  static_cast<unsigned long>(out[TS_HIGH + TS_WORD]),
                  static_cast<unsigned long>(out[TS_HIGH + TS_RUNS]),
                  static_cast<unsigned long>(out[TS_LOW + TS_WORD]),
                  static_cast<unsigned long>(out[TS_LOW + TS_RUNS]));
        TAP_CHECK(out[TS_HIGH + TS_WORD] == CT_WORD);
        TAP_CHECK(out[TS_HIGH + TS_RUNS] == 1u);
        TAP_CHECK(out[TS_LOW + TS_WORD] == CT_WORD);
        TAP_CHECK(out[TS_LOW + TS_RUNS] == 1u);
    }

#if KICKOS_FAULT_ISOLATION
    enum
    {
        DS_ENTRY = 0,
        DS_WORDS = 3
    };
    void ds_mark(void* arg)
    {
        *static_cast<volatile uint64_t*>(arg) = 1u;
    }

    // The entry dies inside its image's constructors: the task ends before its main runs.
    void t_task_start_dead_starter()
    {
        TAP_ASK(.workers = 1, .tasks = 1, .endpoints = 1);
        constexpr uint32_t DS_BLK = 8u * DS_WORDS;
        static_assert(DS_RAM.size[0] == DS_BLK);
        void* const blk = st_ram<DS_RAM, 0>();
        TAP_CHECK(blk != nullptr);
        TAP_CHECK(kos_mem_self_grant(blk, DS_BLK, 0) == 0);
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(blk);
        for (uint32_t i = 0; i < DS_WORDS; i++)
        {
            out[i] = 0;
        }
        kos_task_t t = KOS_TASK_NONE;
        kos_cap_t trigger = KOS_CAP_NONE;
        ArmHold hold;
        TAP_HOLD(hold.task(&t) and hold.cap(&trigger));
        TAP_CHECK(kos_endpoint_create(&trigger) == 0);
        kos_cap_grant const caps[] = {{trigger, KOS_CAP_SIGNAL}};
        uint16_t const dest[] = {static_cast<uint16_t>(ds_trigger())};
        PeerHex const at = peer_hex(reinterpret_cast<uintptr_t>(out + DS_ENTRY));
        PeerRun run("dsmark", at.s);
        run.p.cap_dest = dest;
        TAP_CHECK(run.caps(caps, 1).block(blk, DS_BLK).start(&t) == 0);
        char inside = 0;
        TAP_CHECK(report_await(trigger, &inside, 0));
        // Its main has not run: the entry is parked in the constructor walk.
        int const slain = kos_task_slay(t, STALL_TOLERANT_US);
        if (slain == 0)
        {
            t = KOS_TASK_NONE;
        }
        tap::diag("task start: a starter slain inside its constructors: slay %d, main ran %lu",
                  slain, static_cast<unsigned long>(out[DS_ENTRY]));
        TAP_CHECK(slain == 0);
        TAP_CHECK(out[DS_ENTRY] == 0u);
    }
#endif

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

    // The program's initialized pointers and constructor table hold link-time addresses. Each
    // must equal the address the running code computes for the same object.
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

    // --- Another program's code and data ---------------------------------------------------
    // A peer runs a program of its own at the addresses main's runs at, started with argv
    // { "peer", "foreign", <the block's address in hex> }. Over every page of main's code span
    // and window, peer0 reads one through a system call: one its own spans hold reaches, every
    // other one answers -KOS_EFAULT, so its space maps none of main's text, rodata or data
    // there. The probe's own first bytes, which peer0 runs, are other bytes in main's space.
    enum
    {
        IF_MAIN_CODE = 0, // main's code span, two words
        IF_MAIN_WIN = 2,  // main's window, two words
        IF_PROBE = 4,     // peer_foreign's address in peer0
        IF_BYTES = 5,     // its first word there
        IF_OWN = 6,       // pages of peer0's own spans probed, then reached
        IF_FOREIGN = 8,   // other pages probed, then reached
        IF_WORDS = 10
    };
    static_assert(IF_RAM.size[0] == 8u * IF_WORDS);

    namespace
    {
        uintptr_t if_down(uintptr_t a)
        {
            return a & ~static_cast<uintptr_t>(ASPACE_GRANULE - 1u);
        }

        uintptr_t if_up(uintptr_t a)
        {
            return if_down(a + ASPACE_GRANULE - 1u);
        }

        bool if_reached(uintptr_t page)
        {
            kos_program_t h = KOS_CAP_NONE;
            int const rc = kos_program_find(reinterpret_cast<char const*>(page), &h);
            if (rc == 0)
            {
                (void)kos_handle_close(h);
            }
            return rc != -KOS_EFAULT;
        }
    }

    void peer_foreign(char const* block_hex)
    {
        volatile uint64_t* const io = reinterpret_cast<volatile uint64_t*>(peer_word(block_hex));
        kos_image_header const& h = kickos_image_header;
        uintptr_t const own[4] = {if_down(h.code_start), if_up(h.code_end), if_down(h.window_start),
                                  if_up(h.window_start + h.window_size)};
        for (int span = 0; span < 2; span++)
        {
            uintptr_t const lo = if_down(static_cast<uintptr_t>(io[IF_MAIN_CODE + 2 * span]));
            uintptr_t const hi = if_up(static_cast<uintptr_t>(io[IF_MAIN_CODE + 2 * span + 1]));
            for (uintptr_t page = lo; page < hi; page += ASPACE_GRANULE)
            {
                int at = IF_FOREIGN;
                if ((page >= own[0] and page < own[1]) or (page >= own[2] and page < own[3]))
                {
                    at = IF_OWN;
                }
                io[at] = io[at] + 1u;
                if (if_reached(page))
                {
                    io[at + 1] = io[at + 1] + 1u;
                }
            }
        }
        uintptr_t const probe = reinterpret_cast<uintptr_t>(&peer_foreign);
        io[IF_PROBE] = probe;
        io[IF_BYTES] = *reinterpret_cast<uint64_t const volatile*>(probe);
    }

    void t_image_foreign_unmapped()
    {
        TAP_ASK(.tasks = 1);
        void* const blk = st_ram<IF_RAM, 0>();
        TAP_CHECK(blk != nullptr);
        TAP_CHECK(kos_mem_self_grant(blk, 8u * IF_WORDS, 0) == 0);
        volatile uint64_t* const io = static_cast<volatile uint64_t*>(blk);
        kos_image_header const& h = kickos_image_header;
        for (int i = 0; i < IF_WORDS; i++)
        {
            io[i] = 0;
        }
        io[IF_MAIN_CODE] = h.code_start;
        io[IF_MAIN_CODE + 1] = h.code_end;
        io[IF_MAIN_WIN] = h.window_start;
        io[IF_MAIN_WIN + 1] = h.window_start + h.window_size;
        PeerHex const hex = peer_hex(reinterpret_cast<uintptr_t>(blk));
        kos_task_t t = KOS_TASK_NONE;
        ArmHold hold;
        TAP_HOLD(hold.task(&t));
        TAP_CHECK(PeerRun("foreign", hex.s).block(blk, 8u * IF_WORDS).start(&t) == 0);
        int const status = peer_dead_status(t);
        TAP_CHECK(task_end(&t) == 0);

        uintptr_t const probe = static_cast<uintptr_t>(io[IF_PROBE]);
        bool const probe_in_main = probe >= h.code_start and probe + 8u <= h.code_end;
        uint64_t mine = 0;
        if (probe_in_main)
        {
            mine = *reinterpret_cast<uint64_t const volatile*>(probe);
        }
        tap::diag("another program's spans: peer0 reached %lu of %lu of its own page(s) and %lu "
                  "of %lu of main's other page(s); at 0x%lx peer0 runs %lx, main holds %lx",
                  static_cast<unsigned long>(io[IF_OWN + 1]),
                  static_cast<unsigned long>(io[IF_OWN]),
                  static_cast<unsigned long>(io[IF_FOREIGN + 1]),
                  static_cast<unsigned long>(io[IF_FOREIGN]), static_cast<unsigned long>(probe),
                  static_cast<unsigned long>(io[IF_BYTES]), static_cast<unsigned long>(mine));
        TAP_CHECK(status == 0);
        TAP_CHECK(io[IF_OWN] != 0 and io[IF_OWN + 1] == io[IF_OWN]);
        TAP_CHECK(io[IF_FOREIGN] != 0);
        TAP_CHECK(io[IF_FOREIGN + 1] == 0);
        TAP_CHECK(probe_in_main);
        TAP_CHECK(mine != io[IF_BYTES]);
    }

#if defined(__x86_64__)
    // The program runs at the fixed user base it was linked at: its header is its first byte,
    // and its code and its data lie in the spans that header states above it.
    void t_image_fixed_base()
    {
        kos_image_header const& h = kickos_image_header;
        uintptr_t const header = reinterpret_cast<uintptr_t>(&h);
        uintptr_t const code = reinterpret_cast<uintptr_t>(&reloc_text_target);
        uintptr_t const data = reinterpret_cast<uintptr_t>(&g_reloc_target);
        tap::diag("header 0x%lx, code 0x%lx, data 0x%lx", static_cast<unsigned long>(header),
                  static_cast<unsigned long>(code), static_cast<unsigned long>(data));
        TAP_CHECK(header == KICKOS_X86_64_PROGRAM_BASE);
        TAP_CHECK(h.code_start == header);
        TAP_CHECK(code > header and code < h.code_end);
        TAP_CHECK(data >= h.window_start and data < h.window_start + h.window_size);
        TAP_CHECK(h.window_start >= h.code_end);
    }
#endif

#if defined(__aarch64__)
    // What SCTLR_EL1, MDSCR_EL1 and PMUSERENR_EL0 leave EL0, each probed in a task of its own:
    // the ops that must trap end it with KOS_EXIT_FAULT, the ones that must not exit it with 0.
    void t_el0_control_bits()
    {
        TAP_ASK(.tasks = 1, .peer_workers = 1);
        struct Probe
        {
            char const* role;
            char const* what;
            int expect;
        };
        constexpr Probe PROBES[] = {
            {"e0daif", "msr daifset (SCTLR_EL1.UMA clear)", KOS_EXIT_FAULT},
            {"e0pmu", "mrs pmccntr_el0 (PMUSERENR_EL0.EN clear)", KOS_EXIT_FAULT},
            {"e0dcc", "mrs mdccsr_el0 (MDSCR_EL1.TDCC set)", KOS_EXIT_FAULT},
            {"e0cache", "__clear_cache and dc zva (SCTLR_EL1.UCT, UCI, DZE set)", 0},
        };
        kos_task_t t = KOS_TASK_NONE;
        ArmHold hold;
        TAP_HOLD(hold.task(&t));
        bool held[sizeof(PROBES) / sizeof(PROBES[0])] = {};
        size_t i = 0;
        for (Probe const& probe : PROBES)
        {
            TAP_CHECK(PeerRun(probe.role).start(&t) == 0);
            int const status = peer_dead_status(t);
            tap::diag("%s: the task ended with %d, expected %d", probe.what, status, probe.expect);
            held[i] = status == probe.expect;
            i++;
            TAP_CHECK(task_end(&t) == 0);
        }
        for (bool const h : held)
        {
            TAP_CHECK(h);
        }
    }
#endif

#if KICKOS_SELFTEST_PEER
    namespace
    {
#if defined(__aarch64__)
        int el0_daif()
        {
            __asm volatile("msr daifset, #2\n\tmsr daifclr, #2" ::: "memory");
            return 0;
        }

        int el0_pmu()
        {
            uint64_t v = 0;
            __asm volatile("mrs %0, pmccntr_el0" : "=r"(v));
            (void)v;
            return 0;
        }

        int el0_dcc()
        {
            uint64_t v = 0;
            __asm volatile("mrs %0, mdccsr_el0" : "=r"(v));
            (void)v;
            return 0;
        }

        // DCZID_EL0.BS is log2 of the block in words, at most 9: 2 KiB.
        constexpr uint64_t DCZID_DZP = 1u << 4;
        constexpr uint64_t DCZID_BS_MASK = 0xF;
        constexpr size_t ZVA_MAX = 2048;
        // One byte past the largest block: the canary a block of ZVA_MAX leaves untouched.
        alignas(ZVA_MAX) uint8_t g_zva[ZVA_MAX + 1];

        int el0_cache()
        {
            memset(g_zva, 0xA5, sizeof(g_zva));
            __builtin___clear_cache(reinterpret_cast<char*>(g_zva),
                                    reinterpret_cast<char*>(g_zva + sizeof(g_zva)));
            uint64_t dczid = 0;
            __asm volatile("mrs %0, dczid_el0" : "=r"(dczid));
            if ((dczid & DCZID_DZP) != 0)
            {
                return 1;
            }
            size_t const block = static_cast<size_t>(4) << (dczid & DCZID_BS_MASK);
            if (block > ZVA_MAX)
            {
                return 2;
            }
            __asm volatile("dc zva, %0" ::"r"(g_zva) : "memory");
            for (size_t i = 0; i < block; i++)
            {
                if (g_zva[i] != 0)
                {
                    return 3;
                }
            }
            if (g_zva[block] != 0xA5)
            {
                return 4;
            }
            return 0;
        }

        int (*g_e0_op)() = nullptr;
        int g_e0_result = -1;

        void e0_run(void*)
        {
            g_e0_result = g_e0_op();
        }

        // `op` on a thread named `name`, so a fault is recorded as that thread's and ends the task.
        int e0_task(int (*op)(), char const* name)
        {
            g_e0_op = op;
            kos::thread::Handle const t = kos::thread::create_caps(e0_run, nullptr, name, 10,
                                                                   nullptr, 0);
            if (not t.valid())
            {
                return 1;
            }
            if (t.join(STALL_TOLERANT_US) != 0)
            {
                return 1;
            }
            return g_e0_result;
        }
#endif
        void* peer_ptr(char const* s)
        {
            return reinterpret_cast<void*>(peer_word(s));
        }
    }

    int peer_roles_aspace(int argc, char** argv)
    {
        char const* const role = argv[1];
        char const* arg = nullptr;
        if (argc == 3)
        {
            arg = argv[2];
        }
        if (argc > 3)
        {
            return PEER_WHAT;
        }
        if (arg != nullptr and peer_is(role, "pstk"))
        {
            return pool_stack_task(arg);
        }
        if (arg == nullptr and peer_is(role, "pin"))
        {
            pin_child(nullptr);
            return 0;
        }
#if defined(__aarch64__)
        if (arg == nullptr and peer_is(role, "e0daif"))
        {
            return e0_task(el0_daif, "e0daif");
        }
        if (arg == nullptr and peer_is(role, "e0pmu"))
        {
            return e0_task(el0_pmu, "e0pmu");
        }
        if (arg == nullptr and peer_is(role, "e0dcc"))
        {
            return e0_task(el0_dcc, "e0dcc");
        }
        if (arg == nullptr and peer_is(role, "e0cache"))
        {
            return e0_task(el0_cache, "e0cache");
        }
#endif
        if (arg != nullptr and peer_is(role, "shr"))
        {
            share_child(peer_ptr(arg));
            return 0;
        }
        if (arg != nullptr and peer_is(role, "shrN"))
        {
            // The same child holding no authority.
            kos_cap_grant const caps[] = {{CH_SHARE_FRAME, KOS_CAP_TRANSFER}};
            auto const w = kos::thread::create_caps(share_child, peer_ptr(arg), "shrN", 10, caps, 1);
            if (not w.valid())
            {
                return 1;
            }
            return w.join(STALL_TOLERANT_US);
        }
        if (arg == nullptr and peer_is(role, "sdc"))
        {
            sd_child(nullptr);
            return 0;
        }
        if (arg == nullptr and peer_is(role, "sdg"))
        {
            sd_grandchild(nullptr);
            return 0;
        }
        if (arg != nullptr and peer_is(role, "spw"))
        {
            space_word_worker(peer_ptr(arg));
            return 0;
        }
        if (arg != nullptr and peer_is(role, "pw"))
        {
            pw_member(peer_ptr(arg));
            return 0;
        }
        if (arg != nullptr and peer_is(role, "sib"))
        {
            return sib_writer(arg);
        }
        if (arg != nullptr and peer_is(role, "ho"))
        {
            ho_reader(peer_ptr(arg));
            return 0;
        }
        if (arg != nullptr and peer_is(role, "hsl"))
        {
            sl_reader(peer_ptr(arg));
            return 0;
        }
        if (arg != nullptr and peer_is(role, "dxD"))
        {
            dx_donor(peer_ptr(arg));
            return 0;
        }
        if (arg != nullptr and peer_is(role, "dxB"))
        {
            dx_borrower(peer_ptr(arg));
            return 0;
        }
        if (arg != nullptr and peer_is(role, "dxC"))
        {
            dx_churn(peer_ptr(arg));
            return 0;
        }
        if (arg == nullptr and peer_is(role, "rt"))
        {
            rt_member(nullptr);
            return 0;
        }
        if (arg == nullptr and peer_is(role, "fs"))
        {
            fs_member(nullptr);
            return 0;
        }
        if (arg == nullptr and peer_is(role, "lrful"))
        {
            lr_member(nullptr);
            return 0;
        }
        if (arg == nullptr and peer_is(role, "lddon"))
        {
            ld_member(nullptr);
            return 0;
        }
        if (arg != nullptr and peer_is(role, "hostile"))
        {
            return hostile_sibling(arg);
        }
        if (arg != nullptr and peer_is(role, "piS"))
        {
            return pi_seated_server(pi_server, arg);
        }
        if (arg != nullptr and peer_is(role, "piCS"))
        {
            return pi_seated_server(pi_call_server, arg);
        }
        if (arg != nullptr and peer_is(role, "piC"))
        {
            return pi_client_at(pi_client, arg);
        }
        if (arg != nullptr and peer_is(role, "piCC"))
        {
            return pi_client_at(pi_call_client, arg);
        }
        if (arg != nullptr and peer_is(role, "dt"))
        {
            dt_reader(peer_ptr(arg));
            return 0;
        }
        if (arg != nullptr and peer_is(role, "ts"))
        {
            return ts_task(arg);
        }
#if KICKOS_FAULT_ISOLATION
        if (arg != nullptr and peer_is(role, "kvic"))
        {
            return kernel_state_task(arg);
        }
        if (arg != nullptr and peer_is(role, "dsmark"))
        {
            ds_mark(peer_ptr(arg));
            return 0;
        }
#if defined(__x86_64__)
        if (arg == nullptr and peer_is(role, "pwc"))
        {
            return pw_holder(pw_cmos, "pwc", PW_CMOS_WINDOW);
        }
        if (arg == nullptr and peer_is(role, "pwb"))
        {
            kos_window const com2 = {PW_COM2, 8u, KOS_WINDOW_PORTS, 0};
            return pw_holder(pw_com2, "pwb", com2);
        }
        if (arg == nullptr and peer_is(role, "pwi"))
        {
            return pw_holder(pw_index, "pwi", PW_CMOS_WINDOW);
        }
#endif
#if defined(KICKOS_SELFTEST_SPARE_DEV)
        if (arg == nullptr and peer_is(role, "wa"))
        {
            return wa_task();
        }
        if (arg == nullptr and peer_is(role, "wah"))
        {
            wa_holder(nullptr);
            return 0;
        }
#endif
#endif
        return PEER_WHAT;
    }
#endif
#endif
}
