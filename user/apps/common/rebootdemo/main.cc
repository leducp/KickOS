// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The handover fires on a TIMEOUT, so a board whose image is otherwise misbehaving still
// comes back for a reflash without a button press.
//
// On a chip with a bootloader entry (rp2040 -> PICOBOOT/UF2, rp2350 -> BOOTSEL, imxrt1062 ->
// HalfKay) the countdown is the last line printed and the board reappears as a flashing
// device. Otherwise the chip declines, the rc line is the verdict, and the image asks for a
// plain reset: a chip with one boots the image again, which asks again.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>

namespace
{
    constexpr uint64_t ARM_NS = 3000000000ull; // 3 s
}

int main(int, char**)
{
    kos::print("[rebootdemo] KickOS reboot-to-bootloader demo\n");
    kos::print("[rebootdemo] handing the chip to its bootloader in 3s\n");
    kos::sleep_ns(ARM_NS);

    int const rc = kos_shutdown(0, KOS_SHUTDOWN_BOOTLOADER);

    char msg[80];
    ksnprintf(msg, sizeof(msg), "[rebootdemo] reboot declined: rc=%d\n", rc);
    kos::print(msg);

    int const reset = kos_shutdown(0, KOS_SHUTDOWN_REBOOT);
    ksnprintf(msg, sizeof(msg), "[rebootdemo] reset declined: rc=%d\n", reset);
    kos::print(msg);
    return 0;
}
