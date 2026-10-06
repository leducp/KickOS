# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The init walk tests' systems: each golden composition and each board's default composition,
# admitted against its control manifest from tools/compose/tests, and the walk's own fixtures,
# each emitted by the tool, with what the kernel build tells the walk and the figures admission
# counts for the init:
#
#   emit_systems.py <scratch directory> <system>=<table.c>... systems=<systems.cc>

import os
import shutil
import sys

from kickos_compose import emit, supply
from test_arms import (C6_MANIFEST, K64F_MANIFEST, MANIFESTS, PLATFORM, REGION_POOLS, RV64_MANIFEST, SENSOR_ALARM,
                       SYSTEMS, TREE, XMC_PROTECTION, manifest_of, mutate, read, write)

F411_MANIFEST = manifest_of("f411disco", "stm32f411", "armv7m", XMC_PROTECTION, 1, REGION_POOLS,
                            (960, 4096, 512, 4096, "4096"))
DEFAULTS = {
    "esp32c6-wroom": C6_MANIFEST,
    "f411disco": F411_MANIFEST,
    "frdmk64f": K64F_MANIFEST,
    "qemu-arm64": MANIFESTS["qemu-arm64.yaml"],
    "qemu-riscv64": RV64_MANIFEST,
    "qemu-x86_64": MANIFESTS["qemu-x86_64.yaml"],
    "xmc4800-relax": MANIFESTS["xmc4800-relax.yaml"],
}

# Two servers, a client of one that serves in turn, a client of both, a client of the client and a
# watcher of four, on q35.
CHAIN = """version: 1
board: qemu-x86_64
stdout: kernel
ends: never
heap: 0

tasks:
  - name: sensor
    entry: sensor_main
    stack: 4096
    priority: 9
    serves: /svc/sensor
    restart: { max: 2 }

  - name: slow
    entry: slow_main
    stack: 4096
    priority: 9
    serves: /svc/slow

  - name: mid
    entry: mid_main
    stack: 4096
    priority: 8
    uses: [/svc/sensor]
    serves: /svc/mid
    restart: { max: 1 }

  - name: late
    entry: late_main
    stack: 4096
    priority: 8
    uses: [/svc/sensor, /svc/slow]

  - name: top
    entry: top_main
    stack: 4096
    priority: 7
    uses: [/svc/mid]

  - name: watcher
    entry: watcher_main
    stack: 4096
    priority: 10
    watches: [sensor, mid, top, late]
"""


# A client of seven servers on a kernel build that delegates eight capabilities at a spawn, past
# the six of the host build the tests compile the walk for.
WIDE = "".join("  - name: s%d\n    entry: sensor_main\n    stack: 4096\n    priority: 9\n    serves: /svc/s%d\n\n"
               % (n, n) for n in range(7))
WIDE = (CHAIN[:CHAIN.index("tasks:\n")] + "tasks:\n" + WIDE + "  - name: wide\n    entry: app_main\n    stack: 4096\n"
        "    priority: 8\n    uses: [%s]\n" % ", ".join("/svc/s%d" % n for n in range(7)))
WIDE_MANIFEST = MANIFESTS["qemu-x86_64.yaml"].replace("  KICKOS_MAX_SPAWN_GRANTS: 6\n", "  KICKOS_MAX_SPAWN_GRANTS: 8\n")


# A two-thread packaged driver with a restart, whose receiver is not its entry thread, a client
# of it and a watcher of it, on the Relax Kit.
TESTDRV = """  testdrv:
    windows: []
    lines: []
    threads:
      - { name: entry, priority: 0, stack: default, caps: 0, badged: 0 }
      - { name: worker, priority: 0, stack: default, caps: 1, badged: 0 }
    endpoints: 1
    notifications: 0
    block: none
    posture: retain
    barrier: none
    console: false
    start: testdrv_start
    receiver: worker
    client: []
"""
DRIVER_RESTART_MANIFEST = MANIFESTS["xmc4800-relax.yaml"].replace("drivers:\n", "drivers:\n" + TESTDRV)
DRIVER_RESTART = """version: 1
board: xmc4800-relax
stdout: kernel
ends: never
heap: 0

tasks:
  - name: drv
    driver: testdrv
    serves: /svc/test
    priority: 11
    restart: { max: 1 }

  - name: client
    entry: app_main
    stack: 2048
    priority: 8
    uses: [/svc/test]

  - name: watcher
    entry: health_main
    stack: 2048
    priority: 10
    watches: [drv]
"""


def golden(name):
    return read(os.path.join(SYSTEMS, name))


def default(board):
    return read(os.path.join(TREE, "boards", board, "composition.yaml"))


