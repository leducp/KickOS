# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

cmake_minimum_required(VERSION 3.24)
project(KickOS VERSION 0.5.1 LANGUAGES C CXX ASM)
set(KICKOS_VERSION "${PROJECT_VERSION}")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(cmake/kickos.cmake)
include(cmake/kconfig.cmake)
# Must precede add_subdirectory(user/apps) (a selftest expectation keys on the chunk
# granule) and the width sum at the bottom of this file.
include(cmake/cap_geometry.cmake)
include(cmake/sched_geometry.cmake)
include(cmake/init_geometry.cmake)
include(cmake/stack_geometry.cmake)
include(cmake/mpu_geometry.cmake)
include(cmake/exit_geometry.cmake)
include(cmake/manifest.cmake)
include(cmake/preset_key.cmake)

# KICKOS is what an add_subdirectory(KickOS) consumer tests with if(KICKOS).
set(KICKOS TRUE CACHE INTERNAL "Building against KickOS")
add_compile_definitions(__KickOS__=1)

# --- Board / arch selection -------------------------------------------------
kickos_board_names(_kickos_board_names)
list(JOIN _kickos_board_names "|" _kickos_board_help)
set(KICKOS_BOARD "sim" CACHE STRING "Target board: ${_kickos_board_help}")
kickos_load_board_descriptor("${KICKOS_BOARD}" KICKOS_ARCH KICKOS_CHIP KICKOS_ARCH_FAMILY)
message(STATUS "KickOS: board=${KICKOS_BOARD} family=${KICKOS_ARCH_FAMILY} arch=${KICKOS_ARCH} chip=${KICKOS_CHIP}")

include_directories("${PROJECT_SOURCE_DIR}/include")

# Add generated includes before subdirectories inherit the include paths.
include_directories("${PROJECT_BINARY_DIR}/generated/include")

# --- Board provisioning -----------------------------------------------------
include(chip_generate)
kickos_chip_generate("${CMAKE_CURRENT_SOURCE_DIR}" "${KICKOS_BOARD}" "${KICKOS_CHIP}"
                     "${KICKOS_ARCH}")
