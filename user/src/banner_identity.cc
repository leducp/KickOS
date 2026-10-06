// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/sys/banner_identity.h>

#include <kickos/diag.h>
#include <kickos/klink.h>

extern "C" char const kickos_identity_build_time[];
extern "C" char const kickos_identity_commit[];
extern "C" char const kickos_app_stamp[] KICKOS_LINK_OPTIONAL;

namespace kickos
{

static_assert(sizeof(KICKOS_VERSION) - 1u <= BANNER_IDENTITY_VERSION_MAX,
              "the version is wider than the identity block's title row");
static_assert(sizeof(KICKOS_BOARD_NAME) - 1u <= BANNER_IDENTITY_BOARD_MAX,
              "the board name is wider than the identity block's board row");

namespace
{
    struct Block
    {
        char* out;
        size_t cap;
        size_t at;

        void put(char c)
        {
            if (at + 1u < cap)
            {
                out[at++] = c;
            }
        }

        // `fmt` with its one `%s` replaced by the first `max` characters of `value`.
        void row(char const* fmt, char const* value, size_t max)
        {
            for (char const* f = fmt; *f != '\0'; f++)
            {
                if (f[0] == '%' and f[1] == 's')
                {
                    for (size_t i = 0; i < max and value[i] != '\0'; i++)
                    {
                        put(value[i]);
                    }
                    f++;
                    continue;
                }
                put(*f);
            }
        }
    };
}

size_t banner_identity(char* out, size_t cap)
{
    if (cap == 0)
    {
        return 0;
    }
    // Through a volatile, as kbanner reads it: a reference that is not weak folds the test away.
    char const* const volatile app = kickos_app_stamp;
    Block b{out, cap, 0};
    b.put('\n');
    b.row(KDIAG_F_BANNER_NAME, KICKOS_VERSION, BANNER_IDENTITY_VERSION_MAX);
    b.row(KDIAG_F_BANNER_BOARD, KICKOS_BOARD_NAME, BANNER_IDENTITY_BOARD_MAX);
    b.row(KDIAG_F_BANNER_BUILD, kickos_identity_build_time, BANNER_IDENTITY_TIME_MAX);
    if (app != nullptr)
    {
        b.row(KDIAG_F_BANNER_APP, app, BANNER_IDENTITY_APP_MAX);
    }
    b.row(KDIAG_F_BANNER_COMMIT, kickos_identity_commit, BANNER_IDENTITY_COMMIT_MAX);
    out[b.at] = '\0';
    return b.at;
}

}
