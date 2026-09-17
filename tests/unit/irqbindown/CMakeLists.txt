# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Who owns an IRQ notification binding, over the real kernel/irq/irq.cc and the real
# kernel/syscall/cap.cc close protocol, both of which the K-seam fixture already compiles.
#
# ONE KERNEL CORE is enough and is the point: nothing here is a placement question, so the
# preset's own posture is the right one and this gate joins kickos_kseam rather than
# compiling a third copy of those sources.
kickos_add_unit_test(NAME irq_bind_own SOURCES bind_owner.cc
  LIBRARIES kickos_kseam)
