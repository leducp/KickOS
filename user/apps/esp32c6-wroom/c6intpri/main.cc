// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-C6 CPU-interrupt-controller identity probe: is the undocumented
// 0x2000_1000 window a SECOND VIEW of INTPRI @0x600C_5000, or a DISTINCT live block?
//
// The tree writes its CPU-interrupt enable at 0x2000_1000+0x00 (regs/plic.h) and never
// writes INTPRI_CORE0_CPU_INT_ENABLE_REG @0x600C_5000+0x00, which resets to 0 (TRM v1.2
// Register 10.64). CPU int 31 and CPU int 30 both deliver on silicon, which TRM section
// 1.6.2 item 2 forbids with that enable at 0.
//
// A direct 0x2000_1000 readback is not reachable from any app on this port: it is a
// Rule 7 reserved block, refused by grant_region_admissible for privileged and
// unprivileged callers alike (kernel/grant/grant.cc), and root and every app thread run
// unprivileged (kernel/init/kmain.cc). So the probe reads INTPRI and asks whether two
// boot-time writes to 0x2000_1000 are visible there. Both are absolute, not deltas:
//
//   A = INTPRI+0x00 bit 31, set at 0x2000_1000+0x00 by inject_doorbell_init
//       (chip_esp32c6.cc) during arch_init.
//   B = INTPRI+0x00 bit 30, set at 0x2000_1000+0x00 by console_buffer_init (kmain.cc,
//       unconditional on this chip) through arch_rv_hw_unmask(UART0_TX_LINE).
//   L = the doorbell delivers AT MEASUREMENT TIME: inject an unbound line, which raises
//       INTPRI FROM_CPU_0 -> CPU int 31, and watch the spurious counter move. Without L
//       an all-zero INTPRI enable proves nothing.
//
// There is deliberately NO in-flight-write test: every 0x2000_1000 write an app can
// provoke was already made at boot with the SAME value, so a re-issued kos_irq_unmask is
// an idempotent re-OR whose delta is 0 whatever the answer. The call is still made as an
// execution witness; a nonzero delta means boot state is not what is described here.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/errno.h>
#include <kickos/libc/fmt.h>

#include <stdint.h>

// Without enforcement there is no Rule 7 and the grant predicates below are inline
// `return true`, so the capture would describe a different machine.
#if !KICKOS_HAVE_MPU
#error "c6intpri requires enforcement: configure with -DKICKOS_HAVE_MPU=1"
#endif

namespace
{
    // TRM v1.2 Table 5.3-2 / section 10.4.2.
    constexpr uintptr_t INTPRI_BASE = 0x600C5000u;
    constexpr uintptr_t PLIC_MX_BASE = 0x20001000u;

    // 256 B: the smallest PMP NAPOT window (pow2, base-aligned) reaching THRESH @0x8C
    // and the FROM_CPU words @0x90. A 128 B window would stop short of both.
    constexpr uint32_t INTPRI_WINDOW = 256u;

    constexpr uint32_t DOORBELL_CPU_INT = 31; // KICKOS_RV_INJECT_DOORBELL_CPU_INT
    constexpr uint32_t DEV_CPU_INT = 30;      // KICKOS_RV_DEV_CPU_INT (UART0 grouped line)
    constexpr uint32_t DOORBELL_BIT = 1u << DOORBELL_CPU_INT;
    constexpr uint32_t DEV_BIT = 1u << DEV_CPU_INT;

    constexpr int UART0_TX_LINE = 16; // kickos::esp32c6::irq::UART0_TX_LINE
    constexpr int PROBE_LINE = 20;    // unbound: an inject on it lands on the spurious counter

    // The offsets where the two register maps disagree, plus the two they share. INTPRI
    // has no read-to-clear register in this range, so snapshotting perturbs nothing.
    struct regdef
    {
        uint32_t off;
        char const* name;
    };
    constexpr regdef REGS[] = {
        {0x00u, "ENABLE      / ENABLE     "},
        {0x04u, "TYPE        / TYPE       "},
        {0x08u, "EIP_STATUS  / CLEAR      "},
        {0x84u, "PRI_30      / PRI_29     "},
        {0x88u, "PRI_31      / PRI_30     "},
        {0x8Cu, "THRESH      / PRI_31     "},
        {0x90u, "FROM_CPU_0  / THRESH     "},
        {0x94u, "FROM_CPU_1  / CLAIM      "},
    };
    constexpr uint32_t REG_COUNT = sizeof(REGS) / sizeof(REGS[0]);

