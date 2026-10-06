// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// XMC4800 console-reclaim test. The composition names the test-owned testusic as stdout over
// USIC0 channel 0; main prints through it, asks it to scramble the channel, which gates the
// channel's clock, and panics. The panic verdict reaching the wire is the kernel reclaiming the
// console out of the published state.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/cap_index.h>
#include <kickos/sys/uart.h>

#include <stdint.h>
#include <stdio.h>

#if !KICKOS_HAVE_MPU
#error "conreclaim requires enforcement: build the board's base variant, not its flat one"
#endif

int main(int, char**)
{
    printf("[conreclaim] writing through the test console driver\n");
    fflush(stdout);

    struct kos_uart_rsp rsp = {};
    int32_t const n = kos_call(KOS_CAP_STDOUT, &rsp, 0u, sizeof(rsp));
    if (n != static_cast<int32_t>(sizeof(rsp)) or rsp.status != 0)
    {
        printf("[conreclaim] ERROR: the scramble request was answered %d, status %d\n",
               static_cast<int>(n), static_cast<int>(rsp.status));
        fflush(stdout);
        return 1;
    }

    // A panic, never a fault: a thread's fault kills it alone and leaves the console with its
    // driver, so nothing would reach the wire. Inside user_panic's 64-byte buffer.
    kos_panic("[conreclaim] PASS: the dead channel came back");
}
