# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Every image this configure emits takes each mem*/str* function from one provider, read from
# the link maps kickos-image-maps.txt lists (the root CMakeLists.txt writes it). Where the kernel
# calls them under the app's names (kickos/kruntime.h) the provider is the kickos_string object;
# elsewhere the kernel has its own copies and the app's are the libc's.

if(NOT KICKOS_ARCH STREQUAL "sim")
  set(_string_provider kickos)
  if(KICKOS_HAVE_ASPACE)
    set(_string_provider libc)
  endif()
  add_test(NAME ${_tag}_images_string_provider
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_string_provider.sh"
            "${CMAKE_NM}" ${_string_provider}
            --images "${PROJECT_BINARY_DIR}/kickos-image-maps.txt")
  kickos_host_gate(${_tag}_images_string_provider TIMEOUT 300)
endif()

# An image naming the libc ahead of KickOS, whose first mem*/str* reference is memset's, so the
# libc answers it before any KickOS archive is read. It links only where no KickOS archive offers
# the name too.
if(KICKOS_ARCH STREQUAL "x86_64")
  add_executable(string_provider_libc_first
    "${CMAKE_CURRENT_SOURCE_DIR}/string_provider/libc_first.cc")
  # Without it the compiler stores the bytes inline and references no memset.
  target_compile_options(string_provider_libc_first PRIVATE -fno-builtin)
  target_link_libraries(string_provider_libc_first PRIVATE c KickOS::kernel KickOS::system_default)
  set(_libc_first_maps "${PROJECT_BINARY_DIR}/kickos-libc-first-maps.txt")
  file(GENERATE OUTPUT "${_libc_first_maps}"
       CONTENT "$<TARGET_FILE:string_provider_libc_first>.map\n")
  add_test(NAME ${_tag}_libc_first_string_provider
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_string_provider.sh"
            "${CMAKE_NM}" libc --images "${_libc_first_maps}")
  kickos_host_gate(${_tag}_libc_first_string_provider TIMEOUT 120)
endif()
