// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include "imagecxx.h"

namespace
{
    struct ExitNote
    {
        ~ExitNote()
        {
            imagecxx_note_exit(imagecxx_instance);
        }
    };
}

int main(int, char** argv)
{
    imagecxx_instance = argv[0];
    // Registered through __cxa_atexit at this first call.
    static ExitNote note;
    (void)note;
    return 0;
}
