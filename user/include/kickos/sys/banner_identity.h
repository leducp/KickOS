// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The boot banner's identity rows, title, board, build, app and commit, for a console the
// kernel's banner never reaches. They are the rows kbanner (kernel/init/kmain.cc) prints, in its
// formats (kickos/diag.h), from the same version, board and app stamp, and the build time and
// commit the same stamp carries. An image with no app stamp prints no app row, as kbanner does.

#ifndef KICKOS_SYS_BANNER_IDENTITY_H
#define KICKOS_SYS_BANNER_IDENTITY_H

#include <stddef.h>

namespace kickos
{

// The rows after a newline, NUL-terminated, cut short where `cap` is too small. Returns the
// length written.
size_t banner_identity(char* out, size_t cap);

}

#endif
