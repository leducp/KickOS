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

# THE LOW EDGE, and the one arm here that expects the trap to be ACCEPTED: the PSP is parked
# with exactly the room the SVC site asks for, which is legal once svc_trampoline moves SP
# onto the caller's own kernel block before it calls anything. What it witnesses is the
# stronger statement, that the syscall returns AND the band the child poisoned below that PSP,
# inside its own stack, is intact.
#
# BUILT ONLY WHERE THE BLOCKS EXIST, because the premise is the transfer. On an armv7m board
# that kept its red zone the same park is REFUSED and correctly so, and what covers that board
# is check_trap_redzone.sh, which measures the descent the red zone reserves for on every
# preset it is registered for. main.cc #errors if this arm is built without the blocks.
if(KICKOS_KERNEL_STACKS)
  kickos_add_diagnostic_app(pspguard_svcdepth
    SOURCES main.cc
    BOARD   ${KICKOS_BOARD})
  target_compile_definitions(pspguard_svcdepth PRIVATE KICKOS_PSPGUARD_MODE=5)
endif()

# The PendSV arms, armv7m only. They provoke an ASYNCHRONOUS switch, so each needs a
# higher-priority ticker thread as a second spawn. THE REASON IS THE CLASS AND NOT THE BOARD:
# the v6-M PendSV class charges the 32-byte push alone, its switcher descending on SP_main,
# so the figure these arms check is not one armv6m has. The thread count is no longer part of
# it: microbit carries four slots and arena to spare, which the block-leg arm below uses on
# that very board. main.cc #errors if one of these is built for armv6m anyway.
# THE BLOCK LEG'S ROOM BOUND, and BOTH arches carry that leg, so both carry the arm. It needs a
# board that carves kernel blocks and does NOT enforce: with enforcement the hardware refuses
# the entry stacking through a kernel-aimed PSP before any handler runs, so the leg is never
# reached and the arm would witness the MemManage instead; without the blocks there is nothing
# to park in. That is the flat armv7m presets plus every armv6m board, armv6m selecting
# ARCH_KERNEL_STACKS_MANDATORY. Same condition main.cc #errors on.
#
# A PendSV arm, so it spawns the ticker too, and it is deliberately outside the armv7m-only
# block below: that exclusion is about where the in-stack arms park, not about thread count.
if(KICKOS_KERNEL_STACKS AND NOT KICKOS_HAVE_MPU)
  kickos_add_diagnostic_app(pspguard_block
    SOURCES main.cc
    BOARD   ${KICKOS_BOARD})
  target_compile_definitions(pspguard_block PRIVATE KICKOS_PSPGUARD_MODE=6)
endif()

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
# measured the FP block refused it". Mode 5 expects no refusal at all and takes the same
# argv shape with the count unused, so a call site cannot silently become the wrong arm.
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
# NEST_SVC_DISPATCH exists on armv7m only, and only the KICKOS_KERNEL_STACKS 0 branch below
# reads it. armv6m cannot reach that branch: Kconfig gives it range 1 1 through
# ARCH_KERNEL_STACKS_MANDATORY, so scraping it there would demand a macro that arch has no
# reason to define.
set(_pg_macros NEST_SVC KERNEL_DEPTH_SVC)
if(NOT KICKOS_KERNEL_STACKS)
  list(APPEND _pg_macros NEST_SVC_DISPATCH)
endif()
foreach(_pg_macro ${_pg_macros})
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
# THE SAME LADDER THE HEADER RESOLVES, and it has to be here rather than read out of one
# macro because the gate scrapes plain integers and cannot evaluate the header's #if. At
# KICKOS_KERNEL_STACKS 1 the dispatch is on the kernel block and the site charges the window
# alone; at 0 it charges the window plus the dispatch.
if(KICKOS_KERNEL_STACKS)
  set(_pg_svc_need "${_pg_NEST_SVC}")
else()
  math(EXPR _pg_svc_need "${_pg_NEST_SVC_DISPATCH} + ${_pg_KERNEL_DEPTH_SVC}")
endif()

