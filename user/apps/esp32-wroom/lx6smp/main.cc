// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32 (Xtensa LX6) shared-kernel silicon probe: the two columns docs/design-multicore.md
// section 1.5 leaves "not established" on the LX6 row. Both are Xtensa configuration OPTIONS
// the integrator wires, so only silicon answers.
//
//   1. THE ATOMIC. S32C1I (Conditional Store Option, ISA summary 4.3.13) against SCOMPARE1
//      (SR 12), between PRO_CPU and APP_CPU on ONE word of internal SRAM. Per ISA summary
//      4.3.13.3 the instruction runs directly on a local DataRAM, while for other memory
//      types it may raise LoadStoreErrorCause or issue an RCW transaction on the PIF bus,
//      per-implementation and gated by ATOMCTL (SR 99). ESP32 internal SRAM is reached by
//      BOTH CPUs over the bus.
//   2. THE IDENTITY. PRID (SR 235, ISA summary 6.4), read on both cores. Its value is wired
//      by the integrator; identical values are a NO.
//
// PLACEMENT IS PART OF THE MEASUREMENT: the shared cells are .bss, which esp32.ld puts in
// internal SRAM2 (DRAM at 0x3FFB0000), and the APP_CPU entry is .text in internal SRAM0
// (IRAM at 0x40080000), both reachable by both CPUs (TRM v5.8 section 3.3.2, Table 3.3-2).
// The two 32 KB caches sit only on the external path, so a PSRAM placement measures another
// question.
//
// APP_CPU NEVER TOUCHES THE UART: it publishes into the shared cells and PRO_CPU reports.
// Every risky instruction is preceded by a console line naming it. An APP_CPU fault reaches
// the ROM vectors and simply stops, which the deadline and the last checkpoint have to make
// diagnosable.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>

#include <stddef.h>
#include <stdint.h>

extern "C"
{
    // Mirrored by the CELL_* offsets in app_cpu.S, pinned by the static_asserts below.
    struct lx6smp_cells
    {
        uint32_t counter;
        uint32_t go;
        uint32_t ck1;
        uint32_t prid;
        uint32_t ck2;
        uint32_t ck3;
        uint32_t ok;
        uint32_t retry;
        uint32_t rounds;
    };

    alignas(16) volatile lx6smp_cells g_lx6smp_cells;

    void kickos_lx6smp_app_entry(void);
}

static_assert(offsetof(lx6smp_cells, counter) == 0, "app_cpu.S CELL_COUNTER");
static_assert(offsetof(lx6smp_cells, go) == 4, "app_cpu.S CELL_GO");
static_assert(offsetof(lx6smp_cells, ck1) == 8, "app_cpu.S CELL_CK1");
static_assert(offsetof(lx6smp_cells, prid) == 12, "app_cpu.S CELL_PRID");
static_assert(offsetof(lx6smp_cells, ck2) == 16, "app_cpu.S CELL_CK2");
static_assert(offsetof(lx6smp_cells, ck3) == 20, "app_cpu.S CELL_CK3");
static_assert(offsetof(lx6smp_cells, ok) == 24, "app_cpu.S CELL_OK");
static_assert(offsetof(lx6smp_cells, retry) == 28, "app_cpu.S CELL_RETRY");
static_assert(offsetof(lx6smp_cells, rounds) == 32, "app_cpu.S CELL_ROUNDS");

namespace
{
    constexpr uint32_t CK1_MAGIC = 0xA11E0001u;
    constexpr uint32_t CK2_MAGIC = 0xA11E0002u;
    constexpr uint32_t CK3_MAGIC = 0xA11E0003u;

    // TRM v5.8 Table 3.3-6 "Peripheral Address Mapping" (p.72).
    constexpr uintptr_t DPORT_BASE = 0x3FF00000u;
    constexpr uintptr_t RTC_CNTL_BASE = 0x3FF48000u;

    // TRM v5.8 chapter 12 "DPort Registers", summary Table 12.2 (p.245) and Registers
    // 12.5 - 12.8 (p.247-248). Each is bit 0 with the rest reserved, except CTRL_REG_D,
    // which is a full 32-bit boot address.
    constexpr uintptr_t APPCPU_CTRL_A = DPORT_BASE + 0x02Cu; // Register 12.5, reset 1
    constexpr uintptr_t APPCPU_CTRL_B = DPORT_BASE + 0x030u; // Register 12.6, reset 0
    constexpr uintptr_t APPCPU_CTRL_C = DPORT_BASE + 0x034u; // Register 12.7, reset 0
    constexpr uintptr_t APPCPU_CTRL_D = DPORT_BASE + 0x038u; // Register 12.8, reset 0
    constexpr uint32_t APPCPU_RESETTING = 1u << 0;
    constexpr uint32_t APPCPU_CLKGATE_EN = 1u << 0;
    constexpr uint32_t APPCPU_RUNSTALL = 1u << 0;

