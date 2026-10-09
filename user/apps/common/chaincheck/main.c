// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <iso646.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if CHAINCHECK_FP
#include <float.h>
#include <math.h>
#endif

static unsigned g_arms = 0;
static unsigned g_failed = 0;

static volatile int g_vint;

static int vint(int x)
{
    g_vint = x;
    return g_vint;
}

static volatile long g_vlong;

static long vlong(long x)
{
    g_vlong = x;
    return g_vlong;
}

static volatile int16_t g_vi16;

static int16_t vi16(int16_t x)
{
    g_vi16 = x;
    return g_vi16;
}

static volatile uint16_t g_vu16;

static uint16_t vu16(uint16_t x)
{
    g_vu16 = x;
    return g_vu16;
}

static volatile int32_t g_vi32;

static int32_t vi32(int32_t x)
{
    g_vi32 = x;
    return g_vi32;
}

static volatile uint32_t g_vu32;

static uint32_t vu32(uint32_t x)
{
    g_vu32 = x;
    return g_vu32;
}

static volatile int64_t g_vi64;

static int64_t vi64(int64_t x)
{
    g_vi64 = x;
    return g_vi64;
}

static volatile uint64_t g_vu64;

static uint64_t vu64(uint64_t x)
{
    g_vu64 = x;
    return g_vu64;
}

static volatile size_t g_vsize;

static size_t vsize(size_t x)
{
    g_vsize = x;
    return g_vsize;
}

static char const* hex(char* out, bool negative, uint64_t magnitude)
{
    char digits[16];
    size_t n = 0;
    size_t at = 0;
    do
    {
        digits[n] = "0123456789abcdef"[magnitude & 0xfu];
        n++;
        magnitude >>= 4;
    } while (magnitude != 0);
    if (negative)
    {
        out[at] = '-';
        at++;
    }
    out[at] = '0';
    out[at + 1] = 'x';
    at += 2;
    while (n > 0)
    {
        n--;
        out[at] = digits[n];
        at++;
    }
    out[at] = '\0';
    return out;
}

static char const* hex_signed(char* out, int64_t v)
{
    if (v < 0)
    {
        return hex(out, true, 0u - (uint64_t)v);
    }
    return hex(out, false, (uint64_t)v);
}

static void say(char const* fmt, ...) __attribute__((format(printf, 1, 2)));

// Through vsnprintf and fputs, not printf: a second format engine does not fit the ESP32-C6's
// code window.
static void say(char const* fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    fputs(line, stdout);
}

static void report(char const* name, bool ok, char const* got, char const* want)
{
    g_arms++;
    if (ok)
    {
        fputs("[chaincheck] ok - ", stdout);
        fputs(name, stdout);
        fputs("\n", stdout);
        return;
    }
    g_failed++;
    fputs("[chaincheck] FAIL - ", stdout);
    fputs(name, stdout);
    fputs(": got ", stdout);
    fputs(got, stdout);
    fputs(" want ", stdout);
    fputs(want, stdout);
    fputs("\n", stdout);
}

static void ck_u64(char const* name, uint64_t got, uint64_t want)
{
    char g[24];
    char w[24];
    report(name, got == want, hex(g, false, got), hex(w, false, want));
}

static void ck_i64(char const* name, int64_t got, int64_t want)
{
    char g[24];
    char w[24];
    report(name, got == want, hex_signed(g, got), hex_signed(w, want));
}