# name: (composition text, manifest, the panic the walk refuses it with at boot or None)
FIXTURES = {
    "golden_arm64": (golden("qemu-arm64.yaml"), MANIFESTS["qemu-arm64.yaml"], None),
    "golden_x86": (golden("qemu-x86_64.yaml"), MANIFESTS["qemu-x86_64.yaml"], None),
    "golden_xmc": (golden("xmc4800-relax.yaml"), MANIFESTS["xmc4800-relax.yaml"], None),
    # The sensor taking the RTC's alarm line on core 1.
    "alarm": (mutate(golden("qemu-arm64.yaml"), [SENSOR_ALARM])[0], MANIFESTS["qemu-arm64.yaml"], None),
    "chain": (CHAIN, MANIFESTS["qemu-x86_64.yaml"], None),
    # The arm64 golden stating the init's priority.
    "init_priority": (golden("qemu-arm64.yaml").replace("heap: 65536\n", "heap: 65536\ninit: { priority: 7 }\n"),
                      MANIFESTS["qemu-arm64.yaml"], None),
    "driver_restart": (DRIVER_RESTART, DRIVER_RESTART_MANIFEST, None),
    "chain_ends": (CHAIN.replace("ends: never", "ends: top"), MANIFESTS["qemu-x86_64.yaml"], None),
    "wide": (WIDE, WIDE_MANIFEST,
             "init: `wide` is spawned with 7 capabilities and 0 windows, and a spawn takes at most 6 and 4"),
}
for board, manifest in DEFAULTS.items():
    FIXTURES["default_" + board.replace("-", "_")] = (default(board), manifest, None)


def flag(value):
    if value:
        return "true"
    return "false"


def limits(admitted):
    """The kernel build's ceilings the scripted kernel holds the walk to, 0 for none."""
    manifest = admitted.manifest
    pools = manifest.pools
    tasks = 0
    if "KICKOS_MAX_TASKS" in pools:
        tasks = pools["KICKOS_MAX_TASKS"] - (manifest.kernel_cores or 1) - 1
    owners = 0
    if manifest.enforced and not admitted.translating:
        owners = pools.get("KICKOS_RAM_OWNER_SLOTS", 0)
    regions = 0
    if not admitted.translating:
        regions = manifest.free_regions or 0
    return (pools.get("KICKOS_CAP_TABLE_SUPPLY", 0), tasks, pools.get("KICKOS_MAX_THREADS", 0),
            pools.get("KICKOS_MAX_ENDPOINTS", 0), pools.get("KICKOS_MAX_NOTIFY", 0),
            pools.get("KICKOS_MAX_IRQ_HANDLES", 0), pools.get("KICKOS_MAX_DOMAINS", 0), regions, owners,
            pools.get("KICKOS_TASK_ENDPOINT_BUDGET", 0), pools.get("KICKOS_TASK_NOTIFY_BUDGET", 0),
            pools.get("KICKOS_TASK_IRQ_HANDLE_BUDGET", 0), manifest.min_stack or 0, manifest.stack_stride or 0,
            manifest.kernel_cores or 1)


def describe(name, admitted, refusal):
    manifest = admitted.manifest
    figures = dict(supply.init_figures(admitted.tasks, admitted.shared, manifest, admitted.translating))
    quoted = "nullptr"
    if refusal is not None:
        quoted = "\"%s\"" % refusal
    return ("    {\"%s\", &kickos_table_%s, {%s, %s, %s}, %d, %d, %s,\n"
            "     {%d, %d, %d, %d, %d, %d, %d, %d, %d},\n"
            "     {%d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d}},"
            % ((name, name, flag(admitted.translating), flag(manifest.stack_stride is not None),
               flag((manifest.kernel_cores or 1) > 1), manifest.cap_reserved, manifest.amp_ports, quoted,
               figures["cap_slots"], figures["endpoints"], figures["notifications"],
               figures["threads"], figures["tasks"], figures["irq_handles"], figures["domains"],
               figures["reservations"], figures["self_grants"]) + limits(admitted)))


def main(argv):
    if len(argv) < 3:
        print("usage: emit_systems.py <scratch directory> <system>=<table.c>... systems=<systems.cc>",
              file=sys.stderr)
        return 2
    scratch = argv[1]
    shutil.rmtree(scratch, ignore_errors=True)
    shutil.copytree(PLATFORM, os.path.join(scratch, "platform"))
    requests = dict(request.split("=", 1) for request in argv[2:])
    systems = requests.pop("systems", None)
    if systems is None or sorted(requests) != sorted(FIXTURES):
        print("emit_systems.py: the requests name %s, and the systems are %s and systems"
              % (", ".join(sorted(requests)), ", ".join(sorted(FIXTURES))), file=sys.stderr)
        return 1
    rows = []
    for name in sorted(requests):
        text, manifest_text, refusal = FIXTURES[name]
        manifest = os.path.join(scratch, "manifest-%s.yaml" % name)
        write(manifest, manifest_text)
        composition = os.path.join(scratch, "%s.yaml" % name)
        write(composition, text)
        report, admitted = emit.admitted_of(composition, manifest)
        for printed in report.refusals:
            print(printed, file=sys.stderr)
        if admitted is None:
            print("emit_systems.py: %s refused" % name, file=sys.stderr)
            return 1
        report, source = emit.emit(composition, manifest)
        write(requests[name], source)
        rows.append(describe(name, admitted, refusal))
    externs = "".join("    extern struct kos_table_header const* const kickos_table_%s;\n" % name
                      for name in sorted(requests))
    write(systems, "// GENERATED by tests/unit/initwalk/emit_systems.py; edits are overwritten by the next "
                   "build.\n\n#include \"systems.h\"\n\nextern \"C\"\n{\n%s}\n\nnamespace systems\n{\n"
                   "    System const ALL[] = {\n%s\n    };\n"
                   "    size_t const COUNT = sizeof(ALL) / sizeof(ALL[0]);\n}\n" % (externs, "\n".join(rows)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
