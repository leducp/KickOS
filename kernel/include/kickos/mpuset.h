// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A thread's protection set: the regions it is granted, and the descriptor words the
// switch path programs from. The image is DERIVED, so the region array is private and
// every mutator re-encodes before it returns.
//
// A region the backend cannot name exactly gets NO descriptor: the image never rounds a
// base or a size, so the set can be narrower than what hardware would have been asked
// for, never wider.

#ifndef KICKOS_MPUSET_H
#define KICKOS_MPUSET_H

#include <stddef.h>
#include <stdint.h>

#include <kickos/arch/arch.h>
#include <kickos/arch/mpu_overlap.h>
#include <kickos/config.h>
#include <kickos/config/limits.h>

namespace kickos
{
#if KICKOS_HAVE_MPU
    static_assert(KICKOS_MPU_MAX_REGIONS <= ARCH_MPU_ENCODED_SLOTS,
                  "the encoded image carries fewer descriptor slots than the kernel hands it");
#endif
    static_assert(KICKOS_MPU_MAX_REGIONS <= 8, "MpuSet::places_ names a region in three bits");
    static_assert(KICKOS_MAX_THREAD_WINDOWS <= 4,
                  "MpuSet::places_ and window_flags_ hold four places of a spawn list");
    static_assert(KICKOS_MPU_MAX_REGIONS < 32,
                  "the seating bitmask is a uint32_t, and the no-MPU encode shifts by a count "
                  "that reaches the maximum, so 32 is already the undefined shift");

    // Whether an MPU deciding overlaps by `rule` (ARCH_MPU_OVERLAP_*) decides every byte both
    // regions cover as the kernel's checks do: readable where either region grants R
    // (user_range_ok), writable only where both grant W (read_only_overlaps). `lower` and
    // `higher` are named by slot number, and a region seated with no descriptor decides nothing
    // in the hardware. Two memory types never share a byte, on every backend alike. X is not
    // asked: no kernel check reads it.
    constexpr bool mpu_overlap_expressible(int rule, arch_mpu_region const& lower,
                                           bool lower_seated, arch_mpu_region const& higher,
                                           bool higher_seated)
    {
        if (lower.size == 0 or higher.size == 0 or lower.base > higher.base + (higher.size - 1u)
            or higher.base > lower.base + (lower.size - 1u))
        {
            return true;
        }
        if (((lower.attr ^ higher.attr) & (ARCH_MPU_NOCACHE | ARCH_MPU_DEV)) != 0)
        {
            return false;
        }
        uint32_t const lo = lower.attr & (ARCH_MPU_R | ARCH_MPU_W);
        uint32_t const hi = higher.attr & (ARCH_MPU_R | ARCH_MPU_W);
        uint32_t const kernel = ((lo | hi) & ARCH_MPU_R) | (lo & hi & ARCH_MPU_W);
        uint32_t hardware = 0;
        if (not lower_seated or not higher_seated)
        {
            if (lower_seated)
            {
                hardware = lo;
            }
            if (higher_seated)
            {
                hardware = hi;
            }
        }
        else if (rule == ARCH_MPU_OVERLAP_HIGHER)
        {
            hardware = hi;
        }
        else if (rule == ARCH_MPU_OVERLAP_LOWER)
        {
            hardware = lo;
        }
        else if (rule == ARCH_MPU_OVERLAP_UNION)
        {
            hardware = lo | hi;
        }
        else
        {
            return false;
        }
        return hardware == kernel;
    }

    // Both regions seated.
    constexpr bool mpu_overlap_expressible(int rule, arch_mpu_region const& lower,
                                           arch_mpu_region const& higher)
    {
        return mpu_overlap_expressible(rule, lower, true, higher, true);
    }

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

        // The zeroed image that follows grants nothing, as a kmemset'd TCB carries.
        void clear()
        {
            count_ = 0;
            places_ = 0;
            window_flags_ = 0;
            encode();
        }

        // add(), recording the region at `place` in the spawn list with its spawn flags, which
        // a later retype of the region leaves alone.
        [[nodiscard]] bool add_window(uintptr_t base, size_t size, uint32_t attr, uint8_t place,
                                      uint8_t flags)
        {
            uint8_t const at = count_;
            if (not add(base, size, attr))
            {
                return false;
            }
            places_ = static_cast<uint16_t>(places_ | ((PLACE_SEATED | at) << (4u * place)));
            window_flags_ = static_cast<uint8_t>(window_flags_ | ((flags & 3u) << (2u * place)));
            return true;
        }

        // The region of the window at `place` and its spawn flags, or null.
        arch_mpu_region const* window(uint32_t place, uint8_t* flags) const
        {
            if (place >= 4u)
            {
                return nullptr;
            }
            uint32_t const seat = (places_ >> (4u * place)) & 0xFu;
            if ((seat & PLACE_SEATED) == 0)
            {
                return nullptr;
            }
            *flags = static_cast<uint8_t>((window_flags_ >> (2u * place)) & 3u);
            return &regions_[seat & 7u];
        }

        // False, changing nothing, when the set is full. A region the backend seats no
        // descriptor for is still carried: the kernel's range checks read it, and a privileged
        // thread's whole-arena grant is never nameable by one descriptor on a power-of-two
        // backend.
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

