// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// drvdeath's faulter, a program of its own: its record is held for a driver that no longer
// receives, so only the reclaim writes it, and it writes it before it wakes anyone.

int main(int, char**)
{
    __builtin_trap();
}
