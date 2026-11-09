// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// x2APIC is preferred where the processor has it: the whole register block is then reached by
// MSR, and the local APIC window is not in the UEFI memory map the identity map covers.
//
// The timer frequency is not discoverable: there is no identity register, and the CPUID
// leaves for the core crystal clock are absent on this processor model, so it and the
// timestamp counter's are measured against the chip's reference timebase.

#include <kickos/arch/apic.h>
#include <kickos/arch/arch.h>
#include <kickos/arch/clk_q32.h>
#include <kickos/arch/regs.h>

#include <stddef.h>
#include <stdint.h>

extern "C" void kfault_terminate(void) __attribute__((noreturn));
extern "C" void kpanic_enter(void);
namespace kickos
{
    void kputs(char const* s);
}

namespace kickos::x86_64
{
    namespace
    {
        constexpr uint32_t msr_apic_base = 0x1b;
        constexpr uint64_t apic_base_extd = 1ull << 10;
        constexpr uint64_t apic_base_enable = 1ull << 11;
        constexpr uint64_t apic_base_addr_mask = 0x000ffffffffff000ull;

        // x2APIC maps the block one MSR per 16-byte xAPIC offset.
        constexpr uint32_t msr_x2apic_base = 0x800;

        constexpr uint32_t reg_version = 0x030;
        constexpr uint32_t reg_tpr = 0x080;
        constexpr uint32_t reg_eoi = 0x0b0;
        constexpr uint32_t reg_svr = 0x0f0;
        constexpr uint32_t reg_lvt_cmci = 0x2f0;
        constexpr uint32_t reg_icr_lo = 0x300;
        constexpr uint32_t reg_lvt_timer = 0x320;
        constexpr uint32_t reg_lvt_thermal = 0x330;
        constexpr uint32_t reg_lvt_perf = 0x340;
        constexpr uint32_t reg_lvt_lint0 = 0x350;
        constexpr uint32_t reg_lvt_lint1 = 0x360;
        constexpr uint32_t reg_lvt_error = 0x370;
        constexpr uint32_t reg_ticr = 0x380;
        constexpr uint32_t reg_tccr = 0x390;
        constexpr uint32_t reg_tdcr = 0x3e0;

        constexpr uint32_t svr_software_enable = 1u << 8;
        constexpr uint32_t lvt_masked = 1u << 16;
        // A masked entry still holds a vector, and a vector below 16 is what an APIC reports
        // as an illegal one.
        constexpr uint32_t lvt_park_vector = 0xfe;
        constexpr uint32_t tdcr_divide_1 = 0x0b;

        // Fixed delivery, level assert, destination shorthand `self`.
        constexpr uint32_t icr_self_fixed = (1u << 14) | (1u << 18);
        constexpr uint32_t icr_delivery_pending = 1u << 12;
        constexpr uint32_t msr_x2apic_self_ipi = 0x83f;
        constexpr uint32_t msr_x2apic_icr = 0x830;
        constexpr uint32_t icr_poll_bound = 100000;

        // One round's window, in the chip reference's own ticks.
        constexpr uint64_t calibrate_ticks = 20000;
        // Odd, so one round is the median.
        constexpr uint32_t calibrate_rounds = 3;
        constexpr uint32_t stamp_tries = 8;
        // A reference that never moves must cost a refused frequency and not a hang.
        constexpr uint32_t ref_spin_bound = 20000000;

        bool g_x2 = false;
        uintptr_t g_mmio = 0;
        // 64 bits wide for the timestamp counter: a part above 4.295 GHz overflows a 32-bit
        // figure, and the wrapped value is a plausible frequency every conversion would use.
        uint64_t g_timer_hz = 0;
        uint64_t g_tsc_hz = 0;

        // LFENCE first: the counter read is a measurement boundary, and rdtsc is not itself
        // ordered against the instructions around it.
        uint64_t rdtsc(void)
        {
            uint32_t lo = 0;
            uint32_t hi = 0;
            __asm__ volatile("lfence\n\trdtsc" : "=a"(lo), "=d"(hi)::"memory");
            return (static_cast<uint64_t>(hi) << 32) | lo;
        }

        struct cpuid_result
        {
            uint32_t eax;
            uint32_t ebx;
            uint32_t ecx;
            uint32_t edx;
        };

        cpuid_result cpuid(uint32_t leaf)
        {
            cpuid_result r;
            r.eax = 0;
            r.ebx = 0;
            r.ecx = 0;
            r.edx = 0;
            __asm__ volatile("cpuid"
                             : "=a"(r.eax), "=b"(r.ebx), "=c"(r.ecx), "=d"(r.edx)
                             : "a"(leaf), "c"(0u));
            return r;
        }

        void apic_write(uint32_t reg, uint32_t value)
        {
            if (g_x2)
            {
                write_msr(msr_x2apic_base + (reg >> 4), value);
                return;
            }
            *reinterpret_cast<volatile uint32_t*>(g_mmio + reg) = value;
        }

        uint32_t apic_read(uint32_t reg)
        {
            if (g_x2)
            {
                return static_cast<uint32_t>(read_msr(msr_x2apic_base + (reg >> 4)));
            }
            return *reinterpret_cast<volatile uint32_t*>(g_mmio + reg);
        }