    // TRM v5.8 chapter 9, Register 9.1 RTC_CNTL_OPTIONS0_REG (p.195) and Register 9.34
    // RTC_CNTL_SW_CPU_STALL_REG (p.219). p.219 states only the value that STALLS:
    // appcpu_c1[5:0] with appcpu_c0[1:0] == 0x86 stalls APP_CPU. It does not state what
    // releases it, so both fields are cleared and the release is inferred, not sourced.
    constexpr uintptr_t RTC_OPTIONS0 = RTC_CNTL_BASE + 0x000u;
    constexpr uintptr_t RTC_SW_CPU_STALL = RTC_CNTL_BASE + 0x0ACu;
    constexpr uint32_t SW_STALL_APPCPU_C0_MASK = 0x3u << 0;
    constexpr uint32_t SW_STALL_APPCPU_C1_MASK = 0x3Fu << 20;

    // 100000 rounds a side is ~4 ms of contention at 240 MHz.
    constexpr uint32_t ROUNDS = 100000u;
    constexpr uint32_t EXPECTED = 2u * ROUNDS;

    // The retry loop must stay finite: an s32c1i that executes and never succeeds has to
    // report, not hang.
    constexpr uint32_t RETRY_CAP = 8u * ROUNDS + 1024u;

    constexpr uint64_t DEADLINE_NS = 2000000000ull;
    constexpr uint64_t POLL_NS = 1000000ull;

    constexpr char HEXDIGITS[] = "0123456789abcdef";

    char g_line[144];

    inline volatile uint32_t& r32(uintptr_t a)
    {
        return *reinterpret_cast<volatile uint32_t*>(a);
    }

    // ksnprintf has no field width, so a bare %x cannot show a full 32-bit word.
    void hex8(char* out, uint32_t v)
    {
        for (uint32_t i = 0; i < 8u; i++)
        {
            out[i] = HEXDIGITS[(v >> (28u - 4u * i)) & 0xFu];
        }
        out[8] = '\0';
    }

    void say(char const* s)
    {
        kos::print(s);
    }

    void say_hex(char const* tag, uint32_t v)
    {
        char h[9];
        hex8(h, v);
        ksnprintf(g_line, sizeof(g_line), "[lx6smp] %s 0x%s\n", tag, h);
        kos::print(g_line);
    }

    inline void memory_wait()
    {
        __asm volatile("memw" ::: "memory");
    }

    inline uint32_t rd_prid()
    {
        uint32_t v;
        __asm volatile("rsr.prid %0" : "=a"(v));
        return v;
    }

    inline uint32_t rd_atomctl()
    {
        uint32_t v;
        __asm volatile("rsr.atomctl %0" : "=a"(v));
        return v;
    }

    inline uint32_t l32ai(volatile uint32_t* addr)
    {
        uint32_t v;
        __asm volatile("l32ai %0, %1, 0" : "=a"(v) : "a"(addr) : "memory");
        return v;
    }

    // Returns the value S32C1I read from the location: equal to `expect` exactly when the
    // store was performed (ISA summary 8.3.281, p.590).
    inline uint32_t s32c1i(volatile uint32_t* addr, uint32_t expect, uint32_t desired)
    {
        uint32_t ret = desired;
        __asm volatile("wsr.scompare1 %[e]\n\t"
                       "s32c1i %[r], %[a], 0"
                       : [r] "+a"(ret)
                       : [e] "a"(expect), [a] "a"(addr)
                       : "memory");
        return ret;
    }

    bool wait_for(volatile uint32_t const& cell, uint32_t want)
    {
        uint64_t const start = kos_clock_now();
        while (true)
        {
            if (cell == want)
            {
                return true;
            }
            if ((kos_clock_now() - start) >= DEADLINE_NS)
            {
                return cell == want;
            }
            kos_sleep_ns(POLL_NS);
        }
    }

    // PRO_CPU's own half of the contended increment, the same shape app_cpu.S runs.
    // Returns false if the retry cap tripped.
    bool pro_rounds(uint32_t* out_ok, uint32_t* out_retry)
    {
        volatile uint32_t* const addr = &g_lx6smp_cells.counter;
        uint32_t ok = 0;
        uint32_t retry = 0;
        while (ok < ROUNDS)
        {
            uint32_t const old = l32ai(addr);
            uint32_t const got = s32c1i(addr, old, old + 1u);
            if (got == old)
            {
                ok++;
                continue;
            }
            retry++;
            if (retry >= RETRY_CAP)
            {
                *out_ok = ok;
                *out_retry = retry;
                return false;
            }
        }
        *out_ok = ok;
        *out_retry = retry;
        return true;
    }

