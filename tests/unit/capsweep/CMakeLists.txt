# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Two capability sweeps in flight at once. Host-only of necessity: instrumented counters put
# the concurrent path at zero hits across the whole on-target suite, and forcing it there
# needs a spin in the chunk gap plus every churner made RR -- a geometry no board ships.
kickos_add_kseam_gate(NAME cap_sweep SOURCES nested_teardown.cc)
