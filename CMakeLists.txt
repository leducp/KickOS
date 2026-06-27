# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

cmake_minimum_required(VERSION 3.24)
project(KickOS VERSION 0.0.1 LANGUAGES C CXX ASM)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(cmake/kickos.cmake)

# --- Board / arch selection -------------------------------------------------
set(KICKOS_BOARD "sim" CACHE STRING "Target board: sim|qemu|frdmk64f|picopi|f411disco|bluepill")
kickos_resolve_board("${KICKOS_BOARD}" KICKOS_ARCH)
kickos_resolve_chip("${KICKOS_BOARD}" KICKOS_CHIP)
message(STATUS "KickOS: board=${KICKOS_BOARD} arch=${KICKOS_ARCH} chip=${KICKOS_CHIP}")

# --- Config knobs (architecture.md) ----------------------------------------
option(CONFIG_SCHED_PERIODIC_TICK "Force a classic periodic scheduler tick" OFF)
option(KICKOS_BUILD_APPS          "Build in-tree demo applications"          ON)
option(KICKOS_BUILD_TESTS         "Register CTest cases"                     ON)
# Test-only: exposes the guard-probe syscall (kos_guard_addr) used by the MPU
# privilege self-test. Kept out of the production syscall ABI by default.
option(KICKOS_ENABLE_SELFTEST     "Expose test-only self-test syscalls"      OFF)

if(KICKOS_ENABLE_SELFTEST)
  # Directory-wide so kernel dispatch, the user stub, and the app agree.
  add_compile_definitions(KICKOS_ENABLE_SELFTEST=1)
endif()

# Standard install dirs; included before the components so their
# $<INSTALL_INTERFACE:...> can use CMAKE_INSTALL_INCLUDEDIR.
include(GNUInstallDirs)

if(KICKOS_BUILD_TESTS)
  enable_testing()
endif()

# Components are added as they land; guarded so every step boundary configures.
foreach(_dir lib arch kernel user)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_dir}/CMakeLists.txt")
    add_subdirectory(${_dir})
  endif()
endforeach()

# Aggregate usage-requirement target: "link this and you get the whole OS".
# Lets a consumer write a plain add_executable(app ...) + target_link_libraries
# (the link recipe is baked into the exported interface target);
# kickos_add_application() is then optional sugar (and, on MCU, the image step).
# The arch<->kernel callback cycle is resolved as one rescanned archive group.
if(TARGET kickos_kernel)
  add_library(kickos INTERFACE)
  # On MCU the chip backend (arch_init/console + reset vector) joins the rescan
  # group; the arch<->kernel<->chip callback cycles resolve as one archive group.
  if(KICKOS_CHIP AND TARGET kickos_chip_${KICKOS_CHIP})
    target_link_libraries(kickos INTERFACE
      "$<LINK_GROUP:RESCAN,kickos_user,kickos_kernel,kickos_chip_${KICKOS_CHIP},kickos_arch_${KICKOS_ARCH},kickos_lib>")
  else()
    target_link_libraries(kickos INTERFACE
      "$<LINK_GROUP:RESCAN,kickos_user,kickos_kernel,kickos_arch_${KICKOS_ARCH},kickos_lib>")
  endif()
  # OS-agnostic app entry (architecture.md 8j), carried as usage requirements so
  # both the plain add_executable path and kickos_add_application inherit them
  # (in-tree and out-of-tree): the app writes a plain `int main`, renamed to the
  # kernel-called symbol; app.h is force-included so the renamed C++ main gets C
  # linkage (unmangled).
  target_compile_definitions(kickos INTERFACE main=kickos_app_main)
  target_compile_options(kickos INTERFACE -include kickos/app.h)
  if(KICKOS_ARCH STREQUAL "sim")
    find_package(Threads REQUIRED)
    target_link_libraries(kickos INTERFACE Threads::Threads rt)
  else()
    # MCU: the bare-metal link is -nostdlib++ (no libsupc++/libstdc++), so app TUs
    # must compile freestanding too -- otherwise EH/RTTI tables from a destructor
    # reference __gxx_personality_v0 / __cxa_end_cleanup and the link fails. (An
    # app opting into full C++ would additionally link the toolchain's libsupc++;
    # that opt-in is a later story.)
    target_compile_options(kickos INTERFACE
      "$<$<COMPILE_LANGUAGE:CXX>:${KICKOS_FREESTANDING_CXX_FLAGS}>")
  endif()
  # MCU bare-metal link recipe (baked into the interface target): our reset object
  # instead of crt0, the chip linker script, dead-code elimination, and a force-
  # reference to the vector table (else --gc-sections would drop it).
  if(NOT KICKOS_IS_SIM AND KICKOS_LINKER_SCRIPT)
    # -T at the DRIVER level (not -Wl,-T): the picolibc-based toolchain's spec
    # only suppresses its default linker script (picolibc.ld) when it sees a
    # driver-level -T; a -Wl,-T is invisible to that check and both scripts would
    # apply (colliding at address 0).
    target_link_options(kickos INTERFACE
      -nostartfiles
      -nostdlib++              # freestanding C++: no libstdc++ (keep libc/libgcc)
      -Wl,--gc-sections
      -Wl,--no-warn-rwx-segments
      -Wl,-u,g_isr_vector      # force the vector table object in (else gc'd)
      -Wl,-u,_exit             # force our libc syscall stubs in (libc's raise->_exit)
      # In-tree: the source linker script. Installed/exported: the shipped copy
      # under <prefix>/<libdir>/kickos so an out-of-tree consumer links without
      # the KickOS source tree present.
      "$<BUILD_INTERFACE:SHELL:-T ${KICKOS_LINKER_SCRIPT}>"
      "$<INSTALL_INTERFACE:SHELL:-T $<INSTALL_PREFIX>/${CMAKE_INSTALL_LIBDIR}/kickos/${KICKOS_CHIP}.ld>")
  endif()
  install(TARGETS kickos EXPORT KickOSTargets)
