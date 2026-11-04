# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# user/src/gthr.cc's timed waits and thread ids, with the kernel's entries and both clocks
# stubbed. toolchain/ stands in for the toolchain's bits/gthr.h and reaches gthr.cc alone:
# on the test's own sources it would shadow the host's threads model under gtest.
kickos_add_unit_test(NAME gthr_timed
  SOURCES gthr_timed.cc "${PROJECT_SOURCE_DIR}/user/src/gthr.cc"
          "${PROJECT_SOURCE_DIR}/user/src/arch/sim/user_cas.cc"
  INCLUDES "${PROJECT_SOURCE_DIR}/user/include" "${PROJECT_SOURCE_DIR}/system/include"
           "${PROJECT_BINARY_DIR}/generated/include"
  POSTURE KICKOS_LIBC_REENT=1)
set_source_files_properties("${PROJECT_SOURCE_DIR}/user/src/gthr.cc"
  PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/toolchain")
target_link_options(gthr_timed PRIVATE -Wl,--wrap=clock_gettime)

# An overflow in the deadline's arithmetic is undefined, and only UBSan's check fails on it.
# Optional, as tests/unit/kstring's alignment check.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS
  "-fsanitize=signed-integer-overflow -fno-sanitize-recover=signed-integer-overflow")
check_cxx_source_compiles("int main(){return 0;}" KICKOS_HAVE_UBSAN_SIGNED_OVERFLOW)
unset(CMAKE_REQUIRED_FLAGS)
if(KICKOS_HAVE_UBSAN_SIGNED_OVERFLOW)
  target_compile_options(gthr_timed PRIVATE
    -fsanitize=signed-integer-overflow -fno-sanitize-recover=signed-integer-overflow)
  target_link_options(gthr_timed PRIVATE
    -fsanitize=signed-integer-overflow -fno-sanitize-recover=signed-integer-overflow)
else()
  message(STATUS "KickOS: no UBSan signed-overflow check; gthr_timed tests values only")
endif()
