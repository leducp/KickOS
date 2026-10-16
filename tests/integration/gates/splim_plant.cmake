# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The stack limits in privileged code, in trees of their own with a test-only plant. Once per
# arch, from its enforcing base build: the plant does not depend on the MPU posture.
if(NOT KICKOS_HAVE_STACK_LIMIT OR KICKOS_SPLIM_PLANT OR NOT KICKOS_MEMORY_ENFORCED
   OR NOT KICKOS_CONFIG_VARIANT STREQUAL "base")
  return()
endif()
kickos_qemu_machine("${KICKOS_BOARD}" _sp_env _sp_machine)
if(_sp_machine STREQUAL "")
  return()
endif()
add_test(NAME ${_tag}_splim_plant
  COMMAND "${CMAKE_COMMAND}" -E env ${_sp_env} QEMU_MACHINE=${_sp_machine}
          "${PROJECT_SOURCE_DIR}/tests/integration/check_splim_plant.sh" "${PROJECT_SOURCE_DIR}"
          "${CMAKE_COMMAND}" "${_fs_preset_key}")
kickos_boot_timeout(${_tag}_splim_plant
  "${PROJECT_SOURCE_DIR}/tests/integration/check_splim_plant.sh" BOOTS 4 WORK 900)
