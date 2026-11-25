# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# amprelay: the program an AMP node's composition runs to carry another node's standard output,
# that node's composition naming the port it serves as its `stdout: /amp/N`.
add_executable(amprelay amprelay.c)
target_link_libraries(amprelay PRIVATE kickos)
kickos_program_link(amprelay)
