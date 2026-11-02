// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include "contributions.h"

#include <cstdio>

extern "C" int kickos_ilk_throw(int value)
{
    try
    {
        throw value;
    }
    catch (int caught)
    {
        return caught;
    }
}

int main(int, char**)
{
    int const sum =
        kickos_ilk_iface() + kickos_ilk_mid() + kickos_ilk_object() + kickos_ilk_throw(8);
    std::printf("[image-link] %d\n", sum);
    return sum;
}
