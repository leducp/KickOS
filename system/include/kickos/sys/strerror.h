// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Names a kos_errno code. Its table lives in lib/strerror.cc rather than here, so the
// archive member stays unextracted and the prose costs nothing where it is unused.
//
// Lives in the kickos_system library alongside errno.h; keep it dependency-free.

#ifndef KICKOS_SYS_STRERROR_H
#define KICKOS_SYS_STRERROR_H

#ifdef __cplusplus
extern "C"
{
#endif

    // Takes either form a caller has in hand: the NEGATED code a syscall returns, or the
    // bare magnitude. Returns a static string, never null; a code outside the taxonomy
    // names itself as unknown rather than being reported as a neighbour.
    char const* kos_strerror(int rc);

#ifdef __cplusplus
}
#endif

#endif
