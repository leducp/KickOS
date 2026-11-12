# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# THIS FILE SELECTS. Every app below is entered only on a posture where it applies, so no app
# file asks whether it applies: an app declares its target and stops. What an app file may
# still hold is a precondition that is the APP's own and carries operator instructions with it
# (a console the kernel must own, a knob that shapes the image), never a board-level fact that
# is readable here.
#
# Two board groupings recur, and both are arch/chip facts rather than lists to maintain:
#   the four QEMU MPS2 FPGA images (an386/M4F, an505/M33, an500/M7, an385/M3) are exactly the
#     boards QEMU runs as an MPS2 machine, KICKOS_QEMU_MPS2, which the root derives from
#     kickos_qemu_machine.
#   the two emulatable arm64 boards (qemu-arm64, imx8mp-evk) are exactly the armv8a fleet,
#     KICKOS_ARCH STREQUAL "armv8a".
#
# NO board name selects an app: a name list is where an app was once run, and the next board is
# left out in silence. tests/static/check_board_predicates.py refuses one.

# _kos_chip_defines(<out> <symbol>): ON where a source of this build's chip library defines
# <symbol>, which displaces the arch/common/<symbol>_default.cc fallback.
function(_kos_chip_defines out symbol)
  set(_defined OFF)
  if(TARGET kickos_chip_${KICKOS_CHIP})
    get_target_property(_srcs kickos_chip_${KICKOS_CHIP} SOURCES)
    list(FILTER _srcs INCLUDE REGEX "\\.cc$")
    foreach(_src IN LISTS _srcs)
      if(NOT IS_ABSOLUTE "${_src}")
        set(_src "${PROJECT_SOURCE_DIR}/arch/${_src}")
      endif()
      set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                   "${_src}")
      file(READ "${_src}" _txt)
      if(_txt MATCHES "[^A-Za-z0-9_]${symbol}\\([^)]*\\)[ \t\r\n]*{")
        set(_defined ON)
      endif()
    endforeach()
  endif()
  set(${out} ${_defined} PARENT_SCOPE)
endfunction()

# KICKOS_TEST_SHAPE: the axes along which this preset differs from its arch's reference preset.
#   core    the image is built for another core of the arch
#   cpu     the same image runs on another processor model
#   flat    memory is not enforced
#   telem   the trace sink is on
#   tick    the periodic tick
#   paging  another translation mode
#   cores   another core count than the arch's SMP reference
#   root    the root task starts on another core
#   iso     a core is isolated
#   gic     another interrupt controller
#   amp     a node of an AMP partition
set(KICKOS_TEST_SHAPE "" CACHE STRING
    "Axes along which this preset differs from its arch's reference preset")
set(_kos_shape_axes core cpu flat telem tick paging cores root iso gic amp)
foreach(_axis IN LISTS KICKOS_TEST_SHAPE)
  if(NOT _axis IN_LIST _kos_shape_axes)
    message(FATAL_ERROR "KICKOS_TEST_SHAPE names '${_axis}', which is none of: ${_kos_shape_axes}")
  endif()
endforeach()

# kickos_shape_runs(<out> [<axis>...]): ON unless this preset differs from its arch's reference
# along an axis that is not one of <axis>, the axes the app's claim reads.
function(kickos_shape_runs out)
  set(_runs ON)
  foreach(_axis IN LISTS KICKOS_TEST_SHAPE)
    if(NOT _axis IN_LIST ARGN)
      set(_runs OFF)
    endif()
  endforeach()
  set(${out} ${_runs} PARENT_SCOPE)
endfunction()

# kickos_shape_app(<dir> [<axis>...]): add_subdirectory(<dir>) where kickos_shape_runs is ON.
macro(kickos_shape_app dir)
  kickos_shape_runs(_kos_shape_runs ${ARGN})
  if(_kos_shape_runs)
    add_subdirectory(${dir})
  endif()
endmacro()

# The console driver reclaimwit and initdemo create on request, declared once.
if(KICKOS_HAVE_LIBC)
  add_subdirectory(sinkdrv)
endif()

