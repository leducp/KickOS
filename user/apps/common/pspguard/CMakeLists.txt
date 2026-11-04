# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The PSP-BOUNDS security regression. Both ARM backends push a callee block BELOW the live
# PSP in handler mode, where nothing refuses it, so both carry the class; main.cc moves SP
# with ARM asm and #errors anywhere else.

# Every MPU posture: the bounds live in the saved context, not in a region descriptor, so the
# refusal is identical. Enforcement decides only whether the neighbouring word belongs to a
# THREAD, which is the escalation and not the refusal.

# The SYNCHRONOUS pair, on BOTH backends: the SECOND guarded site, the syscall trap, which
# pushes the same block through the same PSP and, on its slow path, hands that PSP to a
# PRIVILEGED trampoline. A guard installed in the switcher alone leaves it open, so this arm
# asserts the refusal names SVCall.
# pspguard_image(<target> <mode> [<kernel symbol the image calls>])
#   One image of the family: main.cc as <target> for arm <mode>. On a core with a stack limit the
#   wild thread is the entry of a program of its own, the same file as <target>_peer, so that its
#   fault ends that task and not main.
function(pspguard_image target mode)
  add_executable(${target} main.cc)
  if(ARGC GREATER 2)
    set_target_properties(${target} PROPERTIES KICKOS_IMAGE_KERNEL_NAMES ${ARGV2})
  endif()
  target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/user/apps/common/include")
  target_compile_definitions(${target} PRIVATE KICKOS_PSPGUARD_MODE=${mode}
                             "KICKOS_PSPGUARD_PEER_NAME=\"${target}_peer\"")
  if(KICKOS_HAVE_STACK_LIMIT)
    add_executable(${target}_peer main.cc)
    target_include_directories(${target}_peer PRIVATE
                               "${PROJECT_SOURCE_DIR}/user/apps/common/include")
    target_link_libraries(${target}_peer PRIVATE kickos)
    target_compile_definitions(${target}_peer PRIVATE KICKOS_PSPGUARD_MODE=${mode}
                               KICKOS_PSPGUARD_PEER=1
                               "KICKOS_PSPGUARD_PEER_NAME=\"${target}_peer\"")
    set(PG_APP ${target})
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/system.yaml.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/${target}.yaml" @ONLY)
    kickos_app_system(${target} COMPOSITION "${CMAKE_CURRENT_BINARY_DIR}/${target}.yaml")
  else()
    target_link_libraries(${target} PRIVATE KickOS::kickos KickOS::system_default)
  endif()
  kickos_emit_image(${target})
endfunction()

pspguard_image(pspguard_svc 2)

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
  pspguard_image(pspguard_svcdepth 5)
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
  # The arm reads the running thread's block top by calling the kernel, which only a board
  # enforcing nothing lets a thread do.
  pspguard_image(pspguard_block 6 kickos_fault_stack_top)
endif()

if(KICKOS_ARCH STREQUAL "armv7m")
  pspguard_image(pspguard 0)

  # The FP arm is a SEPARATE binary: a run observes one refusal. The FPU is read off the
  # board's own compiler flags, and main.cc #errors without __ARM_FP.
  string(FIND "${KICKOS_MCPU}" "mfpu" _pg_has_fpu)
  if(_pg_has_fpu GREATER -1)
    pspguard_image(pspguard_fp 1)
  endif()

  # The two bound legs the in-stack arms above cannot reach: a PSP with room to spare BELOW
  # it, refusable only by the upper bound, and a PSP under the stack, refusable only by the
  # lower one. Each lands in a granted domain region, so hardware stacking succeeds and the
  # software push is what gets refused.
  pspguard_image(pspguard_above 3)
  pspguard_image(pspguard_below 4)

  # The SVC entry that violates the stack limit, on the cores that have one.
  if(KICKOS_HAVE_STACK_LIMIT)
    pspguard_image(pspguard_svcstkof 7)
  endif()
endif()
