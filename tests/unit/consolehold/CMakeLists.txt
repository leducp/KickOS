# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# A thread-fault record while a userspace driver owns the console: built whole in the
# disarmed ring's storage, never written at the driver's device, and received by the driver
# ahead of every queued sender.
#
# console_held: kernel/init/console.cc and kernel/init/console_tx.cc as they ship, with
# tests/unit/consoleseam answering the rest, and a hook at every store a change of the held store
# makes (KICKOS_HELD_STEP). HOST-ONLY: the second record has to open while the first is still
# being written, which no image can arrange.
kickos_add_unit_test(NAME console_held
  SOURCES held_record.cc held_store.cc record_room.cc
          "${PROJECT_SOURCE_DIR}/tests/unit/consoleseam/console_seam.cc"
          "${PROJECT_SOURCE_DIR}/kernel/irq/irq_route.cc"
          "${PROJECT_SOURCE_DIR}/kernel/init/console.cc"
          "${PROJECT_SOURCE_DIR}/kernel/init/console_tx.cc"
          "${PROJECT_SOURCE_DIR}/lib/rtt.cc"
  INCLUDES "${PROJECT_SOURCE_DIR}/tests/unit/consoleseam"
  DEFINITIONS KICKOS_HELD_STEP=1
  KERNEL)

# console_held_rtt: the same two translation units at the RTT posture, RTT recorded by the test
# itself, for a user line the kernel console is offered more than once.
kickos_add_unit_test(NAME console_held_rtt
  SOURCES held_rtt.cc
          "${PROJECT_SOURCE_DIR}/tests/unit/consoleseam/console_seam.cc"
          "${PROJECT_SOURCE_DIR}/kernel/irq/irq_route.cc"
          "${PROJECT_SOURCE_DIR}/kernel/init/console.cc"
          "${PROJECT_SOURCE_DIR}/kernel/init/console_tx.cc"
  INCLUDES "${PROJECT_SOURCE_DIR}/tests/unit/consoleseam"
  POSTURE KICKOS_CONSOLE_RTT=1)

# console_held_recv: the real receive and hand-off of kernel/syscall/syscall_ipc.cc over the
# K-seam, whose held store the arms seed. Needs kickos_kseam_ipc_smp (tests/unit/fusedwake).
kickos_add_unit_test(NAME console_held_recv SOURCES held_recv.cc
  INCLUDES "${PROJECT_SOURCE_DIR}/kernel/syscall"
  LIBRARIES kickos_kseam_ipc_smp)

# console_live: a published console dies with its task, over the real syscall_ipc.cc, cap.cc,
# task.cc and park.cc of the K-seam: sends park while the task lives, a send of no deadline never
# parks, and a writer waits out the dark window until a wake or its own cancellation.
kickos_add_unit_test(NAME console_live SOURCES live_console.cc
  INCLUDES "${PROJECT_SOURCE_DIR}/kernel/syscall"
  LIBRARIES kickos_kseam_ipc_smp)

# console_dark: the dark window end to end, the real console.cc, console_tx.cc and park.cc over a
# K-seam whose console stubs are compiled out (KSEAM_REAL_CONSOLE), dark_seam.cc answering the
# device.
kickos_add_kseam(kickos_kseam_console
  EXTRA "${PROJECT_SOURCE_DIR}/tests/unit/fusedwake/ipc_seam.cc"
        dark_seam.cc
        "${PROJECT_SOURCE_DIR}/kernel/syscall/syscall_ipc.cc"
        "${PROJECT_SOURCE_DIR}/kernel/init/console.cc"
        "${PROJECT_SOURCE_DIR}/kernel/init/console_tx.cc"
  INCLUDES "${PROJECT_SOURCE_DIR}/tests/unit/fusedwake"
           "${PROJECT_SOURCE_DIR}/kernel/syscall"
  POSTURE KSEAM_REAL_CONSOLE=1)

kickos_add_unit_test(NAME console_dark SOURCES dark_e2e.cc
  LIBRARIES kickos_kseam_console)

# console_own_task: a thread of the console's own task holds no stdout, over the same seam.
kickos_add_unit_test(NAME console_own_task SOURCES own_console.cc
  LIBRARIES kickos_kseam_console)

# console_room: a writer the ring refuses, drained by the producer or woken by the TX drain.
kickos_add_unit_test(NAME console_room SOURCES room_wait.cc
  LIBRARIES kickos_kseam_console)
