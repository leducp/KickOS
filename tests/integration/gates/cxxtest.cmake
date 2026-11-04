# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The full-C++ smoke gate riding `cxxtest`.

if(NOT TARGET cxxtest)
  return()
endif()
kickos_app_judge(cxxtest tests/integration/check_qemu_cxxtest.sh)

# One call for every board: the boards that build this app at all are enumerated in
# user/apps/common/CMakeLists.txt, and every emulatable one of them boots the image through
# this same script and reports under the derived <board tag>_cxxtest name.
kickos_add_qemu_test(TARGET cxxtest
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_cxxtest.sh")

# The same image on the x86_64 model without RDSEED, where getentropy has no source.
if(KICKOS_ARCH STREQUAL "x86_64")
  kickos_qemu_machine("${KICKOS_BOARD}" _cx_env _cx_machine)
  if(_cx_machine STREQUAL "")
    return()
  endif()
  if(NOT KICKOS_X86_64_QEMU_CPU MATCHES ",\\+rdseed(,|$)")
    message(FATAL_ERROR "KickOS: KICKOS_X86_64_QEMU_CPU offers no RDSEED to take away")
  endif()
  list(TRANSFORM _cx_env REPLACE ",\\+rdseed(,|$)" "\\1")
  add_test(NAME ${_tag}_cxxtest_no_rdseed
    COMMAND "${CMAKE_COMMAND}" -E env ${_cx_env} QEMU_MACHINE=${_cx_machine}
            "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_cxxtest.sh"
            "$<TARGET_FILE:cxxtest>")
  set_tests_properties(${_tag}_cxxtest_no_rdseed PROPERTIES SKIP_RETURN_CODE 77)
  kickos_boot_timeout(${_tag}_cxxtest_no_rdseed
    "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_cxxtest.sh")
endif()
