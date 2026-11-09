# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# CCR inverted from what bring-up writes, in trees of their own with a test-only plant. From each
# armv7m machine's enforcing base build, where main runs unprivileged.
if(NOT KICKOS_ARCH STREQUAL "armv7m" OR KICKOS_CCR_PLANT OR NOT KICKOS_MEMORY_ENFORCED
   OR NOT KICKOS_CONFIG_VARIANT STREQUAL "base")
  return()
endif()
kickos_qemu_machine("${KICKOS_BOARD}" _ccr_env _ccr_machine)
if(_ccr_machine STREQUAL "")
  return()
endif()
add_test(NAME ${_tag}_ccr_plant
  COMMAND "${CMAKE_COMMAND}" -E env ${_ccr_env} QEMU_MACHINE=${_ccr_machine}
          "${PROJECT_SOURCE_DIR}/tests/integration/check_ccr_plant.sh" "${PROJECT_SOURCE_DIR}"
          "${CMAKE_COMMAND}" "${_fs_preset_key}")
kickos_boot_timeout(${_tag}_ccr_plant
  "${PROJECT_SOURCE_DIR}/tests/integration/check_ccr_plant.sh" BOOTS 6 WORK 900)
