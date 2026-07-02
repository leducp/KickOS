// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// sim startup glue: the host process entry. Brings up the arch backend then
// enters the kernel. On MCU targets the equivalent is the reset handler.

#include <kickos/arch/arch.h>

namespace kickos { int kmain(); }

int main(int, char**) {
  arch_init();
  return kickos::kmain();
}
