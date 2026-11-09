// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The boot banner's identity rows, title, board, build, app and commit, for a console the
// kernel's banner never reaches. They are the rows kbanner (kernel/init/kmain.cc) prints, in its
// formats (kickos/diag.h), from the same version, board and app stamp, and the build time and
// commit the same stamp carries. An image with no app stamp prints no app row, as kbanner does.

#ifndef KICKOS_SYS_BANNER_IDENTITY_H
#define KICKOS_SYS_BANNER_IDENTITY_H

#include <kickos/diag.h>

#include <stddef.h>

// The board's CRLF posture (kickos/sys/console_ring.h): the rows go out under it as the ring's
// bytes do, each '\n' as "\r\n".
#ifndef KICKOS_CONSOLE_CRLF
#define KICKOS_CONSOLE_CRLF 0
#endif

namespace kickos
{

// The widest value each row renders. The version and board are refused past theirs at build;
// the build time, the app stamp and the commit label are cut to theirs. The app stamp is its
// image's name and a 27-character time, so an image name of up to 45 characters renders whole.
constexpr size_t BANNER_IDENTITY_VERSION_MAX = 16;
constexpr size_t BANNER_IDENTITY_BOARD_MAX = 32;
constexpr size_t BANNER_IDENTITY_TIME_MAX = 25;
constexpr size_t BANNER_IDENTITY_APP_MAX = 72;
constexpr size_t BANNER_IDENTITY_COMMIT_MAX = 48;

// The characters of a row format other than its `%s`.
constexpr size_t banner_identity_text(char const* f)
{
    size_t n = 0;
    while (*f != '\0')
    {
        if (f[0] == '%' and f[1] == 's')
        {
            f += 2;
            continue;
        }
        n++;
        f++;
    }
    return n;
}

// The CRs the CRLF posture adds to a text: one per newline, or none.
constexpr size_t banner_identity_crs(char const* f)
{
    size_t n = 0;
    if (KICKOS_CONSOLE_CRLF == 0)
    {
        return n;
    }
    while (*f != '\0')
    {
        if (*f == '\n')
        {
            n++;
        }
        f++;
    }
    return n;
}

// The leading newline and every row's format.
constexpr char BANNER_IDENTITY_FORMATS[] = "\n" KDIAG_F_BANNER_NAME KDIAG_F_BANNER_BOARD
    KDIAG_F_BANNER_BUILD KDIAG_F_BANNER_APP KDIAG_F_BANNER_COMMIT;

// The longest block banner_identity renders, its leading newline and its NUL included.
constexpr size_t BANNER_IDENTITY_MAX
    = banner_identity_text(BANNER_IDENTITY_FORMATS)
      + banner_identity_crs(BANNER_IDENTITY_FORMATS) + BANNER_IDENTITY_VERSION_MAX
      + BANNER_IDENTITY_BOARD_MAX + BANNER_IDENTITY_TIME_MAX + BANNER_IDENTITY_APP_MAX
      + BANNER_IDENTITY_COMMIT_MAX + 1u;

// The rows after a newline, NUL-terminated, cut short where `cap` is too small. Returns the
// length written.
size_t banner_identity(char* out, size_t cap);

}

#endif