static void ck_fmt(char const* name, char const* want, char const* fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void ck_fmt(char const* name, char const* want, char const* fmt, ...)
{
    char got[64];
    char g[72];
    char w[72];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(got, sizeof(got), fmt, ap);
    va_end(ap);
    snprintf(g, sizeof(g), "[%s]", got);
    snprintf(w, sizeof(w), "[%s]", want);
    report(name, strcmp(got, want) == 0, g, w);
}

static uint32_t bit(bool predicate, uint32_t value)
{
    if (predicate)
    {
        return value;
    }
    return 0;
}

#if CHAINCHECK_INT

static void arith32(void)
{
    ck_i64("i32 add to max", vi32(0x7ffffffe) + vi32(1), INT32_MAX);
    ck_i64("i32 sub to negative", vi32(-5) - vi32(7), -12);
    ck_i64("i32 mul negative", vi32(-7) * vi32(6), -42);
    ck_i64("i32 mul near max", vi32(46341) * vi32(46340), 2147441940);
    ck_i64("i32 div truncates", vi32(-7) / vi32(2), -3);
    ck_i64("i32 div both negative", vi32(-7) / vi32(-2), 3);
    ck_i64("i32 mod takes the dividend sign", vi32(-7) % vi32(2), -1);
    ck_i64("i32 mod by negative", vi32(7) % vi32(-2), 1);
    ck_i64("i32 div min by 2", vi32(INT32_MIN) / vi32(2), -1073741824);
    ck_i64("i32 div min by -2", vi32(INT32_MIN) / vi32(-2), 1073741824);
    ck_i64("i32 mod min by 3", vi32(INT32_MIN) % vi32(3), -2);
    ck_i64("i32 shr negative is arithmetic", vi32(-16) >> vint(2), -4);
    ck_i64("i16 promotes before mul", vi16(-300) * vi16(300), -90000);
    ck_i64("u16 promotes before add", vu16(0xffffu) + 1, 65536);
    ck_u64("u32 add wraps", vu32(0xffffffffu) + vu32(2), 1);
    ck_u64("u32 sub wraps", vu32(0) - vu32(1), 0xffffffffu);
    ck_u64("u32 mul wraps", vu32(0x10001u) * vu32(0x10001u), 0x20001u);
    ck_u64("u32 div", vu32(0xffffffffu) / vu32(3), 0x55555555u);
    ck_u64("u32 mod", vu32(0xffffffffu) % vu32(10), 5);
    ck_u64("u32 div by larger", vu32(5) / vu32(0x80000000u), 0);
    ck_u64("u32 div top bit by itself", vu32(0x80000000u) / vu32(0x80000000u), 1);
    ck_u64("u32 shl 0", vu32(0x12345678u) << vint(0), 0x12345678u);
    ck_u64("u32 shl 31", vu32(1) << vint(31), 0x80000000u);
    ck_u64("u32 shr 31", vu32(0x80000000u) >> vint(31), 1);
    ck_u64("u8 store wraps", (uint8_t)vu32(300), 44);
}

static void arith64(void)
{
    ck_i64("i64 add carries", vi64(0xffffffff) + vi64(1), 0x100000000);
    ck_i64("i64 sub borrows", vi64(0x100000000) - vi64(1), 0xffffffff);
    ck_i64("i64 mul", vi64(0x12345678) * vi64(0x9abcdef), 0xb00ea4e242d208);
    ck_i64("i64 mul negative", vi64(-3000000000) * vi64(3), -9000000000);
    ck_u64("u64 mul high words", vu64(0xffffffffu) * vu64(0xffffffffu), 0xfffffffe00000001u);
    ck_u64("u64 mul wraps", vu64(UINT64_MAX) * vu64(UINT64_MAX), 1);
    ck_u64("u64 add wraps", vu64(UINT64_MAX) + vu64(1), 0);
    ck_u64("u64 sub wraps", vu64(0) - vu64(1), UINT64_MAX);
    ck_i64("i64 div", vi64(9000000000) / vi64(7), 1285714285);
    ck_i64("i64 mod", vi64(9000000000) % vi64(7), 5);
    ck_i64("i64 div truncates", vi64(-9000000000) / vi64(7), -1285714285);
    ck_i64("i64 mod takes the dividend sign", vi64(-9000000000) % vi64(7), -5);
    ck_i64("i64 div by negative", vi64(9000000000) / vi64(-7), -1285714285);
    ck_i64("i64 mod by negative", vi64(9000000000) % vi64(-7), 5);
    ck_i64("i64 div min by 2", vi64(INT64_MIN) / vi64(2), INT64_MIN / 2);
    ck_i64("i64 div min by -2", vi64(INT64_MIN) / vi64(-2), 4611686018427387904);
    ck_i64("i64 mod min by 3", vi64(INT64_MIN) % vi64(3), -2);
    ck_i64("i64 div by 2^32", vi64(INT64_MAX) / vi64(0x100000000), 2147483647);
    ck_i64("i64 div by 33 bits", vi64(INT64_MAX) / vi64(0x123456789), 1887436800);
    ck_i64("i64 mod by 33 bits", vi64(INT64_MAX) % vi64(0x123456789), 1266679807);
    ck_u64("u64 div", vu64(UINT64_MAX) / vu64(10), 1844674407370955161u);
    ck_u64("u64 mod", vu64(UINT64_MAX) % vu64(10), 5);
    ck_u64("u64 div by 2^32+1", vu64(UINT64_MAX) / vu64(0x100000001u), 0xffffffffu);
    ck_u64("u64 mod by 2^32+1", vu64(UINT64_MAX) % vu64(0x100000001u), 0);
    ck_u64("u64 div by 32 bits", vu64(0xfedcba9876543210u) / vu64(0x12345678u), 0xe00000077u);
    ck_u64("u64 mod by 32 bits", vu64(0xfedcba9876543210u) % vu64(0x12345678u), 0x48);
    ck_u64("u64 div by 45 bits", vu64(0xfedcba9876543210u) / vu64(0x123456789abcu), 0xe0000);
    ck_u64("u64 mod by 45 bits", vu64(0xfedcba9876543210u) % vu64(0x123456789abcu), 0xc3210);
    ck_u64("u64 div by larger", vu64(5) / vu64(0x8000000000000000u), 0);
    ck_u64("u64 div top bit by itself", vu64(0x8000000000000000u) / vu64(0x8000000000000000u), 1);
    ck_u64("u64 shl 0", vu64(0x0123456789abcdefu) << vint(0), 0x0123456789abcdefu);
    ck_u64("u64 shl 31", vu64(1) << vint(31), 0x80000000u);
    ck_u64("u64 shl 32", vu64(1) << vint(32), 0x100000000u);
    ck_u64("u64 shl 33 across words", vu64(0x80000001u) << vint(33), 0x200000000u);
    ck_u64("u64 shl 63", vu64(1) << vint(63), 0x8000000000000000u);
    ck_u64("u64 shr 31", vu64(0x8000000000000000u) >> vint(31), 0x100000000u);
    ck_u64("u64 shr 32", vu64(0x8000000000000000u) >> vint(32), 0x80000000u);
    ck_u64("u64 shr 63", vu64(0x8000000000000000u) >> vint(63), 1);
    ck_i64("i64 shr 63 is arithmetic", vi64(INT64_MIN) >> vint(63), -1);
    ck_i64("i64 shr 32 is arithmetic", vi64(INT64_MIN) >> vint(32), INT32_MIN);
    ck_i64("i32 widens with its sign", (int64_t)vi32(-1), -1);
    ck_u64("u32 widens with zeros", (uint64_t)vu32(0xffffffffu), 0xffffffffu);
    ck_u64("u64 narrows to u32", (uint32_t)vu64(0x123456789u), 0x23456789u);
    ck_u64("i64 compare across words", bit(vi64(-1) < vi64(1), 1), 1);
    ck_u64("i64 compare negative high words", bit(vi64(-0x100000000) < vi64(-1), 1), 1);
    ck_u64("u64 compare across words", bit(vu64(0xffffffffu) < vu64(0x100000000u), 1), 1);
}

static void bits(void)
{
    ck_u64("clz", (uint64_t)__builtin_clz(vu32(1)), 31);
    ck_u64("ctz", (uint64_t)__builtin_ctz(vu32(0x80000000u)), 31);
    ck_u64("popcount", (uint64_t)__builtin_popcount(vu32(0xf0f0f0f0u)), 16);
    ck_u64("clzll", (uint64_t)__builtin_clzll(vu64(1)), 63);
    ck_u64("ctzll", (uint64_t)__builtin_ctzll(vu64(0x10000000000u)), 40);
    ck_u64("popcountll", (uint64_t)__builtin_popcountll(vu64(0x0123456789abcdefu)), 32);
    ck_u64("bswap16", __builtin_bswap16(vu16(0x1234u)), 0x3412u);
    ck_u64("bswap32", __builtin_bswap32(vu32(0x12345678u)), 0x78563412u);
    ck_u64("bswap64", __builtin_bswap64(vu64(0x0123456789abcdefu)), 0xefcdab8967452301u);
}

#define MEM_LEN 17u
#define MEM_OFF 3u
#define MEM_BUF (MEM_LEN + 2u * MEM_OFF + 8u)

static unsigned char g_src[MEM_BUF];
static unsigned char g_dst[MEM_BUF];

static void mem_seed(void)
{
    for (size_t i = 0; i < MEM_BUF; i++)
    {
        g_src[i] = (unsigned char)vu32(0x40u + (uint32_t)i);
        g_dst[i] = 0xa5u;
    }
}

static unsigned memcpy_errors(void)
{
    unsigned errors = 0;
    for (size_t len = 0; len <= MEM_LEN; len++)
    {
        for (size_t so = 0; so <= MEM_OFF; so++)
        {
            for (size_t d = 0; d <= MEM_OFF; d++)
            {
                mem_seed();
                memcpy(g_dst + 4u + d, g_src + so, vsize(len));
                for (size_t i = 0; i < MEM_BUF; i++)
                {
                    unsigned char want = 0xa5u;
                    if (i >= 4u + d and i < 4u + d + len)
                    {
                        want = g_src[so + i - 4u - d];
                    }
                    if (g_dst[i] != want)
                    {
                        errors++;
                    }
                }
            }
        }
    }
    return errors;
}

static unsigned memset_errors(void)
{
    unsigned errors = 0;
    for (size_t len = 0; len <= MEM_LEN; len++)
    {
        for (size_t d = 0; d <= MEM_OFF; d++)
        {
            mem_seed();
            memset(g_dst + 4u + d, vint(0x3c), vsize(len));
            for (size_t i = 0; i < MEM_BUF; i++)
            {
                unsigned char want = 0xa5u;
                if (i >= 4u + d and i < 4u + d + len)
                {
                    want = 0x3cu;
                }
                if (g_dst[i] != want)
                {
                    errors++;
                }
            }
        }
    }
    return errors;
}

static unsigned memmove_errors(bool forward)
{
    unsigned errors = 0;
    for (size_t len = 0; len <= MEM_LEN; len++)
    {
        for (size_t shift = 1; shift <= MEM_OFF; shift++)
        {
            size_t from = 4u;
            size_t to = 4u + shift;
            if (not forward)
            {
                from = 4u + shift;
                to = 4u;
            }
            mem_seed();
            memmove(g_src + to, g_src + from, vsize(len));
            for (size_t i = 0; i < len; i++)
            {
                if (g_src[to + i] != (unsigned char)(0x40u + from + i))
                {
                    errors++;
                }
            }
        }
    }
    return errors;
}

static void memory(void)
{
    char a[4];
    char b[4];
    a[0] = (char)vint(0x80);
    b[0] = (char)vint(0x01);
    a[1] = '\0';
    b[1] = '\0';
    ck_u64("memcpy lengths 0-17 at offsets 0-3", memcpy_errors(), 0);
    ck_u64("memset lengths 0-17 at offsets 0-3", memset_errors(), 0);
    ck_u64("memmove overlap forward", memmove_errors(true), 0);
    ck_u64("memmove overlap backward", memmove_errors(false), 0);
    ck_u64("memcmp compares bytes unsigned", bit(memcmp(a, b, vsize(1)) > 0, 1), 1);
    ck_u64("strcmp compares bytes unsigned", bit(strcmp(a, b) > 0, 1), 1);
    ck_u64("strlen", strlen(a), 1);
}

static void print_int(void)
{
    char small[4];
    int const n = snprintf(small, sizeof(small), "%d", vint(123456));
    char const* long_max = "2147483647";
    if (sizeof(long) == 8u)
    {
        long_max = "9223372036854775807";
    }
    ck_i64("snprintf returns the untruncated length", n, 6);
    ck_u64("snprintf truncates and terminates", bit(strcmp(small, "123") == 0, 1), 1);
    ck_fmt("printf %d", "-42", "%d", vint(-42));
    ck_fmt("printf %d int min", "-2147483648", "%d", vint(INT_MIN));
    ck_fmt("printf %i", "42", "%i", vint(42));
    ck_fmt("printf %u", "4294967295", "%u", (unsigned)vu32(0xffffffffu));
    ck_fmt("printf %x", "deadbeef", "%x", (unsigned)vu32(0xdeadbeefu));
    ck_fmt("printf %X", "DEADBEEF", "%X", (unsigned)vu32(0xdeadbeefu));
    ck_fmt("printf %o", "777", "%o", (unsigned)vu32(0777u));
    ck_fmt("printf %c", "A", "%c", vint('A'));
    ck_fmt("printf %s", "abc", "%s", "abc");
    ck_fmt("printf %%", "%", "%%");
    ck_fmt("printf %p", "0x1234", "%p", (void*)(uintptr_t)vu32(0x1234u));
    ck_fmt("printf %5d", "   42", "%5d", vint(42));
    ck_fmt("printf %-5d", "42   ", "%-5d", vint(42));
    ck_fmt("printf %05d", "-0042", "%05d", vint(-42));
    ck_fmt("printf %+d", "+42", "%+d", vint(42));
    ck_fmt("printf % d", " 42", "% d", vint(42));
    ck_fmt("printf %#x", "0x2a", "%#x", (unsigned)vu32(42));
    ck_fmt("printf %#x of zero", "0", "%#x", (unsigned)vu32(0));
    ck_fmt("printf %#o", "052", "%#o", (unsigned)vu32(42));
    ck_fmt("printf %.3d", "042", "%.3d", vint(42));
    ck_fmt("printf %8.3d", "    -042", "%8.3d", vint(-42));
    ck_fmt("printf %.0d of zero", "", "%.0d", vint(0));
    ck_fmt("printf %*d", "    42", "%*d", vint(6), vint(42));
    ck_fmt("printf %*d negative width", "42    ", "%*d", vint(-6), vint(42));
    ck_fmt("printf %.*d", "00042", "%.*d", vint(5), vint(42));
    ck_fmt("printf %5s", "  abc", "%5s", "abc");
    ck_fmt("printf %-5s", "abc  ", "%-5s", "abc");
    ck_fmt("printf %.2s", "ab", "%.2s", "abc");
    ck_fmt("printf %.*s", "a", "%.*s", vint(1), "abc");
    ck_fmt("printf %hd", "1", "%hd", vint(65537));
    ck_fmt("printf %hd negative", "-1", "%hd", vint(-1));
    ck_fmt("printf %hu", "65535", "%hu", (unsigned)vu32(0xffffffffu));
    ck_fmt("printf %hx", "2345", "%hx", (unsigned)vu32(0x12345u));
    ck_fmt("printf %ld", "-2147483648", "%ld", vlong(INT32_MIN));
    ck_fmt("printf %ld long max", long_max, "%ld", vlong(LONG_MAX));
    ck_fmt("printf %lu", "4294967295", "%lu", (unsigned long)vu32(0xffffffffu));
    ck_fmt("printf %lx", "deadbeef", "%lx", (unsigned long)vu32(0xdeadbeefu));
}

#endif

#if CHAINCHECK_FP

static double from_bits(uint64_t u)
{
    double d;
    memcpy(&d, &u, sizeof(d));
    return d;
}

static volatile double g_vdbl;

static double vdbl(double x)
{
    g_vdbl = x;
    return g_vdbl;
}

static volatile float g_vflt;

static float vflt(float x)
{
    g_vflt = x;
    return g_vflt;
}

static void ck_f64(char const* name, double got, uint64_t want)
{
    uint64_t bits_got;
    char g[24];
    char w[24];
    memcpy(&bits_got, &got, sizeof(bits_got));
    report(name, bits_got == want, hex(g, false, bits_got), hex(w, false, want));
}

static void ck_f32(char const* name, float got, uint32_t want)
{
    uint32_t bits_got;
    char g[24];
    char w[24];
    memcpy(&bits_got, &got, sizeof(bits_got));
    report(name, bits_got == want, hex(g, false, bits_got), hex(w, false, want));
}

static uint64_t const NAN_BITS = 0x7ff8000000000000u;
static uint64_t const INF_BITS = 0x7ff0000000000000u;

// The RXv3 DFPU handles a denormal operand as 0 while DPSW.DDN (bit 8) is set, as it is from
// reset and in every thread arch_context_init seats (arch/rx/rxv3/arch_rxv3.cc), so a subnormal
// double classifies and prints as zero there. Read from the register rather than assumed, so a
// posture that stops flushing is held to IEEE 754.
static bool subnormals_flush(void)
{
#if defined(__RX_DFPU_INSNS__)
    uint32_t dpsw = 0;
    __asm__ volatile("mvfdc dpsw, %0" : "=r"(dpsw));
    say("[chaincheck] dpsw %08lx ddn %u\n", (unsigned long)dpsw, (unsigned)((dpsw >> 8) & 1u));
    return (dpsw & 0x100u) != 0;
#else
    return false;
#endif
}

// isnan 0x001, isinf 0x002, isfinite 0x004, isnormal 0x008, isunordered against 0 from either
// side 0x030, and fpclassify's answer from bit 8; each built from the compiler's compare codes.
static uint64_t class_of(double v)
{
    return bit(isnan(v), 0x1u) | bit(isinf(v), 0x2u) | bit(isfinite(v), 0x4u)
           | bit(isnormal(v), 0x8u) | bit(__builtin_isunordered(v, 0.0), 0x10u)
           | bit(__builtin_isunordered(0.0, v), 0x20u) | ((uint64_t)fpclassify(v) << 8);
}

static uint64_t class_of_f(float v)
{
    return bit(isnan(v), 0x1u) | bit(isinf(v), 0x2u) | bit(isfinite(v), 0x4u)
           | bit(isnormal(v), 0x8u) | bit(__builtin_isunordered(v, 0.0f), 0x10u)
           | bit(__builtin_isunordered(0.0f, v), 0x20u) | ((uint64_t)fpclassify(v) << 8);
}

static uint64_t want_class(int fpc)
{
    uint64_t const c = (uint64_t)fpc << 8;
    if (fpc == FP_NAN)
    {
        return c | 0x31u;
    }
    if (fpc == FP_INFINITE)
    {
        return c | 0x2u;
    }
    if (fpc == FP_NORMAL)
    {
        return c | 0xcu;
    }
    return c | 0x4u;
}

// The ordered family and the plain compares over one pair, against masks worked out by hand:
// isless 0x001, islessequal 0x002, isgreater 0x004, isgreaterequal 0x008, islessgreater 0x010,
// isunordered 0x020, then < 0x040, <= 0x080, == 0x100 and != 0x200.
static uint64_t order_of(double a, double b)
{
    return bit(__builtin_isless(a, b), 0x1u) | bit(__builtin_islessequal(a, b), 0x2u)
           | bit(__builtin_isgreater(a, b), 0x4u) | bit(__builtin_isgreaterequal(a, b), 0x8u)
           | bit(__builtin_islessgreater(a, b), 0x10u) | bit(__builtin_isunordered(a, b), 0x20u)
           | bit(a < b, 0x40u) | bit(a <= b, 0x80u) | bit(a == b, 0x100u) | bit(a != b, 0x200u);
}

// Under -mdfpu the RX compiler keeps isnan, < and == on the single-precision fcmp and widens
// the unordered family to the double compare, so both paths run here.
static uint64_t order_of_f(float a, float b)
{
    return bit(__builtin_isless(a, b), 0x1u) | bit(__builtin_islessequal(a, b), 0x2u)
           | bit(__builtin_isgreater(a, b), 0x4u) | bit(__builtin_isgreaterequal(a, b), 0x8u)
           | bit(__builtin_islessgreater(a, b), 0x10u) | bit(__builtin_isunordered(a, b), 0x20u)
           | bit(a < b, 0x40u) | bit(a <= b, 0x80u) | bit(a == b, 0x100u) | bit(a != b, 0x200u);
}

#if CHAINCHECK_FLOAT

static uint64_t const SUB_BITS = 0x000fffffffffffffu;

static void classify(bool ftz)
{
    double const qnan = from_bits(NAN_BITS);
    double const inf = from_bits(INF_BITS);
    double const sub = from_bits(SUB_BITS);
    int sub_class = FP_SUBNORMAL;
    if (ftz)
    {
        sub_class = FP_ZERO;
    }
    ck_u64("class qnan", class_of(vdbl(qnan)), want_class(FP_NAN));
    ck_u64("class 0/0", class_of(vdbl(0.0) / vdbl(0.0)), want_class(FP_NAN));
    ck_u64("class +inf", class_of(vdbl(inf)), want_class(FP_INFINITE));
    ck_u64("class -inf", class_of(vdbl(-inf)), want_class(FP_INFINITE));
    ck_u64("class +0", class_of(vdbl(0.0)), want_class(FP_ZERO));
    ck_u64("class -0", class_of(vdbl(-0.0)), want_class(FP_ZERO));
    ck_u64("class 1", class_of(vdbl(1.0)), want_class(FP_NORMAL));
    ck_u64("class -2.5", class_of(vdbl(-2.5)), want_class(FP_NORMAL));
    ck_u64("class max", class_of(vdbl(DBL_MAX)), want_class(FP_NORMAL));
    ck_u64("class min", class_of(vdbl(DBL_MIN)), want_class(FP_NORMAL));
    ck_u64("class sub", class_of(vdbl(sub)), want_class(sub_class));
    ck_u64("class -sub", class_of(vdbl(-sub)), want_class(sub_class));
    ck_u64("classf nan", class_of_f(vflt(__builtin_nanf(""))), want_class(FP_NAN));
    ck_u64("classf -inf", class_of_f(vflt(-__builtin_inff())), want_class(FP_INFINITE));
    ck_u64("classf -0", class_of_f(vflt(-0.0f)), want_class(FP_ZERO));
    ck_u64("classf 1", class_of_f(vflt(1.0f)), want_class(FP_NORMAL));
    ck_u64("classf max", class_of_f(vflt(FLT_MAX)), want_class(FP_NORMAL));
    ck_u64("classf min", class_of_f(vflt(FLT_MIN)), want_class(FP_NORMAL));
    ck_u64("signbit -0", bit(signbit(vdbl(-0.0)), 1), 1);
    ck_u64("signbit +0", bit(signbit(vdbl(0.0)), 1), 0);
    ck_u64("signbit -inf", bit(signbit(vdbl(-inf)), 1), 1);
    ck_u64("signbit -nan", bit(signbit(vdbl(from_bits(NAN_BITS | 0x8000000000000000u))), 1), 1);
    ck_u64("signbit -1f", bit(signbit(vflt(-1.0f)), 1), 1);
}

static void order(void)
{
    double const qnan = from_bits(NAN_BITS);
    double const inf = from_bits(INF_BITS);
    float const qnan_f = __builtin_nanf("");
    ck_u64("order nan,1", order_of(vdbl(qnan), vdbl(1.0)), 0x220);
    ck_u64("order 1,nan", order_of(vdbl(1.0), vdbl(qnan)), 0x220);
    ck_u64("order nan,nan", order_of(vdbl(qnan), vdbl(qnan)), 0x220);
    ck_u64("order 1,2", order_of(vdbl(1.0), vdbl(2.0)), 0x2d3);
    ck_u64("order 2,1", order_of(vdbl(2.0), vdbl(1.0)), 0x21c);
    ck_u64("order 1,1", order_of(vdbl(1.0), vdbl(1.0)), 0x18a);
    ck_u64("order -0,0", order_of(vdbl(-0.0), vdbl(0.0)), 0x18a);
    ck_u64("order -inf,inf", order_of(vdbl(-inf), vdbl(inf)), 0x2d3);
    ck_u64("orderf nan,1", order_of_f(vflt(qnan_f), vflt(1.0f)), 0x220);
    ck_u64("orderf 1,nan", order_of_f(vflt(1.0f), vflt(qnan_f)), 0x220);
    ck_u64("orderf 1,2", order_of_f(vflt(1.0f), vflt(2.0f)), 0x2d3);
    ck_u64("orderf 2,1", order_of_f(vflt(2.0f), vflt(1.0f)), 0x21c);
    ck_u64("orderf 1,1", order_of_f(vflt(1.0f), vflt(1.0f)), 0x18a);
    ck_u64("orderf -0,0", order_of_f(vflt(-0.0f), vflt(0.0f)), 0x18a);
}

static void arith_fp(void)
{
    ck_f64("f64 negate +0", -vdbl(0.0), 0x8000000000000000u);
    ck_f64("f64 0 - 0 is +0", vdbl(0.0) - vdbl(0.0), 0);
    ck_f64("f64 -0 + -0 is -0", vdbl(-0.0) + vdbl(-0.0), 0x8000000000000000u);
    ck_f64("f64 0 * -1 is -0", vdbl(0.0) * vdbl(-1.0), 0x8000000000000000u);
    ck_f64("f64 add rounds", vdbl(0.1) + vdbl(0.2), 0x3fd3333333333334u);
    ck_f64("f64 sub rounds", vdbl(1.0) - vdbl(0.9), 0x3fb9999999999998u);
    ck_f64("f64 mul rounds", vdbl(1.1) * vdbl(1.1), 0x3ff35c28f5c28f5du);
    ck_f64("f64 div 1/3", vdbl(1.0) / vdbl(3.0), 0x3fd5555555555555u);
    ck_f64("f64 div 1/10", vdbl(1.0) / vdbl(10.0), 0x3fb999999999999au);
    ck_f32("f32 negate +0", -vflt(0.0f), 0x80000000u);
    ck_f32("f32 add rounds", vflt(0.1f) + vflt(0.2f), 0x3e99999au);
    ck_f32("f32 mul rounds", vflt(1.1f) * vflt(1.1f), 0x3f9ae148u);
    ck_f32("f32 div 1/3", vflt(1.0f) / vflt(3.0f), 0x3eaaaaabu);
}

static void convert(void)
{
    ck_i64("f64 to i32 max", (int32_t)vdbl(2147483647.0), INT32_MAX);
    ck_i64("f64 to i32 min", (int32_t)vdbl(-2147483648.0), INT32_MIN);
    ck_i64("f64 to i32 truncates to min", (int32_t)vdbl(-2147483648.9), INT32_MIN);
    ck_i64("f64 to i32 truncates", (int32_t)vdbl(-1.5), -1);
    ck_u64("f64 to u32 max", (uint32_t)vdbl(4294967295.0), 0xffffffffu);
    ck_u64("f64 to u32 top bit", (uint32_t)vdbl(2147483648.0), 0x80000000u);
    ck_u64("f64 to u32 truncates to 0", (uint32_t)vdbl(-0.9), 0);
    ck_i64("f64 to i64 2^53", (int64_t)vdbl(9007199254740992.0), 9007199254740992);
    ck_i64("f64 to i64 min", (int64_t)vdbl(-9223372036854775808.0), INT64_MIN);
    ck_i64("f64 to i64 below 2^63", (int64_t)vdbl(9223372036854774784.0), 0x7ffffffffffffc00);
    ck_u64("f64 to u64 2^63", (uint64_t)vdbl(9223372036854775808.0), 0x8000000000000000u);
    ck_u64("f64 to u64 below 2^64", (uint64_t)vdbl(18446744073709549568.0), 0xfffffffffffff800u);
    ck_f64("i32 to f64 min", (double)vi32(INT32_MIN), 0xc1e0000000000000u);
    ck_f64("u32 to f64 max", (double)vu32(0xffffffffu), 0x41efffffffe00000u);
    ck_f64("i64 to f64 ties to even down", (double)vi64(9007199254740993), 0x4340000000000000u);
    ck_f64("i64 to f64 ties to even up", (double)vi64(9007199254740995), 0x4340000000000002u);
    ck_f64("i64 to f64 min", (double)vi64(INT64_MIN), 0xc3e0000000000000u);
    ck_f64("u64 to f64 max", (double)vu64(UINT64_MAX), 0x43f0000000000000u);
    ck_f64("u64 to f64 2^63+1", (double)vu64(0x8000000000000001u), 0x43e0000000000000u);
    ck_i64("f32 to i32 2^24", (int32_t)vflt(16777216.0f), 16777216);
    ck_u64("f32 to u32 top bit", (uint32_t)vflt(2147483648.0f), 0x80000000u);
    ck_i64("f32 to i64 min", (int64_t)vflt(-9223372036854775808.0f), INT64_MIN);
    ck_u64("f32 to u64 below 2^64", (uint64_t)vflt(18446742974197923840.0f), 0xffffff0000000000u);
    ck_f32("i32 to f32 rounds", (float)vi32(16777217), 0x4b800000u);
    ck_f32("u32 to f32 max", (float)vu32(0xffffffffu), 0x4f800000u);
    ck_f32("i64 to f32 min", (float)vi64(INT64_MIN), 0xdf000000u);
    ck_f32("u64 to f32 max", (float)vu64(UINT64_MAX), 0x5f800000u);
    ck_f32("f64 to f32 rounds", (float)vdbl(0.1), 0x3dcccccdu);
    ck_f32("f64 to f32 ties to even", (float)vdbl(1.0 + 0x1p-24), 0x3f800000u);
    ck_f32("f64 to f32 above the tie", (float)vdbl(1.0 + 0x1p-24 + 0x1p-50), 0x3f800001u);
    ck_f64("f32 to f64 is exact", (double)vflt(0.1f), 0x3fb99999a0000000u);
}

static void libm(void)
{
    int e = 0;
    double ip = 0.0;
    double const inf = from_bits(INF_BITS);
    ck_f64("sqrt 4", sqrt(vdbl(4.0)), 0x4000000000000000u);
    ck_f64("sqrt 2.25", sqrt(vdbl(2.25)), 0x3ff8000000000000u);
    ck_f64("sqrt 2 rounds", sqrt(vdbl(2.0)), 0x3ff6a09e667f3bcdu);
    ck_f64("sqrt -0", sqrt(vdbl(-0.0)), 0x8000000000000000u);
    ck_f64("sqrt inf", sqrt(vdbl(inf)), INF_BITS);
    ck_u64("sqrt -1 is nan", bit(isnan(sqrt(vdbl(-1.0))), 1), 1);
    ck_f32("sqrtf 2.25", sqrtf(vflt(2.25f)), 0x3fc00000u);
    ck_f32("sqrtf 2 rounds", sqrtf(vflt(2.0f)), 0x3fb504f3u);
    ck_f64("fabs -0", fabs(vdbl(-0.0)), 0);
    ck_f64("fabs -inf", fabs(vdbl(-inf)), INF_BITS);
    ck_f64("fabs -nan", fabs(vdbl(from_bits(NAN_BITS | 0x8000000000000000u))), NAN_BITS);
    ck_f32("fabsf -3", fabsf(vflt(-3.0f)), 0x40400000u);
    ck_f64("floor -0.5", floor(vdbl(-0.5)), 0xbff0000000000000u);
    ck_f64("floor -0", floor(vdbl(-0.0)), 0x8000000000000000u);
    ck_f64("floor 2.5", floor(vdbl(2.5)), 0x4000000000000000u);
    ck_f64("floor -2.5", floor(vdbl(-2.5)), 0xc008000000000000u);
    ck_f64("floor 2^52+1", floor(vdbl(4503599627370497.0)), 0x4330000000000001u);
    ck_f32("floorf -2.5", floorf(vflt(-2.5f)), 0xc0400000u);
    ck_f64("ceil -0.5", ceil(vdbl(-0.5)), 0x8000000000000000u);
    ck_f64("ceil 2.1", ceil(vdbl(2.1)), 0x4008000000000000u);
    ck_f32("ceilf -0.5", ceilf(vflt(-0.5f)), 0x80000000u);
    ck_f64("trunc -2.7", trunc(vdbl(-2.7)), 0xc000000000000000u);
    ck_f64("trunc -0.3", trunc(vdbl(-0.3)), 0x8000000000000000u);
    ck_f32("truncf -2.7", truncf(vflt(-2.7f)), 0xc0000000u);
    ck_f64("round 2.5", round(vdbl(2.5)), 0x4008000000000000u);
    ck_f64("round -2.5", round(vdbl(-2.5)), 0xc008000000000000u);
    ck_f64("round below one half", round(vdbl(0.49999999999999994)), 0);
    ck_f64("round -0.4", round(vdbl(-0.4)), 0x8000000000000000u);
    ck_f32("roundf 2.5", roundf(vflt(2.5f)), 0x40400000u);
    ck_f64("rint 2.5", rint(vdbl(2.5)), 0x4000000000000000u);
    ck_f64("rint -2.5", rint(vdbl(-2.5)), 0xc000000000000000u);
    ck_f64("nearbyint 3.5", nearbyint(vdbl(3.5)), 0x4010000000000000u);
    ck_i64("lround -2.5", lround(vdbl(-2.5)), -3);
    ck_i64("lrint 2.5", lrint(vdbl(2.5)), 2);
    ck_f64("fmod 5.5 2", fmod(vdbl(5.5), vdbl(2.0)), 0x3ff8000000000000u);
    ck_f64("fmod -5.5 2", fmod(vdbl(-5.5), vdbl(2.0)), 0xbff8000000000000u);
    ck_f64("fmod 1e300 7", fmod(vdbl(1e300), vdbl(7.0)), 0x3ff0000000000000u);
    ck_f64("fmod 10 0.1", fmod(vdbl(10.0), vdbl(0.1)), 0x3fb9999999999972u);
    ck_f64("fmod by inf", fmod(vdbl(5.5), vdbl(inf)), 0x4016000000000000u);
    ck_f32("fmodf 5.5 2", fmodf(vflt(5.5f), vflt(2.0f)), 0x3fc00000u);
    ck_f64("ldexp 0.75 3", ldexp(vdbl(0.75), vint(3)), 0x4018000000000000u);
    ck_f64("ldexp 1 -1022", ldexp(vdbl(1.0), vint(-1022)), 0x0010000000000000u);
    ck_f64("ldexp 1 1024 overflows", ldexp(vdbl(1.0), vint(1024)), INF_BITS);
    ck_f32("ldexpf 0.75 3", ldexpf(vflt(0.75f), vint(3)), 0x40c00000u);
    ck_f64("frexp 8 fraction", frexp(vdbl(8.0), &e), 0x3fe0000000000000u);
    ck_i64("frexp 8 exponent", e, 4);
    ck_f64("frexp -3 fraction", frexp(vdbl(-3.0), &e), 0xbfe8000000000000u);
    ck_i64("frexp -3 exponent", e, 2);
    ck_f64("frexp 0 fraction", frexp(vdbl(0.0), &e), 0);
    ck_i64("frexp 0 exponent", e, 0);
    ck_f64("modf -3.25 fraction", modf(vdbl(-3.25), &ip), 0xbfd0000000000000u);
    ck_f64("modf -3.25 integer", ip, 0xc008000000000000u);
    ck_f64("copysign 1 -0", copysign(vdbl(1.0), vdbl(-0.0)), 0xbff0000000000000u);
}

#endif

#endif

#if CHAINCHECK_FULL

static void print_full(bool ftz)
{
    char const* sub_g = "4.94066e-324";
    if (ftz)
    {
        sub_g = "0";
    }
    double const qnan = from_bits(NAN_BITS);
    double const inf = from_bits(INF_BITS);
    ck_fmt("printf %hhd", "44", "%hhd", vint(300));
    ck_fmt("printf %hhu", "255", "%hhu", (unsigned)vu32(0xffffffffu));
    ck_fmt("printf %hhx", "ab", "%hhx", (unsigned)vu32(0x1abu));
    ck_fmt("printf %lld min", "-9223372036854775808", "%lld", (long long)vi64(INT64_MIN));
    ck_fmt("printf %llu max", "18446744073709551615", "%llu", (unsigned long long)vu64(UINT64_MAX));
    ck_fmt("printf %llx", "123456789abcdef0", "%llx",
           (unsigned long long)vu64(0x123456789abcdef0u));
    ck_fmt("printf %lli", "-5000000000", "%lli", (long long)vi64(-5000000000));
    ck_fmt("printf %22lld", "  -9223372036854775807", "%22lld", (long long)vi64(-INT64_MAX));
    ck_fmt("printf %zu", "12345", "%zu", vsize(12345));
    ck_fmt("printf %zx", "ffff", "%zx", vsize(0xffff));
    ck_fmt("printf %jd min", "-9223372036854775808", "%jd", (intmax_t)vi64(INT64_MIN));
    ck_fmt("printf %ju max", "18446744073709551615", "%ju", (uintmax_t)vu64(UINT64_MAX));
    ck_fmt("printf %td", "-5", "%td", (ptrdiff_t)vint(-5));
    ck_fmt("printf PRId64", "-9000000000", "%" PRId64, vi64(-9000000000));
    ck_fmt("printf PRIx64", "fedcba9876543210", "%" PRIx64, vu64(0xfedcba9876543210u));
    ck_fmt("printf %f 1", "1.000000", "%f", vdbl(1.0));
    ck_fmt("printf %f -2.5", "-2.500000", "%f", vdbl(-2.5));
    ck_fmt("printf %f -0", "-0.000000", "%f", vdbl(-0.0));
    ck_fmt("printf %f of a float", "0.100000", "%f", vflt(0.1f));
    ck_fmt("printf %.20f 0.1", "0.10000000000000000555", "%.20f", vdbl(0.1));
    ck_fmt("printf %.0f 0.5 ties to even", "0", "%.0f", vdbl(0.5));
    ck_fmt("printf %.0f 1.5 ties to even", "2", "%.0f", vdbl(1.5));
    ck_fmt("printf %.0f 2.5 ties to even", "2", "%.0f", vdbl(2.5));
    ck_fmt("printf %.2f 2.675", "2.67", "%.2f", vdbl(2.675));
    ck_fmt("printf %10.3f", "     3.142", "%10.3f", vdbl(3.14159));
    ck_fmt("printf %-10.3f", "-3.142    ", "%-10.3f", vdbl(-3.14159));
    ck_fmt("printf %010.2f", "-000003.14", "%010.2f", vdbl(-3.14159));
    ck_fmt("printf %e", "1.234568e+08", "%e", vdbl(123456789.125));
    ck_fmt("printf %E", "1.500000E+00", "%E", vdbl(1.5));
    ck_fmt("printf %+.2e", "+1.23e+04", "%+.2e", vdbl(12345.678));
    ck_fmt("printf %.3e of zero", "0.000e+00", "%.3e", vdbl(0.0));
    ck_fmt("printf %g 0.1", "0.1", "%g", vdbl(0.1));
    ck_fmt("printf %.17g 0.1", "0.10000000000000001", "%.17g", vdbl(0.1));
    ck_fmt("printf %g 100000", "100000", "%g", vdbl(100000.0));
    ck_fmt("printf %g 1e6", "1e+06", "%g", vdbl(1e6));
    ck_fmt("printf %g 0.0001", "0.0001", "%g", vdbl(0.0001));
    ck_fmt("printf %g 0.00001", "1e-05", "%g", vdbl(0.00001));
    ck_fmt("printf %.3g", "1.23e+06", "%.3g", vdbl(1234567.0));
    ck_fmt("printf %#g", "1.00000", "%#g", vdbl(1.0));
    ck_fmt("printf %G", "1E-10", "%G", vdbl(1e-10));
    ck_fmt("printf %g 1e300", "1e+300", "%g", vdbl(1e300));
    ck_fmt("printf %g max", "1.79769e+308", "%g", vdbl(DBL_MAX));
    ck_fmt("printf %g sub", sub_g, "%g", vdbl(5e-324));
    ck_fmt("printf %f nan", "nan", "%f", vdbl(qnan));
    ck_fmt("printf %f inf", "inf", "%f", vdbl(inf));
    ck_fmt("printf %e -inf", "-inf", "%e", vdbl(-inf));
    ck_fmt("printf %F inf", "INF", "%F", vdbl(inf));
    ck_fmt("printf %a 1", "0x1p+0", "%a", vdbl(1.0));
    ck_fmt("printf %a 0.1", "0x1.999999999999ap-4", "%a", vdbl(0.1));
}

#endif

#if CHAINCHECK_FLOAT

static void floating(void)
{
    bool const ftz = subnormals_flush();
    say("[chaincheck] sizeof(double) %u DBL_MANT_DIG %d\n", (unsigned)sizeof(double), DBL_MANT_DIG);
    classify(ftz);
    order();
    arith_fp();
    convert();
    libm();
}

#endif

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    fputs("[chaincheck] start\n", stdout);
#if CHAINCHECK_INT
    arith32();
    arith64();
    bits();
    memory();
    print_int();
#endif
#if CHAINCHECK_FLOAT
    floating();
#endif
#if CHAINCHECK_FULL
    print_full(subnormals_flush());
#endif
    if (g_failed == 0)
    {
        say("[chaincheck] PASS (%u arms)\n", g_arms);
        return 0;
    }
    say("[chaincheck] FAIL (%u of %u arms)\n", g_failed, g_arms);
    return 1;
}