# --- Every posture ---------------------------------------------------------------------------
add_subdirectory(hello)
kickos_shape_app(hello_c)
add_subdirectory(selftest)
add_subdirectory(stress)
add_subdirectory(mutexbench)
add_subdirectory(userbench)
# Its only witness is the kernel block's high-water line, which a panic prints only where the
# board carves the blocks and reports them.
if(KICKOS_KERNEL_STACKS AND KICKOS_KSTACK_REPORT)
  add_subdirectory(stackdepth)
endif()

add_subdirectory(fault)
add_subdirectory(panicgate)
# A task restarted from its own image, image 1.
if(NOT KICKOS_ARCH STREQUAL "sim")
  kickos_shape_app(restartwit)
endif()

# --- A C library on the target -----------------------------------------------------------
if(KICKOS_HAVE_LIBC)
  kickos_shape_app(chaincheck core)
  kickos_shape_app(libc_exit)
  # Not on the ESP32-C6 AMP nodes.
  if(NOT (KICKOS_CHIP STREQUAL "esp32c6" AND KICKOS_AMP_NODE))
    add_subdirectory(libcrace)
  endif()
  kickos_shape_app(sysdefault core)
  kickos_shape_app(rootauth)
  kickos_shape_app(reclaimwit flat telem)
  add_subdirectory(rebootdemo)
endif()
# The host libc of the sim takes none of KickOS's locks.
if(KICKOS_HAVE_LIBC AND NOT KICKOS_ARCH STREQUAL "sim")
  kickos_shape_app(libc_death)
endif()
# The sim's std::atomic is the host's.
if(KICKOS_HAVE_LIBC AND NOT KICKOS_ARCH STREQUAL "sim")
  add_subdirectory(uatomic)
endif()
# One system whose two images link the two newlib profiles, where the toolchain carries both.
if(KICKOS_HAVE_LIBC AND "nano" IN_LIST KICKOS_LIBC_PROFILES)
  add_subdirectory(libcmix)
endif()
# mpu_fault grants a region to one thread of a task, which a backend that translates cannot:
# its unit of memory is the task's space.
if(KICKOS_HAVE_LIBC AND KICKOS_MEMORY_ENFORCED AND NOT KICKOS_HAVE_ASPACE)
  add_subdirectory(mpu_fault)
endif()
# Keyed on confinement, not on which mechanism enforces it.
if(KICKOS_HAVE_LIBC AND KICKOS_MEMORY_ENFORCED)
  kickos_shape_app(rootfault core paging cpu)
endif()

# --- Memory enforcement --------------------------------------------------------------------
# The RV32 PMP chips hold the user code window as a TOR pair the script bounds.
if(KICKOS_HAVE_LIBC AND KICKOS_MEMORY_ENFORCED AND KICKOS_ARCH STREQUAL "rv32imac")
  add_subdirectory(codewindow)
endif()
# specfault's verdict is that a read of an address nothing grants is DENIED. Where nothing
# enforces memory the read is the bus's to answer, which is no claim of the kernel's.
if(KICKOS_MEMORY_ENFORCED)
  kickos_shape_app(specfault core paging cpu)
endif()

# --- Fault isolation, translation, and the host sim --------------------------------------
if(KICKOS_FAULT_ISOLATION)
  add_subdirectory(faultsurvive)
endif()
if(KICKOS_HAVE_ASPACE)
  add_subdirectory(deadstack)
  kickos_shape_app(kernelhalf paging cpu)
  kickos_shape_app(stackguard paging)
endif()
# Only the uaccess witness's scratch tree builds it: no other kernel answers its call.
if(KICKOS_WITNESS_UACCESS)
  add_subdirectory(uaccess)
endif()
# Only the rewind witness's scratch tree builds it: no other kernel answers its call.
if(KICKOS_WITNESS_REWIND)
  add_subdirectory(ucasrewind)
endif()
# Sim only: each drives something that is only automatable on the host, a loopback "device"
# that is the host wire, a published posture, __builtin_trap, or many kernels in one process.
if(KICKOS_ARCH STREQUAL "sim")
  add_subdirectory(sched_exit)
  add_subdirectory(pubpanic)
  add_subdirectory(simconabi)
  add_subdirectory(uartloop)
  add_subdirectory(drvdeath)
  add_subdirectory(conrestart)
