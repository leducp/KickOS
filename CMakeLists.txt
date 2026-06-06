# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

cmake_minimum_required(VERSION 3.24)
project(KickOS VERSION 0.0.1 LANGUAGES C CXX ASM)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(cmake/kickos.cmake)

# --- Board / arch selection -------------------------------------------------
set(KICKOS_BOARD "sim" CACHE STRING "Target board: sim|frdmk64f|picopi|f411disco|bluepill")
kickos_resolve_board("${KICKOS_BOARD}" KICKOS_ARCH)
message(STATUS "KickOS: board=${KICKOS_BOARD} arch=${KICKOS_ARCH}")

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
  target_link_libraries(kickos INTERFACE
    "$<LINK_GROUP:RESCAN,kickos_user,kickos_kernel,kickos_arch_${KICKOS_ARCH},kickos_lib>")
  if(KICKOS_ARCH STREQUAL "sim")
    find_package(Threads REQUIRED)
    target_link_libraries(kickos INTERFACE Threads::Threads rt)
  endif()
  install(TARGETS kickos EXPORT KickOSTargets)
endif()

if(KICKOS_BUILD_APPS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/user/apps/CMakeLists.txt")
  add_subdirectory(user/apps)
endif()

# --- Install / export: the dependency-inversion package --------------------
# Consumers do find_package(KickOS) + kickos_add_application() out of tree.
include(CMakePackageConfigHelpers)

set(KICKOS_CMAKE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/KickOS")

# CeCILL-C is file-level copyleft: ship the license with the package.
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/licenses/${PROJECT_NAME}")

# Public headers (each component ships its kickos/ subtree; they merge).
install(DIRECTORY
          "${CMAKE_CURRENT_SOURCE_DIR}/arch/include/"
          "${CMAKE_CURRENT_SOURCE_DIR}/arch/${KICKOS_ARCH}/include/"
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
