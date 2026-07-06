// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Golden-vector generator (telemetry.md CI gate 1). Drives the PURE record
// encoders (include/kickos/trace/record.h) with scripted seq/t/payload, writes
// the resulting bytes to argv[1], and prints the EXPECTED canonical decode to
// stdout -- the exact one-line-per-record form kicktrace.py emits with --csv. The
// gate runs kicktrace on the bytes and asserts its output equals this expected
// text: a byte-exact encode -> decode round-trip over every record type.

#include <kickos/trace/record.h>

#include <cstdio>
#include <cstdint>
#include <cstddef>

using namespace kickos::trace;

namespace
{
    FILE* g_bin = nullptr;

    void emit(uint8_t const* rec, size_t n)
    {
        fwrite(rec, 1, n, g_bin);
    }
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: gen_golden <out.bin>\n");
        return 2;
    }
    g_bin = fopen(argv[1], "wb");
    if (g_bin == nullptr)
    {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }

    uint8_t rec[TRACE_MAX_RECORD];
    size_t n;

    // Opening SESSION: sim arch, ts_bits 32, probe 7, attempted 1, anchor big u64.
    n = encode_session(rec, /*seq*/ 0, /*t*/ 0x11223344u, ARCH_SIM, 32,
                       /*probe*/ 7, /*attempted*/ 1, /*anchor*/ 0x0102030405060708ull);
    emit(rec, n);
    printf("SESSION seq=%u t=%u ver=%u arch=%u ts_bits=%u probe=%u attempted=%u anchor=%llu\n",
           0u, 0x11223344u, (unsigned)TRACE_VERSION, (unsigned)ARCH_SIM, 32u, 7u, 1u,
           (unsigned long long)0x0102030405060708ull);

    // First switch: from = no-thread -> tid 1.
    n = encode_switch(rec, 1, 0x11223350u, TRACE_NO_THREAD, 1);
    emit(rec, n);
    printf("SWITCH seq=%u t=%u from=%u to=%u\n", 1u, 0x11223350u, (unsigned)TRACE_NO_THREAD, 1u);

    // A normal switch 1 -> 2.
    n = encode_switch(rec, 2, 0x11223360u, 1, 2);
    emit(rec, n);
    printf("SWITCH seq=%u t=%u from=%u to=%u\n", 2u, 0x11223360u, 1u, 2u);

    // Syscall enter/exit on tid 2, nr 5 (sem_wait).
    n = encode_syscall_enter(rec, 3, 0x11223370u, 2, 5);
    emit(rec, n);
    printf("SYSCALL_ENTER seq=%u t=%u tid=%u nr=%u\n", 3u, 0x11223370u, 2u, 5u);
    n = encode_syscall_exit(rec, 4, 0x11223380u, 2, 5);
    emit(rec, n);
    printf("SYSCALL_EXIT seq=%u t=%u tid=%u nr=%u\n", 4u, 0x11223380u, 2u, 5u);

    // IRQ enter/exit on a device line, then the timer pseudo-line.
    n = encode_irq_enter(rec, 5, 0x11223390u, 7);
    emit(rec, n);
    printf("IRQ_ENTER seq=%u t=%u line=%u\n", 5u, 0x11223390u, 7u);
    n = encode_irq_exit(rec, 6, 0x112233A0u, 7);
    emit(rec, n);
    printf("IRQ_EXIT seq=%u t=%u line=%u\n", 6u, 0x112233A0u, 7u);
    n = encode_irq_enter(rec, 7, 0x112233B0u, TRACE_TIMER_LINE);
    emit(rec, n);
    printf("IRQ_ENTER seq=%u t=%u line=%u\n", 7u, 0x112233B0u, (unsigned)TRACE_TIMER_LINE);
    n = encode_irq_exit(rec, 8, 0x112233C0u, TRACE_TIMER_LINE);
    emit(rec, n);
    printf("IRQ_EXIT seq=%u t=%u line=%u\n", 8u, 0x112233C0u, (unsigned)TRACE_TIMER_LINE);

    // Closing SESSION: far anchor + final attempted count (10 records incl this).
    n = encode_session(rec, 9, 0x11224000u, ARCH_SIM, 32, 7, 10, 0x0102030405069999ull);
    emit(rec, n);
    printf("SESSION seq=%u t=%u ver=%u arch=%u ts_bits=%u probe=%u attempted=%u anchor=%llu\n",
           9u, 0x11224000u, (unsigned)TRACE_VERSION, (unsigned)ARCH_SIM, 32u, 7u, 10u,
           (unsigned long long)0x0102030405069999ull);

    fclose(g_bin);
    return 0;
}
