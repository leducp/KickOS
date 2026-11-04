// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The intruder of the two clients of the packaged k64dspi service (slots.h).

#include "slots.h"

#include <stdlib.h>

int main(int, char**)
{
    exit(kickos::apps::spi_slots::intruder(PROFILE));
}
