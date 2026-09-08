# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# F1 guard for the i.MX RT1062 M7 speculative-access fix (ERR011573): a deliberate read
# of the unbacked, wrapped FlexSPI window. On the fixed Teensy it produces a clean
# MemManage dump; a regression to a readable wrap (fable finding F1) would hang the core.
# Silicon one-shot: the sim/qemu gates have no wrapped external aperture, so no ctest.
kickos_add_app_target(specfault main.cc)
target_link_libraries(specfault PRIVATE kickos)
kickos_emit_image(specfault)
