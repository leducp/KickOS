# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Failure-path gate for kickos::driver::bring_up. The header is user-side and calls nothing
# but the eleven extern "C" kos_* syscalls, so a host program drives the real choreography
# over the recording fake in kos_seam.cc: no kernel, no scheduler, no image.
#
# HOST-ONLY, like the uartclass gate and for the same mechanism: kos_seam.cc defines public
# kos_* names, and a target image linking it would satisfy them from the executable and keep
# the real stubs' archive members out of the link (tests/static/check_class_backend.sh).
#
# driver_service.h uses designated initialisers in a constexpr aggregate and does not parse at
# gnu++17, so it needs the cxx_std_20 kickos_add_unit_test sets.
kickos_add_unit_test(NAME drv_bringup
  SOURCES bringup_unwind.cc kos_seam.cc
  INCLUDES "${PROJECT_SOURCE_DIR}/user/include" "${PROJECT_SOURCE_DIR}/system/include"
           "${PROJECT_BINARY_DIR}/generated/include")
