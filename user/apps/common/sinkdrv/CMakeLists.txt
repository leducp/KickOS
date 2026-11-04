# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The console driver over no device that reclaimwit and initdemo create on request: local to this
# tree, so no package carries it and no board's driver catalogue lists it.
kickos_add_driver(appsink LOCAL SOURCES appsink.cc BLOCK none BARRIER none
                  POSTURE handover CONSOLE START appsink_start
                  THREAD service ENTRY service CAPS ep:wait)
