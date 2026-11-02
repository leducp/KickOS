// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_USER_APPS_COMMON_CXXTEST_PEER_H
#define KICKOS_USER_APPS_COMMON_CXXTEST_PEER_H

// The peer's exit status: 0, or the checks that failed.
enum
{
    CXXTEST_PEER_CTOR_BAD = 1,
    CXXTEST_PEER_CAUGHT_BAD = 2
};

#endif
