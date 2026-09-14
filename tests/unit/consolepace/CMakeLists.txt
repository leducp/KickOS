# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kernel/init/console.cc and kernel/init/console_tx.cc in one program, driven through the
# paced printer, with tests/unit/consoleseam answering the transport and pace_seam.cc the
# names those two leave open.
#
# KICKOS_BENCH=1 is a POSTURE and not an added definition: the paced printer exists only in a
# bench image, the tree carries KICKOS_BENCH=0 at directory scope, and two -D of one name is a
# redefinition this tree compiles with -Werror.
#
# HOST-ONLY: the line rate is a mock slot budget scoped to an instrumented mask, which no
# board can supply.
kickos_add_unit_test(NAME console_pace
  SOURCES pace.cc pace_seam.cc
          "${PROJECT_SOURCE_DIR}/tests/unit/consoleseam/console_seam.cc"
          "${PROJECT_SOURCE_DIR}/kernel/init/console.cc"
          "${PROJECT_SOURCE_DIR}/kernel/init/console_tx.cc"
          "${PROJECT_SOURCE_DIR}/lib/rtt.cc"
  INCLUDES "${PROJECT_SOURCE_DIR}/tests/unit/consoleseam"
  POSTURE KICKOS_BENCH=1
  KERNEL)
