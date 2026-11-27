// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// drvdeath's faulter, a program of its own: the kernel's record of its fault reaches the wire
// only on a console the kernel took back, held for a driver that no longer receives until the
// reclaim writes it, or written by the kernel once reclaimed.

int main(int, char**)
{
    __builtin_trap();
}
