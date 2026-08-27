// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A thread's protection set: the regions it is granted, and the descriptor words the
// switch path programs from. The image is DERIVED, so the region array is private and
// every mutator re-encodes before it returns: there is no expression that changes a base,
// a size or an attribute and leaves the image behind.
//
// A region the backend cannot name exactly gets NO descriptor: the image never rounds a
// base or a size, so the set can be narrower than what hardware would have been asked
// for, never wider. That rule is stated once per arch inside arch_mpu_encode and paid
// once per grant instead of once per switch.

#ifndef KICKOS_MPUSET_H
#define KICKOS_MPUSET_H

#include <stddef.h>
#include <stdint.h>

#include <kickos/arch/arch.h>
#include <kickos/config.h>
#include <kickos/config/limits.h>

namespace kickos
{
#if KICKOS_HAVE_MPU
    static_assert(KICKOS_MPU_MAX_REGIONS <= ARCH_MPU_ENCODED_SLOTS,
                  "the encoded image carries fewer descriptor slots than the kernel hands it");
#endif
    static_assert(KICKOS_MPU_MAX_REGIONS <= 32,
                  "the seating bitmask is a uint32_t, and the no-MPU encode shifts by the count");
    static_assert(KICKOS_MPU_MAX_REGIONS <= UINT8_MAX,
                  "MpuSet::count_ is a byte, which is what keeps the image inside the padding "
                  "the region array already spends");

    class MpuSet
    {
      public:
        arch_mpu_region const* begin() const
        {
            return regions_;
        }
        arch_mpu_region const* end() const
        {
            return regions_ + count_;
        }

        // Drops every region. The zeroed image that follows grants nothing, which is also
        // what a memset'd TCB carries before its first append.
        void clear()
        {
            count_ = 0;
            encode();
        }

        // Appends one region, or answers false and changes nothing because the set is full.
        // A region the backend seats no descriptor for is still carried: it is what the
        // kernel's own range checks read, and a privileged thread's whole-arena grant is
        // never nameable by one descriptor on a power-of-two backend.
        [[nodiscard]] bool add(uintptr_t base, size_t size, uint32_t attr)
        {
            if (full())
            {
                return false;
            }
            regions_[count_].base = base;
            regions_[count_].size = size;
            regions_[count_].attr = attr;
            count_++;
            encode();
            return true;
        }

        // For a grant whose whole point is that the hardware enforces it. Answers false
        // and changes nothing when the set is full or the backend seats no descriptor for
        // the region, so the caller returns an error instead of handing back memory the
        // thread would fault on.
        //
        // NOT COMPILED ON A TRANSLATING BACKEND. There the enforcement is the mapping and
        // no descriptor is ever seated, so encode() below has no honest answer to give and
        // a caller reading its all-seated one would take a description for a guarantee.
#if !KICKOS_HAVE_ASPACE
        [[nodiscard]] bool add_enforced(uintptr_t base, size_t size, uint32_t attr)
        {
            if (full())
            {
                return false;
            }
            regions_[count_].base = base;
            regions_[count_].size = size;
            regions_[count_].attr = attr;
            count_++;
            if ((encode() >> (count_ - 1u)) & 1u)
            {
                return true;
            }
            count_--;
            encode();
            return false;
        }
#endif

        // Appends the arch's app-wide static regions (code and static data). They come from
        // the linker script, so they are encodable by construction and there are never more
        // of them than a fresh set holds.
        void append_statics()
        {
            count_ = static_cast<uint8_t>(
                count_
                + arch_domain_static_regions(&regions_[count_], KICKOS_MPU_MAX_REGIONS - count_));
            encode();
        }

        // Loads this set on switch-in.
        void apply() const
        {
#if KICKOS_HAVE_MPU
            arch_mpu_apply(regions_, count_, &image_);
#else
            arch_mpu_apply(regions_, count_, nullptr);
#endif
        }

      private:
        bool full() const
        {
            return count_ >= KICKOS_MPU_MAX_REGIONS;
        }

        // The seating bitmask. Where no descriptor exists the answer is all-seated, which
        // is honest only because nothing is enforced there either; add_enforced is
        // compiled out on the one backend where that pair comes apart.
        uint32_t encode()
        {
#if KICKOS_HAVE_MPU
            return arch_mpu_encode(regions_, count_, &image_);
#else
            return (static_cast<uint32_t>(1) << count_) - 1u;
#endif
        }

        arch_mpu_region regions_[KICKOS_MPU_MAX_REGIONS] = {};
        uint8_t count_ = 0;
#if KICKOS_HAVE_MPU
        arch_mpu_encoded image_ = {};
#endif
    };
}

#endif