endif()

# Apps link a full image: the sim links against host libc; an MCU board links
# once its chip layer (startup + linker script) is actually built. A board whose
# chip is named but not yet brought up (no arch/arm/chip/<chip> dir -> no chip
# target) builds the libraries only.
if(KICKOS_BUILD_APPS
   AND (KICKOS_IS_SIM OR (KICKOS_CHIP AND TARGET kickos_chip_${KICKOS_CHIP}))
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/user/apps/CMakeLists.txt")
  add_subdirectory(user/apps)
endif()

# --- Install / export: the dependency-inversion package --------------------
# Consumers do find_package(KickOS) + kickos_add_application() out of tree.
include(CMakePackageConfigHelpers)

set(KICKOS_CMAKE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/KickOS")

# CeCILL-C is file-level copyleft: ship the license with the package.
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/licenses/${PROJECT_NAME}")

# Public headers (each component ships its kickos/ subtree; they merge). The arch
# backend's own include dir (which holds the per-arch context.h) lives at a path
# the arch subdir reports -- arch/sim/include vs arch/arm/armv7m/include -- so
# use that, not a arch/${ARCH}/include guess (which is wrong for the ARM tree).
install(DIRECTORY
          "${CMAKE_CURRENT_SOURCE_DIR}/arch/include/"
          "${KICKOS_ARCH_INCLUDE_DIR}/"
          "${CMAKE_CURRENT_SOURCE_DIR}/kernel/include/"
          "${CMAKE_CURRENT_SOURCE_DIR}/lib/include/"
          "${CMAKE_CURRENT_SOURCE_DIR}/user/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

# No NAMESPACE: kickos_add_application() links and asserts the bare target names
# (kickos_arch_<arch>, ...), identically in-tree and installed. Do not add one.
install(EXPORT KickOSTargets
        FILE KickOSTargets.cmake
        DESTINATION "${KICKOS_CMAKE_DIR}")

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/KickOSConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/KickOSConfig.cmake"
  INSTALL_DESTINATION "${KICKOS_CMAKE_DIR}")

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/KickOSConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion)

# The build helper is shipped verbatim so kickos_add_application() is available.
install(FILES
          "${CMAKE_CURRENT_BINARY_DIR}/KickOSConfig.cmake"
          "${CMAKE_CURRENT_BINARY_DIR}/KickOSConfigVersion.cmake"
          "${CMAKE_CURRENT_SOURCE_DIR}/cmake/kickos.cmake"
        DESTINATION "${KICKOS_CMAKE_DIR}")

# Ship the ARM cross toolchain file for MCU packages: an out-of-tree consumer
# configures with -DCMAKE_TOOLCHAIN_FILE=<prefix>/.../toolchain-arm-none-eabi.cmake
# and gets the CPU/multilib baseline + the LINK_GROUP RESCAN feature declaration.
if(NOT KICKOS_ARCH STREQUAL "sim")
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchain-arm-none-eabi.cmake"
          DESTINATION "${KICKOS_CMAKE_DIR}")
endif()