    void launch_app_cpu(uint32_t entry)
    {
        say("[lx6smp] MMIO DPORT+0x038 APPCPU_CTRL_REG_D <- entry (TRM Register 12.8)\n");
        r32(APPCPU_CTRL_D) = entry;
        say_hex("readback DPORT+0x038 =", r32(APPCPU_CTRL_D));

        say("[lx6smp] MMIO RTC_CNTL+0x000 OPTIONS0 SW_STALL_APPCPU_C0 <- 0 (TRM Register 9.1)\n");
        r32(RTC_OPTIONS0) = r32(RTC_OPTIONS0) & ~SW_STALL_APPCPU_C0_MASK;
        say("[lx6smp] MMIO RTC_CNTL+0x0AC SW_CPU_STALL SW_STALL_APPCPU_C1 <- 0 (TRM Register 9.34)\n");
        r32(RTC_SW_CPU_STALL) = r32(RTC_SW_CPU_STALL) & ~SW_STALL_APPCPU_C1_MASK;

        say("[lx6smp] MMIO DPORT+0x030 APPCPU_CLKGATE_EN <- 1 (TRM Register 12.6)\n");
        r32(APPCPU_CTRL_B) = r32(APPCPU_CTRL_B) | APPCPU_CLKGATE_EN;

        say("[lx6smp] MMIO DPORT+0x034 APPCPU_RUNSTALL <- 0 (TRM Register 12.7)\n");
        r32(APPCPU_CTRL_C) = r32(APPCPU_CTRL_C) & ~APPCPU_RUNSTALL;

        say("[lx6smp] MMIO DPORT+0x02C APPCPU_RESETTING <- 1 then 0 (TRM Register 12.5)\n");
        r32(APPCPU_CTRL_A) = r32(APPCPU_CTRL_A) | APPCPU_RESETTING;
        r32(APPCPU_CTRL_A) = r32(APPCPU_CTRL_A) & ~APPCPU_RESETTING;
    }

    void report_app_side(char const* phase)
    {
        ksnprintf(g_line, sizeof(g_line), "[lx6smp] --- APP_CPU cells, %s ---\n", phase);
        kos::print(g_line);
        say_hex("APP_CPU ck1 (alive)     =", g_lx6smp_cells.ck1);
        say_hex("APP_CPU PRID            =", g_lx6smp_cells.prid);
        say_hex("APP_CPU ck2 (post-PRID) =", g_lx6smp_cells.ck2);
        say_hex("APP_CPU ck3 (post-loop) =", g_lx6smp_cells.ck3);
        ksnprintf(g_line, sizeof(g_line),
                  "[lx6smp] APP_CPU stores %u retries %u\n",
                  static_cast<unsigned>(g_lx6smp_cells.ok),
                  static_cast<unsigned>(g_lx6smp_cells.retry));
        kos::print(g_line);
    }

    void verdict(uint32_t pro_prid, uint32_t pro_ok, uint32_t pro_retry, bool pro_capped)
    {
        uint32_t const final_count = g_lx6smp_cells.counter;
        uint32_t const app_prid = g_lx6smp_cells.prid;
        bool const app_finished = g_lx6smp_cells.ck3 == CK3_MAGIC;
        bool const contended = (pro_retry != 0) or (g_lx6smp_cells.retry != 0);

        ksnprintf(g_line, sizeof(g_line),
                  "[lx6smp] counter %u expected %u (PRO_CPU stores %u retries %u)\n",
                  static_cast<unsigned>(final_count), static_cast<unsigned>(EXPECTED),
                  static_cast<unsigned>(pro_ok), static_cast<unsigned>(pro_retry));
        kos::print(g_line);

        if (pro_capped)
        {
            say("[lx6smp] VERDICT INCONCLUSIVE PRO_CPU hit the retry cap; s32c1i never succeeded\n");
            return;
        }
        if (g_lx6smp_cells.ck1 != CK1_MAGIC)
        {
            say("[lx6smp] VERDICT INCONCLUSIVE APP_CPU never wrote ck1; it did not start\n");
            return;
        }
        if (g_lx6smp_cells.ck2 != CK2_MAGIC)
        {
            say("[lx6smp] VERDICT IDENTITY_FAULT APP_CPU stopped between ck1 and ck2: rsr.prid killed it\n");
            return;
        }
        if (not app_finished)
        {
            say("[lx6smp] VERDICT ATOMIC_FAULT APP_CPU stopped between ck2 and ck3: the l32ai/s32c1i loop killed it\n");
            return;
        }
        if (final_count != EXPECTED)
        {
            say("[lx6smp] VERDICT EXCLUSION_NO s32c1i executes and does NOT exclude across the two CPUs\n");
            return;
        }
        if (not contended)
        {
            say("[lx6smp] VERDICT UNWITNESSED counter is exact but neither core retried: the phases did not overlap\n");
            return;
        }
        if (pro_prid == app_prid)
        {
            say("[lx6smp] VERDICT IDENTITY_NO exclusion holds but the two PRID values are equal\n");
            return;
        }
        say("[lx6smp] VERDICT BOTH_YES s32c1i excludes across CPUs and the two PRID values differ\n");
    }
}

