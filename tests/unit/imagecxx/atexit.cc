// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The __cxa_atexit handlers two localized instances register run at host exit in the reverse
// of their registration, each in its own instance.

#include "imagecxx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace
{
    char const* s_order[3] = {};
    int s_count = 0;

    void verify(void)
    {
        int rc = 1;
        if (s_count == 2 and strcmp(s_order[0], "b") == 0 and strcmp(s_order[1], "a") == 0)
        {
            printf("PASS: b's handler, then a's\n");
            rc = 0;
        }
        else
        {
            printf("FAIL: %d handler(s) ran:", s_count);
            for (int i = 0; i < s_count; i++)
            {
                printf(" %s", s_order[i]);
            }
            printf(", not b then a\n");
        }
        fflush(stdout);
        _exit(rc);
    }
}

extern "C" void imagecxx_note_exit(char const* instance)
{
    if (s_count < 3)
    {
        s_order[s_count] = instance;
    }
    s_count++;
}

int main()
{
    // Registered first, so it runs after both instances' handlers.
    atexit(verify);
    // Read by the handlers after main returns.
    static char a[] = "a";
    static char b[] = "b";
    char* argv_a[] = {a, nullptr};
    char* argv_b[] = {b, nullptr};
    imagecxx_exit_a_main(1, argv_a);
    imagecxx_exit_b_main(1, argv_b);
    return 0;
}
