// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Failure-path gate for kickos::driver::bring_up and its unwind, driven against the real
// header over the recording seam in kos_seam.h.
//
// Every arm compares the FULL ordered trace. Weakening one to a subset silently drops the
// only check on `unwind`'s three orderings: lines then endpoint, endpoint close before any
// cancel, both before the diagnostic print.
//
// What this gate CANNOT witness: whether kos_thread_kill was honoured. Cancellation is
// cooperative and honoured only inside kos_irq_wait, so no spawned thread runs here and a
// peer's death stays a silicon question (tests/check_sim_drvdeath.sh). Nor does it witness
// that a close reclaims the console: only the order of the calls that would.

#include <kickos/sys/driver_service.h>

#include <kickos/sys/errno.h>

#include "kos_seam.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace drv = kickos::driver;

namespace
{
    int g_failures = 0;

    void check(bool ok, char const* what)
    {
        if (ok)
        {
            return;
        }
        printf("not ok - %s\n", what);
        g_failures++;
    }

    void check_trace(char const* want, char const* what)
    {
        char const* got = kos_seam_trace();
        if (strcmp(got, want) == 0)
        {
            return;
        }
        printf("not ok - %s\n", what);
        printf("#   want: %s\n#    got: %s\n", want, got);
        g_failures++;
    }

    void check_says(char const* needle, char const* what)
    {
        if (strstr(kos_seam_msg(), needle) != nullptr)
        {
            return;
        }
        printf("not ok - %s (diagnostic was \"%s\")\n", what, kos_seam_msg());
        g_failures++;
    }

    // ---------------------------------------------------------------------------
    // A synthetic instance, not any chip's.

    constexpr uintptr_t K_BASE = 0x4000c000u;
    constexpr uint32_t K_BLOCK = 256u;
    constexpr uint16_t K_READY_OFF = 8u;

    void t_irq(void*) {}
    void t_worker(void*) {}
    void t_console(void*) {}

    int g_block_init_rc = 0;

    int block_init(void* blk, struct kos_service_cfg const*)
    {
        // The latch ADDRESS is published here and the latch is SET by the spawn fake: no
        // child runs in this gate, so nothing else can stand in for reaching the loop.
        g_seam.latch = reinterpret_cast<volatile uint32_t*>(
            static_cast<unsigned char*>(blk) + K_READY_OFF);
        *g_seam.latch = 0u;
        return g_block_init_rc;
    }

