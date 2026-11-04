# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# main links the board's newlib profile and its peer program the other one, each running its libc
# arms in its own image.
set(_libcmix_main ${KICKOS_LIBC_DEFAULT})
set(_libcmix_peer nano)
if(_libcmix_main STREQUAL "nano")
  set(_libcmix_peer full)
endif()
add_executable(libcmix main.cc)
# Without exceptions: the peer's static destructor prints, which would otherwise link the exception
# runtime.
set_source_files_properties(main.cc PROPERTIES COMPILE_OPTIONS -fno-exceptions)
target_include_directories(libcmix PRIVATE "${PROJECT_SOURCE_DIR}/user/apps/common/include")
add_executable(libcmix_peer main.cc)
target_include_directories(libcmix_peer PRIVATE "${PROJECT_SOURCE_DIR}/user/apps/common/include")
target_link_libraries(libcmix_peer PRIVATE kickos)
target_compile_definitions(libcmix_peer PRIVATE LIBCMIX_PEER=1)
kickos_image_libc(libcmix ${_libcmix_main})
kickos_image_libc(libcmix_peer ${_libcmix_peer})
# The nRF51's 32 KiB arena seats too few thread slots for both tasks' FILE lock arms: this image
# alone links against more SRAM, and its emulator runs are given the same.
if(KICKOS_CHIP STREQUAL "nrf51")
  set(_libcmix_sram 36864)
  target_link_options(libcmix PRIVATE "LINKER:--defsym=__kickos_ram_length=${_libcmix_sram}")
  set_target_properties(libcmix PROPERTIES
    KICKOS_IMAGE_QEMU_ARGS "-global;nrf51-soc.sram-size=${_libcmix_sram}")
endif()
kickos_app_system(libcmix COMPOSITION system.yaml)
kickos_emit_image(libcmix)
set_target_properties(libcmix PROPERTIES KICKOS_LIBCMIX_PROFILES "${_libcmix_main};${_libcmix_peer}")
