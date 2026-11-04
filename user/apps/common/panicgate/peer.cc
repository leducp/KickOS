// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// panicgate's case 6 task, a program of its own holding no authority: its panic ends its task
// alone with KOS_EXIT_PANIC.

#include <kickos/kos.h>

int main(int, char**)
{
    kos_panic("[panicgate] task message");
    return 1;
}
