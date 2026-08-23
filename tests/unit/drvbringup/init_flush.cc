// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Shutdown-flush gate for the default init body (system/init/common/default_init_run.cc).
// The run body calls four extern "C" seams and nothing else, so the real body runs here over
// the recording fakes below.
//
// What is checked is STRUCTURAL: every probe carries a real deadline, and a refused probe is
// not followed by a second one. The park behind that rule is one a fake cannot stage: an
// untimed probe on a console service that is alive but not back in kos_recv keeps
// recv_holders nonzero, so the endpoint never reads as dead and root parks forever.
//
// kos_send is deliberately left unfaked: a body that goes back to the untimed send fails to
// LINK, before any arm runs. Do not add it.
//
// HOST-ONLY: this TU defines public kos_* names, so a target image linking it would satisfy
// them from the executable and keep the real syscall stubs' archive member out of the link.

#include <kickos/sys.h>
#include <kickos/sys/abi.h>
#include <kickos/sys/cap_index.h>
#include <kickos/sys/driver_service.h>
#include <kickos/sys/errno.h>
#include <kickos/sys/init.h>

#include <gtest/gtest.h>

#include <vector>

namespace
{
    struct Probe
    {
        kos_cap_t cap;
        size_t len;
        uint32_t timeout_us;
    };

    struct Seam
    {
        std::vector<Probe> probes;
        std::vector<int32_t> probe_rc; // per call, in order; past the end = 0
        int narrow_rc;
        uint8_t narrow_mask;
        int narrow_calls;
        int main_calls;
        int main_rc;
    };

    Seam g_seam;

    void seam_reset()
    {
        g_seam = Seam{};
        g_seam.main_rc = 7;
    }
}

extern "C"
{
    int kos_cap_narrow(kos_cap_t, uint8_t mask)
    {
        g_seam.narrow_calls++;
        g_seam.narrow_mask = mask;
        return g_seam.narrow_rc;
    }

    int32_t kos_send_timed(kos_cap_t ep, void const*, size_t len, uint32_t timeout_us)
    {
        size_t const n = g_seam.probes.size();
        g_seam.probes.push_back(Probe{ep, len, timeout_us});
        if (n < g_seam.probe_rc.size())
        {
            return g_seam.probe_rc[n];
        }
        return 0;
    }

    uint8_t kickos_app_authority(void)
    {
        return KOS_AUTH_MEMORY | KOS_AUTH_SYSTEM;
    }

    int kickos_app_main(int, char**)
    {
        g_seam.main_calls++;
        return g_seam.main_rc;
    }
}

namespace
{
    // Zero length MEANS flush, and KOS_TIMEOUT_NONE arms no deadline.
    void expect_well_formed(std::vector<Probe> const& probes)
    {
        for (Probe const& p : probes)
        {
            EXPECT_EQ(p.cap, KOS_CAP_STDOUT);
            EXPECT_EQ(p.len, 0u);
            EXPECT_NE(p.timeout_us, KOS_TIMEOUT_NONE);
            EXPECT_NE(p.timeout_us, 0u);
        }
    }

    TEST(InitFlush, DrainedConsoleTakesBothProbes)
    {
        seam_reset();
        EXPECT_EQ(kickos_default_init_run(0, nullptr), 7);
        EXPECT_EQ(g_seam.main_calls, 1);
        ASSERT_EQ(g_seam.probes.size(), 2u);
        expect_well_formed(g_seam.probes);
    }

    // The same budget as the bring-up probe on this same endpoint; a divergence here is a
    // second policy for one question.
    TEST(InitFlush, ProbeBudgetIsTheHandoverBudget)
    {
        seam_reset();
        (void)kickos_default_init_run(0, nullptr);
        ASSERT_EQ(g_seam.probes.size(), 2u);
        EXPECT_EQ(g_seam.probes[0].timeout_us, kickos::driver::KOS_DRV_HANDOVER_PROBE_US);
        EXPECT_EQ(g_seam.probes[1].timeout_us, kickos::driver::KOS_DRV_HANDOVER_PROBE_US);
    }

    // The wedged service: alive, holding the endpoint, never back in kos_recv. The first
    // probe spends its whole budget for nothing, and a second would spend it again.
    TEST(InitFlush, WedgedConsoleCostsOneBudgetAndReturns)
    {
        seam_reset();
        g_seam.probe_rc = {-KOS_ETIMEDOUT};
        EXPECT_EQ(kickos_default_init_run(0, nullptr), 7);
        ASSERT_EQ(g_seam.probes.size(), 1u);
        expect_well_formed(g_seam.probes);
    }

    // Nothing published: there is no drain to start, so the second probe is dead weight.
    TEST(InitFlush, NoPublishedConsoleSkipsTheSecondProbe)
    {
        seam_reset();
        g_seam.probe_rc = {-KOS_EBADF};
        EXPECT_EQ(kickos_default_init_run(0, nullptr), 7);
        EXPECT_EQ(g_seam.probes.size(), 1u);
    }

    // A negative main status must reach kos_shutdown unchanged, not be overwritten by the
    // flush result.
    TEST(InitFlush, MainStatusSurvivesTheFlush)
    {
        seam_reset();
        g_seam.main_rc = -3;
        g_seam.probe_rc = {-KOS_ETIMEDOUT};
        EXPECT_EQ(kickos_default_init_run(0, nullptr), -3);
    }

    TEST(InitFlush, RefusedNarrowRunsNeitherMainNorProbe)
    {
        seam_reset();
        g_seam.narrow_rc = -KOS_EBADF;
        EXPECT_EQ(kickos_default_init_run(0, nullptr), -KOS_EBADF);
        EXPECT_EQ(g_seam.main_calls, 0);
        EXPECT_EQ(g_seam.probes.size(), 0u);
    }

    TEST(InitFlush, NarrowCarriesTheAppMask)
    {
        seam_reset();
        (void)kickos_default_init_run(0, nullptr);
        EXPECT_EQ(g_seam.narrow_calls, 1);
        EXPECT_EQ(g_seam.narrow_mask, static_cast<uint8_t>(KOS_AUTH_MEMORY | KOS_AUTH_SYSTEM));
    }
}
