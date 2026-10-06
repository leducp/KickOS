// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The boot banner's identity rows, title, board and commit, for a console the kernel's banner
// never reaches. They are the rows kbanner (kernel/init/kmain.cc) prints, in its formats
// (kickos/diag.h), from the same version and board and the commit the same stamp carries.

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
