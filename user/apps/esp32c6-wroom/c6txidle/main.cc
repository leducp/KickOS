// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-C6 witness of the console's transmission-complete wait. arch_console_flush_sync returns
// once TXFIFO_CNT is 0 and FSM_STATUS.ST_UTX_OUT reads the idle encoding, which the C6 TRM does
// not print and the kernel takes from the ESP32's. Once the banner is out, the kernel-side half
// (arch/riscv/chip/esp32c6/txidle_probe_esp32c6.cc) loads a TAIL line into UART0's FIFO, waits
// for the FIFO's last byte to leave it, calls that flush, and the moment it returns holds the
// transmitter in reset: the line's last byte reaches the wire whole only where the flush waited
// for it. This image reports what that half recorded, and judges the transmitter's state while
// the line shifted, once it had been quiet, and at the flush's return.

#include <kickos/kos.h>
#include <kickos/libc/fmt.h>
#include <kickos/sys/emit.h>

#include <stdint.h>

#if KICKOS_HAVE_MPU
#error "c6txidle reads a kernel word: build the board's flat variant"
#endif

extern "C" uint32_t kickos_c6_txidle_record[8];

namespace
{
    // Mirrored from txidle_probe_esp32c6.cc.
    enum : uint32_t
    {
        REC_DONE,
        REC_FRAME,
        REC_HELD,
        REC_IDLE_BITS,
        REC_STATE_BUSY,
        REC_STATE_QUIET,
        REC_STATE_RETURN,
    };
    constexpr uint32_t ST_UTX_OUT_IDLE = 0u; // the encoding under test
}

int main(int, char**)
{
    uint32_t const* const rec = kickos_c6_txidle_record;
    if (rec[REC_DONE] == 2u)
    {
        kickos::emit("[c6txidle] ERROR: the console FIFO never stayed empty\n");
        return 1;
    }
    if (rec[REC_DONE] != 1u)
    {
        kickos::emit("[c6txidle] ERROR: the kernel probe recorded nothing\n");
        return 1;
    }
    uint32_t const frame = rec[REC_FRAME];
    uint32_t const held = rec[REC_HELD];
    uint32_t const busy = rec[REC_STATE_BUSY];
    uint32_t const quiet = rec[REC_STATE_QUIET];
    uint32_t const ret = rec[REC_STATE_RETURN];
    // The transmitter may count TX_IDLE_NUM bit times after the stop bit before it idles; past
    // twice that, the flush ran to its spin bound instead.
    uint32_t const late = frame * (2u + (2u * rec[REC_IDLE_BITS]) / 10u);
    char line[160];
    ksnprintf(line, sizeof(line),
              "[c6txidle] frame %u ticks, flush held %u ticks past the FIFO's last byte (late past"
              " %u)\n",
              static_cast<unsigned>(frame), static_cast<unsigned>(held), static_cast<unsigned>(late));
    kickos::emit(line);
    ksnprintf(line, sizeof(line),
              "[c6txidle] ST_UTX_OUT %u while shifting, %u after 10 ms quiet, %u at the flush's"
              " return; idle is %u\n",
              static_cast<unsigned>(busy), static_cast<unsigned>(quiet), static_cast<unsigned>(ret),
              static_cast<unsigned>(ST_UTX_OUT_IDLE));
    kickos::emit(line);
    if (busy == ST_UTX_OUT_IDLE)
    {
        kickos::emit("[c6txidle] FAIL the transmitter reads the idle encoding while it shifts\n");
        return 1;
    }
    if (quiet != ST_UTX_OUT_IDLE or ret != ST_UTX_OUT_IDLE)
    {
        kickos::emit("[c6txidle] FAIL the transmitter does not idle at the encoding the flush waits for\n");
        return 1;
    }
    if (held < frame / 2u)
    {
        kickos::emit("[c6txidle] FAIL the flush returned before the last frame could finish\n");
        return 1;
    }
    if (held > late)
    {
        kickos::emit("[c6txidle] FAIL the flush returned late, at its spin bound\n");
        return 1;
    }
    kickos::emit("[c6txidle] PASS the flush returned once the last frame had left\n");
    return 0;
}
