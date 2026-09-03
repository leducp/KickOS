# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

kickos_add_application(hello
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})

if(KICKOS_BUILD_TESTS AND KICKOS_ARCH STREQUAL "sim")
  add_test(
    NAME    hello_demo
    COMMAND "${CMAKE_COMMAND}" -E env
            "${PROJECT_SOURCE_DIR}/tests/integration/check_hello.py"
            "$<TARGET_FILE:hello>")
  set_tests_properties(hello_demo PROPERTIES TIMEOUT 15)
endif()

if(KICKOS_BUILD_TESTS AND KICKOS_BOARD IN_LIST KICKOS_QEMU_MPS2_BOARDS)
  kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_hello TARGET hello BOARD ${KICKOS_BOARD}
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_hello.sh")
endif()

if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "microbit")
  kickos_add_qemu_test(NAME microbit_hello TARGET hello BOARD microbit
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_hello.sh")
endif()

if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-riscv" AND NOT KICKOS_HAVE_MPU)
  kickos_add_qemu_test(NAME qemu_riscv_hello TARGET hello BOARD qemu-riscv
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_hello.sh")
endif()

if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-riscv64")
  kickos_add_qemu_test(NAME qemu_riscv64_hello TARGET hello BOARD qemu-riscv64
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_hello.sh")
endif()

if(KICKOS_BUILD_TESTS AND KICKOS_BOARD IN_LIST KICKOS_QEMU_ARM64_BOARDS)
  string(REPLACE "-" "_" _tag "${KICKOS_BOARD}")
  kickos_add_qemu_test(NAME ${_tag}_hello TARGET hello BOARD ${KICKOS_BOARD}
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_hello.sh")
endif()

# The secondary-arrival gate, on the smallest image that boots the chip: every core comes up
# in arch_init, so the app itself is only what carries the release there. The expected count is
# the configured one, and the script boots the image a second time on a machine one core short.
# TIMEOUT covers two boots plus the arrival spin bound the refusal path has to reach.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-arm64" AND KICKOS_NUM_CORES GREATER 1)
  kickos_add_qemu_test(NAME qemu_arm64_smp_arrival TARGET hello BOARD qemu-arm64
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_smp_arrival.sh"
    ARGS ${KICKOS_NUM_CORES} "pong 3" armv8a
    TIMEOUT 240)
endif()

# The same gate on the second backend. Its short-machine arm binds the release loop to the count
# the same way, by a different mechanism: rv64 has no firmware to refuse a start, so the missing
# hart never publishes arrival and the bounded wait names it.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-riscv64" AND KICKOS_NUM_CORES GREATER 1)
  kickos_add_qemu_test(NAME qemu_riscv64_smp_arrival TARGET hello BOARD qemu-riscv64
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_smp_arrival.sh"
    ARGS ${KICKOS_NUM_CORES} "pong 3" rv64imac
    TIMEOUT 240)
endif()

# The doorbell-and-lock gate, on the same image: the round runs in arch_init, so the app is only
# what carries it there. It reads QEMU's own GIC trace beside the console, which is why it needs
# no app of its own and why its verdict does not rest on a number the image printed.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-arm64" AND KICKOS_NUM_CORES GREATER 1)
  kickos_add_qemu_test(NAME qemu_arm64_smp_doorbell TARGET hello BOARD qemu-arm64
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_smp_doorbell.sh"
    ARGS ${KICKOS_NUM_CORES} "pong 3" armv8a
    TIMEOUT 240)
  # Serial: the emulator arm reads whether a core took the doorbell INTERRUPT, and a core that
  # is already spinning in the lock's acquire loop answers by polling instead and acknowledges
  # nothing. Which path runs depends on host scheduling, the guest clock tracking host time, so
  # peers competing for CPU turn a correct image red.
  set_tests_properties(qemu_arm64_smp_doorbell PROPERTIES RUN_SERIAL TRUE)
endif()

# The same gate on the second backend, with ONE channel rather than two and its header saying
# so: the CLINT store that raises a doorbell carries no trace event, so no arm here counts
# raises. What the trap log does carry is the hart and the cause, which is what the per-peer
# and initiator arms read.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-riscv64" AND KICKOS_KERNEL_CORES GREATER 1)
  kickos_add_qemu_test(NAME qemu_riscv64_smp_doorbell TARGET hello BOARD qemu-riscv64
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_smp_doorbell.sh"
    ARGS ${KICKOS_NUM_CORES} "pong 3" rv64imac
    TIMEOUT 240)
  # Serial for the same reason as the arm64 arm: a peer already spinning in the acquire loop
  # answers by POLLING and takes no interrupt, so a starved peer and a broken vector read alike.
  set_tests_properties(qemu_riscv64_smp_doorbell PROPERTIES RUN_SERIAL TRUE)
endif()

# The TLB maintenance the map editor spends, read out of the linked image. Registered from BOTH
# arm64 postures because the shareability it requires differs between them and the gate is handed
# the core count; it runs no image, so it carries the host label.
if(KICKOS_BUILD_TESTS AND KICKOS_ARCH STREQUAL "armv8a")
  add_test(NAME tlbi_shareability
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_tlbi_shareability.sh"
            "$<TARGET_FILE:hello>" "${KICKOS_KERNEL_CORES}" "${CMAKE_NM}" "${CMAKE_OBJDUMP}")
  set_tests_properties(tlbi_shareability PROPERTIES TIMEOUT 120 LABELS host)
endif()

# Three orderings the arm64 entry and timer paths owe, read out of the linked image: the SPSel
# select ahead of the first stack write, and the ISB after each CNTP_CTL_EL0 disable ahead of the
# Device write it protects. Registered on the ARCH and not on a board or a core count: all three
# bodies are compiled on every armv8a posture, both GIC versions and one core included.
# It runs no image, so it carries the host label.
#
# Structural because no arm64 machine in the fleet can witness any of the three: QEMU enters at
# reset with PSTATE.SP already 1, and its timer model deasserts on the register write.
if(KICKOS_BUILD_TESTS AND KICKOS_ARCH STREQUAL "armv8a")
  add_test(NAME arm64_entry_order
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_arm64_entry_order.sh"
            "$<TARGET_FILE:hello>" "${CMAKE_NM}" "${CMAKE_OBJDUMP}")
  set_tests_properties(arm64_entry_order PROPERTIES TIMEOUT 120 LABELS host)
endif()

# The doorbell service body's instruction barrier and its position, read out of the linked image.
# Keyed on the CORE count, not the kernel-core count: the service body is compiled whenever the
# image drives more than one core, so it exists under AMP, where one kernel schedules one core.
# It runs no image, so it carries the host label.
#
# armv8a ONLY, and rv64imac is absent by ruling rather than by oversight: the instruction-side
# barrier is FENCE.I there, and Zifencei is not in that board's ISA baseline, so the backend has
# no such instruction to assert. Its service body carries the TRANSLATION-side fence alone.
if(KICKOS_BUILD_TESTS AND KICKOS_ARCH STREQUAL "armv8a" AND KICKOS_NUM_CORES GREATER 1)
  add_test(NAME doorbell_isb
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_doorbell_isb.sh"
            "$<TARGET_FILE:hello>" "${CMAKE_NM}" "${CMAKE_OBJDUMP}")
  set_tests_properties(doorbell_isb PROPERTIES TIMEOUT 120 LABELS host)
endif()

# The store->load fence both sides of the rv64imac interrupt-controller handshake owe, read out
# of the linked image. UNCONDITIONAL on the core count, as the fence is: the three words the
# handshake turns on are image-wide rather than per hart, so a fence keyed on the count would
# encode an assumption about who may call that the backend does not enforce.
# It runs no image, so it carries the host label.
#
# rv64imac ONLY. The other backends reach an interrupt controller in hardware and take no
# software handshake between two words; where one exists it is a different shape and this
# reader's word names do not occur.
if(KICKOS_BUILD_TESTS AND KICKOS_ARCH STREQUAL "rv64imac")
  add_test(NAME rv64_irq_fence
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_rv64_irq_fence.sh"
            "$<TARGET_FILE:hello>" "${CMAKE_NM}" "${CMAKE_OBJDUMP}")
  set_tests_properties(rv64_irq_fence PROPERTIES TIMEOUT 120 LABELS host)
endif()

# The boundary between the doorbell's rendezvous half and its scheduling half, read out of the
# linked image. Keyed on the KERNEL-core count: the reschedule cell and the dispatch's scheduler
# entry exist only where one kernel schedules more than one core, and under AMP there is no
# scheduling half to separate.
# It runs no image, so it carries the host label.
if(KICKOS_BUILD_TESTS AND KICKOS_KERNEL_CORES GREATER 1
   AND (KICKOS_ARCH STREQUAL "armv8a" OR KICKOS_ARCH STREQUAL "rv64imac"))
  add_test(NAME doorbell_generic
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_doorbell_generic.sh"
            "$<TARGET_FILE:hello>" "${CMAKE_NM}" "${CMAKE_OBJDUMP}" "${KICKOS_ARCH}")
  set_tests_properties(doorbell_generic PROPERTIES TIMEOUT 120 LABELS host)
endif()

if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-x86_64")
  kickos_add_qemu_test(NAME qemu_x86_64_hello TARGET hello BOARD qemu-x86_64
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_hello.sh")
endif()
