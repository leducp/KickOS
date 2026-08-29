// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/libc/string.h>

#include <stdint.h>

// Not compiled for the host sim. These are global symbols with default visibility, so a
// definition here PREEMPTS the host libc's for the whole process, the C++ runtime and
// every host library included, and no call site says so.
#if !KICKOS_ARCH_SIM

namespace
{
    // may_alias is load-bearing: without it the word accesses below and the byte
    // accesses to the same buffer are unrelated types and may be reordered against
    // each other.
    typedef unsigned long __attribute__((__may_alias__)) Word;

    constexpr size_t WORD_BYTES = sizeof(Word);
    constexpr uintptr_t WORD_MASK = static_cast<uintptr_t>(WORD_BYTES) - 1u;

    // A word is only ever loaded or stored at a word-aligned address: armv6m and lx6
    // fault on an unaligned one, and -mno-unaligned-access, carried for the K64F's
    // SRAM_L|SRAM_U seam (arch/arm/chip/mk64f/mk64f.ld), binds the compiler and not
    // library assembly. Operands whose misalignment differs can never both be
    // aligned, so they stay on the byte path.
    //
    // The word path is entered only where at least one whole word is guaranteed to move,
    // which is the bytes the byte prologue consumes plus one word. The prologue's length
    // is fixed by the destination alignment alone, so it is known before the branch.

    // The prologue walks the pointer UP to the next boundary.
    constexpr size_t word_min_up(uintptr_t p)
    {
        return static_cast<size_t>((0u - p) & WORD_MASK) + WORD_BYTES;
    }

    // The mirror walks the pointer DOWN to the boundary below it.
    constexpr size_t word_min_down(uintptr_t p)
    {
        return static_cast<size_t>(p & WORD_MASK) + WORD_BYTES;
    }

    // memset's threshold. Its word path builds the fill word first, which costs more than the
    // single word it would then store.
    constexpr size_t MEMSET_WORD_MIN = 2u * WORD_BYTES;

    // always_inline is load-bearing: out of line, memcpy has to build a frame to keep dst alive
    // across the call.
    //
    // Correct for overlapping ranges when d < s: each word is read before the store
    // that could reach it, and the store never touches an address at or above the
    // next word to be read.
    inline __attribute__((always_inline)) void copy_ascending(unsigned char* d,
                                                              unsigned char const* s, size_t n)
    {
        uintptr_t const du = reinterpret_cast<uintptr_t>(d);
        uintptr_t const su = reinterpret_cast<uintptr_t>(s);
        if (n >= word_min_up(du) and ((du ^ su) & WORD_MASK) == 0u)
        {
            while ((reinterpret_cast<uintptr_t>(d) & WORD_MASK) != 0u)
            {
                *d++ = *s++;
                n--;
            }
            Word* dw = reinterpret_cast<Word*>(d);
            Word const* sw = reinterpret_cast<Word const*>(s);
            while (n >= WORD_BYTES)
            {
                *dw++ = *sw++;
                n -= WORD_BYTES;
            }
            d = reinterpret_cast<unsigned char*>(dw);
            s = reinterpret_cast<unsigned char const*>(sw);
        }
        while (n > 0)
        {
            *d++ = *s++;
            n--;
        }
    }

    // The mirror, for d > s: each store lands above the source word just read.
    void copy_descending(unsigned char* d, unsigned char const* s, size_t n)
    {
        d += n;
        s += n;
        uintptr_t const du = reinterpret_cast<uintptr_t>(d);
        uintptr_t const su = reinterpret_cast<uintptr_t>(s);
        if (n >= word_min_down(du) and ((du ^ su) & WORD_MASK) == 0u)
        {
            while ((reinterpret_cast<uintptr_t>(d) & WORD_MASK) != 0u)
            {
                *--d = *--s;
                n--;
            }
            Word* dw = reinterpret_cast<Word*>(d);
            Word const* sw = reinterpret_cast<Word const*>(s);
            while (n >= WORD_BYTES)
            {
                *--dw = *--sw;
                n -= WORD_BYTES;
            }
            d = reinterpret_cast<unsigned char*>(dw);
            s = reinterpret_cast<unsigned char const*>(sw);
        }
        while (n > 0)
        {
            *--d = *--s;
            n--;
        }
    }
}

extern "C"
{

void* memcpy(void* dst, void const* src, size_t n)
{
    copy_ascending(static_cast<unsigned char*>(dst), static_cast<unsigned char const*>(src), n);
    return dst;
}

void* memset(void* dst, int c, size_t n)
{
    unsigned char* d = static_cast<unsigned char*>(dst);
    unsigned char const b = static_cast<unsigned char>(c);
    if (n >= MEMSET_WORD_MIN)
    {
        while ((reinterpret_cast<uintptr_t>(d) & WORD_MASK) != 0u)
        {
            *d++ = b;
            n--;
        }
        Word fill = b;
        fill |= fill << 8;
        fill |= fill << 16;
        fill |= fill << 16 << 16; // folds away where a word is 32 bits
        Word* dw = reinterpret_cast<Word*>(d);
        while (n >= WORD_BYTES)
        {
            *dw++ = fill;
            n -= WORD_BYTES;
        }
        d = reinterpret_cast<unsigned char*>(dw);
    }
    while (n > 0)
    {
        *d++ = b;
        n--;
    }
    return dst;
}

void* memmove(void* dst, void const* src, size_t n)
{
    unsigned char* d = static_cast<unsigned char*>(dst);
    unsigned char const* s = static_cast<unsigned char const*>(src);
    if (d == s or n == 0)
    {
        return dst;
    }
    if (d < s)
    {
        copy_ascending(d, s, n);
    }
    else
    {
        copy_descending(d, s, n);
    }
    return dst;
}

int memcmp(void const* a, void const* b, size_t n)
{
    unsigned char const* pa = static_cast<unsigned char const*>(a);
    unsigned char const* pb = static_cast<unsigned char const*>(b);
    for (size_t i = 0; i < n; i++)
    {
        if (pa[i] != pb[i])
        {
            return static_cast<int>(pa[i]) - static_cast<int>(pb[i]);
        }
    }
    return 0;
}

size_t strlen(char const* s)
{
    size_t n = 0;
    while (s[n] != '\0')
    {
        n++;
    }
    return n;
}

size_t strnlen(char const* s, size_t maxlen)
{
    size_t n = 0;
    while (n < maxlen and s[n] != '\0')
    {
        n++;
    }
    return n;
}
}

#endif // !KICKOS_ARCH_SIM
