// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// SEGGER RTT up-channel writer. The control block below reproduces the
// documented on-target layout that J-Link locates by scanning RAM for the
// acID string "SEGGER RTT"; it is statically initialized so the C-runtime
// .data copy sets it up with no runtime init. Only that memory layout is
// borrowed (a wire protocol, not vendor source) -- the ring logic is original.

#include <kickos/rtt.h>

#include <stdint.h>

namespace
{
    // Field NAMES are ours (KickOS style); only the field ORDER and sizes are
    // fixed -- J-Link locates and reads this block by offset, not by name.
    struct RingBuffer
    {
        char const* name;
        char* buffer;
        uint32_t size;
        uint32_t wr_off; // written by target only
        uint32_t rd_off; // written by host only
        uint32_t flags;
    };

    // Two up channels now: 0 = console text, 1 = binary telemetry. The host
    // (J-Link / OpenOCD `rtt`) reads up_count then iterates up[0..up_count-1],
    // so the array MUST hold up_count entries laid out contiguously before down[].
    struct ControlBlock
    {
        char id[16]; // holds "SEGGER RTT" -- the signature J-Link scans RAM for
        int32_t up_count;
        int32_t down_count;
        RingBuffer up[2];
        RingBuffer down[1];
    };

    char up_buf[1024];
    // Telemetry ch1 ring. On a data-cached core (M7 / ESP32 / RX72M) this must
    // live in uncached RAM so the probe sees writes; a non-issue on cacheless
    // M0-M4 (the M1 targets). Size is a build knob: the default is MCU-RAM-safe;
    // the sim/CI raises it (a host drains only at shutdown, so it must hold a
    // whole run) via -DKICKOS_RTT_CH1_SIZE.
#ifndef KICKOS_RTT_CH1_SIZE
#define KICKOS_RTT_CH1_SIZE 4096
#endif
    char up1_buf[KICKOS_RTT_CH1_SIZE];
    char down_buf[16];
}

extern "C"
{
    // Flags = 0 selects NO_BLOCK_SKIP: a full buffer drops rather than blocks.
    ControlBlock _SEGGER_RTT = {
        {'S', 'E', 'G', 'G', 'E', 'R', ' ', 'R', 'T', 'T', 0, 0, 0, 0, 0, 0},
        2,
        1,
        {{"Terminal", up_buf, sizeof up_buf, 0, 0, 0},
         {"Telemetry", up1_buf, sizeof up1_buf, 0, 0, 0}},
        {{"Terminal", down_buf, sizeof down_buf, 0, 0, 0}},
    };
}

// Single-core console: this only advances WrOff and reads the host-owned RdOff,
// so a torn concurrent target writer (thread vs. ISR interleaving) is out of
// scope for M1 -- callers must not write the up-channel from two contexts at
// once.
extern "C" void kickos_rtt_write(char const* buf, size_t n)
{
    RingBuffer& up = _SEGGER_RTT.up[0];
    uint32_t const size = up.size;

    for (size_t i = 0; i < n; i++)
    {
        uint32_t next = up.wr_off + 1;
        if (next >= size)
        {
            next = 0;
        }
        if (next == up.rd_off)
        {
            break; // full: drop the rest, never block (host may be detached)
        }
        up.buffer[up.wr_off] = buf[i];
        KICKOS_RTT_PUBLISH_BARRIER(); // publish the byte before the offset
        up.wr_off = next;
    }
}

// Channel 1 (telemetry), RECORD-ATOMIC. Callers must serialize (the kernel
// frontend holds IrqLock across seq+stamp+write), so this is single-writer here;
// it only advances WrOff and reads the host-owned RdOff. Whole-record-or-drop: a
// half-written binary record would desync the decoder forever, so we check free
// space for all `n` bytes up front and drop the record entirely if it won't fit.
extern "C" int kickos_rtt_write_record_ch1(uint8_t const* rec, size_t n)
{
    RingBuffer& up = _SEGGER_RTT.up[1];
    uint32_t const size = up.size;
    uint32_t const wr = up.wr_off;
    uint32_t const rd = up.rd_off;
    // Bytes currently pending, and free space (one slot reserved so wr==rd means
    // empty, never "full"): usable capacity is size-1.
    uint32_t used = (wr + size - rd) % size;
    uint32_t freeb = size - 1u - used;
    if (n > freeb)
    {
        return 0; // drop the whole record; caller counts it
    }
    uint32_t w = wr;
    for (size_t i = 0; i < n; i++)
    {
        up.buffer[w] = static_cast<char>(rec[i]);
        w++;
        if (w >= size)
        {
            w = 0;
        }
    }
    KICKOS_RTT_PUBLISH_BARRIER(); // publish all payload bytes before the offset
    up.wr_off = w;                // single commit -> the record appears atomically
    return 1;
}

// Drain pending ch1 bytes (rd_off -> wr_off) into `out`, advancing rd_off.
// Host-side (sim flush); not called on the measured path.
extern "C" size_t kickos_rtt_ch1_drain(char* out, size_t max)
{
    RingBuffer& up = _SEGGER_RTT.up[1];
    uint32_t const size = up.size;
    size_t copied = 0;
    while (copied < max && up.rd_off != up.wr_off)
    {
        out[copied] = up.buffer[up.rd_off];
        copied++;
        uint32_t next = up.rd_off + 1;
        if (next >= size)
        {
            next = 0;
        }
        up.rd_off = next;
    }
    return copied;
}
