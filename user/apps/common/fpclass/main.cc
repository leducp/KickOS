// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Classify and print doubles through the compiler and newlib, and compare each answer with the
// one IEEE 754 gives, a subnormal's under the FPU's own denormal posture (subnormals_flush
// below). Under -mdfpu on the RX72M, GNURX's compare once branched the unordered way for every
// ordered pair, so isnan read 1.0 as a NaN and printf printed "nan" for it, while `<` and `==`
// stayed right; the toolchain's kickos-rx-dfpu-compare patch is the fix and this is its
// witness on silicon. Every value is read through a volatile, so the compiler folds none of
// them and the code under test is the code the board runs.
//
// One line per arm, each ending in ok or FAIL, then a verdict. The gate,
// tests/integration/check_fpclass.sh, requires every arm's line by name.

#include <kickos/kos.h>

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace
{
    int g_bad = 0;
    char g_line[128];

    // The kernel console drops what its ring cannot hold, so each line gets time to drain.
    void say()
    {
        kos::print(g_line);
        kos::sleep_ns(20'000'000);
    }

    double from_bits(uint64_t u)
    {
        double d;
        memcpy(&d, &u, sizeof(d));
        return d;
    }

    // A branch on the predicate, so the code under test is the compiler's compare-and-branch.
    unsigned bit(bool predicate, unsigned value)
    {
        if (predicate)
        {
            return value;
        }
        return 0;
    }

    char const* verdict(bool ok)
    {
        if (ok)
        {
            return "ok";
        }
        return "FAIL";
    }

    // The RXv3 DFPU handles a denormal operand as 0 while DPSW.DDN (bit 8) is set, as it is
    // from reset and in every thread arch_context_init seats (arch/rx/rxv3/arch_rxv3.cc), so
    // a subnormal double classifies and prints as zero there. Read from the register rather
    // than assumed, so a posture that stops flushing is held to IEEE 754.
    bool subnormals_flush()
    {
#if defined(__RX_DFPU_INSNS__)
        uint32_t dpsw = 0;
        __asm__ volatile("mvfdc dpsw, %0" : "=r"(dpsw));
        snprintf(g_line, sizeof(g_line), "[fpclass] dpsw %08lx ddn %u\n",
                 static_cast<unsigned long>(dpsw), static_cast<unsigned>((dpsw >> 8) & 1u));
        say();
        return (dpsw & 0x100u) != 0;
#else
        return false;
#endif
    }

    struct Class
    {
        char const* name;
        double v;
        int nan, inf, finite, normal, fpc;
    };

    // isnan, isinf, isfinite, isnormal and fpclassify, which GCC builds from the unordered
    // compare codes; isunordered against 0 from either side.
    void arm_class(Class const& c)
    {
        volatile double x = c.v;
        double const v = x;
        int const is_nan = static_cast<int>(bit(isnan(v), 1));
        int const is_inf = static_cast<int>(bit(isinf(v), 1));
        int const fin = static_cast<int>(bit(isfinite(v), 1));
        int const nrm = static_cast<int>(bit(isnormal(v), 1));
        int const fpc = fpclassify(v);
        int const un = static_cast<int>(bit(__builtin_isunordered(v, 0.0), 1)
                                        + bit(__builtin_isunordered(0.0, v), 2));
        int const want_un = static_cast<int>(bit(c.nan != 0, 3));
        bool const ok = is_nan == c.nan and is_inf == c.inf and fin == c.finite
                        and nrm == c.normal and fpc == c.fpc and un == want_un;
        if (not ok)
        {
            g_bad++;
        }
        snprintf(g_line, sizeof(g_line),
                 "[fpclass] class %s nan %d inf %d fin %d nrm %d fpc %d un %d %s\n", c.name,
                 is_nan, is_inf, fin, nrm, fpc, un, verdict(ok));
        say();
    }

    // The ordered family over pairs a NaN takes either side of, and the plain compares beside
    // it, against masks worked out by hand rather than by the compiler under test. The bits
    // are isless 001, islessequal 002, isgreater 004, isgreaterequal 008, islessgreater 010,
    // isunordered 020, then < 040, <= 080, == 100 and != 200.
    void arm_order(char const* name, double av, double bv, unsigned want)
    {
        volatile double xa = av;
        volatile double xb = bv;
        double const a = xa;
        double const b = xb;
        unsigned const got = bit(__builtin_isless(a, b), 1u)
                             | bit(__builtin_islessequal(a, b), 2u)
                             | bit(__builtin_isgreater(a, b), 4u)
                             | bit(__builtin_isgreaterequal(a, b), 8u)
                             | bit(__builtin_islessgreater(a, b), 16u)
                             | bit(__builtin_isunordered(a, b), 32u) | bit(a < b, 64u)
                             | bit(a <= b, 128u) | bit(a == b, 256u) | bit(a != b, 512u);
        if (got != want)
        {
            g_bad++;
        }
        snprintf(g_line, sizeof(g_line), "[fpclass] order %s got %03x want %03x %s\n", name,
                 got, want, verdict(got == want));
        say();
    }

    // The same masks for float. Under -mdfpu the RX compiler keeps isnan, < and == on the
    // single-precision fcmp and widens the unordered family to the double compare, so both
    // paths run here.
    void arm_order_f(char const* name, float av, float bv, unsigned want)
    {
        volatile float xa = av;
        volatile float xb = bv;
        float const a = xa;
        float const b = xb;
        unsigned const got = bit(__builtin_isless(a, b), 1u)
                             | bit(__builtin_islessequal(a, b), 2u)
                             | bit(__builtin_isgreater(a, b), 4u)
                             | bit(__builtin_isgreaterequal(a, b), 8u)
                             | bit(__builtin_islessgreater(a, b), 16u)
                             | bit(__builtin_isunordered(a, b), 32u) | bit(a < b, 64u)
                             | bit(a <= b, 128u) | bit(a == b, 256u) | bit(a != b, 512u);
        if (got != want)
        {
            g_bad++;
        }
        snprintf(g_line, sizeof(g_line), "[fpclass] orderf %s got %03x want %03x %s\n", name,
                 got, want, verdict(got == want));
        say();
    }

    void arm_fmt(char const* name, char const* fmt, double v, char const* want)
    {
        volatile double x = v;
        char got[40];
        snprintf(got, sizeof(got), fmt, x);
        bool const ok = strcmp(got, want) == 0;
        if (not ok)
        {
            g_bad++;
        }
        snprintf(g_line, sizeof(g_line), "[fpclass] fmt %s [%s] want [%s] %s\n", name, got, want,
                 verdict(ok));
        say();
    }
}

