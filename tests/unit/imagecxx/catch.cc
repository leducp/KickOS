// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include "imagecxx.h"

#include <optional>
#include <typeinfo>

int main(int, char**)
{
    imagecxx_note_typeinfo("catch", &typeid(std::bad_optional_access));
    try
    {
        imagecxx_cross();
    }
    catch (std::bad_optional_access const&)
    {
        return 0;
    }
    catch (...)
    {
        return 2;
    }
    return 1;
}
