// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Asserts what a KICKOS_BENCH image grants a thread of it. The KOS_SYS_BENCH ops that reach
// the interrupt controller or the inter-core doorbell answer -KOS_EPERM without KOS_AUTH_IRQ,
// the end-to-end arm takes the line from a cap the caller may wait on rather than from a
// number, and every caller-supplied count is refused above the ceiling abi.h names.
//
// Three ops take no authority at all. RAISE carries no line of its own, the arm having chosen
// one from a cap, and it answers -KOS_EBUSY to anybody until that waiter has parked. RESET
// answers 0 to anybody and clears the distributions. CLOSE answers -KOS_EPERM to a thread
// that is not the armed waiter and counts the span into `dropped`, but ends the span either
// way, which is what the root close after the child reads.
//
// Root declares KOS_AUTH_IRQ and the child declares nothing, so each root arm is the positive
// control for the child arm beside it.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/cap_index.h>
#include <kickos/sys/errno.h>
#include <kickos/libc/fmt.h>
#include <kickos/sys/emit.h>
#include <kickos/sys/irq_free.h>

using kickos::emit;

KICKOS_APP_AUTHORITY(KOS_AUTH_MEMORY | KOS_AUTH_SYSTEM | KOS_AUTH_IRQ);

namespace
{
    constexpr int SETUP_LINE = KICKOS_IRQ_FREE_BASE + 0;
    constexpr int ARM_LINE = KICKOS_IRQ_FREE_BASE + 1;
    // Root holds no cap at this index: its table has slot 0 (the console) and whatever the
    // claim below installs, so the arm reaches the resolve and is refused there.
    constexpr uint32_t NO_SUCH_CAP = 30;
    // The child's only cap: a signal-only copy of the line root claimed. It cannot wait, so
    // the arm must refuse a holder that cannot wait on the line it would name.
    constexpr int CH_IRQ_SIGNAL = 1;

    int failures = 0;
    int arms = 0;

    void check(bool ok, char const* what)
    {
        char msg[112];
        if (ok)
        {
            arms = arms + 1;
            ksnprintf(msg, sizeof(msg), "[benchauth] ok - %s\n", what);
            emit(msg);
            return;
        }
        failures = failures + 1;
        ksnprintf(msg, sizeof(msg), "[benchauth] ERROR: %s\n", what);
        emit(msg);
    }

    void report_rc(char const* what, int64_t rc)
    {
        char msg[112];
        ksnprintf(msg, sizeof(msg), "[benchauth]   %s rc=%d\n", what, static_cast<int>(rc));
        emit(msg);
    }

    // At one kernel core the doorbell arm is compiled out ahead of every check in it, so the
    // refusal to expect there is -KOS_ENOSYS and the bound cannot be read at all.
    bool doorbell_refused(int64_t rc, int32_t want)
    {
#if KICKOS_KERNEL_CORES > 1
        return rc == want;
#else
        (void)want;
        return rc == -KOS_ENOSYS;
#endif
    }

    // Authority 0 and one SIGNAL-only IRQ cap: what a child of the bench app is.
    void unauthorised(void*)
    {
        int64_t rc = kos_bench(KOS_BENCH_OP_IRQ_SETUP, SETUP_LINE, 0);
        report_rc("child irq_setup", rc);
        check(rc == -KOS_EPERM, "child cannot attach a tier-2 handler");

        // Asked at the ceiling, so the refusal is the authority gate and not the bound.
        rc = kos_bench(KOS_BENCH_OP_IRQ_SWEEP, KOS_BENCH_SAMPLES_MAX, 0);
        report_rc("child irq_sweep", rc);
        check(rc == -KOS_EPERM, "child cannot sweep the line");

        rc = kos_bench(KOS_BENCH_OP_IRQ_WCASE, 0, KOS_BENCH_SAMPLES_MAX);
        report_rc("child irq_wcase", rc);
        check(rc == -KOS_EPERM, "child cannot sweep a masked span");

        rc = kos_bench(KOS_BENCH_OP_DOORBELL_PROBE, 0, KOS_BENCH_ROUNDS_MAX);
        report_rc("child doorbell_probe", rc);
        check(doorbell_refused(rc, -KOS_EPERM), "child cannot run doorbell rounds");

        rc = kos_bench(KOS_BENCH_OP_E2E_ARM, CH_IRQ_SIGNAL, 0);
        report_rc("child e2e_arm (SIGNAL-only cap)", rc);
        check(rc == -KOS_EPERM, "a cap without WAIT cannot name the span's line");

        // The three ops that take no authority, over the span root re-armed above. The raise
        // and the reset leave it alone; the close ends it, and the root arm after the join
        // reads that.
        rc = kos_bench(KOS_BENCH_OP_E2E_RAISE, 0, 0);
        report_rc("child e2e_raise", rc);
        check(rc == -KOS_EBUSY, "a raise the parked waiter has not invited injects nothing");

        rc = kos_bench(KOS_BENCH_OP_RESET, 0, 0);
        report_rc("child reset", rc);
        check(rc == 0, "reset takes no authority and clears the distributions");

        rc = kos_bench(KOS_BENCH_OP_E2E_CLOSE, 0, 0);
        report_rc("child e2e_close", rc);
        check(rc == -KOS_EPERM, "a close from anyone but the armed waiter is dropped");
    }
}