set(KICKOS_CHIP_LIMITS_H "${PROJECT_BINARY_DIR}/generated/include/kickos/chip_limits.h")
set(KICKOS_CHIP_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/arch/${KICKOS_ARCH_FAMILY}/chip/${KICKOS_CHIP}/include")
if(EXISTS "${KICKOS_CHIP_INCLUDE_DIR}")
  include_directories("${KICKOS_CHIP_INCLUDE_DIR}")
else()
  set(KICKOS_CHIP_INCLUDE_DIR "")
endif()

set(KICKOS_BOARD_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/boards/${KICKOS_BOARD}/include")
if(EXISTS "${KICKOS_BOARD_INCLUDE_DIR}")
  include_directories("${KICKOS_BOARD_INCLUDE_DIR}")
else()
  set(KICKOS_BOARD_INCLUDE_DIR "")
endif()

# --- Configuration from Kconfig --------------------------------------------
# Each variant defconfig is a complete statement, not a delta on another.
set(KICKOS_CONFIG_VARIANT "base" CACHE STRING
    "Configuration variant under boards/<board>/configs/")
set(KICKOS_DEFCONFIG
    "${CMAKE_CURRENT_SOURCE_DIR}/boards/${KICKOS_BOARD}/configs/${KICKOS_CONFIG_VARIANT}/defconfig")
if(EXISTS "${KICKOS_DEFCONFIG}")

  # Offer cached KICKOS_* values to the generator, which validates their types.
  # Use assert: for command-line/preset names so unknown requests are rejected.
  # Read cache properties without marking unused variables as consumed.
  set(_kcfg_req "")
  get_cmake_property(_kcfg_vars VARIABLES)
  foreach(_kcfg_v IN LISTS _kcfg_vars)
    if(NOT _kcfg_v MATCHES "^KICKOS_[A-Z0-9_]+$")
      continue()
    endif()
    unset(_kcfg_type)
    get_property(_kcfg_type CACHE "${_kcfg_v}" PROPERTY TYPE)
    # INTERNAL entries are the build's own derived state, never a request. Offering them put
    # a previous configure's value, a $<TARGET_OBJECTS:> of a target since deleted among
    # them, on the defconfig target's command line before the build had rewritten it.
    if(_kcfg_type STREQUAL "INTERNAL")
      continue()
    endif()
    set(_kcfg_shape "offer")
    if(NOT DEFINED _kcfg_type)
      set(_kcfg_val "${${_kcfg_v}}")
    else()
      get_property(_kcfg_val CACHE "${_kcfg_v}" PROPERTY VALUE)
      if(_kcfg_type STREQUAL "UNINITIALIZED")
        set(_kcfg_shape "assert")
      endif()
    endif()
    # A raw semicolon would split one candidate into several arguments, and the tail of a
    # string value then reaches the generator as a request of its own.
    string(REPLACE ";" "\\;" _kcfg_val "${_kcfg_val}")
    list(APPEND _kcfg_req "${_kcfg_shape}:${_kcfg_v}=${_kcfg_val}")
  endforeach()
  if(KICKOS_NEWLIB_FLAVOR STREQUAL "nano")
    list(APPEND _kcfg_req "CONFIG_KICKOS_NEWLIB_NANO=y")
  else()
    list(APPEND _kcfg_req "CONFIG_KICKOS_NEWLIB_NANO=n")
  endif()

  kickos_kconfig_generate("${CMAKE_CURRENT_SOURCE_DIR}" "${KICKOS_DEFCONFIG}"
                          "${PROJECT_BINARY_DIR}/generated" "${_kcfg_req}")

  # Saved before the fragment include() below overwrites them.
  set(_cmake_board "${KICKOS_BOARD}")
  set(_cmake_arch "${KICKOS_ARCH}")
  set(_cmake_chip "${KICKOS_CHIP}")
  set(_cmake_family "${KICKOS_ARCH_FAMILY}")
  include("${PROJECT_BINARY_DIR}/generated/kickos_config.cmake")
  kickos_kconfig_agree("the board" "${_cmake_board}" "${KICKOS_BOARD}")
  kickos_kconfig_agree("the arch" "${_cmake_arch}" "${KICKOS_ARCH}")
  kickos_kconfig_agree("the arch family" "${_cmake_family}" "${KICKOS_ARCH_FAMILY}")
  kickos_kconfig_agree("the chip" "${_cmake_chip}" "${KICKOS_CHIP}")

  set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" APPEND
               PROPERTY CMAKE_CONFIGURE_DEPENDS
               ${KICKOS_KCONFIG_SOURCES} "${PROJECT_BINARY_DIR}/generated/.config")
  message(STATUS "KickOS: configuration from ${KICKOS_KCONFIG_BASE}")

  kickos_find_kconfig(_kcfg_py _kcfg_found)
  if(_kcfg_found)
    add_custom_target(menuconfig
      COMMAND "${CMAKE_COMMAND}" -E env
              "KCONFIG_CONFIG=${PROJECT_BINARY_DIR}/generated/.config"
              "${_kcfg_py}" "${KICKOS_KCONFIG_BIN_MENUCONFIG}"
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      USES_TERMINAL
      COMMENT "Editing ${PROJECT_BINARY_DIR}/generated/.config (rebuild to apply)")
    add_custom_target(savedefconfig
      COMMAND "${CMAKE_COMMAND}" -E env
              "KCONFIG_CONFIG=${PROJECT_BINARY_DIR}/generated/.config"
              "${_kcfg_py}" "${KICKOS_KCONFIG_BIN_SAVEDEFCONFIG}"
              --kconfig Kconfig --out "${KICKOS_DEFCONFIG}"
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      COMMENT "Writing the minimal defconfig to ${KICKOS_DEFCONFIG}")
    add_custom_target(defconfig
      COMMAND "${CMAKE_COMMAND}" -E rm -f "${PROJECT_BINARY_DIR}/generated/.config"
      COMMAND "${_kcfg_py}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/kconfig/genconfig.py"
              "${CMAKE_CURRENT_SOURCE_DIR}" "${KICKOS_DEFCONFIG}"
              "${PROJECT_BINARY_DIR}/generated" ${_kcfg_req}
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      COMMENT "Reloading the live configuration from ${KICKOS_DEFCONFIG}")
  endif()
else()
  file(GLOB _variants
       "${CMAKE_CURRENT_SOURCE_DIR}/boards/${KICKOS_BOARD}/configs/*/defconfig")
  set(_have "")
  foreach(_v ${_variants})
    get_filename_component(_vdir "${_v}" DIRECTORY)
    get_filename_component(_vname "${_vdir}" NAME)
    list(APPEND _have "${_vname}")
  endforeach()
  message(FATAL_ERROR
    "Board '${KICKOS_BOARD}' has no configuration variant '${KICKOS_CONFIG_VARIANT}': "
    "${KICKOS_DEFCONFIG} does not exist. Variants it does ship: ${_have}. A preset "
    "selects a board and a variant and nothing else, so a new posture is a new "
    "defconfig under boards/${KICKOS_BOARD}/configs/, not a -D.")
endif()

set(KICKOS_HAVE_LIBC 1)
add_compile_definitions(KICKOS_LIBC_REENT=${KICKOS_LIBC_REENT}
                        KICKOS_LINKER_WEAK_UNDEF=${KICKOS_LINKER_WEAK_UNDEF})

option(KICKOS_BUILD_APPS          "Build in-tree demo applications"          ON)
option(KICKOS_BUILD_TESTS         "Register CTest cases"                     ON)
# A package built with them carries their archives and lists them in its catalogue, so only a
# witness preset sets it.
option(KICKOS_TEST_DRIVERS "Build tests/drivers, the packaged drivers the driver restart witness composes" OFF)
option(KICKOS_BUILD_INTEGRATION_TESTS
       "Register the CTest cases that RUN a KickOS image (sim, qemu, out-of-tree export)" ON)

set(_kickos_gtest_found OFF)
if(KICKOS_ARCH STREQUAL "sim")
  # GTest is IMPORTED for Release only and its interface properties are gated on
  # $<CONFIG:Release>; without this mapping a MinSizeRel sim build gets no include path.
  foreach(_cfg MINSIZEREL RELWITHDEBINFO DEBUG)
    if(NOT DEFINED CMAKE_MAP_IMPORTED_CONFIG_${_cfg})
      set(CMAKE_MAP_IMPORTED_CONFIG_${_cfg} ${_cfg} Release "")
    endif()
  endforeach()
  find_package(GTest QUIET CONFIG)
  if(GTest_FOUND)
    set(_kickos_gtest_found ON)
  endif()
endif()
option(KICKOS_BUILD_UNIT_TESTS "Build the host unit tests (needs GTest)" ${_kickos_gtest_found})
if(KICKOS_BUILD_UNIT_TESTS AND NOT _kickos_gtest_found)
  if(NOT KICKOS_ARCH STREQUAL "sim")
    message(FATAL_ERROR
      "KICKOS_BUILD_UNIT_TESTS=ON on board '${KICKOS_BOARD}' (arch ${KICKOS_ARCH}). The host "
      "unit tests are native executables and are registered on the host arch only; a board "
      "build must never need GTest. Configure the sim preset to run them.")
  endif()
  message(FATAL_ERROR
    "KICKOS_BUILD_UNIT_TESTS=ON but find_package(GTest CONFIG) found nothing. Install it with "
    "Conan and point the configure at the generators folder:\n"
    "  conan install ${CMAKE_CURRENT_SOURCE_DIR}/conan/conanfile.py --output-folder=<gen> "
    "-s compiler.cppstd=20 --build=missing\n"
    "  cmake --preset sim -DCMAKE_PREFIX_PATH=<gen>")
endif()
if(KICKOS_ARCH STREQUAL "sim" AND NOT _kickos_gtest_found)
  message(STATUS
    "KickOS: GTest not found, host unit tests OFF. Get it from Conan "
    "(conan install conan/conanfile.py --output-folder=<gen>) and add <gen> to CMAKE_PREFIX_PATH.")
endif()
# option() writes the cache ONCE, so a directory first configured without GTest keeps
# KICKOS_BUILD_UNIT_TESTS=OFF after one is installed.
if(_kickos_gtest_found AND NOT KICKOS_BUILD_UNIT_TESTS)
  message(STATUS
    "KickOS: GTest was found but KICKOS_BUILD_UNIT_TESTS is OFF in the cache. Re-run with "
    "-DKICKOS_BUILD_UNIT_TESTS=ON, or configure a fresh build directory.")
endif()
# The bools a header an application includes reads without the generated board_config.h: every
# in-tree TU takes them here and a consumer through kickos_core's interface, both from this list.
set(KICKOS_APP_BOOL_DEFS "")
foreach(_k KICKOS_BENCH KICKOS_BENCH_SCHED KICKOS_MULTI_INSTANCE)
  if(${_k})
    list(APPEND KICKOS_APP_BOOL_DEFS ${_k}=1)
  else()
    list(APPEND KICKOS_APP_BOOL_DEFS ${_k}=0)
  endif()
endforeach()
add_compile_definitions(${KICKOS_APP_BOOL_DEFS})

# CMake's stock MinSizeRel flags drop -g.
add_compile_options($<$<CONFIG:MinSizeRel>:-g>)

# A kernel built with this carries a system call that reads and writes user memory from ring 0
# on request, so only the uaccess witness's scratch configure, which also passes the marker below,
# may turn it on.
option(KICKOS_WITNESS_UACCESS "Plant a ring-0 access to a user pointer, for the uaccess witness" OFF)
if(KICKOS_WITNESS_UACCESS)
  if(NOT "${KICKOS_WITNESS_UACCESS_SCRATCH}" STREQUAL "check_uaccess.sh")
    message(FATAL_ERROR "KickOS: WITNESS_UACCESS_REFUSED: KICKOS_WITNESS_UACCESS builds a kernel that "
      "reads and writes user memory on request. Only tests/integration/check_uaccess.sh "
      "configures it, in a scratch tree of its own.")
  endif()
  add_compile_definitions(KICKOS_WITNESS_UACCESS=1)
endif()

# A kernel built with this runs scheduler passes no workload asked for, so only the repass
# witness's scratch configure, which also passes the marker below, may turn it on.
option(KICKOS_WITNESS_REPASS "Plant a second pass and a switch back, for the repass witness" OFF)
if(KICKOS_WITNESS_REPASS)
  if(NOT "${KICKOS_WITNESS_REPASS_SCRATCH}" STREQUAL "check_repass.sh")
    message(FATAL_ERROR "KickOS: WITNESS_REPASS_REFUSED: KICKOS_WITNESS_REPASS builds a "
      "kernel that runs extra scheduler passes on purpose. Only "
      "tests/integration/check_repass.sh configures it, in a scratch tree of its own.")
  endif()
  if(NOT KICKOS_KERNEL_CORES GREATER 1)
    message(FATAL_ERROR "KICKOS_WITNESS_REPASS needs more than one kernel core")
  endif()
  add_compile_definitions(KICKOS_WITNESS_REPASS=1)
endif()

option(KICKOS_SMP_TRACE "Record a per-core park/wake event ring for SMP diagnosis" OFF)
if(KICKOS_SMP_TRACE)
  add_compile_definitions(KICKOS_SMP_TRACE=1)
else()
  add_compile_definitions(KICKOS_SMP_TRACE=0)
endif()

# Select native chip transport or SEGGER RTT. Do not cache resolved config
# values: they would override later defconfig changes.
if(KICKOS_CONSOLE STREQUAL "chip")
  set(_con_chip 1)
  set(_con_rtt 0)
elseif(KICKOS_CONSOLE STREQUAL "rtt")
  set(_con_chip 0)
  set(_con_rtt 1)
elseif(KICKOS_CONSOLE STREQUAL "both")
  set(_con_chip 1)
  set(_con_rtt 1)
else()
  set(_con_chip 0)
  set(_con_rtt 0)
endif()
# Bare-metal UART consoles need '\n' lowered to CR+LF. The sim must stay raw, else '\r'
# leaks into the TAP stream and breaks the test harness.
if(KICKOS_ARCH STREQUAL "sim")
  set(_con_crlf 0)
else()
  set(_con_crlf 1)
endif()
add_compile_definitions(KICKOS_CONSOLE_CHIP=${_con_chip} KICKOS_CONSOLE_RTT=${_con_rtt} KICKOS_CONSOLE_CRLF=${_con_crlf})
message(STATUS "KickOS: console=${KICKOS_CONSOLE} (chip=${_con_chip} rtt=${_con_rtt} crlf=${_con_crlf})")

include("${CMAKE_SOURCE_DIR}/cmake/boot_arena.cmake")
# What the link spells before a C name, which a toolchain whose ABI adds one declares.
if(NOT DEFINED KICKOS_C_SYMBOL_PREFIX)
  set(KICKOS_C_SYMBOL_PREFIX "")
endif()
add_compile_definitions(KICKOS_TLS_FROM_SP=${KICKOS_TLS_FROM_SP})
if(KICKOS_TLS)
  kickos_pow2_ceil("${KICKOS_USER_STACK_SIZE}" _tls_stride)
  kickos_pow2_ceil("${KICKOS_ROOT_STACK_SIZE}" _tls_root_stride)
  if(KICKOS_TLS_FROM_SP AND NOT _tls_stride EQUAL _tls_root_stride)
    message(FATAL_ERROR
      "KickOS: KICKOS_TLS is on and this board's stacks do not share a stride. "
      "KICKOS_USER_STACK_SIZE ${KICKOS_USER_STACK_SIZE} rounds to ${_tls_stride} and "
      "KICKOS_ROOT_STACK_SIZE ${KICKOS_ROOT_STACK_SIZE} rounds to ${_tls_root_stride}. "
      "The ARM thread pointer is SP masked down to the stack block and one image has one "
      "mask, so a thread on the smaller stride would read its neighbour's thread_local. "
      "State both at the same power of two in "
      "boards/${KICKOS_BOARD}/configs/<variant>/defconfig, or set KICKOS_TLS=n there and "
      "let a thread_local fail the link instead.")
  endif()
  foreach(_k KICKOS_USER_STACK_SIZE KICKOS_ROOT_STACK_SIZE)
    if(KICKOS_TLS_FROM_SP AND NOT ${${_k}} EQUAL _tls_stride)
      message(FATAL_ERROR
        "KickOS: KICKOS_TLS is on and ${_k} is ${${_k}}, which is not a power of two: it "
        "rounds to a stride of ${_tls_stride}. The thread pointer is SP masked down to that "
        "stride and a block must BE one, because two smaller blocks fit inside a single "
        "stride and would mask to the same base. State ${_tls_stride} in "
        "boards/${KICKOS_BOARD}/configs/<variant>/defconfig, or set KICKOS_TLS=n there.")
    endif()
  endforeach()
  set(_tls_log2 0)
  set(_tls_p 1)
  while(_tls_p LESS _tls_stride)
    math(EXPR _tls_p "${_tls_p} * 2")
    math(EXPR _tls_log2 "${_tls_log2} + 1")
  endwhile()
  set(_kickos_tls_defs KICKOS_TLS=1 KICKOS_TLS_STRIDE=${_tls_stride}
                       KICKOS_TLS_STRIDE_LOG2=${_tls_log2})
  if(KICKOS_TLS_FROM_SP)
    message(STATUS "KickOS: the thread pointer is SP masked to the stride")
    set(KICKOS_STACK_STRIDE ${_tls_stride})
  endif()
  message(STATUS "KickOS: TLS stride ${_tls_stride} (1 << ${_tls_log2})")
else()
  set(_kickos_tls_defs KICKOS_TLS=0)
endif()
# The shipped arch.h reads the stride wherever the generated header turns KICKOS_TLS on.
add_compile_definitions(${_kickos_tls_defs})

if(KICKOS_REENT_IN_TCB AND NOT KICKOS_TLS)
  message(FATAL_ERROR
    "KickOS: KICKOS_TLS is off on arch '${KICKOS_ARCH}' with "
    "KICKOS_KERNEL_CORES=${KICKOS_KERNEL_CORES}. There each thread reaches its libc "
    "reentrant state through the first word of its TLS control block at the thread "
    "pointer, and only KICKOS_TLS carves that block and seats the register. Set "
    "KICKOS_TLS=y in boards/${KICKOS_BOARD}/configs/<variant>/defconfig.")
endif()
add_compile_definitions(KICKOS_REENT_PER_THREAD=${KICKOS_REENT_PER_THREAD}
                        KICKOS_REENT_IN_TCB=${KICKOS_REENT_IN_TCB}
                        KICKOS_ARCH_SIM=${KICKOS_ARCH_SIM})

# The chip file's facts: its protection unit, which Kconfig's selection is held to below, and
# whether its cores have an unprivileged mode at all.
include("${PROJECT_BINARY_DIR}/generated/chip/chip.cmake")
if(KICKOS_CHIP_PRIVILEGE)
  set(KICKOS_HAVE_PRIV_RING 1)
else()
  set(KICKOS_HAVE_PRIV_RING 0)
endif()
if(KICKOS_CHIP_STACK_LIMIT)
  set(KICKOS_HAVE_STACK_LIMIT 1)
else()
  set(KICKOS_HAVE_STACK_LIMIT 0)
endif()
add_compile_definitions(KICKOS_HAVE_STACK_LIMIT=${KICKOS_HAVE_STACK_LIMIT})

# Do not override fault isolation: duplicate architecture and fallback
# definitions would make archive order select the implementation.
# sim is x86_64-only: arch_fault_is_user_thread reads a host layout it only knows there.
set(KICKOS_FAULT_ISOLATION 0)
set(KICKOS_FAULT_OUTCOME panic)
if(KICKOS_HAVE_PRIV_RING AND KICKOS_ARCH_FAULT_ISOLATION
   AND NOT (KICKOS_ARCH STREQUAL "sim" AND NOT CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "x86_64"))
  set(KICKOS_FAULT_ISOLATION 1)
  set(KICKOS_FAULT_OUTCOME thread-kill)
endif()
add_compile_definitions(KICKOS_FAULT_ISOLATION=${KICKOS_FAULT_ISOLATION})

kickos_chip_protection_disagreement(_chip_disagrees "${KICKOS_HAVE_MPU}" "${KICKOS_HAVE_ASPACE}"
  "${KICKOS_CHIP_REGION_UNIT}" "${KICKOS_CHIP_TRANSLATES}" "platform/${KICKOS_CHIP}/chip.yaml")
if(NOT _chip_disagrees STREQUAL "")
  message(FATAL_ERROR "KickOS: board '${KICKOS_BOARD}' (chip ${KICKOS_CHIP}): ${_chip_disagrees}")
endif()
if(KICKOS_HAVE_MPU AND KICKOS_ARCH_FAMILY STREQUAL "arm")
  kickos_chip_arm_backend(KICKOS_ARM_MPU_BACKEND KICKOS_ARM_PMSAV8_SOURCE
                          "${KICKOS_CHIP_REGION_UNIT}")
endif()

add_compile_definitions(KICKOS_HAVE_MPU=${KICKOS_HAVE_MPU})

# Whether QEMU runs this board as one of its MPS2 machines, which the gates and apps that need its
# semihosting exit and its emulated MPU key on.
kickos_qemu_machine("${KICKOS_BOARD}" _kickos_qemu_env _kickos_qemu_machine)
set(KICKOS_QEMU_MPS2 OFF)
if(_kickos_qemu_machine MATCHES "^mps2-")
  set(KICKOS_QEMU_MPS2 ON)
endif()

add_compile_definitions(KICKOS_HAVE_ASPACE=${KICKOS_HAVE_ASPACE})

include(amp_share)
kickos_multi_instance_heap_check(MULTI "${KICKOS_MULTI_INSTANCE}" TRANSLATES "${KICKOS_HAVE_ASPACE}"
                                 ARCH "${KICKOS_ARCH}" ORIGIN "board '${KICKOS_BOARD}'")

# Read only by kickos_split_image_tu (cmake/kickos.cmake); empty leaves that function inert.
# Set here because kernel/CMakeLists.txt calls the same function and cannot see arch/'s scope.
set(KICKOS_SPLIT_IMAGE_CODE_MODEL "")
if(KICKOS_ARCH STREQUAL "armv8a")
  set(KICKOS_SPLIT_IMAGE_CODE_MODEL -mcmodel=large)
endif()

add_compile_definitions(KICKOS_MEMORY_ENFORCED=${KICKOS_MEMORY_ENFORCED})

# Select the chip MPU backend, defaulting to PMSAv7. Use NONE without an MPU.
if(NOT KICKOS_HAVE_MPU)
  set(_kickos_arm_mpu 0)
elseif(KICKOS_ARM_MPU_BACKEND STREQUAL "SYSMPU")
  set(_kickos_arm_mpu 3)
elseif(KICKOS_ARM_MPU_BACKEND STREQUAL "PMSAV8")
  set(_kickos_arm_mpu 2)
elseif(NOT KICKOS_ARM_MPU_BACKEND OR KICKOS_ARM_MPU_BACKEND STREQUAL "PMSAV7")
  set(_kickos_arm_mpu 1)
else()
  message(FATAL_ERROR
    "KICKOS_ARM_MPU_BACKEND must be unset, PMSAV7, PMSAV8 or SYSMPU on board "
    "'${KICKOS_BOARD}' (got '${KICKOS_ARM_MPU_BACKEND}')")
endif()
add_compile_definitions(
  KICKOS_ARM_MPU_NONE=0
  KICKOS_ARM_MPU_PMSAV7=1
  KICKOS_ARM_MPU_PMSAV8=2
  KICKOS_ARM_MPU_SYSMPU=3
  KICKOS_ARM_MPU=${_kickos_arm_mpu})

add_compile_definitions(KICKOS_ARCH_HAS_IPC_FASTPATH=${KICKOS_ARCH_HAS_IPC_FASTPATH})

# The generated header guards its own define with #ifndef, so this wins where both are seen.
add_compile_definitions(KICKOS_NUM_CORES=${KICKOS_NUM_CORES})
# Beside the count, so an APP TU can ask how many cores one kernel schedules on: a claim about
# two threads' relative progress is a claim about that number.
add_compile_definitions(KICKOS_KERNEL_CORES=${KICKOS_KERNEL_CORES})

# Beside them again, for an app TU that has to ask which MODEL it runs under.
add_compile_definitions(KICKOS_MULTICORE_MODEL_SHARED=${KICKOS_MULTICORE_MODEL_SHARED})

# Shared-image node IDs are core indices and must fit the doorbell matrix.
if(KICKOS_AMP_NODE AND NOT KICKOS_AMP_OWN_IMAGE
   AND KICKOS_AMP_NODES GREATER KICKOS_DOORBELL_CORES)
  message(FATAL_ERROR
    "KickOS: KICKOS_AMP_NODES is ${KICKOS_AMP_NODES} and the doorbell's matrix is "
    "${KICKOS_DOORBELL_CORES} cores wide. Under the shared image a node's identity IS its core "
    "register, so amp::core_of answers the node index itself and a node above the matrix's last "
    "row is rung on a core that has none. KICKOS_NUM_CORES is the authority on the width: state "
    "at most that many nodes, or raise the core count, in "
    "boards/${KICKOS_BOARD}/configs/${KICKOS_CONFIG_VARIANT}/defconfig.")
endif()

include(isolated_cores)
kickos_isolated_cores_check(
  ISOLATED      "${KICKOS_ISOLATED_CORES}"
  KERNEL_CORES  "${KICKOS_KERNEL_CORES}"
  ORIGIN        "${KICKOS_DEFCONFIG}")
# An APP TU asks this to know which cores it may pin onto.
add_compile_definitions(KICKOS_ISOLATED_CORES=${KICKOS_ISOLATED_CORES})

kickos_root_core_mask_check(
  MASK          "${KICKOS_ROOT_CORE_MASK}"
  KERNEL_CORES  "${KICKOS_KERNEL_CORES}"
  SHARED        "${KICKOS_MULTICORE_MODEL_SHARED}"
  ORIGIN        "${KICKOS_DEFCONFIG}")
# The gate reading root's first-thread witness asks this for the core it requires.
add_compile_definitions(KICKOS_ROOT_CORE_MASK=${KICKOS_ROOT_CORE_MASK})

# Expose AMP node configuration to apps that do not include board_config.h.
add_compile_definitions(KICKOS_AMP_NODE=${KICKOS_AMP_NODE})
add_compile_definitions(KICKOS_AMP_NODES=${KICKOS_AMP_NODES})
add_compile_definitions(KICKOS_AMP_OWN_IMAGE=${KICKOS_AMP_OWN_IMAGE})

# The lines of the devices the chip file gives the kernel, which irq_line_admit refuses and
# irq_kernel_line_reserved claims; a doorbell's only where one rings.
include(kernel_lines)
set(_kl_rings OFF)
if(KICKOS_NUM_CORES GREATER 1 OR KICKOS_AMP_NODE)
  set(_kl_rings ON)
endif()
kickos_kernel_lines(KICKOS_KERNEL_LINES "${KICKOS_CHIP}" ${_kl_rings}
                    "${KICKOS_CHIP_KERNEL_LINES}" "${KICKOS_CHIP_DOORBELL_KERNEL_LINES}")
list(LENGTH KICKOS_KERNEL_LINES KICKOS_KERNEL_LINE_COUNT)
string(REPLACE ";" "," _kl_csv "${KICKOS_KERNEL_LINES}")
add_compile_definitions("KICKOS_KERNEL_LINES=${_kl_csv}"
                        KICKOS_KERNEL_LINE_COUNT=${KICKOS_KERNEL_LINE_COUNT})
if(KICKOS_AMP_DIAG_REPORT)
  add_compile_definitions(KICKOS_AMP_DIAG_REPORT=1
                          KICKOS_AMP_DIAG_TAG=${KICKOS_AMP_DIAG_TAG})
  message(STATUS "KickOS: AMP peer self-report ON (tag ${KICKOS_AMP_DIAG_TAG}): a peer writes "
                 "its node index, its vector-table base and that tag into the shared region")
endif()

include("${CMAKE_SOURCE_DIR}/cmake/amp_partition.cmake")

add_compile_definitions(KICKOS_MIN_STACK_SIZE=${KICKOS_MIN_STACK_SIZE})
message(STATUS "KickOS: caller-stack floor KICKOS_MIN_STACK_SIZE=${KICKOS_MIN_STACK_SIZE} (arch=${KICKOS_ARCH})")

# RX MPU wedge-localizer trace (arch/rx/rxv3/arch_rxv3.cc), command line only.
if(DEFINED KICKOS_RX_MPU_TRACE)
  add_compile_definitions(KICKOS_RX_MPU_TRACE=${KICKOS_RX_MPU_TRACE})
endif()

set(_telem 0)
if(KICKOS_TELEMETRY STREQUAL "rtt")
  set(_telem 1)
endif()
add_compile_definitions(KICKOS_TELEMETRY=${_telem} KICKOS_TELEMETRY_RTT=${_telem}
                        KICKOS_TRACE_ARCH=${KICKOS_TRACE_ARCH})
set(KICKOS_TELEMETRY_ON ${_telem}) # consumed by component CMakeLists (asm/FP flags)
message(STATUS "KickOS: telemetry=${KICKOS_TELEMETRY} (have_trace_clock=${KICKOS_HAVE_TRACE_CLOCK} arch_id=${KICKOS_TRACE_ARCH})")

# Override the RTT publication barrier on weakly ordered cores.
# The definition must contain the complete fence call.
if(DEFINED KICKOS_RTT_PUBLISH_BARRIER_DEF)
  add_compile_definitions("KICKOS_RTT_PUBLISH_BARRIER()=${KICKOS_RTT_PUBLISH_BARRIER_DEF}")
endif()

# Passed as a bare token (-DKICKOS_APP_TZ=+0200): escaped quotes double up on a reconfigure of
# an existing build dir, so app.h stringizes the token in C.
execute_process(COMMAND date +%z OUTPUT_VARIABLE _apptz OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT _apptz STREQUAL "")
  add_compile_definitions(KICKOS_APP_TZ=${_apptz})
endif()

# Must precede the components: their $<INSTALL_INTERFACE:...> uses CMAKE_INSTALL_INCLUDEDIR.
include(GNUInstallDirs)

# Here and not down in the install section: cmake/x86_64_boot.cmake installs the x86_64 link tool
# and section script there too, and is included well before it.
set(KICKOS_CMAKE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/KickOS")

if(KICKOS_BUILD_TESTS)
  include(tests/cmake/kickos_tests.cmake)
endif()

# The version and board the boot banner names, and the identity rows a USB device console repeats
# (kickos/sys/banner_identity.h) name too.
set(KICKOS_BANNER_IDENTITY KICKOS_VERSION="${KICKOS_VERSION}" KICKOS_BOARD_NAME="${KICKOS_BOARD}")

foreach(_dir lib arch kernel user system)
  add_subdirectory(${_dir})
endforeach()
if(KICKOS_TEST_DRIVERS)
  add_subdirectory(tests/drivers)
endif()

# After the trees above: the gates under it compile kernel and lib sources, and read the
# include path and definitions off the targets those trees create.
if(KICKOS_BUILD_TESTS AND KICKOS_ARCH STREQUAL "sim")
  add_subdirectory(tests/unit)
endif()

if(TARGET kickos_kernel)
  # Consumers link KickOS::kernel, never kickos_core directly.
  add_library(kickos_core INTERFACE)
  # CMake forbids a library in two groups within one target; build a group
  # for each configuration below. Members go by kickos_export_name.
  set(_kickos_head_targets kickos_user kickos_kernel)
  set(_kickos_tail_targets kickos_arch_${KICKOS_ARCH} kickos_lib)
  if(KICKOS_CHIP AND TARGET kickos_chip_${KICKOS_CHIP})
    list(PREPEND _kickos_tail_targets kickos_chip_${KICKOS_CHIP})
  endif()
  foreach(_kickos_part head tail)
    set(_kickos_${_kickos_part}_names "")
    foreach(_kickos_t IN LISTS _kickos_${_kickos_part}_targets)
      kickos_export_name(${_kickos_t} _kickos_name)
      list(APPEND _kickos_${_kickos_part}_names "${_kickos_name}")
    endforeach()
  endforeach()
  list(JOIN _kickos_head_names "," _kickos_head)
  list(JOIN _kickos_tail_names "," _kickos_tail_group)
  set(_kickos_group "${_kickos_head},${_kickos_tail_group}")
  # Link the interface-only kickos_system outside the rescan group.
  target_link_libraries(kickos_core INTERFACE kickos_system)
  # The app writes `int main`, renamed to the kernel-called symbol; app.h is force-included
  # below so the renamed C++ main gets C linkage (unmangled).
  target_compile_definitions(kickos_core INTERFACE main=kickos_app_main)
  # Evaluated for the consuming target, so each image's TUs name that image (app.h's stamp).
  target_compile_definitions(kickos_core INTERFACE
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:KICKOS_APP_IMAGE=\"$<TARGET_PROPERTY:NAME>\">")
  # Export every macro affecting shipped layouts or inline definitions to avoid ODR mismatch.
  target_compile_definitions(kickos_core INTERFACE
    KICKOS_TELEMETRY=${_telem} KICKOS_TELEMETRY_RTT=${_telem}
    KICKOS_TRACE_ARCH=${KICKOS_TRACE_ARCH}
    KICKOS_HAVE_MPU=${KICKOS_HAVE_MPU}
    KICKOS_HAVE_ASPACE=${KICKOS_HAVE_ASPACE}
    KICKOS_HAVE_STACK_LIMIT=${KICKOS_HAVE_STACK_LIMIT}
    KICKOS_AMP_NODE=${KICKOS_AMP_NODE}
    KICKOS_ISOLATED_CORES=${KICKOS_ISOLATED_CORES}
    KICKOS_MEMORY_ENFORCED=${KICKOS_MEMORY_ENFORCED}
    KICKOS_ARM_MPU_NONE=0
    KICKOS_ARM_MPU_PMSAV7=1
    KICKOS_ARM_MPU_PMSAV8=2
    KICKOS_ARM_MPU_SYSMPU=3
    KICKOS_ARM_MPU=${_kickos_arm_mpu}
    KICKOS_ARCH_SIM=${KICKOS_ARCH_SIM}
    KICKOS_LIBC_REENT=${KICKOS_LIBC_REENT}
    KICKOS_LINKER_WEAK_UNDEF=${KICKOS_LINKER_WEAK_UNDEF}
    ${KICKOS_APP_BOOL_DEFS}
    ${_kickos_tls_defs}
    # Always define SPI backend flags because app builds use -Wundef.
    KICKOS_SPI_LOCAL=$<BOOL:${KICKOS_SPI_LOCAL_ENGINE}>)
  target_compile_options(kickos_core INTERFACE -include kickos/app.h)
  # Force-include the seam header so an app-TU-defined kickos_init_entry gets C linkage.
  # SHELL: keeps this second -include from being de-duplicated against the app.h one above.
  target_compile_options(kickos_core INTERFACE "SHELL:-include kickos/sys/init.h")
  if(KICKOS_ARCH STREQUAL "sim")
    find_package(Threads REQUIRED)
    target_link_libraries(kickos_core INTERFACE Threads::Threads rt)
  endif()
  # The force-refs below name objects nothing references, which --gc-sections would
  # otherwise drop.
  if(NOT (KICKOS_ARCH STREQUAL "sim") AND KICKOS_LINKER_SCRIPT)
    target_link_options(kickos_core INTERFACE
      -nostartfiles
      -Wl,--gc-sections
      -Wl,--no-warn-rwx-segments
      # Every input section must be named by a rule in the chip script; an unnamed ALLOC
      # orphan lands at an address nobody chose. arch/common/sections.ld.h has the vocabulary.
      -Wl,--orphan-handling=error
      # ARM's vector table (g_isr_vector) is a data object nothing references. RX pulls its
      # vector tables via ENTRY(_start) + KEEP() and defines no such symbol to force.
      "$<$<STREQUAL:${KICKOS_ARCH_FAMILY},arm>:-Wl,-u,g_isr_vector>"
      $<$<BOOL:${KICKOS_NODE_VECTOR_FORCE_SYMBOL}>:-Wl,--require-defined=${KICKOS_NODE_VECTOR_FORCE_SYMBOL}>
      # Defined only where idle's thread-local carve assert expands (arch/common/sections.ld.h).
      "$<$<AND:$<BOOL:${KICKOS_TLS}>,$<NOT:$<BOOL:${KICKOS_TLS_FROM_SP}>>>:-Wl,--require-defined=${KICKOS_C_SYMBOL_PREFIX}__kickos_tls_idle_checked>"
      # Force libc stubs into the link. RX prefixes C symbols, so _exit becomes __exit.
      "$<$<STREQUAL:${KICKOS_ARCH_FAMILY},rx>:-Wl,-u,__exit>"
      "$<$<NOT:$<STREQUAL:${KICKOS_ARCH_FAMILY},rx>>:-Wl,-u,_exit>"
      # A chip with a checksummed flash second stage (RP2040 boot2) must force its
      # .boot2 blob object in too; nothing references it.
      $<$<BOOL:${KICKOS_BOOT2_FORCE_SYMBOL}>:-Wl,-u,${KICKOS_BOOT2_FORCE_SYMBOL}>
      "$<BUILD_INTERFACE:SHELL:-T ${KICKOS_LINKER_SCRIPT}>"
      "$<INSTALL_INTERFACE:SHELL:-T $<INSTALL_PREFIX>/${CMAKE_INSTALL_LIBDIR}/kickos/${KICKOS_CHIP}.ld>")
    # Track the linker script as a dependency; -T alone does not trigger relinking.
    set_property(TARGET kickos_core APPEND PROPERTY INTERFACE_LINK_DEPENDS
      "$<BUILD_INTERFACE:${KICKOS_LINKER_SCRIPT}>"
      "$<INSTALL_INTERFACE:$<INSTALL_PREFIX>/${CMAKE_INSTALL_LIBDIR}/kickos/${KICKOS_CHIP}.ld>")
  endif()

  # x86_64's link rule runs ld with no compiler driver (tools/x86_64-link.sh), so the PE section
  # script, the exit stub and the boot and kernel-landing objects ride the leaves, and the tools
  # that link runs are its dependencies.
  if(KICKOS_ARCH STREQUAL "x86_64")
    set(_kos_pe_script "${CMAKE_CURRENT_SOURCE_DIR}/arch/x86/x86_64/pe_image.ld")
    set(_kos_pe_script_installed "$<INSTALL_PREFIX>/${KICKOS_CMAKE_DIR}/pe_image.ld")
    target_link_options(kickos_core INTERFACE
      "LINKER:-u,_exit"
      "$<BUILD_INTERFACE:LINKER:-T,${_kos_pe_script}>"
      "$<INSTALL_INTERFACE:LINKER:-T,${_kos_pe_script_installed}>")
    set_property(TARGET kickos_core APPEND PROPERTY INTERFACE_LINK_DEPENDS
      "$<BUILD_INTERFACE:${_kos_pe_script}>"
      "$<INSTALL_INTERFACE:${_kos_pe_script_installed}>")
    foreach(_kos_tool x86_64-link.sh x86_64-krel.sh x86_64-map.awk check-x86_64-no-got.sh
                      check-x86_64-weak-undef.sh)
      set_property(TARGET kickos_core APPEND PROPERTY INTERFACE_LINK_DEPENDS
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tools/${_kos_tool}>"
        "$<INSTALL_INTERFACE:$<INSTALL_PREFIX>/${KICKOS_CMAKE_DIR}/${_kos_tool}>")
    endforeach()
    set(_kos_boot_target kickos_x86_64_boot)
    if(KICKOS_KERNEL_CORES GREATER 1)
      set(_kos_boot_target kickos_x86_64_boot_ap)
    endif()
    kickos_export_name(${_kos_boot_target} _kos_boot_name)
    kickos_export_name(kickos_x86_64_landed_kernel _kos_landed_name)
    target_link_libraries(kickos_core INTERFACE
      "$<TARGET_OBJECTS:${_kos_boot_name}>" "$<TARGET_OBJECTS:${_kos_landed_name}>")
  endif()
  set(_kickos_scripted_link FALSE)
  if(NOT (KICKOS_ARCH STREQUAL "sim") AND (KICKOS_LINKER_SCRIPT OR KICKOS_ARCH STREQUAL "x86_64"))
    set(_kickos_scripted_link TRUE)
  endif()

  # The leaf an app links, KickOS::kernel, with its system target beside it. The KickOS
  # toolchain's nano C++ archives keep the unwind path on either newlib profile.
  add_library(kickos_kernel_leaf INTERFACE)
  set_target_properties(kickos_kernel_leaf PROPERTIES EXPORT_NAME kernel)
  target_link_libraries(kickos_kernel_leaf INTERFACE kickos_core)
  if(TARGET kickos_string)
    kickos_export_name(kickos_string _kickos_string_name)
    target_link_libraries(kickos_kernel_leaf INTERFACE "$<TARGET_OBJECTS:${_kickos_string_name}>")
  endif()
  # Every system target's table defines the symbol (<kickos/sys/table.h>), so a link of none
  # fails naming it and a link of two fails on its second definition.
  target_link_options(kickos_kernel_leaf INTERFACE
    "LINKER:--require-defined=${KICKOS_C_SYMBOL_PREFIX}kickos_link_one_system_target")
  # The split rv64 image's gp anchors the APP's small data and an unprivileged thread writes
  # gp, so no kernel access may resolve through it. Only gp relaxation makes such an access,
  # out of an ordinary upper/lower pair landing within gp +/- 0x800.
  if(KICKOS_ARCH STREQUAL "rv64imac" AND KICKOS_HAVE_ASPACE)
    target_link_options(kickos_kernel_leaf INTERFACE "LINKER:--no-relax-gp")
  endif()
  if(_kickos_scripted_link)
    target_link_libraries(kickos_kernel_leaf INTERFACE
      "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/cmake/kernel_leaf.ld>"
      "$<INSTALL_INTERFACE:$<INSTALL_PREFIX>/${CMAKE_INSTALL_LIBDIR}/kickos/kernel/kernel_leaf.ld>")
    install(FILES cmake/kernel_leaf.ld DESTINATION "${CMAKE_INSTALL_LIBDIR}/kickos/kernel")
  endif()
  kickos_export_name(kickos_cxx_throw _kickos_cxx_throw_name)
  # Keep the MCU runtime in the rescan group so newlib can resolve _sbrk
  # from kickos_user after _sbrk_r is extracted.
  if(KICKOS_ARCH STREQUAL "sim")
    target_link_libraries(kickos_kernel_leaf INTERFACE "$<LINK_GROUP:RESCAN,${_kickos_group}>")
  elseif(KICKOS_ARCH STREQUAL "x86_64")
    # No compiler driver appends the toolchain's libraries, so the group names them.
    target_link_libraries(kickos_kernel_leaf INTERFACE
      "$<LINK_GROUP:RESCAN,${_kickos_group},stdc++,supc++,c,m,gcc,${_kickos_cxx_throw_name}>")
  else()
    target_link_libraries(kickos_kernel_leaf INTERFACE
      "$<LINK_GROUP:RESCAN,${_kickos_group},stdc++,supc++,c>")
  endif()
  # In-tree TUs get __KickOS__ from the add_compile_definitions above; this is what carries
  # it to find_package consumers.
  target_compile_definitions(kickos_core INTERFACE __KickOS__=1)
  if(NOT (KICKOS_ARCH STREQUAL "sim"))
    # Link the custom terminate handler as an object before libstdc++.
    add_library(kickos_cxx_rt OBJECT system/cxx/vterminate.cc)
    target_compile_features(kickos_cxx_rt PRIVATE cxx_std_20)
    target_compile_options(kickos_cxx_rt PRIVATE ${KICKOS_WARN_FLAGS} -fno-exceptions -fno-rtti)
    # An object library links nothing: kickos_user/kickos_kernel here are usage
    # requirements (include dirs) only, and PRIVATE keeps them off consumers.
    target_link_libraries(kickos_cxx_rt PRIVATE kickos_core kickos_user kickos_kernel)
    # Extracted only through --wrap=__cxa_allocate_exception; nothing may reference it directly.
    add_library(kickos_cxx_throw_objects OBJECT system/cxx/exception_report.cc)
    target_compile_features(kickos_cxx_throw_objects PRIVATE cxx_std_20)
    target_compile_options(kickos_cxx_throw_objects PRIVATE
      ${KICKOS_WARN_FLAGS} -fexceptions -frtti)
    target_link_libraries(kickos_cxx_throw_objects PRIVATE kickos_core kickos_user kickos_kernel)
    add_library(kickos_cxx_throw STATIC $<TARGET_OBJECTS:kickos_cxx_throw_objects>)

    kickos_export_name(kickos_user _kickos_user_name)
    kickos_export_name(kickos_cxx_rt _kickos_cxx_rt_name)
    # Full C++: omitting -nostdlib++ lets the g++ driver auto-link the toolchain
    # libstdc++/libsupc++ (EH, RTTI, new/delete) over newlib.
    target_compile_options(kickos_kernel_leaf INTERFACE
      "$<$<COMPILE_LANGUAGE:CXX>:-fexceptions;-frtti>")
    # A second rescan group, one layer out: the driver-appended libstdc++'s operator new
    # must reach down to `_sbrk`. Listed after kickos_core so the core group resolves first.
    # x86_64 has no driver, and its first group holds the C++ libraries.
    if(NOT KICKOS_ARCH STREQUAL "x86_64")
      target_link_libraries(kickos_kernel_leaf INTERFACE
        "$<LINK_GROUP:RESCAN,stdc++,supc++,c,${_kickos_user_name},${_kickos_cxx_throw_name}>")
    endif()
    target_link_libraries(kickos_kernel_leaf INTERFACE "$<TARGET_OBJECTS:${_kickos_cxx_rt_name}>")
    target_link_options(kickos_kernel_leaf INTERFACE "LINKER:--wrap=__cxa_allocate_exception")
  endif()

  # kickos_core is a link dependency of the leaf, so it must be in the export set.
  kickos_export_targets(kickos_core kickos_kernel_leaf)
  if(TARGET kickos_cxx_rt)
    kickos_export_targets(kickos_cxx_rt kickos_cxx_throw)
  endif()
endif()

if(KICKOS_ARCH STREQUAL "x86_64")
  include(cmake/x86_64_boot.cmake)
endif()

# Apps need a chip target (or the sim); a board whose chip is named but not yet brought up
# builds the libraries only.
if(KICKOS_BUILD_APPS
   AND ((KICKOS_ARCH STREQUAL "sim") OR (KICKOS_CHIP AND TARGET kickos_chip_${KICKOS_CHIP})))
  add_subdirectory(user/apps)
endif()

# Register tests after app targets exist.
if(KICKOS_BUILD_TESTS)
  add_subdirectory(tests/integration)
  add_subdirectory(tests/static)
endif()

if(KICKOS_BUILD_TESTS AND TARGET kickos_kernel_leaf AND KICKOS_ARCH MATCHES "^(sim|x86_64)$")
  add_subdirectory(tests/composition)
endif()

# Resolve after system providers and app demand declarations are available.
kickos_cap_table_resolve(KICKOS_CAP_TABLE_SLOTS KICKOS_CAP_TABLE_CHILD_WIDTH
                         KICKOS_CAP_TABLE_REPLY_MAX)
# Every generated header the package installs.
set(_kos_generated_headers board_config.h chip_mmap.h chip_limits.h config/mpu_geometry.h
                           sys/exit_status.h)
foreach(_in kernel/include/kickos/config/cap_width.h kernel/include/kickos/config/amp_ports.h
            kernel/include/kickos/config/priorities.h kernel/include/kickos/config/stack_geometry.h
            user/include/kickos/sys/driver_geometry.h user/include/kickos/sys/table_version.h
            user/include/kickos/sys/init_geometry.h)
  string(REGEX REPLACE "^[a-z]+/include/kickos/" "" _h "${_in}")
  configure_file("${PROJECT_SOURCE_DIR}/${_in}.in"
                 "${PROJECT_BINARY_DIR}/generated/include/kickos/${_h}" @ONLY)
  list(APPEND _kos_generated_headers "${_h}")
endforeach()
if(TARGET kickos_core)
  # The in-tree include_directories at the top of this file does not reach an
  # add_subdirectory/FetchContent consumer's own targets; this does.
  target_include_directories(kickos_core INTERFACE
    "$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
endif()

# After every subdirectory, so the driver catalogue is whole.
kickos_export_manifest("${PROJECT_BINARY_DIR}/export")
# The same name the installed package sets, for a consumer built beside this tree.
set(KICKOS_MANIFEST "${PROJECT_BINARY_DIR}/export/manifest.yaml" CACHE INTERNAL
    "The export manifest a composition is admitted against")
# KickOS::system_default: the export's default composition as a system target, so a build whose
# default no longer fits fails its configure.
if(TARGET kickos_kernel_leaf)
  kickos_compose(kickos_system_default
                 "${PROJECT_BINARY_DIR}/export/boards/${KICKOS_BOARD}/composition.yaml"
                 EXPORT_NAME system_default)
  # The apps' own systems, which kickos_app_system (user/apps/CMakeLists.txt) records.
  get_property(_kickos_app_systems GLOBAL PROPERTY KICKOS_APP_SYSTEMS)
  foreach(_system IN LISTS _kickos_app_systems)
    get_property(_composition GLOBAL PROPERTY KICKOS_APP_SYSTEM_${_system})
    kickos_compose(${_system} "${_composition}")
  endforeach()
endif()
# Every image this configure emits as `<image>|<stdout>|<judge>|<args>`: the stdout is `kernel`,
# the packaged driver its system names, or `-` for an image composing no system of its own; the
# judge is the gate script KICKOS_IMAGE_JUDGE names, `emulator` or `emulator-owed` for an image
# kickos_emulator_judged marks, by whether a test this build runs names it, `human` or
# `inapplicable` for one kickos_human_judged or kickos_inapplicable marks, or `-`; the args are the
# judge's, `;`-separated. The bench reads it. Deferred behind the systems a directory composes
# once the manifest is written.
function(_kickos_write_images)
  get_property(_images GLOBAL PROPERTY KICKOS_EMITTED_IMAGES)
  get_property(_run_images GLOBAL PROPERTY KICKOS_RUN_IMAGES)
  set(_rows "")
  set(_unbooted "")
  foreach(_image IN LISTS _images)
    get_target_property(_judge ${_image} KICKOS_IMAGE_JUDGE)
    if(NOT _judge)
      set(_judge "-")
    endif()
    if(_judge STREQUAL "emulator" AND NOT _image IN_LIST _run_images)
      set(_judge "emulator-owed")
    endif()
    get_target_property(_judge_args ${_image} KICKOS_IMAGE_JUDGE_ARGS)
    if(NOT _judge_args)
      set(_judge_args "")
    endif()
    set(_stdout "-")
    get_target_property(_image_system ${_image} KICKOS_APP_SYSTEM)
    if(_image_system AND TARGET ${_image_system})
      get_target_property(_stdout ${_image_system} KICKOS_SYSTEM_STDOUT)
      if(NOT _stdout)
        message(FATAL_ERROR "${_image}: its system ${_image_system} states no KICKOS_SYSTEM_STDOUT")
      endif()
    else()
      get_target_property(_image_links ${_image} LINK_LIBRARIES)
      if("KickOS::system_default" IN_LIST _image_links)
        set(_stdout kernel)
      endif()
    endif()
    string(APPEND _rows "${_image}|${_stdout}|${_judge}|${_judge_args}\n")
    get_target_property(_why ${_image} KICKOS_IMAGE_UNBOOTED)
    if(NOT _image IN_LIST _run_images AND NOT _why
       AND NOT _judge MATCHES "^(human|inapplicable)$")
      list(APPEND _unbooted ${_image})
    endif()
  endforeach()
  kickos_write_if_changed("${PROJECT_BINARY_DIR}/kickos-images.txt" "${_rows}")
  # Where this build boots its images, each one a test boots or one that says why none can.
  kickos_qemu_machine("${KICKOS_BOARD}" _env _machine)
  if(KICKOS_BUILD_TESTS AND KICKOS_BUILD_INTEGRATION_TESTS
     AND (KICKOS_ARCH STREQUAL "sim" OR NOT _machine STREQUAL "") AND _unbooted)
    list(JOIN _unbooted " " _unbooted)
    message(FATAL_ERROR "this build boots its images, and no test boots ${_unbooted}: register "
      "the test that does, or state why none can (kickos_unbooted, kickos_inapplicable, "
      "kickos_human_judged)")
  endif()
  # Every image's link map, one per line, for the host gates that read them all.
  if(NOT KICKOS_ARCH STREQUAL "sim")
    set(_maps "")
    foreach(_image IN LISTS _images)
      get_target_property(_map ${_image} KICKOS_IMAGE_MAP)
      string(APPEND _maps "${_map}\n")
    endforeach()
    file(GENERATE OUTPUT "${PROJECT_BINARY_DIR}/kickos-image-maps.txt" CONTENT "${_maps}")
  endif()
endfunction()
cmake_language(DEFER DIRECTORY "${PROJECT_SOURCE_DIR}" CALL _kickos_write_images)
# --- Install / export: the dependency-inversion package --------------------
include(CMakePackageConfigHelpers)

# CeCILL-C is file-level copyleft: ship the license with the package.
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/licenses/${PROJECT_NAME}")

set(_kos_hdr_exclude "")
if(NOT KICKOS_HAVE_MPU)
  set(_kos_hdr_exclude PATTERN "mpu_encoded.h" EXCLUDE)
endif()
install(DIRECTORY
          "${CMAKE_CURRENT_SOURCE_DIR}/include/"
          "${CMAKE_CURRENT_SOURCE_DIR}/arch/include/"
          "${KICKOS_ARCH_INCLUDE_DIR}/"
          "${CMAKE_CURRENT_SOURCE_DIR}/kernel/include/"
          "${CMAKE_CURRENT_SOURCE_DIR}/lib/include/"
          "${CMAKE_CURRENT_SOURCE_DIR}/user/include/"
          "${CMAKE_CURRENT_SOURCE_DIR}/system/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        PATTERN "*.h.in" EXCLUDE
        ${_kos_hdr_exclude})

# The generated headers the prebuilt libraries were compiled against: a consumer including a
# kernel header under another cap width disagrees with them on sizeof(Thread).
foreach(_h IN LISTS _kos_generated_headers)
  get_filename_component(_d "${CMAKE_INSTALL_INCLUDEDIR}/kickos/${_h}" DIRECTORY)
  install(FILES "${PROJECT_BINARY_DIR}/generated/include/kickos/${_h}" DESTINATION "${_d}")
endforeach()

# The chip's constants, which config/board.h requires for KICKOS_MAX_IRQ (see the check at
# the top of this file).
if(KICKOS_CHIP_INCLUDE_DIR)
  install(DIRECTORY "${KICKOS_CHIP_INCLUDE_DIR}/"
          DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
endif()
if(KICKOS_BOARD_INCLUDE_DIR)
  install(DIRECTORY "${KICKOS_BOARD_INCLUDE_DIR}/"
          DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
endif()

install(EXPORT KickOSTargets
        NAMESPACE KickOS::
        FILE KickOSTargets.cmake
        DESTINATION "${KICKOS_CMAKE_DIR}")

# Export the class-backend selections used by this build.
get_property(_kos_classes GLOBAL PROPERTY KICKOS_CLASS_BACKEND_CLASSES)
set(KICKOS_CLASS_BACKEND_DECLS "")
if(_kos_classes)
  list(REMOVE_DUPLICATES _kos_classes)
  foreach(_kos_class IN LISTS _kos_classes)
    kickos_class_backend("${_kos_class}" _kos_backend)
    string(APPEND KICKOS_CLASS_BACKEND_DECLS
      "kickos_select_class_backend(${_kos_class} ${_kos_backend})\n")
  endforeach()
endif()

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/KickOSConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/KickOSConfig.cmake"
  INSTALL_DESTINATION "${KICKOS_CMAKE_DIR}")

# ExactVersion: at 0.x CMake treats the whole 0 series as one compatibility bucket, so a
# request for an older 0.x would configure green against this one. Relax at the ABI freeze.
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/KickOSConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY ExactVersion)

# Where kickos_compose finds the manifest, with the descriptions and default composition it
# names beside it.
install(FILES "${PROJECT_BINARY_DIR}/export/manifest.yaml" DESTINATION "${KICKOS_CMAKE_DIR}")
if(EXISTS "${PROJECT_BINARY_DIR}/export/platform")
  install(DIRECTORY "${PROJECT_BINARY_DIR}/export/platform" DESTINATION "${KICKOS_CMAKE_DIR}")
endif()
if(EXISTS "${PROJECT_BINARY_DIR}/export/boards")
  install(DIRECTORY "${PROJECT_BINARY_DIR}/export/boards" DESTINATION "${KICKOS_CMAKE_DIR}")
endif()
# The host tool that admits a composition against that manifest and emits its table.
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools/compose/kickos_compose"
        DESTINATION "${KICKOS_CMAKE_DIR}/compose"
        FILES_MATCHING PATTERN "*.py")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/tools/compose/pyproject.toml"
              "${CMAKE_CURRENT_SOURCE_DIR}/tools/compose/uv.lock"
        DESTINATION "${KICKOS_CMAKE_DIR}/compose")

# kickos.cmake include()s cap_table.cmake, driver_geometry.cmake, driver_metadata.cmake,
# heap_symbol.cmake, qemu_cpu.cmake and compose.cmake list-dir-relative, so they must land
# beside it.
install(FILES
          "${CMAKE_CURRENT_BINARY_DIR}/KickOSConfig.cmake"
          "${CMAKE_CURRENT_BINARY_DIR}/KickOSConfigVersion.cmake"
          "${CMAKE_CURRENT_SOURCE_DIR}/cmake/kickos.cmake"
          "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cap_table.cmake"
          "${CMAKE_CURRENT_SOURCE_DIR}/cmake/driver_geometry.cmake"
          "${CMAKE_CURRENT_SOURCE_DIR}/cmake/driver_metadata.cmake"
          "${CMAKE_CURRENT_SOURCE_DIR}/cmake/heap_symbol.cmake"
          "${CMAKE_CURRENT_SOURCE_DIR}/cmake/qemu_cpu.cmake"
          "${CMAKE_CURRENT_SOURCE_DIR}/cmake/compose.cmake"
        DESTINATION "${KICKOS_CMAKE_DIR}")

# Install the toolchain that configured this build, with board.cmake beside it.
if(NOT KICKOS_ARCH STREQUAL "sim")
  if(NOT CMAKE_TOOLCHAIN_FILE)
    message(FATAL_ERROR "KickOS: board '${KICKOS_BOARD}' is a cross target configured with no "
      "CMAKE_TOOLCHAIN_FILE, so the package would ship none and no out-of-tree consumer could "
      "build against it.")
  endif()
  set(_kos_tc_install "${CMAKE_TOOLCHAIN_FILE}")
  # Include recursively referenced toolchain fragments. Exclude board.cmake,
  # which is generated below.
  set(_kos_tc_pending "${CMAKE_TOOLCHAIN_FILE}")
  set(_kos_tc_seen "")
  while(_kos_tc_pending)
    list(POP_FRONT _kos_tc_pending _kos_tc_from)
    file(READ "${_kos_tc_from}" _kos_tc_text)
    string(REGEX MATCHALL "\\\${CMAKE_CURRENT_LIST_DIR}/[A-Za-z0-9_.+-]+\\.(cmake|h)"
           _kos_tc_incs "${_kos_tc_text}")
    foreach(_kos_inc IN LISTS _kos_tc_incs)
      string(REPLACE "\${CMAKE_CURRENT_LIST_DIR}/" "" _kos_inc "${_kos_inc}")
      if(_kos_inc STREQUAL "board.cmake" OR _kos_inc IN_LIST _kos_tc_seen)
        continue()
      endif()
      list(APPEND _kos_tc_seen "${_kos_inc}")
      if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/${_kos_inc}")
        message(FATAL_ERROR "KickOS: ${_kos_tc_from} includes ${_kos_inc}, which is "
          "not in cmake/, so the package would ship a toolchain file that cannot configure.")
      endif()
      list(APPEND _kos_tc_install "${CMAKE_CURRENT_SOURCE_DIR}/cmake/${_kos_inc}")
      list(APPEND _kos_tc_pending "${CMAKE_CURRENT_SOURCE_DIR}/cmake/${_kos_inc}")
    endforeach()
  endwhile()
  list(REMOVE_DUPLICATES _kos_tc_install)
  install(FILES ${_kos_tc_install} DESTINATION "${KICKOS_CMAKE_DIR}")
# The CPU baseline is baked in: a package has no arch/ tree for the toolchain file to read a
# cpu.cmake out of. Only the knobs the descriptor leaves to the chip are appended.
  set(_kos_baked "${PROJECT_BINARY_DIR}/generated/board.cmake")
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/boards/${KICKOS_BOARD}/board.cmake" _kos_desc)
  set(_kos_bake "")
  if(DEFINED KICKOS_MCPU AND NOT _kos_desc MATCHES "(^|\n)set\\(KICKOS_MCPU[ \t]")
    string(JOIN " " _kos_mcpu_txt ${KICKOS_MCPU})
    string(APPEND _kos_bake "set(KICKOS_MCPU ${_kos_mcpu_txt})\n")
  endif()
  if(DEFINED KICKOS_MFLOAT_ABI AND NOT _kos_desc MATCHES "(^|\n)set\\(KICKOS_MFLOAT_ABI[ \t]")
    string(APPEND _kos_bake "set(KICKOS_MFLOAT_ABI ${KICKOS_MFLOAT_ABI})\n")
  endif()
  if(NOT _kos_bake STREQUAL "")
    string(APPEND _kos_desc
      "\n# Appended at install: the CPU baseline this package was built with, which in tree\n"
      "# comes from arch/${KICKOS_ARCH_FAMILY}/chip/${KICKOS_CHIP}/cpu.cmake.\n"
      "${_kos_bake}")
  endif()
  if(KICKOS_BOARD_NEWLIB STREQUAL "nano")
    if(KICKOS_FULL_NEWLIB)
      set(_kos_package_newlib full)
    else()
      set(_kos_package_newlib nano)
    endif()
    string(APPEND _kos_desc
      "\n# Appended at install: the newlib profile this package's libraries were built with,\n"
      "# the only one the shipped toolchain file accepts.\n"
      "set(KICKOS_PACKAGE_NEWLIB ${_kos_package_newlib})\n")
  endif()
  kickos_write_if_changed("${_kos_baked}" "${_kos_desc}")
  install(FILES "${_kos_baked}" DESTINATION "${KICKOS_CMAKE_DIR}")
endif()