int main(int, char**)
{
    say("[lx6smp] ESP32 LX6 shared-kernel probe: S32C1I across PRO_CPU/APP_CPU, and PRID\n");

    uint32_t const entry = reinterpret_cast<uint32_t>(&kickos_lx6smp_app_entry);
    say_hex("APP_CPU entry (.text, IRAM internal SRAM0) =", entry);
    say_hex("shared cells (.bss, DRAM internal SRAM2)   =",
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_lx6smp_cells)));

    g_lx6smp_cells.counter = 0;
    g_lx6smp_cells.go = 0;
    g_lx6smp_cells.ck1 = 0;
    g_lx6smp_cells.prid = 0;
    g_lx6smp_cells.ck2 = 0;
    g_lx6smp_cells.ck3 = 0;
    g_lx6smp_cells.ok = 0;
    g_lx6smp_cells.retry = 0;
    g_lx6smp_cells.rounds = ROUNDS;
    memory_wait();

    // The atomic goes first: a fault on rsr.prid ends the run and already answers the
    // identity column.
    say("[lx6smp] RISKY next: l32ai on the shared counter (ISA summary 4.3.12)\n");
    uint32_t const seen = l32ai(&g_lx6smp_cells.counter);
    say_hex("l32ai read              =", seen);

    say("[lx6smp] RISKY next: s32c1i on the shared counter, single core (ISA summary 4.3.13)\n");
    uint32_t const got = s32c1i(&g_lx6smp_cells.counter, seen, seen + 1u);
    say_hex("s32c1i returned         =", got);
    say_hex("counter after           =", g_lx6smp_cells.counter);

    say("[lx6smp] RISKY next: rsr.prid on PRO_CPU (SR 235, ISA summary 6.4)\n");
    uint32_t const pro_prid = rd_prid();
    say_hex("PRO_CPU PRID            =", pro_prid);

    g_lx6smp_cells.counter = 0;
    memory_wait();

    launch_app_cpu(entry);

    bool const app_up = wait_for(g_lx6smp_cells.ck2, CK2_MAGIC);
    if (app_up)
    {
        say("[lx6smp] APP_CPU reached ck2 within the deadline\n");
    }
    else
    {
        say("[lx6smp] APP_CPU did NOT reach ck2 within 2 s\n");
    }
    report_app_side("before the contended phase");

    say("[lx6smp] opening the contended phase: 100000 s32c1i increments per core\n");
    g_lx6smp_cells.go = 1;
    memory_wait();

    uint32_t pro_ok = 0;
    uint32_t pro_retry = 0;
    bool const pro_done = pro_rounds(&pro_ok, &pro_retry);

    bool const app_done = wait_for(g_lx6smp_cells.ck3, CK3_MAGIC);
    if (not app_done)
    {
        say("[lx6smp] APP_CPU did NOT reach ck3 within 2 s\n");
    }
    report_app_side("after the contended phase");
    verdict(pro_prid, pro_ok, pro_retry, not pro_done);

    // Last, so a refusal here costs no measurement: ATOMCTL is SR 99 and RSR of a number
    // >= 64 raises PrivilegedCause if CRING != 0 (ISA summary 8.3.276, p.585).
    say("[lx6smp] RISKY next: rsr.atomctl (SR 99, ISA summary 4.3.13.4)\n");
    uint32_t const atomctl = rd_atomctl();
    say_hex("ATOMCTL                 =", atomctl);
    say("[lx6smp] ATOMCTL fields WB[5:4] WT[3:2] BY[1:0]: 0=exception 1=RCW 2=internal\n");

    say("[lx6smp] probe complete\n");
    while (true)
    {
        kos_sleep_ns(1000000000ull);
    }
}
