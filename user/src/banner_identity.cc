// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/sys/banner_identity.h>

#include <kickos/diag.h>
#include <kickos/libc/fmt.h>

extern "C" char const kickos_identity_commit[];

namespace kickos
{

size_t banner_identity(char* out, size_t cap)
{
    if (cap == 0)
    {
        return 0;
    }
    int const n = ksnprintf(out, cap, "\n" KDIAG_F_BANNER_NAME KDIAG_F_BANNER_BOARD
                            KDIAG_F_BANNER_COMMIT, KICKOS_VERSION, KICKOS_BOARD_NAME,
                            kickos_identity_commit);
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
