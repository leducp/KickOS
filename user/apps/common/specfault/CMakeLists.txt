# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# F1 guard for the i.MX RT1062 M7 speculative-access fix (ERR011573): a deliberate read
# of the unbacked, wrapped FlexSPI window. On the fixed Teensy it produces a clean
# MemManage dump; a regression to a readable wrap (fable finding F1) would hang the core.
# Diagnostic app (own binary -- ends the process); built under KICKOS_ENABLE_SELFTEST.
# Silicon one-shot: the sim/qemu gates have no wrapped external aperture, so no ctest.
kickos_add_diagnostic_app(specfault
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
