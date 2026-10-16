# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# A holder's death closes each block its device windows name, through arch_periph_disable, while
# it still holds them. Host-only: the order against a second holder's admission has no on-target
# observable, and the bus gate it closes is silicon's (frdmk64f's bench pass).
kickos_add_unit_test(NAME window_close SOURCES window_close.cc
  LIBRARIES kickos_kseam)
