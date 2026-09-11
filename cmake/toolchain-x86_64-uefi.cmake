# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Toolchain for the KickOS x86_64 target. The image firmware loads is a PE32+ UEFI
# application, built here with the HOST gcc and the HOST binutils: the target ISA is the
# host's, so only the object format differs, and GNU ld carries the PE+ emulation (i386pep)
# that turns the ELF objects into that image.

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

include("${CMAKE_CURRENT_LIST_DIR}/toolchain-common.cmake")

set(KICKOS_TOOLCHAIN_DEFAULT_BOARD "qemu-x86_64")
set(KICKOS_BOARD "${KICKOS_TOOLCHAIN_DEFAULT_BOARD}" CACHE STRING "Target board: qemu-x86_64")

kickos_toolchain_board_descriptor("x86_64")
kickos_toolchain_cpu_baseline("x86_64" "x86")
kickos_toolchain_export_baseline("${_kos_cpu}")

set(KICKOS_ARCH_FAMILY "x86" CACHE STRING "KickOS ISA family (arm|rx|xtensa|riscv|arm64|x86)")

# Host binutils, not a cross prefix: only the object format differs.
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
kickos_toolchain_flags_init("${_kos_common}")
string(APPEND CMAKE_CXX_FLAGS_INIT " -fno-exceptions -fno-rtti")

kickos_toolchain_bare_metal_rules()
