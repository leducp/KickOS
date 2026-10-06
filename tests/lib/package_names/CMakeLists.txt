# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The consumer behind gate.sh's package_names(): it finds an installed KickOS package and
# refuses unless each leaf a user links is exported as KickOS::<name>, every archive group
# member is a KickOS:: name or a toolchain library, and KickOS::kernel is KickOS::kickos_cxx
# with exactly the KICKOS_EXPECT_PROVIDERS members taken out of its archive group, the kernel
# archive among the rest as KickOS::kernel_archive. It then defines one image per full-C++
# leaf, whose generation resolves every genex the leaf carries, and lists their link targets in
# link_targets for the caller to read the link lines of.

cmake_minimum_required(VERSION 3.24)
project(kickos_package_names LANGUAGES C CXX ASM)

find_package(KickOS REQUIRED)

foreach(_name kernel kickos kickos_cxx)
  if(NOT TARGET KickOS::${_name})
    message(FATAL_ERROR "the package exports no KickOS::${_name}")
  endif()
endforeach()
if(NOT KICKOS_EXPECT_PROVIDERS)
  message(FATAL_ERROR "KICKOS_EXPECT_PROVIDERS is empty, so KickOS::kernel's archive group "
                      "has nothing to be compared against")
endif()

set(_toolchain_libs c m gcc stdc++ supc++)
# The members of a leaf's first archive group, every group's members checked on the way.
function(_archive_group leaf out_text out_members)
  get_property(_links TARGET ${leaf} PROPERTY INTERFACE_LINK_LIBRARIES)
  string(REGEX MATCHALL "LINK_GROUP:RESCAN,[^>]*" _groups "${_links}")
  if(NOT _groups)
    message(FATAL_ERROR "${leaf} links no archive group: '${_links}'")
  endif()
  foreach(_group IN LISTS _groups)
    string(REPLACE "LINK_GROUP:RESCAN," "" _group "${_group}")
    string(REPLACE "," ";" _members "${_group}")
    foreach(_m IN LISTS _members)
      if(NOT _m MATCHES "^KickOS::" AND NOT _m IN_LIST _toolchain_libs)
        message(FATAL_ERROR "${leaf}'s archive group names ${_m}, which is neither a KickOS:: "
                            "name nor a toolchain library")
      endif()
    endforeach()
  endforeach()
  list(GET _groups 0 _first)
  string(REPLACE "LINK_GROUP:RESCAN," "" _first "${_first}")
  set(${out_text} "${_first}" PARENT_SCOPE)
  string(REPLACE "," ";" _members "${_first}")
  set(${out_members} "${_members}" PARENT_SCOPE)
endfunction()
_archive_group(KickOS::kickos _kickos_group _kickos_members)
_archive_group(KickOS::kickos_cxx _cxx_group _cxx_members)
_archive_group(KickOS::kernel _kernel_group _kernel_members)

if(NOT "KickOS::kernel_archive" IN_LIST _kernel_members)
  message(FATAL_ERROR "KickOS::kernel's archive group holds no KickOS::kernel_archive: "
                      "${_kernel_group}")
endif()
foreach(_p IN LISTS KICKOS_EXPECT_PROVIDERS)
  if(NOT _p IN_LIST _cxx_members)
    message(FATAL_ERROR "KickOS::kickos_cxx's archive group holds no ${_p}: ${_cxx_group}")
  endif()
endforeach()
set(_expected ${_cxx_members})
list(REMOVE_ITEM _expected ${KICKOS_EXPECT_PROVIDERS})
if(NOT _kernel_members STREQUAL _expected)
  string(REPLACE ";" "," _expected "${_expected}")
  message(FATAL_ERROR "KickOS::kernel's archive group is ${_kernel_group}, not ${_expected}")
endif()

# Apart from that group, the heap size symbol, which KickOS::kernel leaves to its system target,
# the system target's symbol KickOS::kernel requires, the script giving the heap a value until it
# fails on that symbol, and the old leaf's gate assignment, which a system target emits its own
# of, the two leaves carry the same link line and C++ flags.
foreach(_prop INTERFACE_LINK_LIBRARIES INTERFACE_LINK_OPTIONS INTERFACE_COMPILE_OPTIONS)
  get_property(_cxx TARGET KickOS::kickos_cxx PROPERTY ${_prop})
  get_property(_kernel TARGET KickOS::kernel PROPERTY ${_prop})
  string(REPLACE "${_cxx_group}" "<group>" _cxx "${_cxx}")
  if(_prop STREQUAL "INTERFACE_LINK_OPTIONS")
    if(_kernel MATCHES "KICKOS_USER_HEAP_SIZE")
      message(FATAL_ERROR "KickOS::kernel defines KICKOS_USER_HEAP_SIZE: '${_kernel}'")
    endif()
    if(NOT _kernel MATCHES "LINKER:--require-defined=_?kickos_link_one_system_target")
      message(FATAL_ERROR "KickOS::kernel does not require kickos_link_one_system_target: "
                          "'${_kernel}'")
    endif()
    string(REGEX REPLACE ";?LINKER:--require-defined=_?kickos_link_one_system_target" ""
           _kernel "${_kernel}")
    string(REGEX REPLACE ";?LINKER:--defsym=KICKOS_USER_HEAP_SIZE=(0x[0-9A-Fa-f]+|[0-9]+)" ""
           _cxx "${_cxx}")
  endif()
  if(_prop STREQUAL "INTERFACE_LINK_LIBRARIES")
    string(REGEX REPLACE ";[^;]*/kernel_leaf\\.ld" "" _kernel "${_kernel}")
    string(REPLACE ";$<TARGET_OBJECTS:KickOS::kickos_gate>" "" _cxx "${_cxx}")
  endif()
  string(REPLACE "${_kernel_group}" "<group>" _kernel "${_kernel}")
  if(NOT _cxx STREQUAL _kernel)
    message(FATAL_ERROR "KickOS::kernel's ${_prop} '${_kernel}' is not KickOS::kickos_cxx's "
                        "'${_cxx}'")
  endif()
endforeach()

# The arm of check_oot_export_mcu.sh: an image reaching KickOS::kernel through a library of its
# own and linking KickOS::kickos_cxx besides, which kickos_emit_image refuses.
if(KICKOS_PROBE_MIXED_LEAVES)
  add_library(mixed_kernel INTERFACE)
  target_link_libraries(mixed_kernel INTERFACE KickOS::kernel)
  add_executable(mixed_probe main.cc)
  target_link_libraries(mixed_probe PRIVATE mixed_kernel KickOS::kickos_cxx)
  kickos_emit_image(mixed_probe)
endif()

set(_link_targets "")
foreach(_leaf kernel kickos_cxx)
  add_executable(${_leaf}_probe main.cc)
  target_link_libraries(${_leaf}_probe PRIVATE KickOS::${_leaf})
  string(APPEND _link_targets "${_leaf}_probe\n")
endforeach()
file(WRITE "${CMAKE_BINARY_DIR}/link_targets" "${_link_targets}")
