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

# --- Board provisioning knobs (architecture.md item 10: hardware/RAM facts that
#     leave the global config for the board layer) ---------------------------
# Board-derived defaults; each is `if(NOT DEFINED)`-guarded so a -D on the
# command line genuinely overrides it. The SAME values are applied BOTH to our
# own libraries (add_compile_definitions, below) AND to the exported `kickos`
# interface target (target_compile_definitions, further down) -- otherwise an
# installed MCU package would compile a consumer's TUs (which include the shipped
# instance.h / config headers) with the #ifndef defaults (16 / 64 KiB / 32),
# disagreeing with the prebuilt .a's layout -> silent ODR/size mismatch.
#
# KICKOS_MAX_IRQ: in-kernel IRQ-table size, per the chip's NVIC line count. MUST
# stay >= the startup vector table's IRQ count (startup.S .rept) -- they are the
# same fact expressed in two files; keep them in step when adding a chip.
if(KICKOS_BOARD STREQUAL "frdmk64f")
  set(_ki_max_irq 86)          # K64: IRQ0..85
elseif(KICKOS_BOARD STREQUAL "f411disco")
  set(_ki_max_irq 86)          # STM32F411
elseif(KICKOS_BOARD STREQUAL "bluepill")
  set(_ki_max_irq 43)          # STM32F103 medium-density: 43 maskable lines
elseif(KICKOS_BOARD STREQUAL "qemu")
  set(_ki_max_irq 48)          # mps2-an386 (matches its startup .rept 48)
else()
  set(_ki_max_irq 32)          # sim
endif()

# Thread pool + bootstrap stacks. The sim keeps the generous config-header
# defaults (16 x 64 KiB pool, 64 KiB idle/root); a bare-metal board (K64F: 256
# KiB SRAM) right-sizes them. A syscall runs on the caller's stack, so 8 KiB is
# generous for the deepest kernel dispatch, not minimal; 16 threads keeps the
# selftest (the M0 demo, item 10) runnable within 256 KiB (16 x 8 KiB = 128 KiB).
if(KICKOS_ARCH STREQUAL "sim")
  set(_ki_max_threads 16)
  set(_ki_user_stack 65536)
  set(_ki_idle_stack 65536)
  set(_ki_root_stack 65536)
else()
  set(_ki_max_threads 16)
  set(_ki_user_stack 8192)
  set(_ki_idle_stack 2048)
  set(_ki_root_stack 8192)
endif()

if(NOT DEFINED KICKOS_MAX_IRQ)
  set(KICKOS_MAX_IRQ ${_ki_max_irq})
endif()
if(NOT DEFINED KICKOS_MAX_THREADS)
  set(KICKOS_MAX_THREADS ${_ki_max_threads})
endif()
if(NOT DEFINED KICKOS_USER_STACK_SIZE)
  set(KICKOS_USER_STACK_SIZE ${_ki_user_stack})
endif()
if(NOT DEFINED KICKOS_IDLE_STACK_SIZE)
  set(KICKOS_IDLE_STACK_SIZE ${_ki_idle_stack})
endif()
if(NOT DEFINED KICKOS_ROOT_STACK_SIZE)
  set(KICKOS_ROOT_STACK_SIZE ${_ki_root_stack})
endif()

# The single source of truth applied to our libs now and the `kickos` interface
# later (so in-tree apps and out-of-tree consumers see identical values).
set(KICKOS_PROVISION_DEFS
  KICKOS_MAX_IRQ=${KICKOS_MAX_IRQ}
  KICKOS_MAX_THREADS=${KICKOS_MAX_THREADS}
  KICKOS_USER_STACK_SIZE=${KICKOS_USER_STACK_SIZE}
  KICKOS_IDLE_STACK_SIZE=${KICKOS_IDLE_STACK_SIZE}
  KICKOS_ROOT_STACK_SIZE=${KICKOS_ROOT_STACK_SIZE})
add_compile_definitions(${KICKOS_PROVISION_DEFS})

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
  # Provisioning knobs travel to app TUs (in-tree) and to out-of-tree consumers
  # (via KickOSTargets.cmake), so the shipped instance.h/config headers compile
  # to the same layout as the prebuilt libraries.
  target_compile_definitions(kickos INTERFACE ${KICKOS_PROVISION_DEFS})
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