        [[noreturn]] void refuse(char const* what)
        {
            kpanic_enter();
            kickos::kputs(what);
            kfault_terminate();
        }

        void mask_local_vectors(void)
        {
            // Max LVT Entry, the version register's bits 23:16, is the count of local-vector
            // sources minus one. Four of them exist on every local APIC; each source past
            // those exists only where this field says so (Intel SDM Vol 3, Table 12-1), and
            // writing one that does not is a general protection fault.
            uint32_t const max_lvt = (apic_read(reg_version) >> 16) & 0xffu;
            uint32_t const off = lvt_masked | lvt_park_vector;
            apic_write(reg_lvt_timer, off);
            apic_write(reg_lvt_lint0, off);
            apic_write(reg_lvt_lint1, off);
            apic_write(reg_lvt_error, off);
            if (max_lvt >= 4)
            {
                apic_write(reg_lvt_perf, off);
            }
            if (max_lvt >= 5)
            {
                apic_write(reg_lvt_thermal, off);
            }
            if (max_lvt >= 6)
            {
                apic_write(reg_lvt_cmci, off);
            }
        }

        // Exact, and overflow-free for any product of two clock rates a 64-bit register holds.
        uint64_t scale(uint64_t value, uint64_t from_hz, uint64_t to_hz)
        {
            uint64_t const whole = value / from_hz;
            uint64_t const rest = value - (whole * from_hz);
            return (whole * to_hz) + ((rest * to_hz) / from_hz);
        }

        // Each read is dated by the midpoint of the timestamps around it, kept doubled so no
        // halving rounds it.
        struct Stamp
        {
            uint64_t ref;
            uint32_t count;
            uint64_t ref_at2;
            uint64_t count_at2;
        };

        // The guest can stall between any two reads, so only the tightest of several tries
        // dates both reads.
        Stamp stamp(void)
        {
            Stamp best = {0, 0, 0, 0};
            uint64_t best_width = UINT64_MAX;
            for (uint32_t i = 0; i < stamp_tries; i++)
            {
                uint64_t const t0 = rdtsc();
                uint64_t const ref = kickos_x86_ref_ticks();
                uint64_t const t1 = rdtsc();
                uint32_t const count = apic_read(reg_tccr);
                uint64_t const t2 = rdtsc();
                if (t2 - t0 < best_width)
                {
                    best_width = t2 - t0;
                    best = {ref, count, t0 + t1, t1 + t2};
                }
            }
            return best;
        }

        struct Rates
        {
            uint64_t timer_hz;
            uint64_t tsc_hz;
        };

        Rates measure_round(uint64_t ref_hz)
        {
            apic_write(reg_ticr, 0xffffffffu);
            Stamp const a = stamp();
            uint32_t spin = 0;
            while (kickos_x86_ref_ticks() - a.ref < calibrate_ticks and spin < ref_spin_bound)
            {
                spin++;
            }
            Stamp const b = stamp();
            Rates r = {0, 0};
            uint64_t const ref = b.ref - a.ref;
            if (ref == 0)
            {
                return r;
            }
            r.tsc_hz = scale(b.ref_at2 - a.ref_at2, ref * 2u, ref_hz);
            uint64_t const count_cycles2 = b.count_at2 - a.count_at2;
            if (count_cycles2 == 0)
            {
                return r;
            }
            // Reloaded at its maximum each round and one-shot, so a round cannot wrap it.
            uint64_t const ticks = static_cast<uint64_t>(a.count - b.count);
            r.timer_hz = scale(ticks * 2u, count_cycles2, r.tsc_hz);
            return r;
        }

        // Both rates come from one round, and the median discards a round whose reference
        // poll missed a whole period.
        void calibrate(void)
        {
            uint64_t const ref_hz = kickos_x86_ref_hz();
            if (ref_hz == 0)
            {
                refuse("KickOS: x86_64 chip reports no reference rate\n");
            }
            kickos_x86_ref_start();
            // One-shot (mode bits 18:17 clear) and MASKED: the countdown runs whether or not
            // the entry can deliver, which is what makes a measurement possible before any
            // vector is armed.
            apic_write(reg_lvt_timer, lvt_masked | vector_timer);

            Rates rounds[calibrate_rounds];
            for (uint32_t i = 0; i < calibrate_rounds; i++)
            {
                rounds[i] = measure_round(ref_hz);
                if (rounds[i].tsc_hz == 0)
                {
                    refuse("KickOS: x86_64 chip reference timebase or timestamp counter did not "
                           "advance\n");
                }
            }
            apic_write(reg_ticr, 0);

            for (uint32_t i = 1; i < calibrate_rounds; i++)
            {
                Rates const r = rounds[i];
                uint32_t j = i;
                while (j > 0 and rounds[j - 1].tsc_hz > r.tsc_hz)
                {
                    rounds[j] = rounds[j - 1];
                    j--;
                }
                rounds[j] = r;
            }
            g_timer_hz = rounds[calibrate_rounds / 2].timer_hz;
            g_tsc_hz = rounds[calibrate_rounds / 2].tsc_hz;

            if (g_timer_hz == 0)
            {
                refuse("KickOS: x86_64 local APIC timer did not count\n");
            }
        }
    }

