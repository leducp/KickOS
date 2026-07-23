// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// KickOS telemetry record format + PURE encoders (telemetry.md deliverable 3).
//
// This header is deliberately dependency-free (only <stdint.h>/<stddef.h>) and
// carries NO globals and NO clock: every encoder is a referentially-transparent
// function of its arguments, so the golden-vector CI gate can drive them with a
// scripted seq/timestamp and get byte-for-byte deterministic output on the host.
// It lives in the repo-root include/ (like units.h) so the kernel frontend and a
// host decoder/test can share ONE source of truth for the wire format.
//
// Wire format: little-endian (all KickOS targets are LE), fixed length per event
// type (self-delimiting -- the decoder maps type -> length, never guesses). A
// half-written record would desynchronize the decoder permanently, so the
// frontend checks free space for the WHOLE record and drops it atomically.
//
// Common prefix (every record): type:u8 @0, seq:u16 @1, t:u32 @3  => 7 bytes.
//   `seq`  monotonic per session; a gap tells the decoder it lost records.
//   `t`    the trace clock (arch_trace_now) at emit; u32, may wrap (ts_bits/anchors
//          in SESSION let the decoder reconstruct absolute time).

#ifndef KICKOS_TRACE_RECORD_H
#define KICKOS_TRACE_RECORD_H

#include <stdint.h>
#include <stddef.h>

namespace kickos
{
    namespace trace
    {
        // Event type tags (record[0]). Kept nonzero so a zeroed buffer never
        // decodes as a valid record.
        enum EventType : uint8_t
        {
            EV_SESSION = 1,
            EV_SWITCH = 2,
            EV_SYSCALL_ENTER = 3,
            EV_SYSCALL_EXIT = 4,
            EV_IRQ_ENTER = 5,
            EV_IRQ_EXIT = 6
        };

        // Architecture id carried in the SESSION record (host tool labels traces).
        // These ids are WIRE VALUES: the build's KICKOS_TRACE_ARCH ladder
        // (CMakeLists.txt) and the decoder (tools/kicktrace.py) key off them, so
        // they MUST NOT be reordered or renumbered. The static_assert below pins the
        // last id so a reorder trips the build.
        enum ArchId : uint8_t
        {
            ARCH_SIM = 0,
            ARCH_ARMV7M = 1,
            ARCH_ARMV6M = 2,
            ARCH_XTENSA = 3,
            ARCH_RX = 4,
            ARCH_RISCV = 5
        };
        // Pin EVERY id: a reorder of the intermediate entries must trip the build, not
        // just a change to the last one (KICKOS_TRACE_ARCH in CMake must match these).
        static_assert(ARCH_SIM == 0 and ARCH_ARMV7M == 1 and ARCH_ARMV6M == 2
                          and ARCH_XTENSA == 3 and ARCH_RX == 4 and ARCH_RISCV == 5,
                      "KICKOS_TRACE_ARCH ids must match enum ArchId");

        enum : uint16_t
        {
            // Pseudo IRQ line for the periodic/one-shot timer ISR (kickos_isr_timer):
            // it has no NVIC line number, so it is reported under this sentinel.
            TRACE_TIMER_LINE = 0xFFFE,
            // "No thread" sentinel (e.g. the outgoing side of the very first switch).
            TRACE_NO_THREAD = 0xFFFF
        };

        enum : uint32_t
        {
            // 'KTRC' as an LE u32 (bytes 43 52 54 4B): the SESSION sync word the
            // decoder scans for to (re)anchor on the stream.
            TRACE_MAGIC = 0x4B545243u
        };

        enum : uint8_t
        {
            TRACE_VERSION = 1
        };

        // Fixed record lengths, by event type.
        enum : size_t
        {
            REC_SESSION = 28,
            REC_SWITCH = 11,
            REC_SYSCALL = 11,
            REC_IRQ = 9,
            // Buffer size a caller must provide to any encoder (the largest record).
            TRACE_MAX_RECORD = 28
        };

