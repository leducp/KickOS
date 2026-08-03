// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The handover fires on a TIMEOUT rather than at the end of the run: a board whose image
// is otherwise misbehaving still comes back for a reflash without a button press.
//
// Root makes the call; selftest's reboot_priv owns the unprivileged refusal arm.
//
// On a chip with a backend (rp2040 -> PICOBOOT/UF2, rp2350 -> BOOTSEL, imxrt1062 ->
// HalfKay) the countdown is the last line printed and the board reappears as a flashing
// device. Everywhere else the fallback declines and the rc line is the verdict.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h> // ksnprintf: report the refusal rc
#include <kickos/sys/emit.h> // publish-aware write (kos_print is dropped once published)

using kickos::emit;

namespace
{
    constexpr uint64_t ARM_NS = 3000000000ull; // 3 s
}

int main(int, char**)
{
    emit("[rebootdemo] KickOS reboot-to-bootloader demo\n");
    emit("[rebootdemo] handing the chip to its bootloader in 3s\n");
    kos::sleep_ns(ARM_NS);

    int const rc = kos_reboot();

    // Reached only where the chip declined: a backend that took the call never returns.
    char msg[80];
    ksnprintf(msg, sizeof(msg), "[rebootdemo] reboot declined: rc=%d\n", rc);
    emit(msg);
    kos_shutdown(0);
    return 0;
}
