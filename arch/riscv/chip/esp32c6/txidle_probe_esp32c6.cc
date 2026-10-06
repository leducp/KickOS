// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The console flush's transmission-complete witness for c6txidle, run in M-mode once the banner
// is out and before the ring is armed. Not from U-mode: there an ungranted UART0 read returns 0
// without a trap, which is the idle encoding under test.

#include <kickos/arch/arch.h>

#if !KICKOS_HAVE_MPU && !KICKOS_AMP_OWN_IMAGE

#include <kickos/config/limits.h>

#include <stddef.h>
#include <stdint.h>

#include "regs/clint.h"
#include "regs/uart.h"

namespace reg = kickos::esp32c6::reg;

extern "C"
{
    // Read by c6txidle, which mirrors the indices below.
    uint32_t kickos_c6_txidle_record[8] = {};
    extern uint32_t SystemCoreClock;
}

namespace
{
    enum : uint32_t
    {
        REC_DONE,         // 1 once the line went out, 2 where the FIFO never stayed empty
        REC_FRAME,        // one frame in MTIME ticks, measured over the line
        REC_HELD,         // ticks from the FIFO's last byte leaving it to the flush's return
        REC_IDLE_BITS,    // TX_IDLE_NUM
        REC_STATE_BUSY,   // ST_UTX_OUT while the line shifts
        REC_STATE_QUIET,  // after 10 ms of an empty FIFO
        REC_STATE_RETURN, // at the flush's return
    };

    char const TAIL[] = "[c6txidle] TAIL 0123456789abcdef0123456789abcdef <<<TXIDLE-END>>>";

    // 10 ms outlasts TX_IDLE_NUM's ceiling of 1023 bit times at 115200 baud, so the transmitter
    // has idled. MTIME counts CPU_CLK, which arch_init has published.
    uint64_t quiet_ticks()
    {
        return SystemCoreClock / 100u;
    }
    uint64_t quiet_give_up_ticks()
    {
        return static_cast<uint64_t>(SystemCoreClock) * 2u;
    }
    uint64_t hold_ticks()
    {
        return SystemCoreClock / 500u;
    }

    inline volatile uint32_t& r32(uintptr_t a)
    {
        return *reinterpret_cast<volatile uint32_t*>(a);
    }

    uint64_t ticks()
    {
        volatile uint32_t* const mt = &r32(reg::clint::MTIME);
        uint32_t hi = 0;
        uint32_t lo = 0;
        uint32_t hi2 = 0;
        do
        {
            hi = mt[1];
            lo = mt[0];
            hi2 = mt[1];
        } while (hi != hi2);
        return (static_cast<uint64_t>(hi) << 32) | lo;
    }

    uint32_t tx_queued()
    {
        return (r32(reg::uart::STATUS) >> reg::uart::TXFIFO_CNT_S) & reg::uart::TXFIFO_CNT_MASK;
    }

    uint32_t tx_state()
    {
        return (r32(reg::uart::FSM_STATUS) >> reg::uart::ST_UTX_OUT_S) & reg::uart::ST_UTX_OUT_MASK;
    }

    bool quiet()
    {
        uint64_t const give_up = ticks() + quiet_give_up_ticks();
        uint64_t empty_since = ticks();
        while (ticks() < give_up)
        {
            uint64_t const now = ticks();
            if (tx_queued() != 0)
            {
                empty_since = now;
            }
            else if (now - empty_since >= quiet_ticks())
            {
                return true;
            }
        }
        return false;
    }
}

// The moment the flush returns, the transmitter is held in reset, which ends a frame still
// shifting: the line's last byte reaches the wire whole only where the flush waited for it.
extern "C" void kickos_c6_txidle_probe(void)
{
    uint32_t* const rec = kickos_c6_txidle_record;
    if (not quiet())
    {
        rec[REC_DONE] = 2u;
        return;
    }
    rec[REC_STATE_QUIET] = tx_state();
    rec[REC_IDLE_BITS] = (r32(reg::uart::IDLE_CONF) >> reg::uart::IDLE_CONF_TX_IDLE_NUM_S) & 0x3FFu;
    uint32_t const clk_conf = r32(reg::uart::CLK_CONF);

    size_t const n = sizeof(TAIL) - 1u;
    static_assert(sizeof(TAIL) - 1u <= reg::uart::TXFIFO_LIMIT,
                  "the TAIL line is longer than the FIFO");
    for (size_t i = 0; i < n; i++)
    {
        r32(reg::uart::FIFO) = static_cast<uint8_t>(TAIL[i]);
    }
    uint64_t const loaded = ticks();
    rec[REC_STATE_BUSY] = tx_state();
    for (uint32_t spin = 0; tx_queued() != 0 and spin < KICKOS_POLL_SPIN_MAX; spin++)
    {
    }
    uint64_t const emptied = ticks();
    arch_console_flush_sync();
    r32(reg::uart::CLK_CONF) = clk_conf | reg::uart::CLK_CONF_TX_RST_CORE;
    uint64_t const returned = ticks();
    rec[REC_STATE_RETURN] = tx_state();
    uint64_t const until = returned + hold_ticks();
    while (ticks() < until)
    {
    }
    r32(reg::uart::CLK_CONF) = clk_conf;
    r32(reg::uart::FIFO) = '\r';
    r32(reg::uart::FIFO) = '\n';

    // The FIFO's count falls to 0 as its last byte enters the shifter: n - 1 frames after the
    // first did.
    rec[REC_FRAME] = static_cast<uint32_t>((emptied - loaded) / (n - 1u));
    rec[REC_HELD] = static_cast<uint32_t>(returned - emptied);
    rec[REC_DONE] = 1u;
}

#endif
