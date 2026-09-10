// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// KDIAG_FAULT_LINE_MAX is a STACK figure, not a taste: kprintf_fault's array is the largest
// term of the descent tests/static/check_trap_redzone.sh measures as EXITK, and as EXIT where
// no kernel block is carved. Cut small, it can start truncating a fault record silently, which
// is what this gate refuses. It runs the real formatter, and the host's 64-bit pointers make
// every %p wider here than on any target that enforces the classes.

#include <kickos/diag.h>
#include <kickos/libc/fmt.h>

#include <gtest/gtest.h>

#include <stdint.h>
#include <string>

namespace
{
    // Longer than any status_name a backend hands kickos_fault_record: "ESR_EL1", "MPESTS",
    // "si_code" and "vec:err" are the longest in arch/, at 7.
    char const* const kStatus = "0123456789ABCDEF";

    void* wild_ptr()
    {
        return reinterpret_cast<void*>(~static_cast<uintptr_t>(0));
    }

    // What kvsnprintf WOULD have written. A line that fits answers below the capacity; one that
    // truncated answers at or above it, which is the only way truncation is visible at all.
    int width(char const* fmt, ...)
    {
        char buf[KDIAG_FAULT_LINE_MAX];
        va_list ap;
        va_start(ap, fmt);
        int const n = kvsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        return n;
    }

    // The bound counts the NUL, so a line of exactly the capacity has already lost a byte.
    void expect_fits(int n)
    {
        EXPECT_LT(n, static_cast<int>(KDIAG_FAULT_LINE_MAX));
    }
}

// The four fixed-shape records, at arguments no target can exceed.
TEST(FaultLine, every_fixed_record_fits_the_bound)
{
    expect_fits(width(KDIAG_F_FAULT_PC_LOST));
    expect_fits(width(KDIAG_F_FAULT_PC, wild_ptr()));
    expect_fits(width(KDIAG_F_FAULT_PC_STAT, wild_ptr(), kStatus, ~0ull));
    expect_fits(width(KDIAG_F_FAULT_ADDR, wild_ptr()));
}

// check_faultsurvive.sh greps this one on the wire, so its spelling is fault.cc's and not the
// catalogue's; it is the sixth kprintf_fault format and belongs in this corpus anyway.
TEST(FaultLine, the_selftest_trap_witness_record_fits_the_bound)
{
    expect_fits(width("[trapwitness] CORRUPTED 0x%x\n", ~0u));
}

// The banner is the only fault line with an unbounded argument, so the bound spends its whole
// remainder on the thread name. 38 is that remainder in the longer of the two columns, and
// every thread name in the tree is six characters or fewer; tests/lib/gate.sh matches the
// banner by name, so this is what keeps that match from meeting a truncated line.
TEST(FaultLine, the_banner_holds_a_thirty_eight_character_thread_name)
{
    std::string const name(38, 'n');
    expect_fits(width(KDIAG_F_THREAD_FAULT, name.c_str()));
}

// The positive control: without it every assertion above passes on a formatter that silently
// reports the capacity instead of the true width.
TEST(FaultLine, a_name_past_the_bound_is_reported_as_truncated)
{
    std::string const name(4096, 'n');
    int const n = width(KDIAG_F_THREAD_FAULT, name.c_str());
    EXPECT_GT(n, static_cast<int>(KDIAG_FAULT_LINE_MAX));
}
