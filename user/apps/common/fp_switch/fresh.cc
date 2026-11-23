// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A thread with no FP state, switched in straight after a peer that left all of s0-s31 and
// FPSCR's flags set, must find none of them: once through the switcher (a fresh thread the peer
// creates above itself) and once through the SVC fastpath (a server woken by the peer's
// register call). Built -mgeneral-regs-only: the reader's first FP instruction is its read.

#include <kickos/kos.h>
#include <kickos/libc/fmt.h>
#include <kickos/sys.h>
#include <kickos/sys/abi.h>

#include <stdint.h>

#if not defined(__arm__) or not defined(__ARM_FP)
#error "fresh.cc reads the ARMv7-M FP register file"
#endif

extern "C" int32_t arch_syscall_reg(uint32_t* io);

namespace
{
    using kos::child_cap;

    constexpr uint32_t SEED = 0x5EED0000u;
    // NZCV, QC and the cumulative exception flags: what FPDSCR does not reseed on first use.
    constexpr uint32_t FPSCR_FLAGS = 0xF800009Fu;
    constexpr uint8_t PRIO_READER = 30;

    uint32_t g_seed[32];
    uint32_t g_read[33];
    bool g_ran = false;
    kos_cap_t g_ep = KOS_CAP_NONE;
    uint32_t g_io[KOS_CALL_REG_WORDS + 3];

    void peek()
    {
        __asm volatile("vstmia %0, {s0-s31}\n\t"
                       "vmrs   r1, fpscr\n\t"
                       "str    r1, [%0, #128]\n\t" ::"r"(g_read)
                       : "r1", "memory");
    }

    void fresh(void*)
    {
        peek();
        g_ran = true;
    }

    void server(void*) // caps: E(WAIT)@0
    {
        uint32_t buf[1] = {};
        struct kos_reply_recv_opts opts = {};
        opts.ep = child_cap(0);
        opts.until = KOS_NEVER;
        int const n = kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &opts);
        peek();
        g_ran = true;
        if (n >= 0 and opts.info.reply_cap != KOS_CAP_NONE)
        {
            (void)kos_reply(opts.info.reply_cap, buf, 0);
        }
    }

    // Integer code only, so the bank the caller loaded is still live when the trap switches.
    void spawn_fresh()
    {
        (void)kos::thread::create(fresh, nullptr, "fresh", PRIO_READER);
    }
    void call_server()
    {
        (void)arch_syscall_reg(g_io);
    }

    // Loads every FP register with the seed and FPSCR with its flags, then runs fn.
    void seeded(void (*fn)())
    {
        __asm volatile("vldmia %[s], {s0-s31}\n\t"
                       "vmsr   fpscr, %[f]\n\t"
                       "blx    %[fn]\n\t"
                       "movs   r0, #0\n\t"
                       "vmsr   fpscr, r0\n\t"
                       :
                       : [s] "r"(g_seed), [f] "r"(FPSCR_FLAGS), [fn] "r"(fn)
                       : "r0", "r1", "r2", "r3", "r12", "lr", "cc", "memory", "s0", "s1", "s2",
                         "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11", "s12", "s13",
                         "s14", "s15", "s16", "s17", "s18", "s19", "s20", "s21", "s22", "s23",
                         "s24", "s25", "s26", "s27", "s28", "s29", "s30", "s31");
    }

    bool clean(char const* how)
    {
        char b[96];
        if (not g_ran)
        {
            ksnprintf(b, sizeof(b), "  FP FAIL: %s: the reader never ran\n", how);
            kos::print(b);
            return false;
        }
        bool ok = true;
        for (uint32_t i = 0; i < 32u; i++)
        {
            if (g_read[i] == g_seed[i])
            {
                ksnprintf(b, sizeof(b), "  FP FAIL: %s: read its peer's s%u\n", how,
                          static_cast<unsigned>(i));
                kos::print(b);
                ok = false;
            }
        }
        if ((g_read[32] & FPSCR_FLAGS) != 0u)
        {
            ksnprintf(b, sizeof(b), "  FP FAIL: %s: read its peer's FPSCR flags 0x%x\n", how,
                      static_cast<unsigned>(g_read[32] & FPSCR_FLAGS));
            kos::print(b);
            ok = false;
        }
        return ok;
    }

    void reset()
    {
        g_ran = false;
        for (uint32_t i = 0; i < 33u; i++)
        {
            g_read[i] = 0;
        }
    }
}

bool fp_fresh_starts_clean()
{
    for (uint32_t i = 0; i < 32u; i++)
    {
        g_seed[i] = SEED | (i << 8) | (0xE0u - i);
    }
    reset();
    seeded(spawn_fresh);
    bool const switched = clean("a fresh thread");

    reset();
    kos_cap_t ep = KOS_CAP_NONE;
    if (kos_endpoint_create(&ep) != 0)
    {
        kos::print("  FP FAIL: no endpoint for the fastpath reader\n");
        return false;
    }
    kos_cap_grant const caps[] = {{ep, KOS_CAP_WAIT}};
    kos::thread::Handle const srv =
        kos::thread::create_caps(server, nullptr, "fpsrv", PRIO_READER, caps, 1);
    if (not srv.valid())
    {
        kos::print("  FP FAIL: the fastpath reader could not be created\n");
        return false;
    }
    g_ep = ep;
    g_io[0] = KOS_SYS_CALL_REG;
    g_io[1] = static_cast<uint32_t>(g_ep);
    g_io[2] = static_cast<uint32_t>(kos_call_lens_pack(4, 0));
    seeded(call_server);
    (void)srv.join();
    (void)kos_handle_close(ep);
    bool const fast = clean("a server the fastpath woke");
    return switched and fast;
}
