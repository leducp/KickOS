// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Constexpr user-defined literals shared by kernel and userspace: time
// _ns/_us/_ms/_s -> uint64_t ns, size _B/_KiB/_MiB -> size_t bytes (_KiB/_MiB
// are x1024). The leading underscore is mandatory (bare suffixes are reserved).
// Visible directly inside namespace kickos (inline namespace); elsewhere bring
// them in with `using namespace kickos::units;`.

#ifndef KICKOS_UNITS_H
#define KICKOS_UNITS_H

#include <stdint.h>
#include <stddef.h>

namespace kickos
{
    inline namespace units
    {
        // Time -> canonical uint64_t nanoseconds.
        constexpr uint64_t operator""_ns(unsigned long long v)
        {
            return static_cast<uint64_t>(v);
        }
        constexpr uint64_t operator""_us(unsigned long long v)
        {
            return static_cast<uint64_t>(v) * 1000ull;
        }
        constexpr uint64_t operator""_ms(unsigned long long v)
        {
            return static_cast<uint64_t>(v) * 1000000ull;
        }
        constexpr uint64_t operator""_s(unsigned long long v)
        {
            return static_cast<uint64_t>(v) * 1000000000ull;
        }

        // Size -> canonical size_t bytes.
        constexpr size_t operator""_B(unsigned long long v)
        {
            return static_cast<size_t>(v);
        }
        constexpr size_t operator""_KiB(unsigned long long v)
        {
            return static_cast<size_t>(v) * 1024ull;
        }
        constexpr size_t operator""_MiB(unsigned long long v)
        {
            return static_cast<size_t>(v) * 1024ull * 1024ull;
        }
    }
}

#endif
