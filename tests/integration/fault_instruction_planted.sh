#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# An objdump for check_fault_instruction.sh's negative control: a main that names `wait` only as
# a symbol and an operand, and executes no such instruction.
#
#   fault_instruction_planted.sh -d <image>

printf 'ffc00400 <_kickos_app_main>:\n'
printf 'ffc00400:\t05 6a ac 00                   \tbsr.a\tffc0b070 <_wait>\n'
printf 'ffc00404:\tfb 12 da d4 c0 ff             \tmov.l\t#0xffc0d4da, r1 ; wait\n'
printf 'ffc0040a:\t02                            \trts\n'
printf 'ffc0040c <_next>:\n'
