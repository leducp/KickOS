// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// An image whose first reference to a mem*/str* name is memset's, linked with the libc named ahead
// of KickOS (tests/integration/gates/string_provider.cmake).

#include <kickos/sys.h>

#include <string.h>

namespace
{
    char line[16];
}

int main(int, char**)
{
    memset(line, '-', sizeof(line) - 1);
    kos_print(line);
    return 0;
}
