# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Toolchain for the KickOS x86_64 target. The image firmware loads is a PE32+ UEFI
# application, built here with the HOST gcc and the HOST binutils: the target ISA is the
# host's, so only the object format differs, and GNU ld carries the PE+ emulation (i386pep)
# that turns the ELF objects into that image.
#
# This file introduces the family value "x86" (arm|rx|xtensa|riscv|arm64|x86).

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(KICKOS_BOARD "qemu-x86_64" CACHE STRING "Target board: qemu-x86_64")

# Board descriptor: sets KICKOS_ARCH / KICKOS_ARCH_FAMILY / KICKOS_CHIP and any board CPU flag.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../boards/${KICKOS_BOARD}/board.cmake")
  include("${CMAKE_CURRENT_LIST_DIR}/../boards/${KICKOS_BOARD}/board.cmake") # in-tree
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/board.cmake")
  include("${CMAKE_CURRENT_LIST_DIR}/board.cmake")
  set(KICKOS_BOARD "${KICKOS_BOARD_ID}" CACHE STRING "Target board" FORCE)
else()
  message(FATAL_ERROR "KickOS x86_64 toolchain: no board descriptor for '${KICKOS_BOARD}'")
endif()

set(_kos_cpu_chip "${CMAKE_CURRENT_LIST_DIR}/../arch/x86/chip/${KICKOS_CHIP}/cpu.cmake")
if(EXISTS "${_kos_cpu_chip}")
  include("${_kos_cpu_chip}")
endif()

if(NOT DEFINED KICKOS_MCPU)
  message(FATAL_ERROR "KickOS x86_64 toolchain: board '${KICKOS_BOARD}' resolved no CPU "
    "baseline from boards/${KICKOS_BOARD}/board.cmake or "
    "arch/x86/chip/${KICKOS_CHIP}/cpu.cmake")
endif()
set(_kos_cpu ${KICKOS_MCPU})

set(KICKOS_ARCH        "${KICKOS_ARCH}" CACHE STRING "KickOS arch backend selected by this toolchain")
set(KICKOS_ARCH_FAMILY "x86"            CACHE STRING "KickOS ISA family (arm|rx|xtensa|riscv|arm64|x86)")

set(KICKOS_MCPU_FLAGS "${_kos_cpu}" CACHE INTERNAL "Per-chip x86_64 -march baseline")

find_program(CMAKE_C_COMPILER   gcc     REQUIRED)
find_program(CMAKE_CXX_COMPILER g++     REQUIRED)
find_program(CMAKE_ASM_COMPILER gcc     REQUIRED)
find_program(CMAKE_OBJCOPY      objcopy REQUIRED)
find_program(CMAKE_OBJDUMP      objdump REQUIRED)
find_program(CMAKE_NM           nm      REQUIRED)
find_program(CMAKE_READELF      readelf REQUIRED)
find_program(CMAKE_SIZE         size)

# The image is linked by ld directly: the compiler driver cannot select the PE+ emulation,
# and the link takes no crt and no library at all.
find_program(KICKOS_X86_64_LD ld REQUIRED)

# Refused here: an ld without the PE+ emulation reports an unrecognised -m at the link step
# and nothing there says the toolchain was the problem.
execute_process(COMMAND "${KICKOS_X86_64_LD}" -V
                OUTPUT_VARIABLE _kos_ld_emulations
                ERROR_VARIABLE  _kos_ld_emulations
                RESULT_VARIABLE _kos_ld_rc)
if(NOT _kos_ld_rc EQUAL 0 OR NOT "${_kos_ld_emulations}" MATCHES "i386pep")
  message(FATAL_ERROR
    "KickOS x86_64 toolchain: ${KICKOS_X86_64_LD} lists no i386pep emulation, so it cannot "
    "write the PE32+ image UEFI loads. `ld -V` must name i386pep among its supported "
    "emulations (Debian: binutils-x86-64-linux-gnu, verified on 2.47).")
endif()

# No linker script and no startup during CMake's compiler probe: the image is linked by a
# custom command.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# -fpie is load-bearing and not a hardening flag: it keeps every reference from CODE
# PC-relative, so no instruction carries an absolute address. Built -fno-pic the same
# sources emit R_X86_64_32/32S all through the text.
#
# -mno-red-zone: privileged x86_64 text takes interrupts on its own stack.
#
# An undefined external reference stays on R_X86_64_REX_GOTPCRELX whatever this line says,
# -fvisibility=hidden covering only what a translation unit DEFINES. Each such declaration
# carries __attribute__((visibility("hidden"))), and tools/check-x86_64-no-got.sh refuses a
# survivor before every link.
#
# -mno-sse -mno-mmx -mno-80387 keep every vector and x87 register out of the generated code;
# switch.S and trap_x86_64.S save neither. They bind the COMPILER alone, the machine being
# bound in arch/x86/x86_64/entry_x86_64.cc (CR0.EM/TS/MP, CR4.OSFXSR/OSXMMEXCPT/OSXSAVE).
# The SysV x86_64 ABI passes floating point in XMM, so -mno-sse makes the compiler REFUSE a
# float or double argument or return; a future consumer of one is soft-float.
string(JOIN " " _kos_common ${_kos_cpu}
       -ffreestanding -fno-stack-protector -fno-stack-clash-protection
       -mno-red-zone -fpie -mcmodel=small -fno-ident
       -mno-sse -mno-mmx -mno-80387
       -fno-asynchronous-unwind-tables -fno-unwind-tables
       -ffunction-sections -fdata-sections)
set(CMAKE_C_FLAGS_INIT   "${_kos_common}")
set(CMAKE_CXX_FLAGS_INIT "${_kos_common} -fno-exceptions -fno-rtti")
set(CMAKE_ASM_FLAGS_INIT "${_kos_common}")

foreach(_lang C CXX ASM)
  set(CMAKE_${_lang}_LINK_GROUP_USING_RESCAN_SUPPORTED TRUE)
  set(CMAKE_${_lang}_LINK_GROUP_USING_RESCAN "LINKER:--start-group" "LINKER:--end-group")
endforeach()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