int main(int, char**)
{
    kos::print("[fpclass] start\n");

    volatile double zero = 0.0;
    snprintf(g_line, sizeof(g_line), "[fpclass] sizeof(double) %u DBL_MANT_DIG %d\n",
             static_cast<unsigned>(sizeof(double)), DBL_MANT_DIG);
    say();

    double const qnan = from_bits(0x7ff8000000000000ull);
    double const inf = from_bits(0x7ff0000000000000ull);
    double const sub = from_bits(0x000fffffffffffffull);
    bool const ftz = subnormals_flush();
    int sub_fpc = FP_SUBNORMAL;
    char const* sub_g = "4.94066e-324";
    if (ftz)
    {
        sub_fpc = FP_ZERO;
        sub_g = "0";
    }
    Class const classes[] = {
        {"qnan", qnan, 1, 0, 0, 0, FP_NAN},
        {"0/0", zero / zero, 1, 0, 0, 0, FP_NAN},
        {"+inf", inf, 0, 1, 0, 0, FP_INFINITE},
        {"-inf", -inf, 0, 1, 0, 0, FP_INFINITE},
        {"+0", 0.0, 0, 0, 1, 0, FP_ZERO},
        {"-0", -0.0, 0, 0, 1, 0, FP_ZERO},
        {"1", 1.0, 0, 0, 1, 1, FP_NORMAL},
        {"-2.5", -2.5, 0, 0, 1, 1, FP_NORMAL},
        {"max", DBL_MAX, 0, 0, 1, 1, FP_NORMAL},
        {"min", DBL_MIN, 0, 0, 1, 1, FP_NORMAL},
        {"sub", sub, 0, 0, 1, 0, sub_fpc},
        {"-sub", -sub, 0, 0, 1, 0, sub_fpc},
    };
    for (Class const& c : classes)
    {
        arm_class(c);
    }

    arm_order("nan,1", qnan, 1.0, 0x220);
    arm_order("1,nan", 1.0, qnan, 0x220);
    arm_order("nan,nan", qnan, qnan, 0x220);
    arm_order("1,2", 1.0, 2.0, 0x2d3);
    arm_order("2,1", 2.0, 1.0, 0x21c);
    arm_order("1,1", 1.0, 1.0, 0x18a);
    arm_order("-inf,inf", -inf, inf, 0x2d3);

    float const qnan_f = __builtin_nanf("");
    arm_order_f("nan,1", qnan_f, 1.0f, 0x220);
    arm_order_f("1,nan", 1.0f, qnan_f, 0x220);
    arm_order_f("1,2", 1.0f, 2.0f, 0x2d3);
    arm_order_f("2,1", 2.0f, 1.0f, 0x21c);
    arm_order_f("1,1", 1.0f, 1.0f, 0x18a);

    arm_fmt("f1", "%f", 1.0, "1.000000");
    arm_fmt("f-2.5", "%f", -2.5, "-2.500000");
    arm_fmt("g0.1", "%g", 0.1, "0.1");
    arm_fmt("e", "%e", 123456789.125, "1.234568e+08");
    arm_fmt("17g", "%.17g", 0.1, "0.10000000000000001");
    arm_fmt("gbig", "%g", 1e300, "1e+300");
    arm_fmt("gmax", "%g", DBL_MAX, "1.79769e+308");
    arm_fmt("gsub", "%g", 5e-324, sub_g);
    arm_fmt("fnan", "%f", qnan, "nan");
    arm_fmt("finf", "%f", inf, "inf");
    arm_fmt("e-inf", "%e", -inf, "-inf");

    if (g_bad == 0)
    {
        kos::print("[fpclass] PASS\n");
    }
    else
    {
        snprintf(g_line, sizeof(g_line), "[fpclass] FAIL %d\n", g_bad);
        say();
    }
    // Returning from main is a kos_shutdown(0), which is what ends the qemu run.
    return 0;
}
