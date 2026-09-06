// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A peer node reporting on ITSELF into the region every node of the partition already
// writes, and the primary printing what it reads.
//
// Three of the five cells carry values the reader already knows, deliberately: a report whose
// known cells read back wrong is an artefact and says so, and a report with no known cells
// cannot tell you which it is. Whoever changes the cell set keeps that property.
//
// The third known cell is the reporting node's own core clock: a partition runs one clock
// tree, so a peer whose figure differs from the primary's is deriving every delay, timeout
// and baud divisor from a number nothing programmed.

#ifndef KICKOS_AMPDIAG_H
#define KICKOS_AMPDIAG_H

namespace kickos
{
    namespace amp
    {
#if defined(KICKOS_AMP_DIAG_REPORT) && KICKOS_AMP_DIAG_REPORT
        // On a node whose index is not 0, before anything else can be published.
        void diag_peer_publish();
        // On node 0, after its peers are released. BOUNDED: a peer that never reports
        // costs a line and not a hang.
        void diag_primary_report();
        // On node 0, from the ordered terminal path, so every crossing the run made is
        // behind it. Reads the doorbell's per-core service counter out of the region the
        // partition shares, the one counter a node can read for a core it does not drive.
        //
        // Its known value is this node's own pair: counts(self).serviced is at least one
        // (window_init drains once, ringing nothing) and never below arch_ipi_counts(self
        // core). A reading that breaks either is an artefact.
        void diag_primary_doorbell_report();
#else
        inline void diag_peer_publish() {}
        inline void diag_primary_report() {}
        inline void diag_primary_doorbell_report() {}
#endif
    }
}

#endif