    // Two lines: dropping to one makes a partial claim indistinguishable from none, which
    // retires the arm that separates `claimed` from `line_count`.
    constexpr drv::Descriptor k_two = {
        .tag = "[drvfake] ",
        .expected_base = K_BASE,
        .block_size = K_BLOCK,
        .ready_offset = K_READY_OFF,
        .ep_posture = drv::KOS_DRV_EP_HANDOVER,
        .svc_kind = KOS_SVC_CONSOLE,
        .line_count = 2,
        .thread_count = 2,
        .barrier_after = 1,
        .lines = {{16, KOS_IRQ_LEVEL}, {17, KOS_IRQ_LEVEL}},
        .threads = {{.entry = t_irq,
                     .name = "drvirq",
                     .prio_delta = 1,
                     .arg = drv::KOS_DRV_ARG_BLOCK,
                     .mem_grant = true,
                     .window_grant = true,
                     .cap_count = 2,
                     .caps = {{drv::KOS_DRV_RES_LINE0, KOS_CAP_WAIT},
                              {drv::KOS_DRV_RES_LINE1, KOS_CAP_WAIT}}},
                    {.entry = t_console,
                     .name = nullptr,
                     .prio_delta = 0,
                     .arg = drv::KOS_DRV_ARG_BLOCK,
                     .mem_grant = true,
                     .window_grant = false,
                     .cap_count = 1,
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT}}}},
        .block_init = block_init
    };

    // Three threads leave TWO live peers at a third-spawn failure, and two is the fewest that
    // tells reverse cancellation from forward.
    constexpr drv::Descriptor k_three = {
        .tag = "[drvfake3] ",
        .expected_base = K_BASE,
        .block_size = K_BLOCK,
        .ready_offset = K_READY_OFF,
        .ep_posture = drv::KOS_DRV_EP_HANDOVER,
        .svc_kind = KOS_SVC_CONSOLE,
        .line_count = 2,
        .thread_count = 3,
        .barrier_after = 1,
        .lines = {{16, KOS_IRQ_LEVEL}, {17, KOS_IRQ_LEVEL}},
        .threads = {{.entry = t_irq,
                     .name = "drvirq",
                     .prio_delta = 1,
                     .arg = drv::KOS_DRV_ARG_BLOCK,
                     .mem_grant = true,
                     .window_grant = true,
                     .cap_count = 2,
                     .caps = {{drv::KOS_DRV_RES_LINE0, KOS_CAP_WAIT},
                              {drv::KOS_DRV_RES_LINE1, KOS_CAP_WAIT}}},
                    {.entry = t_worker,
                     .name = "drvwork",
                     .prio_delta = 0,
                     .arg = drv::KOS_DRV_ARG_NONE,
                     .mem_grant = false,
                     .window_grant = false,
                     .cap_count = 0,
                     .caps = {}},
                    {.entry = t_console,
                     .name = nullptr,
                     .prio_delta = 0,
                     .arg = drv::KOS_DRV_ARG_BLOCK,
                     .mem_grant = true,
                     .window_grant = false,
                     .cap_count = 1,
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT}}}},
        .block_init = block_init
    };

    // barrier_after == thread_count, the position only RETAIN admits: a one-thread service
    // whose latch is polled after its only spawn and before the endpoint reaches the app.
    // Under HANDOVER leg L8 refuses this shape, so no console descriptor can reach it.
    constexpr drv::Descriptor k_tail_barrier = {
        .tag = "[drvtail] ",
        .expected_base = K_BASE,
        .block_size = K_BLOCK,
        .ready_offset = K_READY_OFF,
        .ep_posture = drv::KOS_DRV_EP_RETAIN,
        .svc_kind = KOS_SVC_SPI,
        .line_count = 0,
        .thread_count = 1,
        .barrier_after = 1,
        .lines = {},
        .threads = {{.entry = t_worker,
                     .name = nullptr,
                     .prio_delta = 0,
                     .arg = drv::KOS_DRV_ARG_BLOCK,
                     .mem_grant = true,
                     .window_grant = false,
                     .cap_count = 1,
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT}}}},
        .block_init = block_init
    };

    static_assert(drv::valid(k_two), "the two-thread gate descriptor is not a driver shape");
    static_assert(drv::valid(k_three), "the three-thread gate descriptor is not a driver shape");
    static_assert(drv::valid(k_tail_barrier),
                  "the tail-barrier gate descriptor is not a driver shape");

    struct kos_service_cfg cfg_of(uint8_t kind, uintptr_t base)
    {
        struct kos_service_cfg cfg = {};
        cfg.name = "drvfake";
        cfg.mmio_base = base;
        cfg.mmio_window = 0x40u;
        cfg.hz = 115200u;
        cfg.prio = 8u;
        cfg.kind = kind;
        return cfg;
    }

    void begin()
    {
        kos_seam_reset();
        g_block_init_rc = 0;
    }

    // ---------------------------------------------------------------------------
    // THE POSITIVE CONTROL, and it may not be removed: a fake that refused everything makes
    // every failure arm below pass while proving nothing about the failure paths.
    void case_a_complete_bring_up_touches_no_unwind()
    {
        begin();
        struct kos_service_cfg const cfg = cfg_of(KOS_SVC_CONSOLE, K_BASE);
        check(drv::bring_up(k_two, &cfg, nullptr) == 0, "a complete bring-up returns 0");
        check_trace("alloc grant ep10 pub10 claim11 claim12 spawn50 spawn51"
                    " close11 close12 close10 probe",
                    "a complete bring-up claims, spawns, drops its lines and probes");
        check(kos_seam_msg()[0] == '\0', "a complete bring-up prints no diagnostic");
    }

    // A refusal must leave NO trace at all: no arena block, no endpoint, no publish. The rc
    // alone would pass on a guard that refused after allocating.
    void case_a_refused_cfg_has_no_effect()
    {
        begin();
        struct kos_service_cfg const wrong = cfg_of(KOS_SVC_SPI, K_BASE);
        check(drv::bring_up(k_two, &wrong, nullptr) == -1, "a wrong-kind cfg is refused");
        check_trace("print print", "a wrong-kind cfg allocates nothing");

        begin();
        struct kos_service_cfg const elsewhere = cfg_of(KOS_SVC_CONSOLE, K_BASE + 0x1000u);
        check(drv::bring_up(k_two, &elsewhere, nullptr) == -1, "a foreign mmio_base is refused");
        check_trace("print print", "a foreign mmio_base allocates nothing");

        begin();
        struct kos_service_cfg const cfg = cfg_of(KOS_SVC_CONSOLE, K_BASE);
        kos_cap_t out = KOS_CAP_NONE;
        check(drv::bring_up(k_two, &cfg, &out) == -1,
              "an out_ep under HANDOVER is refused");
        check_trace("print print", "a posture mismatch allocates nothing");
    }

    // The first claim fails, so `claimed` is 0 and NOTHING may be closed but the endpoint.
    // Closing line[0] here would close KOS_CAP_NONE.
    void case_the_first_irq_claim_fails()
    {
        begin();
        g_seam.irq_claim_fail_at = 1;
        struct kos_service_cfg const cfg = cfg_of(KOS_SVC_CONSOLE, K_BASE);
        check(drv::bring_up(k_two, &cfg, nullptr) == -1, "a refused first line fails bring-up");
        check_trace("alloc grant ep10 pub10 claim! close10 print print",
                    "a refused first line closes the endpoint and no line");
        check_says("irq_claim failed", "the diagnostic names the failed claim");
    }

    // The second claim fails, so EXACTLY the first line is closed. This is the arm that
    // separates `claimed` from `line_count`.
    void case_the_second_irq_claim_fails()
    {
        begin();
        g_seam.irq_claim_fail_at = 2;
        struct kos_service_cfg const cfg = cfg_of(KOS_SVC_CONSOLE, K_BASE);
        check(drv::bring_up(k_two, &cfg, nullptr) == -1, "a refused second line fails bring-up");
        check_trace("alloc grant ep10 pub10 claim11 claim! close11 close10 print print",
                    "a refused second line closes the one line it did claim, then the endpoint");
    }

    void case_the_first_spawn_fails()
    {
        begin();
        g_seam.spawn_fail_at = 1;
        struct kos_service_cfg const cfg = cfg_of(KOS_SVC_CONSOLE, K_BASE);
        check(drv::bring_up(k_two, &cfg, nullptr) == -1, "a refused first spawn fails bring-up");
        check_trace("alloc grant ep10 pub10 claim11 claim12 spawn!"
                    " close11 close12 close10 print print",
                    "a refused first spawn closes both lines and cancels nobody");
        check_says("spawn failed", "the diagnostic names the failed spawn");
    }

    // One live peer at the failure: the close-before-cancel ordering becomes observable.
    void case_a_later_spawn_fails_and_the_peer_is_cancelled()
    {
        begin();
        g_seam.spawn_fail_at = 2;
        struct kos_service_cfg const cfg = cfg_of(KOS_SVC_CONSOLE, K_BASE);
        check(drv::bring_up(k_two, &cfg, nullptr) == -1, "a refused later spawn fails bring-up");
        check_trace("alloc grant ep10 pub10 claim11 claim12 spawn50 spawn!"
                    " close11 close12 close10 kill50 print print",
                    "a refused later spawn closes the endpoint BEFORE cancelling its peer");
    }

    // Two live peers: reverse cancellation is distinguishable from forward.
    void case_peers_are_cancelled_in_reverse_spawn_order()
    {
        begin();
        g_seam.spawn_fail_at = 3;
        struct kos_service_cfg const cfg = cfg_of(KOS_SVC_CONSOLE, K_BASE);
        check(drv::bring_up(k_three, &cfg, nullptr) == -1, "a refused third spawn fails bring-up");
        check_trace("alloc grant ep10 pub10 claim11 claim12 spawn50 spawn51 spawn!"
                    " close11 close12 close10 kill51 kill50 print print",
                    "two live peers are cancelled in reverse spawn order");
    }

    // The readiness timeout at its real width: KOS_DRV_READY_WAIT_MAX sleeps of
    // KOS_DRV_READY_WAIT_NS, about a second on silicon and free here.
    void case_a_thread_that_never_reaches_its_loop()
    {
        begin();
        g_seam.latch_on_spawn = false;
        struct kos_service_cfg const cfg = cfg_of(KOS_SVC_CONSOLE, K_BASE);
        check(drv::bring_up(k_two, &cfg, nullptr) == -1, "an unset latch fails bring-up");
        check_trace("alloc grant ep10 pub10 claim11 claim12 spawn50 sleep*1000"
                    " close11 close12 close10 kill50 print print",
                    "the readiness poll sleeps its full budget, then unwinds one live peer");
        check_says("never reached its loop", "the diagnostic names the readiness timeout");
    }

    // The barrier is STRICTLY between the spawns: the poll must not run before the first
    // spawn, where no thread could yet have latched.
    void case_the_barrier_sits_between_the_spawns()
    {
        begin();
        g_seam.latch_on_spawn = false;
        struct kos_service_cfg const cfg = cfg_of(KOS_SVC_CONSOLE, K_BASE);
        (void)drv::bring_up(k_two, &cfg, nullptr);
        char const* const trace = kos_seam_trace();
        char const* const first_spawn = strstr(trace, "spawn50");
        char const* const first_sleep = strstr(trace, "sleep*");
        check(first_spawn != nullptr and first_sleep != nullptr and first_spawn < first_sleep,
              "the readiness poll runs after the first spawn, never before it");
    }

    // The LAST barrier position, which only a one-thread service uses. Both halves are needed:
    // the rc alone cannot tell "polled after the spawn" from "never polled", so the
    // discriminating witness is the timeout, whose sleep token can only appear after spawn50.
    void case_the_barrier_can_sit_after_the_last_spawn()
    {
        begin();
        struct kos_service_cfg const cfg = cfg_of(KOS_SVC_SPI, K_BASE);
        kos_cap_t out = KOS_CAP_NONE;
        check(drv::bring_up(k_tail_barrier, &cfg, &out) == 0,
              "a latch polled after the only spawn completes bring-up");
        check(out != KOS_CAP_NONE, "the retained endpoint reaches the caller");
        check_trace("alloc grant ep10 spawn50", "the poll adds no sleep when the latch is set");

        begin();
        g_seam.latch_on_spawn = false;
        kos_cap_t out2 = KOS_CAP_NONE;
        check(drv::bring_up(k_tail_barrier, &cfg, &out2) == -1,
              "an unset latch at the last position fails bring-up");
        check_trace("alloc grant ep10 spawn50 sleep*1000 close10 kill50 print print",
                    "the poll runs AFTER the only spawn, then unwinds it");
        check_says("never reached its loop", "the diagnostic names the readiness timeout");
    }

    // A descriptor no static_assert vetted: the belt at the top of bring_up is what keeps the
    // spawn loop from writing past ThreadSet::t[]. Not constexpr, so no leg runs at compile
    // time.
    drv::Descriptor const k_overwide = {
        .tag = "[drvwide] ",
        .expected_base = K_BASE,
        .block_size = 0,
        .ready_offset = drv::KOS_DRV_READY_NONE,
        .ep_posture = drv::KOS_DRV_EP_HANDOVER,
        .svc_kind = KOS_SVC_CONSOLE,
        .line_count = 0,
        .thread_count = drv::KOS_DRV_THREADS_MAX + 1,
        .barrier_after = 1,
        .lines = {},
        .threads = {{.entry = t_console,
                     .name = nullptr,
                     .prio_delta = 0,
                     .arg = drv::KOS_DRV_ARG_NONE,
                     .mem_grant = false,
                     .window_grant = false,
                     .cap_count = 1,
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT}}}},
        .block_init = nullptr
    };

    void case_an_unvetted_descriptor_is_refused_before_anything_is_taken()
    {
        begin();
        struct kos_service_cfg const cfg = cfg_of(KOS_SVC_CONSOLE, K_BASE);
        check(drv::bring_up(k_overwide, &cfg, nullptr) == -1,
              "a descriptor no leg vetted is refused");
        check_trace("print print", "the refusal allocates nothing and spawns nothing");
        check_says("well-formed driver shape", "the diagnostic names the descriptor");
    }

    // block_init refuses: the block is allocated and granted, and there is no endpoint yet
    // to close. A close here would close KOS_CAP_NONE.
    void case_block_init_refuses_the_cfg()
    {
        begin();
        g_block_init_rc = -KOS_EINVAL;
        struct kos_service_cfg const cfg = cfg_of(KOS_SVC_CONSOLE, K_BASE);
        check(drv::bring_up(k_two, &cfg, nullptr) == -1, "a refused block_init fails bring-up");
        check_trace("alloc grant print print", "a refused block_init closes nothing");
    }

    void case_the_publish_fails()
    {
        begin();
        g_seam.console_publish_fails = true;
        struct kos_service_cfg const cfg = cfg_of(KOS_SVC_CONSOLE, K_BASE);
        check(drv::bring_up(k_two, &cfg, nullptr) == -1, "a refused publish fails bring-up");
        check_trace("alloc grant ep10 pub! close10 print print",
                    "a refused publish closes the endpoint it could not publish");
    }

    // This arm and the next differ only in their SIDE EFFECTS: both refusals return a
    // negative code, so no rc assertion can tell them apart.
    void case_the_handover_probe_reports_a_dead_driver()
    {
        begin();
        g_seam.send_timed_rc = -KOS_EPIPE;
        struct kos_service_cfg const cfg = cfg_of(KOS_SVC_CONSOLE, K_BASE);
        check(drv::bring_up(k_two, &cfg, nullptr) == -KOS_EPIPE,
              "an EPIPE probe returns EPIPE unchanged");
        check_trace("alloc grant ep10 pub10 claim11 claim12 spawn50 spawn51"
                    " close11 close12 close10 probe kill51 kill50 print print",
                    "an EPIPE probe cancels every peer in reverse order");
        check_says("died during bring-up", "the diagnostic names the dead thread");
    }

    void case_a_timed_out_handover_probe_cancels_nothing()
    {
        begin();
        g_seam.send_timed_rc = -KOS_ETIMEDOUT;
        struct kos_service_cfg const cfg = cfg_of(KOS_SVC_CONSOLE, K_BASE);
        check(drv::bring_up(k_two, &cfg, nullptr) == -KOS_ETIMEDOUT,
              "a timed-out probe returns ETIMEDOUT unchanged");
        check_trace("alloc grant ep10 pub10 claim11 claim12 spawn50 spawn51"
                    " close11 close12 close10 probe",
                    "a timed-out probe cancels nobody and prints nothing");
        check(kos_seam_msg()[0] == '\0', "a timed-out probe prints no diagnostic");
    }
}

int main()
{
    case_a_complete_bring_up_touches_no_unwind();
    case_a_refused_cfg_has_no_effect();
    case_an_unvetted_descriptor_is_refused_before_anything_is_taken();
    case_the_barrier_can_sit_after_the_last_spawn();
    case_block_init_refuses_the_cfg();
    case_the_publish_fails();
    case_the_first_irq_claim_fails();
    case_the_second_irq_claim_fails();
    case_the_first_spawn_fails();
    case_a_later_spawn_fails_and_the_peer_is_cancelled();
    case_peers_are_cancelled_in_reverse_spawn_order();
    case_a_thread_that_never_reaches_its_loop();
    case_the_barrier_sits_between_the_spawns();
    case_the_handover_probe_reports_a_dead_driver();
    case_a_timed_out_handover_probe_cancels_nothing();

    if (g_failures != 0)
    {
        printf("FAIL: %d driver bring-up failure-path check(s) failed\n", g_failures);
        return 1;
    }
    printf("ok - driver bring-up unwind (claim, readiness, spawn, handover tail)\n");
    return 0;
}
