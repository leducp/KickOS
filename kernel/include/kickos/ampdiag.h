// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A peer node reporting on ITSELF into the region every node already writes, and the primary
// printing what it reads.
//
// Three of the five cells carry values the reader already knows, deliberately: a report whose
// known cells read back wrong is an artefact and says so. Whoever changes the cell set keeps
// that property.
//
// The third known cell is the node's own core clock: a partition runs one clock tree, so a
// peer whose figure differs from the primary's derives every delay, timeout and baud divisor
// from a number nothing programmed.

#ifndef KICKOS_AMPDIAG_H
#define KICKOS_AMPDIAG_H

namespace kickos
{
    namespace amp
    {
#if defined(KICKOS_AMP_DIAG_REPORT) && KICKOS_AMP_DIAG_REPORT
        void diag_peer_publish();
        // BOUNDED: a peer that never reports costs a line and not a hang.
        void diag_primary_report();
#else
        inline void diag_peer_publish() {}
        inline void diag_primary_report() {}
#endif
    }
}

#endif
