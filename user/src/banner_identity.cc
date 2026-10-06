// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/sys/banner_identity.h>

#include <kickos/diag.h>
#include <kickos/klink.h>
#include <kickos/libc/fmt.h>

extern "C" char const kickos_identity_build_time[];
extern "C" char const kickos_identity_commit[];
extern "C" char const kickos_app_build_time[] KICKOS_LINK_OPTIONAL;

namespace kickos
{

size_t banner_identity(char* out, size_t cap)
{
    if (cap == 0)
    {
        return 0;
    }
    // Through a volatile, as kbanner reads it: a reference that is not weak folds the test away.
    char const* const volatile app = kickos_app_build_time;
    int n = 0;
    if (app != nullptr)
    {
        n = ksnprintf(out, cap, "\n" KDIAG_F_BANNER_NAME KDIAG_F_BANNER_BOARD KDIAG_F_BANNER_BUILD
                      KDIAG_F_BANNER_APP KDIAG_F_BANNER_COMMIT, KICKOS_VERSION, KICKOS_BOARD_NAME,
                      kickos_identity_build_time, app, kickos_identity_commit);
    }
    else
    {
        n = ksnprintf(out, cap, "\n" KDIAG_F_BANNER_NAME KDIAG_F_BANNER_BOARD KDIAG_F_BANNER_BUILD
                      KDIAG_F_BANNER_COMMIT, KICKOS_VERSION, KICKOS_BOARD_NAME,
                      kickos_identity_build_time, kickos_identity_commit);
    }
    if (n < 0)
    {
        out[0] = '\0';
        return 0;
    }
    if (static_cast<size_t>(n) >= cap)
    {
        return cap - 1;
    }
    return static_cast<size_t>(n);
}

}
