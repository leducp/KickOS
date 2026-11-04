# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The consumer behind gate.sh's package_names(): it finds an installed KickOS package and
# refuses unless the leaf a user links is exported as KickOS::kernel, every archive group member
# is a KickOS:: name or a toolchain library, the kernel archive among the kernel group's as
# KickOS::kernel_archive, and the leaf requires a system target and defines no heap of its own.
# It then defines one image on the leaf, whose generation resolves every genex the leaf carries,
# and lists its link target in link_targets for the caller to read the link line of.

cmake_minimum_required(VERSION 3.24)
project(kickos_package_names LANGUAGES C CXX ASM)

find_package(KickOS REQUIRED)

if(NOT TARGET KickOS::kernel)
  message(FATAL_ERROR "the package exports no KickOS::kernel")
endif()

set(_toolchain_libs c m gcc stdc++ supc++)
# The members of a leaf's kernel archive group, every group's members checked on the way. A
# member named by its file is the archive of that KickOS:: name.
function(_archive_group leaf out_text out_members)
  get_property(_links TARGET ${leaf} PROPERTY INTERFACE_LINK_LIBRARIES)
  string(REGEX REPLACE "\\$<TARGET_FILE:(KickOS::[A-Za-z0-9_]+)>" "\\1" _links "${_links}")
  string(REGEX MATCHALL "LINK_GROUP:(RESCAN|KICKOS_KERNEL),[^>]*" _groups "${_links}")
  if(NOT _groups)
    message(FATAL_ERROR "${leaf} links no archive group: '${_links}'")
  endif()
  set(_kernel "")
  foreach(_group IN LISTS _groups)
    if(_group MATCHES "^LINK_GROUP:KICKOS_KERNEL,")
      set(_kernel "${_group}")
    endif()
    string(REGEX REPLACE "^LINK_GROUP:[A-Z_]+," "" _group "${_group}")
    string(REPLACE "," ";" _members "${_group}")
    foreach(_m IN LISTS _members)
      if(NOT _m MATCHES "^KickOS::" AND NOT _m IN_LIST _toolchain_libs)
        message(FATAL_ERROR "${leaf}'s archive group names ${_m}, which is neither a KickOS:: "
                            "name nor a toolchain library")
      endif()
    endforeach()
  endforeach()
  if(_kernel STREQUAL "")
    list(GET _groups 0 _kernel)
  endif()
  string(REGEX REPLACE "^LINK_GROUP:[A-Z_]+," "" _kernel "${_kernel}")
  set(${out_text} "${_kernel}" PARENT_SCOPE)
  string(REPLACE "," ";" _members "${_kernel}")
  set(${out_members} "${_members}" PARENT_SCOPE)
endfunction()
_archive_group(KickOS::kernel _kernel_group _kernel_members)

if(NOT "KickOS::kernel_archive" IN_LIST _kernel_members)
  message(FATAL_ERROR "KickOS::kernel's archive group holds no KickOS::kernel_archive: "
                      "${_kernel_group}")
endif()

get_property(_kernel TARGET KickOS::kernel PROPERTY INTERFACE_LINK_OPTIONS)
if(_kernel MATCHES "KICKOS_USER_HEAP_SIZE")
  message(FATAL_ERROR "KickOS::kernel defines KICKOS_USER_HEAP_SIZE: '${_kernel}'")
endif()
if(NOT _kernel MATCHES "LINKER:--require-defined=_?kickos_link_one_system_target")
  message(FATAL_ERROR "KickOS::kernel does not require kickos_link_one_system_target: "
                      "'${_kernel}'")
endif()

add_executable(kernel_probe main.cc)
target_link_libraries(kernel_probe PRIVATE KickOS::kernel)
kickos_image_link(kernel_probe)
file(WRITE "${CMAKE_BINARY_DIR}/link_targets" "kernel_probe\n")
