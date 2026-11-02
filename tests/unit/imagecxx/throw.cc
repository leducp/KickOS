// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include "imagecxx.h"

#include <optional>
#include <typeinfo>

int main(int, char**)
{
    imagecxx_note_typeinfo("throw", &typeid(std::bad_optional_access));
    throw std::bad_optional_access();
}
