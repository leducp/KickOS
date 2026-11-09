// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// KOS_SYS_WITNESS_NODE: a stray line raised on the calling core, and a ring-0 read of a word,
// made on purpose. Built only into the node witness's own kernel (<kickos/sys/witness.h>), an
// own-image AMP node's.

#include <kickos/arch/arch.h>
#include <kickos/irq.h>
#include <kickos/irq_route.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>

#include <kickos/poll_bound.h>
#include <kickos/sys/abi.h>
#include <kickos/sys/witness.h>

#include "syscall_internal.h"

#if not KICKOS_WITNESS_NODE
#error "syscall_witness_node.cc is the node witness's alone"
#endif

namespace kickos
{
    namespace
    {
        void bound_nothing(void*) {}

        int64_t raise_stray(int line)
        {
            int const admit = irq_line_admit(line);
            if (admit != 0)
            {
                return admit;
            }
            // The attach is the test that nothing holds the line; its detach leaves it masked.
            if (not irq_attach(line, bound_nothing, nullptr))
            {
                return -KOS_EBUSY;
            }
            irq_detach(line);
            uint32_t const before = irq_spurious_count();
            {
                IrqLock lock;
                irq_line_op(line, LineOp::UNMASK, lock);
                if (not irq_inject(line, lock))
                {
                    irq_line_op(line, LineOp::MASK, lock);
                    return -KOS_ENOSYS;
                }
            }
            uint32_t moved = 0;
            for (uint32_t spin = 0; spin < KICKOS_POLL_SPIN_MAX and moved == 0; spin++)
            {
                moved = irq_spurious_count() - before;
            }
            return static_cast<int64_t>(moved);
        }

        // The address is announced first, so the judge can hold the fault's address against it.
        __attribute__((noinline)) uint32_t plant_read(uintptr_t at)
        {
            kprintf("[node] node %u on core %u: ring 0 reads 0x%x\n",
                    static_cast<unsigned>(KICKOS_AMP_NODE_ID),
                    static_cast<unsigned>(KICKOS_AMP_SELF_CORE), static_cast<unsigned>(at));
            return *reinterpret_cast<uint32_t const volatile*>(at);
        }
    }

    int64_t witness_node(uintptr_t op, uintptr_t arg)
    {
        switch (op)
        {
            case KOS_WITNESS_NODE_STRAY:
            {
                if (arg >= static_cast<uintptr_t>(KICKOS_MAX_IRQ))
                {
                    return -KOS_EINVAL;
                }
                return raise_stray(static_cast<int>(arg));
            }
            case KOS_WITNESS_NODE_READ:
            {
                return plant_read(arg);
            }
            default:
            {
                return -KOS_EINVAL;
            }
        }
    }
}
