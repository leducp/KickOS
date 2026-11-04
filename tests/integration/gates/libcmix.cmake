# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Two images of one system, each linking its own newlib profile: each runs its libc arms, and
# the links of both take each libc member from that image's profile alone.

if(NOT TARGET libcmix)
  return()
endif()
get_target_property(_libcmix_profiles libcmix KICKOS_LIBCMIX_PROFILES)
list(GET _libcmix_profiles 0 _libcmix_main)
list(GET _libcmix_profiles 1 _libcmix_peer)
set(_libcmix_args --main ${_libcmix_main} --peer ${_libcmix_peer})
kickos_app_judge(libcmix tests/integration/check_libcmix.sh ARGS ${_libcmix_args})
# The emulator options an image linked for another memory than the chip's needs.
get_target_property(_libcmix_qemu libcmix KICKOS_IMAGE_QEMU_ARGS)
set(_libcmix_qemu_args)
if(_libcmix_qemu)
  set(_libcmix_qemu_args QEMU_ARGS ${_libcmix_qemu})
endif()
kickos_add_qemu_test(TARGET libcmix SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_libcmix.sh"
                     ARGS ${_libcmix_args} ${_libcmix_qemu_args})

# Image 0 is the init, which main's link carries; image 1 is main and image 2 the peer.
add_test(NAME ${_tag}_libcmix_profiles
  COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_libc_profile.sh" "${CMAKE_NM}"
          "$<TARGET_FILE:libcmix>" --carve full=${KICKOS_REENT_CARVE_full}
          --carve nano=${KICKOS_REENT_CARVE_nano}
          0=${_libcmix_main} 1=${_libcmix_main} 2=${_libcmix_peer})
kickos_host_gate(${_tag}_libcmix_profiles TIMEOUT 60)
set_tests_properties(${_tag}_libcmix_profiles PROPERTIES FIXTURES_REQUIRED kickos_build)

# libcmix creates no exception, so its link takes none of the exception runtime.
add_test(NAME ${_tag}_libcmix_no_eh_runtime
  COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_no_eh_runtime.sh"
          "$<TARGET_FILE:libcmix>.map")
kickos_host_gate(${_tag}_libcmix_no_eh_runtime TIMEOUT 60)
set_tests_properties(${_tag}_libcmix_no_eh_runtime PROPERTIES FIXTURES_REQUIRED kickos_build)
