# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Failure-path gate for kickos::driver::bring_up. The header is user-side and calls nothing
# but the eleven extern "C" kos_* syscalls, so a host program drives the real choreography
# over the recording fake in kos_seam.cc: no kernel, no scheduler, no image.
#
# HOST-ONLY, like the uartclass gate and for the same mechanism: kos_seam.cc defines public
# kos_* names, and a target image linking it would satisfy them from the executable and keep
# the real stubs' archive members out of the link (tests/check_class_backend.sh).
#
# cxx_std_20 is set explicitly. The other host gates compile at the compiler default because
# their subjects are C++17-clean; driver_service.h uses designated initialisers in a
# constexpr aggregate and does not parse at gnu++17.
add_executable(drv_bringup bringup_unwind.cc kos_seam.cc)
target_include_directories(drv_bringup PRIVATE
  "${PROJECT_SOURCE_DIR}/user/include" "${PROJECT_SOURCE_DIR}/system/include"
  "${PROJECT_BINARY_DIR}/generated/include")
target_compile_features(drv_bringup PRIVATE ${KICKOS_CXX_STANDARD})
target_compile_options(drv_bringup PRIVATE ${KICKOS_WARN_FLAGS})

# 30 s like the other host gates, and three orders of magnitude of headroom: the readiness
# arm's KOS_DRV_READY_WAIT_MAX sleeps are no-ops here, so the whole suite is sub-millisecond.
add_test(NAME drv_bringup COMMAND "$<TARGET_FILE:drv_bringup>")
set_tests_properties(drv_bringup PROPERTIES TIMEOUT 30 LABELS host)
