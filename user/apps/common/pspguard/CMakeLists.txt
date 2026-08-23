# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The PSP-BOUNDS security regression. Both ARM backends push a callee block BELOW the live
# PSP in handler mode, where nothing refuses it, so both carry the class; main.cc moves SP
# with ARM asm and #errors anywhere else.
if(NOT (KICKOS_ARCH STREQUAL "armv7m" OR KICKOS_ARCH STREQUAL "armv6m"))
  return()
endif()

# Every MPU posture: the bounds live in the saved context, not in a region descriptor, so the
# refusal is identical. Enforcement decides only whether the neighbouring word belongs to a
# THREAD, which is the escalation and not the refusal.

# The SYNCHRONOUS pair, on BOTH backends: the SECOND guarded site, the syscall trap, which
# pushes the same block through the same PSP and, on its slow path, hands that PSP to a
# PRIVILEGED trampoline. A guard installed in the switcher alone leaves it open, so this arm
# asserts the refusal names SVCall.
kickos_add_diagnostic_app(pspguard_svc
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
if(NOT TARGET pspguard_svc)
  return() # diagnostics off -> app not built; skip its tests
endif()
target_compile_definitions(pspguard_svc PRIVATE KICKOS_PSPGUARD_MODE=2)

# The SVC slow path's KERNEL DESCENT. The push leg ACCEPTS this arm's PSP, so the only bound
# that can refuse it is one that also charges what syscall_dispatch descends below the frame
# while running privileged in thread mode on that same PSP.
kickos_add_diagnostic_app(pspguard_svcdepth
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
target_compile_definitions(pspguard_svcdepth PRIVATE KICKOS_PSPGUARD_MODE=5)

# The PendSV arms, armv7m only. They provoke an ASYNCHRONOUS switch, so each needs a
# higher-priority ticker thread as a second spawn, and the runnable armv6m board is microbit,
# whose defconfig states CONFIG_KICKOS_MAX_THREADS=2 and no arena slack; the v6-M PendSV
# class also charges the 32-byte push alone, its switcher descending on SP_main. main.cc
# #errors if one of these is built for armv6m anyway.
if(KICKOS_ARCH STREQUAL "armv7m")
  kickos_add_diagnostic_app(pspguard
    SOURCES main.cc
    BOARD   ${KICKOS_BOARD})
  target_compile_definitions(pspguard PRIVATE KICKOS_PSPGUARD_MODE=0)

  # The FP arm is a SEPARATE binary: a run observes one refusal. The FPU is read off the
  # board's own compiler flags, and main.cc #errors without __ARM_FP.
  string(FIND "${KICKOS_MCPU}" "mfpu" _pg_has_fpu)
  if(_pg_has_fpu GREATER -1)
    kickos_add_diagnostic_app(pspguard_fp
      SOURCES main.cc
      BOARD   ${KICKOS_BOARD})
    target_compile_definitions(pspguard_fp PRIVATE KICKOS_PSPGUARD_MODE=1)
  endif()

  # The two bound legs the in-stack arms above cannot reach: a PSP with room to spare BELOW
  # it, refusable only by the upper bound, and a PSP under the stack, refusable only by the
  # lower one. Each lands in a granted domain region, so hardware stacking succeeds and the
  # software push is what gets refused.
  kickos_add_diagnostic_app(pspguard_above
    SOURCES main.cc
    BOARD   ${KICKOS_BOARD})
  target_compile_definitions(pspguard_above PRIVATE KICKOS_PSPGUARD_MODE=3)
  kickos_add_diagnostic_app(pspguard_below
    SOURCES main.cc
    BOARD   ${KICKOS_BOARD})
  target_compile_definitions(pspguard_below PRIVATE KICKOS_PSPGUARD_MODE=4)
endif()

if(NOT KICKOS_BUILD_TESTS)
  return()
endif()
if(NOT (KICKOS_BOARD IN_LIST KICKOS_QEMU_MPS2_BOARDS OR KICKOS_BOARD STREQUAL "microbit"))
  return()
endif()

# The byte count the refusal must report, carried by the caller and not sniffed by the
# script: only this expectation separates "some leg refused the PSP" from "the leg that
# measured the FP block refused it".
set(_pg_gate "${PROJECT_SOURCE_DIR}/tests/integration/check_pspguard.sh")
# The SVC site's expected byte count is the whole SVC extent, read out of the header the
# guard on THIS arch reads, and the banner is the one that arch's fault reporter prints.
# Both macros must stay the plain integers tests/static/check_trap_redzone.sh scrapes: a
# header that stopped spelling them that way stops the configure instead of quietly pinning
# a stale expectation. The PendSV arms' 36 and 100 are KICKOS_ARMV7M_TRAP_FRAME and
# _FRAME_MAX, written out.
if(KICKOS_ARCH STREQUAL "armv6m")
  set(_pg_hdr
      "${PROJECT_SOURCE_DIR}/arch/arm/armv6m/include/kickos/arch/armv6m_trap_stack.h")
  set(_pg_pfx KICKOS_ARMV6M_TRAP)
  set(_pg_banner ARMV6M)
else()
  set(_pg_hdr
      "${PROJECT_SOURCE_DIR}/arch/arm/armv7m/include/kickos/arch/armv7m_trap_stack.h")
  set(_pg_pfx KICKOS_ARMV7M_TRAP)
  set(_pg_banner ARMV7M)
endif()
foreach(_pg_macro NEST_SVC KERNEL_DEPTH_SVC)
  file(STRINGS "${_pg_hdr}" _pg_hit
       REGEX "^#define ${_pg_pfx}_${_pg_macro} +[0-9]+$")
  list(LENGTH _pg_hit _pg_n)
  if(NOT _pg_n EQUAL 1)
    message(FATAL_ERROR
      "pspguard: ${_pg_pfx}_${_pg_macro} is not one plain integer in ${_pg_hdr}, "
      "so the SVC expectation cannot be derived from the figure the guard enforces")
  endif()
  string(REGEX REPLACE "^.* " "" _pg_${_pg_macro} "${_pg_hit}")
endforeach()
math(EXPR _pg_svc_need "${_pg_NEST_SVC} + ${_pg_KERNEL_DEPTH_SVC}")

if(KICKOS_BOARD STREQUAL "microbit")
  # The armv6m pair. Both arms are refused at the SVC class's whole extent: the site charges
  # that figure above the arm selector, so mode 2, whose PSP has no room for even the push,
  # reports the same count as mode 5, whose PSP has room for the push and not for the
  # descent.
  kickos_add_qemu_test(NAME microbit_pspguard_svc TARGET pspguard_svc
    BOARD microbit
    SCRIPT "${_pg_gate}" ARGS 2 ${_pg_svc_need} SVCall "no room below" ${_pg_banner})
  kickos_add_qemu_test(NAME microbit_pspguard_svcdepth TARGET pspguard_svcdepth
    BOARD microbit
    SCRIPT "${_pg_gate}" ARGS 5 ${_pg_svc_need} SVCall "no room below" ${_pg_banner})
  return()
endif()

kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_pspguard TARGET pspguard
  BOARD ${KICKOS_BOARD}
  SCRIPT "${_pg_gate}" ARGS 0 36 PendSV "no room below" ${_pg_banner})
kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_pspguard_svc TARGET pspguard_svc
  BOARD ${KICKOS_BOARD}
  SCRIPT "${_pg_gate}" ARGS 2 ${_pg_svc_need} SVCall "no room below" ${_pg_banner})

kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_pspguard_svcdepth
  TARGET pspguard_svcdepth
  BOARD ${KICKOS_BOARD}
  SCRIPT "${_pg_gate}" ARGS 5 ${_pg_svc_need} SVCall "no room below" ${_pg_banner})
kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_pspguard_above TARGET pspguard_above
  BOARD ${KICKOS_BOARD}
  SCRIPT "${_pg_gate}" ARGS 3 36 PendSV "at or above stack_hi" ${_pg_banner})
kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_pspguard_below TARGET pspguard_below
  BOARD ${KICKOS_BOARD}
  SCRIPT "${_pg_gate}" ARGS 4 36 PendSV "under stack_lo" ${_pg_banner})
if(TARGET pspguard_fp)
  kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_pspguard_fp TARGET pspguard_fp
    BOARD ${KICKOS_BOARD}
    SCRIPT "${_pg_gate}" ARGS 1 100 PendSV "no room below" ${_pg_banner})
endif()
