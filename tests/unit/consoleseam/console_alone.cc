// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What kernel/init/console.cc leaves undefined when it is compiled WITHOUT console_tx.cc and
// without a kernel behind it, apart from what a gate records and console_leaves.cc answers: the
// device writes, the ring's arm, the dark wait, the console's server and the current thread.

#include <kickos/arch/arch.h>
#include <kickos/console_tx.h>
#include <kickos/kernel.h>
#include <kickos/sys/errno.h>

extern "C"
{
    int arch_in_isr(void) { return 0; }

    void arch_console_flush_sync(void) {}

    int console_tx_insert_record_line(char const*, size_t, int) { return 0; }
    void console_tx_flush_sync(void) {}
    void console_tx_disarm(void) {}
    int console_held_append(uint32_t, char const*, uint32_t, uint32_t) { return 0; }
    int console_held_commit(uint32_t) { return 0; }
    void console_held_abandon(uint32_t) {}
    void console_held_write_sync(void) {}
    void console_held_clear(void) {}
    uint32_t console_held_ready(void) { return 0; }
    char const* console_held_data(void) { return nullptr; }
    void console_held_take(uint32_t) {}
}

namespace kickos
{
    bool task_serves_console(Task const*) { return false; }

    // No ring to wait on: the write ends.
    int console_room_wait(char const*, size_t, int) { return -KOS_ECANCELED; }

    void cap_console_deliver(Held) {}
}
