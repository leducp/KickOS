// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The peer's exit status, which main reports.

#ifndef KICKOS_USER_APPS_COMMON_TLSPROBE_TLSPROBE_H
#define KICKOS_USER_APPS_COMMON_TLSPROBE_TLSPROBE_H

enum
{
    TLSPROBE_PEER_OK = 0,
    TLSPROBE_PEER_TEMPLATE = 1,  // an initialised thread_local missed its image's value
    TLSPROBE_PEER_ZERO = 2,      // a zero-initialised one did not read zero
    TLSPROBE_PEER_READ_BACK = 3  // one read back other than what the thread wrote
};

#endif
