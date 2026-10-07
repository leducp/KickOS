# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Admission gate for the S7 non-cacheable memory type, over the real kernel/grant/grant.cc and
# kernel/domain/domain.cc. The include path and definitions come from the kernel target
# VERBATIM: both must compile exactly as the shipping build compiles them. That build sets
# KICKOS_HAVE_MPU=1; the enforcement-off posture is a header inline and not this gate's subject.
# What no case reaches is dropped at link, so the seam carries only what the cases call.
kickos_add_unit_test(NAME grant_nocache
  SOURCES nocache_admission.cc
          "${PROJECT_SOURCE_DIR}/kernel/grant/grant.cc"
          "${PROJECT_SOURCE_DIR}/kernel/domain/domain.cc"
          "${PROJECT_SOURCE_DIR}/kernel/init/instance.cc"
  INCLUDES $<TARGET_PROPERTY:kickos_kernel,INCLUDE_DIRECTORIES>
  DEFINITIONS $<TARGET_PROPERTY:kickos_kernel,COMPILE_DEFINITIONS>)
target_compile_options(grant_nocache PRIVATE -ffunction-sections -fdata-sections)
target_link_options(grant_nocache PRIVATE -Wl,--gc-sections)
