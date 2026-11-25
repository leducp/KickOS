// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Node 0 of the ampcon partition: its relay task carries node 1's standard output.

#include <stdio.h>

int main(void)
{
    printf("ampcon: node 0 relays node 1's standard output\n");
    return 0;
}
