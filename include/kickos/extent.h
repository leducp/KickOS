// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The checked page-extent arithmetic every range check needs, shared by the kernel's range
// list and reservation paths and by an arch map editor's own bounds test.
//
// The granule's one source of truth is the arch seam query, so it arrives as a parameter.

#ifndef KICKOS_EXTENT_H
#define KICKOS_EXTENT_H

#include <stddef.h>
#include <stdint.h>

namespace kickos
{
    // A power of two, zero excluded.
    constexpr bool is_pow2(size_t v)
    {
        if (v == 0)
        {
            return false;
        }
        return (v & (v - 1u)) == 0;
    }

    // Writes the end of [base, base + pages * granule) and answers true. Fails closed: false
    // where the byte count overflows a pointer, where the end wraps past it, and where
    // either input is 0. A caller that must tell an empty extent from a wrapped one tests
    // `pages` itself first.
    constexpr bool extent_end(uintptr_t base, size_t pages, size_t granule, uintptr_t* end)
    {
        if (pages == 0 or granule == 0)
        {
            return false;
        }
        // Both operands widen first: a narrower product would be computed over a count
        // nobody passed.
        uintptr_t const bytes = static_cast<uintptr_t>(pages) * static_cast<uintptr_t>(granule);
        if (bytes / granule != pages)
        {
            return false;
        }
        if (base + bytes < base)
        {
            return false;
        }
        *end = base + bytes;
        return true;
    }

    // The last byte of [base, base + pages * granule), `base` required granule-aligned.
    // extent_end refuses a zero-page range, so the end is one past a byte that exists and the
    // subtraction below cannot underflow.
    constexpr bool extent_last_aligned(uintptr_t base, size_t pages, size_t granule,
                                       uintptr_t* last)
    {
        if ((base & (granule - 1u)) != 0)
        {
            return false;
        }
        uintptr_t end = 0;
        if (not extent_end(base, pages, granule, &end))
        {
            return false;
        }
        *last = end - 1;
        return true;
    }
}

#endif