if(KICKOS_BOARD STREQUAL "microbit")
  # The armv6m pair, and they are opposite claims now: mode 2 is refused at the SVC class's
  # whole extent, mode 5 is ACCEPTED at the low edge of that same extent and answers with the
  # band verdict instead of a refusal. Both declare `contained`, the refusal costing this
  # backend the offending thread and not the system since it gained the seam.
  kickos_add_qemu_test(NAME microbit_pspguard_svc TARGET pspguard_svc
    BOARD microbit
    SCRIPT "${_pg_gate}" ARGS 2 ${_pg_svc_need} SVCall "no room below" ${_pg_banner}
                         contained)
  kickos_add_qemu_test(NAME microbit_pspguard_svcdepth TARGET pspguard_svcdepth
    BOARD microbit
    SCRIPT "${_pg_gate}" ARGS 5 ${_pg_svc_need} SVCall "no room below" ${_pg_banner}
                         contained)
  # The block leg's room bound. THE BOARD THE HAZARD IS REAL ON: it carves blocks and has no
  # MPU, so a thread can aim its own PSP into kernel .bss and nothing refuses the entry. 32 is
  # KICKOS_ARMV6M_TRAP_FRAME, this arch's whole PendSV push, written out the way the MPS2 arms
  # write theirs. The refusal classifies as `under stack_lo`, the blocks sitting below every
  # arena stack, and main.cc refuses to run the arm if that stops being true.
  if(TARGET pspguard_block)
    kickos_add_qemu_test(NAME microbit_pspguard_block TARGET pspguard_block
      BOARD microbit
      SCRIPT "${_pg_gate}" ARGS 6 32 PendSV "under stack_lo" ${_pg_banner} contained)
  endif()
  return()
endif()

kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_pspguard TARGET pspguard
  BOARD ${KICKOS_BOARD}
  SCRIPT "${_pg_gate}" ARGS 0 36 PendSV "no room below" ${_pg_banner} contained)
kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_pspguard_svc TARGET pspguard_svc
  BOARD ${KICKOS_BOARD}
  SCRIPT "${_pg_gate}" ARGS 2 ${_pg_svc_need} SVCall "no room below" ${_pg_banner} contained)

kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_pspguard_svcdepth
  TARGET pspguard_svcdepth
  BOARD ${KICKOS_BOARD}
  SCRIPT "${_pg_gate}" ARGS 5 ${_pg_svc_need} SVCall "no room below" ${_pg_banner} contained)
# The block leg's room bound, on the MPS2 presets that carve blocks and do NOT enforce. Built
# only there, hence the TARGET guard: with enforcement the app is not compiled at all. 36 is
# KICKOS_ARMV7M_TRAP_FRAME, this arch's PendSV push, written out the way the arms above write
# theirs. The refusal classifies as `under stack_lo`, the blocks sitting below every arena
# stack, and main.cc refuses to run the arm if that stops being true.
if(TARGET pspguard_block)
  kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_pspguard_block TARGET pspguard_block
    BOARD ${KICKOS_BOARD}
    SCRIPT "${_pg_gate}" ARGS 6 36 PendSV "under stack_lo" ${_pg_banner} contained)
endif()
kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_pspguard_above TARGET pspguard_above
  BOARD ${KICKOS_BOARD}
  SCRIPT "${_pg_gate}" ARGS 3 36 PendSV "at or above stack_hi" ${_pg_banner} contained)
kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_pspguard_below TARGET pspguard_below
  BOARD ${KICKOS_BOARD}
  SCRIPT "${_pg_gate}" ARGS 4 36 PendSV "under stack_lo" ${_pg_banner} contained)
if(TARGET pspguard_fp)
  kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_pspguard_fp TARGET pspguard_fp
    BOARD ${KICKOS_BOARD}
    SCRIPT "${_pg_gate}" ARGS 1 100 PendSV "no room below" ${_pg_banner} contained)

  # THE SWITCH CHAIN ACROSS A CONTAINED REFUSAL. Containment performs a physical swap without
  # going through either switcher, so it emits its own SWITCH record; kicktrace's structural
  # pass walks the chain requiring each `from` to equal the previous `to`, and a swap that
  # emitted nothing breaks the NEXT record rather than its own. telemetry_qemu_structural
  # covers the ordinary PendSV tail on tele_pingpong and never refuses a PSP, so only this
  # arm reaches the containment emit. Registered where telemetry is actually compiled.
  if(KICKOS_BOARD STREQUAL "qemu" AND KICKOS_TELEMETRY)
    kickos_add_qemu_test(NAME telemetry_qemu_contained TARGET pspguard_fp BOARD qemu
      SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/telemetry/check_qemu.py"
      TIMEOUT 40 ARGS "${PROJECT_SOURCE_DIR}/tools/kicktrace.py")
  endif()
endif()
