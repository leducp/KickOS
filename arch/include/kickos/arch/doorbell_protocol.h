// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The doorbell protocol a shared-kernel backend takes whole rather than spelling for itself:
// the cells, the raise over them, the rendezvous wait, and the bring-up check of the coupling
// between the kernel lock and the doorbell.
//
// A BACKEND SUPPLIES TWO THINGS. What is the part's, through <kickos/arch/doorbell_part.h>: the
// padding a cell owes, the spin a wait costs, the console a refusal reaches and the wording of
// each refusal. And the bodies below, which the protocol calls but cannot own.

#ifndef KICKOS_ARCH_DOORBELL_PROTOCOL_H
#define KICKOS_ARCH_DOORBELL_PROTOCOL_H

#include <kickos/arch/doorbell_cells.h>

#include <stdint.h>

// ONE guard over everything below, the part header included. This file ships from the
// always-installed root while <kickos/arch/doorbell_part.h> ships from
// ${KICKOS_ARCH_INCLUDE_DIR} for the CONFIGURED arch alone, so an image with no doorbell
// installs this header beside no part header at all and must still compile standalone
// (tests/integration/check_oot_export.sh). A second #if agreeing with this one by hand is the
// shape that gate cannot see going wrong: it would compile standalone and break the build of
// any posture the two conditions disagree about.
#if (KICKOS_NUM_CORES > 1 || KICKOS_AMP_NODE)

#include <kickos/arch/doorbell_part.h>

namespace kickos::doorbell
{
    using PartRow = Row<KICKOS_DOORBELL_LINE>;
    using PartCell = Cell<KICKOS_DOORBELL_LINE>;

    static_assert(sizeof(PartRow) % KICKOS_DOORBELL_LINE == 0,
                  "a row shorter than a line would share one with the next writer");

    // Both nodes write these, so under one image per node they sit in the region the two link
    // scripts agree on rather than being allocated per image.
    //
    // g_request[i].seq[t]: how many times core i has asked core t. Written by i, read by t.
    // g_answer[t].seq[i]: how far core t has answered core i. Written by t, read by i.
    extern PartRow g_request[KICKOS_DOORBELL_CORES];
    extern PartRow g_answer[KICKOS_DOORBELL_CORES];

    // Whether any peer has asked this core for something it has not answered.
    bool pending(void);

    void hex1(uint32_t v);

#if KICKOS_NUM_CORES > 1
    // What the bring-up check asks of a parked secondary. 0 parks it in its sleep instruction;
    // 1 spins it with its interrupts open; 2 makes it take the lock under its own mask.
    //
    // 1 EXISTS TO GET THE PEERS OUT OF THAT SLEEP: a peer asleep there observes no store, so a
    // step straight to 2 would wait on cores that never read it. The first raise of the 1 phase
    // is what wakes them, and it is a phase whose witness is the vector taking that raise.
    constexpr uint32_t CONTEND_PARK = 0u;
    constexpr uint32_t CONTEND_OPEN = 1u;
    constexpr uint32_t CONTEND_LOCK = 2u;
    extern Seq g_contend;

    // g_held[i]: how many kernel-lock acquisitions core i has completed.
    //
    // g_spinning[i]: nonzero while core i is between publishing intent and releasing the kernel
    // lock, its interrupts masked throughout. A READER HOLDING THE LOCK can conclude the core
    // is in the acquire loop and cannot leave it, which is what makes the overlap the check
    // needs an arrival to wait for rather than a race to win.
    extern PartCell g_held[KICKOS_NUM_CORES];
    extern PartCell g_spinning[KICKOS_NUM_CORES];
#endif
}

extern "C"
{

// The far side of the doorbell run from a spin rather than from the vector: drops this part's
// pending state, runs the backend's service body, and raises again whatever the same cause
// still owes. MASKED THROUGHOUT, the service body not being re-entrant against itself.
void kickos_doorbell_poll(void);

// The part's lowering of the raise, over every core named in `cores`. The CALLING core's bit
// never reaches here: arch_ipi_send services it inline.
void kickos_doorbell_raise(uint32_t cores);

#if KICKOS_NUM_CORES > 1
// The primary's one-shot bring-up check of the mechanism the kernel is about to depend on, run
// once with every secondary parked. Fatal on failure.
void kickos_doorbell_selfcheck(void);
#endif

}

#endif

#endif
