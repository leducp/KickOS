// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A std:: exception thrown in one localized instance is caught in another. Each instance holds
// its own copy of the exception's typeinfo, which this GCC's runtime matches by name.

#include "imagecxx.h"

#include <stdio.h>

namespace
{
    void const* s_throw_typeinfo = nullptr;
    void const* s_catch_typeinfo = nullptr;
}

extern "C" void imagecxx_note_typeinfo(char const* instance, void const* typeinfo)
{
    if (instance[0] == 't')
    {
        s_throw_typeinfo = typeinfo;
    }
    else
    {
        s_catch_typeinfo = typeinfo;
    }
}

extern "C" void imagecxx_cross(void)
{
    char name[] = "throw";
    char* argv[] = {name, nullptr};
    imagecxx_throw_main(1, argv);
}

int main()
{
    char name[] = "catch";
    char* argv[] = {name, nullptr};
    int const rc = imagecxx_catch_main(1, argv);
    if (s_throw_typeinfo == nullptr or s_throw_typeinfo == s_catch_typeinfo)
    {
        printf("FAIL: the two instances share one typeinfo, so no name comparison was made\n");
        return 1;
    }
    if (rc != 0)
    {
        printf("FAIL: the catching instance returned %d, its handler missed the throw\n", rc);
        return 1;
    }
    printf("PASS: caught across instances, each with its own typeinfo\n");
    return 0;
}