    constexpr char HEXDIGITS[] = "0123456789abcdef";

    // ksnprintf has no field width, so a bare %x cannot show a full 32-bit word.
    void hex8(char* out, uint32_t v)
    {
        for (uint32_t i = 0; i < 8u; i++)
        {
            out[i] = HEXDIGITS[(v >> (28u - 4u * i)) & 0xFu];
        }
        out[8] = '\0';
    }

    inline volatile uint32_t& r32(uintptr_t a)
    {
        return *reinterpret_cast<volatile uint32_t*>(a);
    }

    char const* yes_no(bool v)
    {
        if (v)
        {
            return "yes";
        }
        return "no";
    }

    void snapshot(uintptr_t win, uint32_t* out)
    {
        for (uint32_t i = 0; i < REG_COUNT; i++)
        {
            out[i] = r32(win + REGS[i].off);
        }
    }

    void dump(char const* tag, uint32_t const* v)
    {
        for (uint32_t i = 0; i < REG_COUNT; i++)
        {
            char hv[9];
            char ho[9];
            hex8(hv, v[i]);
            hex8(ho, INTPRI_BASE + REGS[i].off);
            char line[96];
            ksnprintf(line, sizeof(line), "[c6intpri] %s 0x%s +0x%x %s = 0x%s\n",
                      tag, ho, static_cast<unsigned>(REGS[i].off), REGS[i].name, hv);
            kos::print(line);
        }
    }

    // The retry cap must stay finite: a dead doorbell has to report, not hang.
    constexpr int SETTLE_TRIES = 20;
    constexpr uint64_t SETTLE_NS = 1000000ull;

    bool doorbell_is_live(void)
    {
        // All lines reset MASKED (arch_rv32imac.cc g_irq_masked), and a masked inject
        // only latches: without this the raise never reaches the controller.
        int const um = kos_irq_unmask(PROBE_LINE);
        char m[72];
        ksnprintf(m, sizeof(m), "[c6intpri] irq_unmask(line %d, unbound) rc %d\n",
                  PROBE_LINE, um);
        kos::print(m);
        if (um != 0)
        {
            return false;
        }
        uint32_t const before = kos_irq_spurious_count();
        kos_irq_inject(PROBE_LINE);
        uint32_t after = before;
        for (int i = 0; i < SETTLE_TRIES; i++)
        {
            after = kos_irq_spurious_count();
            if (after != before)
            {
                break;
            }
            kos_sleep_ns(SETTLE_NS);
        }
        char s[96];
        ksnprintf(s, sizeof(s), "[c6intpri] spurious %u -> %u after inject on CPU int %u\n",
                  static_cast<unsigned>(before), static_cast<unsigned>(after),
                  static_cast<unsigned>(DOORBELL_CPU_INT));
        kos::print(s);
        return after != before;
    }

