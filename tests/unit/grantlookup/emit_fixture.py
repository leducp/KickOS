# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The lookup tests' tables, each a golden composition with its edits, admitted against its
# control manifest from tools/compose/tests and emitted by the tool:
#
#   emit_fixture.py <scratch directory> <fixture>=<output.c>...

import os
import shutil
import sys

from kickos_compose import emit
from test_arms import MANIFESTS, PLATFORM, SENSOR_ALARM, SYSTEMS, mutate, read, write

HISTORY = "  - name: /shm/history\n    size: 64\n"
SENSOR_MAPS = "    maps: { /shm/history: rw }\n    restart"

FIXTURES = {
    # A line by its role, two watched tasks whose table order is not their watch order, and a
    # second region of another size.
    "arm64": ("qemu-arm64.yaml", [SENSOR_ALARM, ("watches: [sensor]", "watches: [app, sensor]"),
                                  (HISTORY, "  - name: /shm/log\n    size: 8192\n    cache: cached\n" + HISTORY),
                                  (SENSOR_MAPS, "    maps: { /shm/history: rw, /shm/log: rw }\n    restart")]),
    # A device window and a port range, ahead of the region.
    "x86": ("qemu-x86_64.yaml", [("devices: [/dev/cmos_rtc]", "devices: [/dev/hpet, /dev/cmos_rtc]")]),
    # Packaged drivers, and a used endpoint ahead of the served one.
    "xmc": ("xmc4800-relax.yaml", []),
}


def main(argv):
    if len(argv) < 3:
        print("usage: emit_fixture.py <scratch directory> <fixture>=<output.c>...", file=sys.stderr)
        return 2
    scratch = argv[1]
    shutil.rmtree(scratch, ignore_errors=True)
    shutil.copytree(PLATFORM, os.path.join(scratch, "platform"))
    for request in argv[2:]:
        fixture, output = request.split("=", 1)
        golden, edits = FIXTURES[fixture]
        manifest = os.path.join(scratch, "manifest-%s.yaml" % fixture)
        write(manifest, MANIFESTS[golden])
        composition = os.path.join(scratch, "%s.yaml" % fixture)
        write(composition, mutate(read(os.path.join(SYSTEMS, golden)), edits)[0])
        report, source = emit.emit(composition, manifest)
        for refusal in report.refusals:
            print(refusal, file=sys.stderr)
        if source is None:
            print("emit_fixture.py: %s refused" % fixture, file=sys.stderr)
            return 1
        write(output, source)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