        // For a grant the hardware must enforce: false, changing nothing, when the set is full
        // or the backend seats no descriptor for the region.
        //
        // NOT COMPILED ON A TRANSLATING BACKEND: no descriptor is ever seated there, and
        // encode()'s all-seated answer would be a description taken for a guarantee.
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

        // add_enforced, except that a region already naming EXACTLY this base and size is
        // RETYPED IN PLACE: two descriptors over one block are two answers, the kernel's range
        // checks reading the first and the hardware obeying the last. Keeping the index matters
        // too, region precedence being positional on PMSAv7.
        //
        // False, changing NOTHING, when the new attributes are unencodable.
        //
        // noinline is LOAD-BEARING, for the reason console_write_user gives in syscall.cc:
        // inlined, this search costs the `syscall_body` frame the SVC and SYSK red zones are
        // measured on a spill slot, on every board.
        [[nodiscard]] __attribute__((noinline)) bool add_enforced_retyping(uintptr_t base,
                                                                          size_t size,
                                                                          uint32_t attr)
        {
            for (uint8_t i = 0; i < count_; i++)
            {
                if (regions_[i].base != base or regions_[i].size != size)
                {
                    continue;
                }
                uint32_t const was = regions_[i].attr;
                regions_[i].attr = attr;
                if ((encode() >> i) & 1u)
                {
                    return true;
                }
                regions_[i].attr = was;
                encode();
                return false;
            }
            return add_enforced(base, size, attr);
        }
#endif

#if KICKOS_HAVE_MPU and not KICKOS_HAVE_ASPACE
        // Whether this MPU decides every byte `r`, at slot `at`, shares with the set as the
        // kernel's checks do. A region already at `at` is the one `r` replaces.
        [[nodiscard]] __attribute__((noinline)) bool admits(arch_mpu_region const& r,
                                                            uint8_t at, bool r_seated) const
        {
            for (uint8_t i = 0; i < count_; i++)
            {
                bool const seated = arch_mpu_encoded_seated(&image_, i);
                bool ok = true;
                if (i < at)
                {
                    ok = mpu_overlap_expressible(ARCH_MPU_OVERLAP, regions_[i], seated, r,
                                                 r_seated);
                }
                else if (i > at)
                {
                    ok = mpu_overlap_expressible(ARCH_MPU_OVERLAP, r, r_seated, regions_[i],
                                                 seated);
                }
                if (not ok)
                {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] bool overlaps_expressible() const
        {
            for (uint8_t i = 0; i < count_; i++)
            {
                if (not admits(regions_[i], i, arch_mpu_encoded_seated(&image_, i)))
                {
                    return false;
                }
            }
            return true;
        }

        // admits(), at the slot add_enforced_retyping would seat the region in.
        [[nodiscard]] bool retyping_expressible(uintptr_t base, size_t size, uint32_t attr) const
        {
            uint8_t at = count_;
            for (uint8_t i = 0; i < count_; i++)
            {
                if (regions_[i].base == base and regions_[i].size == size)
                {
                    at = i;
                    break;
                }
            }
            arch_mpu_region const r = {base, size, attr};
            return admits(r, at, true);
        }
#endif

        // Drops every device region, which releases the thread's device windows: the one-holder
        // check reads them here. For a thread past its last return to user code.
        void drop_devices()
        {
            uint8_t kept = 0;
            for (uint8_t i = 0; i < count_; i++)
            {
                if ((regions_[i].attr & ARCH_MPU_DEV) == 0)
                {
                    regions_[kept] = regions_[i];
                    kept++;
                }
            }
            count_ = kept;
            places_ = 0;
            window_flags_ = 0;
            encode();
        }

        // The linker script's regions, encodable by construction and never more than a fresh
        // set holds.
        void append_statics()
        {
            count_ = static_cast<uint8_t>(
                count_
                + arch_domain_static_regions(&regions_[count_], KICKOS_MPU_MAX_REGIONS - count_));
            encode();
        }

        void apply() const
        {
#if KICKOS_HAVE_MPU
            arch_mpu_apply(regions_, count_, &image_);
#endif
        }

        // Loads this set AT ONCE, for a grant live before its syscall returns. NOT the switch
        // path: on a deferred backend this must not become the image a pended switch's epilogue
        // programs.
        void apply_now() const
        {
#if KICKOS_HAVE_MPU
            arch_mpu_apply_now(regions_, count_, &image_);
#endif
        }

      private:
        bool full() const
        {
            return count_ >= KICKOS_MPU_MAX_REGIONS;
        }

        // The seating bitmask. All-seated where no descriptor exists, honest only because
        // nothing is enforced there either; add_enforced is compiled out where that comes apart.
        uint32_t encode()
        {
#if KICKOS_HAVE_MPU
            return arch_mpu_encode(regions_, count_, &image_);
#else
            return (static_cast<uint32_t>(1) << count_) - 1u;
#endif
        }

        static constexpr uint32_t PLACE_SEATED = 8u;

        arch_mpu_region regions_[KICKOS_MPU_MAX_REGIONS] = {};
        uint8_t count_ = 0;
        // Bits 2p, 2p + 1: the spawn flags of the window at place p.
        uint8_t window_flags_ = 0;
        // Bits 4p to 4p + 3: PLACE_SEATED and the region index of the window at place p.
        uint16_t places_ = 0;
#if KICKOS_HAVE_MPU
        arch_mpu_encoded image_ = {};
#endif
    };
}

#endif