endif()

# --- Per-ISA ------------------------------------------------------------------------------
# The PSP-bounds and privilege-ring probes move SP with ARM asm and read ARM system registers;
# each main.cc #errors anywhere else.
if(KICKOS_ARCH STREQUAL "armv7m" OR KICKOS_ARCH STREQUAL "armv6m")
  kickos_shape_app(ringpriv core)
  add_subdirectory(pspguard)
endif()
# Needs a privilege ring to escape from, and an emulator's debugger to place the preemption.
if(KICKOS_ARCH STREQUAL "armv7m" AND KICKOS_KERNEL_STACKS AND KICKOS_FAULT_ISOLATION)
  add_subdirectory(privwindow)
endif()
# The SVC slow path's copy of an FP-live caller's {s0-s15, FPSCR} onto its kernel block, which
# only the transfer posture makes and only an FP register file can show.
if(KICKOS_ARCH STREQUAL "armv7m" AND KICKOS_KERNEL_STACKS
   AND NOT KICKOS_MFLOAT_ABI STREQUAL "soft")
  add_subdirectory(svcfp)
endif()
# The nested-trap witness: rv32imac is the one backend that runs syscall dispatch on the
# caller's own stack AND builds its trap frames in software, so it is the only one where an
# interrupt taken mid-dispatch can put a frame at an unbounded depth on a thread's stack. Its
# deep syscall is a spawn the region rules must refuse, and without an MPU backend nothing
# refuses it.
if(KICKOS_ARCH STREQUAL "rv32imac" AND KICKOS_HAVE_MPU)
  add_subdirectory(trapnest)
endif()
# Built only where the board compiles TLS: elsewhere the thread_local is a link error naming
# __aeabi_read_tp, which is the intended diagnostic but not a fleet build failure.
if(KICKOS_TLS)
  kickos_shape_app(tlsprobe core)
endif()
# Built only where an emulator runs it: its only witness is a QEMU gate.
kickos_qemu_machine("${KICKOS_BOARD}" _kos_errno_env _kos_errno_machine)
if(NOT _kos_errno_machine STREQUAL "")
  kickos_shape_app(errnoprobe root core)
endif()
# The budgets' witnesses, eight declarations over two programs, on an emulator or the sim.
# Not on the 32 KiB nRF51, whose arena seats fewer threads than its declarations' budgets.
if(NOT KICKOS_CHIP STREQUAL "nrf51"
   AND (NOT _kos_errno_machine STREQUAL "" OR KICKOS_ARCH STREQUAL "sim"))
  add_subdirectory(budgetwit)
endif()

# Per-thread stdout regression. newlib_stubs.cc (the _write under test) is compiled ONLY for
# the cross toolchains, NOT the sim, so the sim gives zero coverage. The QEMU mps2 images are
# the automatable armv7m gates: they run and exit, and arch_shutdown forwards the exit status
# over semihosting. All threads share one address space there, so the app's globals are a valid
# cross-thread channel.
if(KICKOS_QEMU_MPS2)
  add_subdirectory(initdemo)
endif()

# --- Above one core -----------------------------------------------------------------------
# THE TARGETS THEMSELVES are gated and not just their ctest entries: slaypeer's main.cc pins by
# core mask and reads a printf formatter, and taskleave calls kos_mem_count, which is not
# declared where KICKOS_HAVE_ASPACE is 0. At one core a victim cannot be RUNNING anywhere but
# where the slayer is, which is the whole hazard.
if(KICKOS_HAVE_LIBC AND KICKOS_BUILD_TESTS AND KICKOS_KERNEL_CORES GREATER 1)
  add_subdirectory(slaypeer)
  if(KICKOS_HAVE_ASPACE)
    add_subdirectory(taskleave)
  endif()
endif()

# --- The own-image AMP partition ----------------------------------------------------------
# AFTER selftest above: the partition targets consume $<TARGET_FILE:selftest>, and a target has
# to exist before another directory can name it.
if(KICKOS_AMP_NODE AND KICKOS_AMP_OWN_IMAGE)
  add_subdirectory(ampping)
