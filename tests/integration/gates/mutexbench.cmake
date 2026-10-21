# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

if(NOT TARGET mutexbench)
  return()
endif()
kickos_unbooted(mutexbench "a measurement whose rows are read, not judged")
