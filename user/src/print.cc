// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/sys.h>
#include <kickos/sys/emit.h>

#include <string.h>

void kos_print(char const* s)
{
    (void)kickos::stdout_write(s, strlen(s));
}