endif()

# --- Full-C++ opt-in smoke test -----------------------------------------------------------
# The toolchain's libstdc++ over newlib, so not the sim, whose runtime and terminate handler are
# the host's; and where the board's heap holds cxxtest's high-water, 6,136 B
# (docs/reference/boards.md). Every link runs --orphan-handling=error, so a linker script that
# homes no EH table refuses the image.
file(STRINGS "${PROJECT_SOURCE_DIR}/boards/${KICKOS_BOARD}/composition.yaml" _kos_heap
     REGEX "^heap: *[0-9]+" LIMIT_COUNT 1)
string(REGEX REPLACE "^heap: *([0-9]+).*$" "\\1" _kos_heap "${_kos_heap}")
if(NOT KICKOS_ARCH STREQUAL "sim" AND _kos_heap GREATER_EQUAL 6136)
  kickos_shape_app(cxxtest flat paging)
  kickos_shape_app(cxxterm core flat)
endif()

# --- Telemetry ----------------------------------------------------------------------------
if(KICKOS_TELEMETRY_ON)
  add_subdirectory(tele_pingpong)
endif()
if(KICKOS_TELEMETRY_ON AND KICKOS_ARCH STREQUAL "sim")
  add_subdirectory(tele_flood)
endif()

# --- FP context-switch torture test -------------------------------------------------------
# An armv7m core with an FPU, which a float ABI other than soft states, and every armv8a, x86_64
# and lx6 one.
if((KICKOS_ARCH STREQUAL "armv7m" AND NOT KICKOS_MFLOAT_ABI STREQUAL "soft")
   OR KICKOS_ARCH MATCHES "^(armv8a|x86_64|lx6)$")
  kickos_shape_app(fp_switch core)
endif()

# --- GPIO from userspace: the boards whose layout gpioblink carries -------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/gpioblink/systems/${KICKOS_BOARD}.yaml")
  add_subdirectory(gpioblink)
endif()

# --- Bring-up LED blink -------------------------------------------------------------------
# Where the board file gives the kernel no LED, arch_diag_led_set is the no-op fallback and blink
# would read as a dead board.
if(KICKOS_KERNEL_LED)
  add_subdirectory(blink)
endif()

# Its composition names the board's packaged USB CDC console driver.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/usbcdcwit/systems/${KICKOS_BOARD}.yaml"
   AND NOT KICKOS_AMP_NODE)
  add_subdirectory(usbcdcwit)
endif()
# Its composition names the board's packaged IRQ UART console driver; it #errors without
# enforcement.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/uartconsole/systems/${KICKOS_BOARD}.yaml"
   AND KICKOS_HAVE_MPU)
  add_subdirectory(uartconsole)
endif()

# --- Deliberate HARDWARE harnesses, OFF by default ----------------------------------------
# The knobs are declared unconditionally so they reach the cache and `cmake -LH` on every
# board; only the descent is gated.
option(KICKOS_CLOCK_RETUNE_TEST "clockretune: privileged runtime clock-retune HW harness" OFF)
# Only a chip that defines arch_cpu_clock_set, under enforcement; elsewhere the retune is the
# fallback no-op and this harness would prove nothing.
_kos_chip_defines(_kos_clock_set arch_cpu_clock_set)
if(KICKOS_CLOCK_RETUNE_TEST AND _kos_clock_set AND KICKOS_HAVE_MPU)
  add_subdirectory(clockretune)
endif()
# Arch-neutral, so it builds for any board. A capture's judge also times it by the arrival stamps
# on the capture host; under an emulator the kernel clock alone times it.
if(NOT KICKOS_ARCH STREQUAL "sim" AND NOT KICKOS_AMP_NODE)
  add_subdirectory(wallclock)
endif()
# Its only witness is a QEMU gate, and its waiter a C++ thread, which needs the heap cxxtest needs.
if(KICKOS_HAVE_LIBC AND NOT _kos_errno_machine STREQUAL "" AND NOT KICKOS_AMP_NODE
   AND _kos_heap GREATER_EQUAL 6136)
  kickos_shape_app(wallwait)
endif()