int main(int, char**)
{
    // --- Root holds KOS_AUTH_IRQ: the positive control for every child arm below ---------
    int64_t rc = kos_bench(KOS_BENCH_OP_IRQ_SETUP, SETUP_LINE, 0);
    report_rc("root irq_setup", rc);
    check(rc != -KOS_EPERM, "KOS_AUTH_IRQ reaches the tier-2 attach");

    rc = kos_bench(KOS_BENCH_OP_IRQ_SWEEP, KOS_BENCH_SAMPLES_MAX, 0);
    report_rc("root irq_sweep at the ceiling", rc);
    check(rc >= 0, "a sweep at KOS_BENCH_SAMPLES_MAX is admitted");

    // --- No caller asks the kernel for unbounded work -----------------------------------
    rc = kos_bench(KOS_BENCH_OP_IRQ_SWEEP, KOS_BENCH_SAMPLES_MAX + 1u, 0);
    report_rc("root irq_sweep above the ceiling", rc);
    check(rc == -KOS_EINVAL, "a sweep above KOS_BENCH_SAMPLES_MAX is refused");

    rc = kos_bench(KOS_BENCH_OP_IRQ_WCASE, 0, KOS_BENCH_SAMPLES_MAX + 1u);
    report_rc("root irq_wcase above the ceiling", rc);
    check(rc == -KOS_EINVAL, "a masked-span sweep above KOS_BENCH_SAMPLES_MAX is refused");

    rc = kos_bench(KOS_BENCH_OP_DOORBELL_PROBE, 0, KOS_BENCH_ROUNDS_MAX + 1u);
    report_rc("root doorbell_probe above the ceiling", rc);
    check(doorbell_refused(rc, -KOS_EINVAL),
          "doorbell rounds above KOS_BENCH_ROUNDS_MAX are refused");

    // --- The end-to-end arm takes a cap, not a line number ------------------------------
    rc = kos_bench(KOS_BENCH_OP_E2E_ARM, NO_SUCH_CAP, 0);
    report_rc("root e2e_arm (no such cap)", rc);
    check(rc == -KOS_EBADF, "KOS_AUTH_IRQ alone does not name a line to arm");

    kos_cap_t irq = KOS_CAP_NONE;
#if KICKOS_KERNEL_CORES > 1
    // A line is claimed by a thread pinned where it runs; only the claim needs root there.
    kos_thread_t const self = kos_thread_self();
    (void)kos_thread_set_affinity(self, 1u << 0);
#endif
    int const crc = kos_irq_claim(ARM_LINE, KOS_IRQ_EDGE, &irq);
#if KICKOS_KERNEL_CORES > 1
    (void)kos_thread_set_affinity(self, 0);
#endif
    report_rc("root irq_claim", crc);
    check(crc == 0, "root claims the line the arm will name");

    rc = kos_bench(KOS_BENCH_OP_E2E_ARM, irq, 0);
    report_rc("root e2e_arm (own cap)", rc);
    check(rc == 0, "the line the caller may wait on is armed");

    // --- What RAISE, RESET and CLOSE answer, none of them taking authority --------------
    rc = kos_bench(KOS_BENCH_OP_E2E_CLOSE, 0, 0);
    report_rc("root e2e_close (armed, never raised)", rc);
    check(rc == -KOS_EBUSY, "the armed waiter's close of a span that never woke is dropped");

    rc = kos_bench(KOS_BENCH_OP_E2E_RAISE, 0, 0);
    report_rc("root e2e_raise (no span open)", rc);
    check(rc == -KOS_EBUSY, "KOS_AUTH_IRQ does not make a raise land either");

    rc = kos_bench(KOS_BENCH_OP_RESET, 0, 0);
    report_rc("root reset", rc);
    check(rc == 0, "reset answers 0 and leaves the e2e state alone");

    // The child's close below has to have a live span to be refused over, and the reset
    // above leaves the state untouched on purpose.
    rc = kos_bench(KOS_BENCH_OP_E2E_ARM, irq, 0);
    report_rc("root e2e_arm (re-armed for the child)", rc);
    check(rc == 0, "the span is open again");

    // --- The same ops from a thread that declared nothing -------------------------------
    kos_cap_grant caps[] = {{irq, KOS_CAP_SIGNAL}};
    auto child = kos::thread::create_caps(unauthorised, nullptr, "bauth_c", 1, caps, 1);
    check(child.valid(), "the unauthorised child was spawned");
    if (child.valid())
    {
        (void)kos_thread_join(child.id(), KOS_TIMEOUT_NONE);
    }

    // -KOS_EINVAL and not the -KOS_EBUSY the same call answered over an armed span above: the
    // child's refused close ended it.
    rc = kos_bench(KOS_BENCH_OP_E2E_CLOSE, 0, 0);
    report_rc("root e2e_close (after the child's)", rc);
    check(rc == -KOS_EINVAL, "a refused close still consumed the span root had armed");

    char msg[96];
    if (failures == 0)
    {
        ksnprintf(msg, sizeof(msg), "[benchauth] PASS (%d arms)\n", arms);
    }
    else
    {
        ksnprintf(msg, sizeof(msg), "[benchauth] FAIL (%d failed)\n", failures);
    }
    emit(msg);
    return 0;
}
