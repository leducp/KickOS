# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# A published console driver that prints at its start, restarted once, against the packaged
# simcon its composition names as stdout. The death knob is simcon's (system/driver/sim/simcon).
add_executable(conrestart main.cc)
kickos_app_system(conrestart COMPOSITION system.yaml)
kickos_emit_image(conrestart)
