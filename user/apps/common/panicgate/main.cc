// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// kos_panic wire gate, in its own binary because the panic ends the system. One source,
// five images selected by KICKOS_PANICGATE_CASE, because only one panic can be observed
// per run:
//   1  a readable message must reach the wire after the kernel's trusted banner
//   2  a null pointer must reach the documented fallback text
//   3  a pointer in no region this thread holds and outside every linker-defined extent
//      must reach the SAME fallback text, and must not fault the kernel: the copy runs
//      privileged, so admitting it would take the fault inside kaccess_from_user
//   4  a message longer than the kernel buffer must appear truncated WITH A VISIBLE
//      marker, and with the tail absent
//   5  control bytes in the message must be replaced in place, so the panic cannot
//      continue onto lines that read as kernel output
//
// Case 3 discriminates only while root is UNPRIVILEGED: a privileged caller passes
// user_readable_ok wholesale.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/emit.h> // publish-aware write (kos_print is dropped once published)

#ifndef KICKOS_PANICGATE_CASE
#error "KICKOS_PANICGATE_CASE must be 1, 2, 3, 4 or 5"
#endif

using kickos::emit;

namespace
{
    // Mapped on no board in the fleet: below every RAM origin the ports use
    // (0x20000000 armv7m, 0x38000000 an505, 0x40800000 esp32c6, 0x80000000 virt) and
    // above every flash/ROM extent. A read here BUSFAULTS rather than returning junk,
    // which is what makes the case a gate and not a formality.
    constexpr uintptr_t WILD = 0xCCCCCCC0u;

    // The kernel buffer is 64 including the terminator, so 63 bytes survive, and the
    // last 3 of those carry the truncation marker: 60 kept bytes then "...". "WXYCUTME"
    // must not reach the wire: WXY because the marker overwrote it, CUTME because it was
    // past the buffer.
    constexpr char const* LONG_MSG =
        "[panicgate] abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYCUTME";
    static_assert(sizeof("[panicgate] abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUV")
                      == 61,
                  "the kept prefix is the kernel buffer's 63 bytes less the 3-byte marker");

    // LF, TAB, CR and DEL inside an otherwise printable message. Split from the
    // neighbouring literals so the \x7f escape cannot absorb a following hex digit.
    constexpr char const* CTL_MSG = "[panicgate] ctl" "\n\t\r\x7f" " end";
}

int main(int, char**)
{
#if KICKOS_PANICGATE_CASE == 1
    emit("[panicgate] case 1: readable message\n");
    kos_panic("[panicgate] message on the wire");
#elif KICKOS_PANICGATE_CASE == 2
    emit("[panicgate] case 2: null message\n");
    kos_panic(nullptr);
#elif KICKOS_PANICGATE_CASE == 3
    emit("[panicgate] case 3: unreadable message\n");
    kos_panic(reinterpret_cast<char const*>(WILD));
#elif KICKOS_PANICGATE_CASE == 4
    emit("[panicgate] case 4: oversized message\n");
    kos_panic(LONG_MSG);
#else
    emit("[panicgate] case 5: control bytes in the message\n");
    kos_panic(CTL_MSG);
#endif
    emit("[panicgate] ERROR: kos_panic returned\n");
    return 1;
}