        // --- little-endian store helpers (explicit, host-endian-independent) ----
        inline void put_u8(uint8_t* p, uint8_t v)
        {
            p[0] = v;
        }
        inline void put_u16(uint8_t* p, uint16_t v)
        {
            p[0] = static_cast<uint8_t>(v & 0xFFu);
            p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
        }
        inline void put_u32(uint8_t* p, uint32_t v)
        {
            p[0] = static_cast<uint8_t>(v & 0xFFu);
            p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
            p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
            p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
        }
        inline void put_u64(uint8_t* p, uint64_t v)
        {
            put_u32(p, static_cast<uint32_t>(v & 0xFFFFFFFFu));
            put_u32(p + 4, static_cast<uint32_t>((v >> 32) & 0xFFFFFFFFu));
        }

        // Common 7-byte prefix. Returns the offset past it (7).
        inline size_t put_prefix(uint8_t* buf, uint8_t type, uint16_t seq, uint32_t t)
        {
            put_u8(buf + 0, type);
            put_u16(buf + 1, seq);
            put_u32(buf + 3, t);
            return 7;
        }

        // --- PURE encoders: (buf, seq, t, payload...) -> record length ----------

        inline size_t encode_session(uint8_t* buf, uint16_t seq, uint32_t t,
                                     uint8_t arch, uint8_t ts_bits,
                                     uint16_t probe_overhead,
                                     uint32_t records_attempted,
                                     uint64_t t_anchor)
        {
            size_t o = put_prefix(buf, EV_SESSION, seq, t);
            put_u32(buf + o, TRACE_MAGIC);
            put_u8(buf + o + 4, TRACE_VERSION);
            put_u8(buf + o + 5, arch);
            put_u8(buf + o + 6, ts_bits);
            put_u16(buf + o + 7, probe_overhead);
            put_u32(buf + o + 9, records_attempted);
            put_u64(buf + o + 13, t_anchor);
            return REC_SESSION; // 7 + 21
        }

        inline size_t encode_switch(uint8_t* buf, uint16_t seq, uint32_t t,
                                    uint16_t from_tid, uint16_t to_tid)
        {
            size_t o = put_prefix(buf, EV_SWITCH, seq, t);
            put_u16(buf + o, from_tid);
            put_u16(buf + o + 2, to_tid);
            return REC_SWITCH;
        }

        inline size_t encode_syscall_enter(uint8_t* buf, uint16_t seq, uint32_t t,
                                           uint16_t tid, uint16_t nr)
        {
            size_t o = put_prefix(buf, EV_SYSCALL_ENTER, seq, t);
            put_u16(buf + o, tid);
            put_u16(buf + o + 2, nr);
            return REC_SYSCALL;
        }

        inline size_t encode_syscall_exit(uint8_t* buf, uint16_t seq, uint32_t t,
                                          uint16_t tid, uint16_t nr)
        {
            size_t o = put_prefix(buf, EV_SYSCALL_EXIT, seq, t);
            put_u16(buf + o, tid);
            put_u16(buf + o + 2, nr);
            return REC_SYSCALL;
        }

        inline size_t encode_irq_enter(uint8_t* buf, uint16_t seq, uint32_t t,
                                       uint16_t line)
        {
            size_t o = put_prefix(buf, EV_IRQ_ENTER, seq, t);
            put_u16(buf + o, line);
            return REC_IRQ;
        }

        inline size_t encode_irq_exit(uint8_t* buf, uint16_t seq, uint32_t t,
                                      uint16_t line)
        {
            size_t o = put_prefix(buf, EV_IRQ_EXIT, seq, t);
            put_u16(buf + o, line);
            return REC_IRQ;
        }

        // Record length for a given type tag, or 0 if unknown (decoder guard).
        inline size_t record_len(uint8_t type)
        {
            if (type == EV_SESSION)
            {
                return REC_SESSION;
            }
            if (type == EV_SWITCH)
            {
                return REC_SWITCH;
            }
            if (type == EV_SYSCALL_ENTER or type == EV_SYSCALL_EXIT)
            {
                return REC_SYSCALL;
            }
            if (type == EV_IRQ_ENTER or type == EV_IRQ_EXIT)
            {
                return REC_IRQ;
            }
            return 0;
        }
    }
}

#endif