    void probe(void* arg)
    {
        uintptr_t const win = reinterpret_cast<uintptr_t>(arg);

        uint32_t pre[REG_COUNT];
        snapshot(win, pre);
        dump("PRE ", pre);

        bool const a_set = (pre[0] & DOORBELL_BIT) != 0;
        bool const b_set = (pre[0] & DEV_BIT) != 0;
        char hdr[128];
        ksnprintf(hdr, sizeof(hdr),
                  "[c6intpri] A: CPU int %u (inject_doorbell_init) bit 0x%x in INTPRI ENABLE: %s\n",
                  static_cast<unsigned>(DOORBELL_CPU_INT), static_cast<unsigned>(DOORBELL_BIT),
                  yes_no(a_set));
        kos::print(hdr);
        ksnprintf(hdr, sizeof(hdr),
                  "[c6intpri] B: CPU int %u (console_buffer_init) bit 0x%x in INTPRI ENABLE: %s\n",
                  static_cast<unsigned>(DEV_CPU_INT), static_cast<unsigned>(DEV_BIT),
                  yes_no(b_set));
        kos::print(hdr);

        bool const live = doorbell_is_live();
        char l[80];
        ksnprintf(l, sizeof(l), "[c6intpri] L: doorbell delivers at measurement time: %s\n",
                  yes_no(live));
        kos::print(l);

        // Witness, NOT a discriminator: the console armed this line at boot, so every
        // register touched here is rewritten with the value it already holds and the
        // console keeps draining (uart0_quiesce_once is latched, the binding stands).
        char a[120];
        ksnprintf(a, sizeof(a),
                  "[c6intpri] re-unmasking line %d (idempotent) -> 0x20001000+0x00 |= 0x%x\n",
                  UART0_TX_LINE, static_cast<unsigned>(DEV_BIT));
        kos::print(a);
        int const rc = kos_irq_unmask(UART0_TX_LINE);
        // Snapshot before the next print: the console drains through this very line, so a
        // console write between the call and the read puts ISR traffic inside the window.
        uint32_t post[REG_COUNT];
        snapshot(win, post);

        char r[64];
        ksnprintf(r, sizeof(r), "[c6intpri] irq_unmask(line %d) rc %d\n", UART0_TX_LINE, rc);
        kos::print(r);
        dump("POST", post);

        uint32_t const delta = pre[0] ^ post[0];
        char hd[9];
        hex8(hd, delta);
        char d[120];
        ksnprintf(d, sizeof(d),
                  "[c6intpri] D: ENABLE delta across the idempotent re-unmask = 0x%s (expect 0)\n",
                  hd);
        kos::print(d);

        if (a_set and b_set)
        {
            kos::print("[c6intpri] VERDICT SECOND_VIEW 0x20001000 and 0x600c5000 are one enable\n");
        }
        else if ((not a_set) and (not b_set) and live)
        {
            kos::print("[c6intpri] VERDICT DISTINCT 0x20001000 is a separate live enable block\n");
        }
        else if ((not a_set) and (not b_set) and (not live))
        {
            kos::print("[c6intpri] VERDICT INCONCLUSIVE doorbell did not deliver; measurement void\n");
        }
        else
        {
            kos::print("[c6intpri] VERDICT PARTIAL A and B disagree; read the dumps\n");
        }

        kos_cap_t idle = KOS_CAP_NONE;

        (void)kos_sem_create(0, &idle);
        while (true)
        {
            if (idle == KOS_CAP_NONE)
            {
                kos_sleep_ns(1000000000ull);
                continue;
            }
            kos_sem_wait(idle);
        }
    }
}

// AUTH_IRQ must be held here for the child to be SEATED with it: authority narrows,
// never widens.
KICKOS_APP_AUTHORITY(KOS_AUTH_MEMORY | KOS_AUTH_IRQ);

int main(int, char**)
{
    kos::print("[c6intpri] INTPRI 0x600c5000 vs undocumented CPU int ctl 0x20001000\n");

    // Pure predicates: nothing here perturbs the state the probe is about to read.
    uintptr_t const hits = kos_grant_probe(KOS_GRANT_OP_HITS_RESERVED, PLIC_MX_BASE, INTPRI_WINDOW);
    uintptr_t const dev_mx = kos_grant_probe(KOS_GRANT_OP_DEV_UNPRIVILEGED, PLIC_MX_BASE, INTPRI_WINDOW);
    uintptr_t const dev_ip = kos_grant_probe(KOS_GRANT_OP_DEV_UNPRIVILEGED, INTPRI_BASE, INTPRI_WINDOW);
    char g[144];
    ksnprintf(g, sizeof(g),
              "[c6intpri] grant hits_reserved(0x20001000)=%u dev(0x20001000)=%u dev(0x600c5000)=%u\n",
              static_cast<unsigned>(hits), static_cast<unsigned>(dev_mx),
              static_cast<unsigned>(dev_ip));
    kos::print(g);
    kos::print("[c6intpri] 0x20001000 is Rule 7 reserved and root is unprivileged: no direct readback\n");

    auto const drv = kos::thread::spawn(probe, reinterpret_cast<void*>(INTPRI_BASE),
                                        "c6intpri", 10, KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                        /*mem=*/nullptr, /*mem_size=*/0,
                                        /*stack=*/nullptr, /*stack_size=*/0,
                                        /*mmio=*/reinterpret_cast<void*>(INTPRI_BASE),
                                        INTPRI_WINDOW,
                                        /*caps=*/nullptr, /*cap_count=*/0,
                                        KOS_AUTH_IRQ);
    if (not drv.valid())
    {
        char e[80];
        ksnprintf(e, sizeof(e), "[c6intpri] VERDICT INCONCLUSIVE probe spawn failed rc %d\n", drv.error());
        kos::print(e);
    }

    kos_cap_t idle = KOS_CAP_NONE;

    (void)kos_sem_create(0, &idle);
    while (true)
    {
        if (idle == KOS_CAP_NONE)
        {
            kos_sleep_ns(1000000000ull);
            continue;
        }
        kos_sem_wait(idle);
    }
}