    void apic_init(void)
    {
        cpuid_result const feat = cpuid(1);
        if ((feat.edx & (1u << 9)) == 0)
        {
            refuse("KickOS: x86_64 processor reports no local APIC\n");
        }

        uint64_t base = read_msr(msr_apic_base);
        if ((base & apic_base_enable) == 0)
        {
            // xAPIC first: enabling and extending in one write is not a transition the
            // architecture defines (Intel SDM Vol 3, Table 12-5).
            base |= apic_base_enable;
            write_msr(msr_apic_base, base);
        }
        if ((feat.ecx & (1u << 21)) != 0)
        {
            write_msr(msr_apic_base, base | apic_base_extd);
            g_x2 = true;
        }
        else
        {
            g_mmio = static_cast<uintptr_t>(base & apic_base_addr_mask);
        }

        // The software enable, and the vector the APIC delivers when a raise is withdrawn
        // before it is dispatched.
        apic_write(reg_svr, svr_software_enable | vector_spurious);
        // Reset leaves the priority threshold blocking everything this port delivers.
        apic_write(reg_tpr, 0);
        mask_local_vectors();
        apic_write(reg_tdcr, tdcr_divide_1);
        calibrate();
    }

    void apic_init_secondary(void)
    {
        // The enable and LVT registers are per processor.
        uint64_t base = read_msr(msr_apic_base);
        if ((base & apic_base_enable) == 0)
        {
            base |= apic_base_enable;
            write_msr(msr_apic_base, base);
        }
        if (g_x2)
        {
            write_msr(msr_apic_base, base | apic_base_extd);
        }
        apic_write(reg_svr, svr_software_enable | vector_spurious);
        apic_write(reg_tpr, 0);
        mask_local_vectors();
        apic_write(reg_tdcr, tdcr_divide_1);
    }

    // desc_init loads the interrupt table ahead of apic_init, so an acknowledgement can arrive
    // while the xAPIC window is still unresolved; without the guard the write lands at offset
    // 0xb0 of the low identity map.
    void apic_eoi(void)
    {
        if (not g_x2 and g_mmio == 0)
        {
            return;
        }
        apic_write(reg_eoi, 0);
    }

    void apic_doorbell(void)
    {
        if (g_x2)
        {
            write_msr(msr_x2apic_self_ipi, vector_doorbell);
            return;
        }
        // Bounded: callers ring the bell with interrupts masked, so a delivery-status bit
        // that never clears would park the system with no interrupt left to recover it.
        uint32_t spin = 0;
        while ((apic_read(reg_icr_lo) & icr_delivery_pending) != 0 and spin < icr_poll_bound)
        {
            spin++;
        }
        apic_write(reg_icr_lo, icr_self_fixed | vector_doorbell);
    }

    void apic_doorbell_send(uint32_t cores)
    {
        for (uint32_t core = 0; core < KICKOS_KERNEL_CORES; ++core)
        {
            if ((cores & (1u << core)) == 0)
            {
                continue;
            }
            // SMP requires x2APIC and APIC IDs 0..N-1, which INIT/SIPI discovery verifies.
            if (g_x2)
            {
                write_msr(msr_x2apic_icr,
                          (static_cast<uint64_t>(core) << 32) | vector_doorbell);
            }
        }
    }

    uint64_t apic_tsc_hz(void)
    {
        return g_tsc_hz;
    }

    bool apic_is_x2(void)
    {
        return g_x2;
    }

    uintptr_t apic_mmio_base(void)
    {
        return g_mmio;
    }

    uint64_t tsc_now(void)
    {
        return rdtsc();
    }

    uint64_t clock_now(void)
    {
        return scale(rdtsc(), g_tsc_hz, ::kickos::KICKOS_NS_PER_SEC);
    }

    // Every arm restarts the countdown, so the delta is recomputed against the clock each time.
    void timer_arm(uint64_t deadline_ns)
    {
        uint64_t const now = clock_now();
        uint64_t delta_ns = 0;
        if (deadline_ns > now)
        {
            delta_ns = deadline_ns - now;
        }
        uint64_t count = scale(delta_ns, ::kickos::KICKOS_NS_PER_SEC, g_timer_hz);
        // Zero in the initial-count register STOPS the timer rather than firing it, and the
        // register is 32 bits wide whatever the deadline asks for.
        if (count == 0)
        {
            count = 1;
        }
        if (count > 0xffffffffull)
        {
            count = 0xffffffffull;
        }
        apic_write(reg_lvt_timer, vector_timer);
        apic_write(reg_ticr, static_cast<uint32_t>(count));
    }

    // A raise already latched in the interrupt-request register cannot be withdrawn: masking
    // the entry stops the next assertion, and the one in flight is still delivered once.
    void timer_disarm(void)
    {
        apic_write(reg_ticr, 0);
        apic_write(reg_lvt_timer, lvt_masked | vector_timer);
    }
}
