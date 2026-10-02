#!/usr/bin/env python3
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Writes the export manifest at configure, from the build-graph facts cmake/manifest.cmake
# hands over as JSON and the resolved .config that JSON names. Standard library only, so a
# configure needs no uv. The schema is tools/compose/kickos_compose/manifest.py's.
#
#   genmanifest.py <facts.json> <manifest.yaml>

import json
import mmap
import os
import re
import sys

# A configure writes nothing into the source tree.
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "compose"))

from kickos_compose.manifest_fields import DRIVER_FIELDS, WINDOWS_KNOB, is_pool

MANIFEST_VERSION = 1

CONFIG_LINE = re.compile(r"CONFIG_(KICKOS_[A-Z0-9_]+)=(.*)")
PLAIN = re.compile(r"[A-Za-z_][A-Za-z0-9_./-]*")
# A plain scalar YAML 1.1 or 1.2 would read as something other than a string.
RESERVED = ("y", "yes", "n", "no", "on", "off", "true", "false", "null", "~")


class Refused(Exception):
    pass


class Hex(int):
    """An integer written in hex: a core mask."""


def read_config(path):
    knobs = {}
    with open(path, encoding="utf-8") as stream:
        for line in stream:
            match = CONFIG_LINE.fullmatch(line.rstrip("\n"))
            if match is None:
                continue
            name, value = match.groups()
            if value.startswith('"'):
                knobs[name] = json.loads(value)
            elif value in ("y", "n"):
                knobs[name] = value == "y"
            else:
                knobs[name] = int(value, 0)
    return knobs


def knob(knobs, name, default=None):
    value = knobs.get(name, default)
    if not isinstance(value, int) or isinstance(value, bool):
        raise Refused("the resolved configuration states no integer %s" % name)
    return value


def scalar(value):
    if isinstance(value, bool):
        if value:
            return "true"
        return "false"
    if isinstance(value, Hex):
        return "0x%X" % value
    if isinstance(value, int):
        return str(value)
    if PLAIN.fullmatch(value) and value.lower() not in RESERVED:
        return value
    return json.dumps(value)


def flow(value):
    if isinstance(value, dict) and not value:
        return "{}"
    if isinstance(value, dict):
        return "{ %s }" % ", ".join("%s: %s" % (key, flow(item)) for key, item in value.items())
    if isinstance(value, list):
        return "[%s]" % ", ".join(flow(item) for item in value)
    return scalar(value)


def block(lines, mapping, indent):
    """Each pair of `mapping` as a line, a nested mapping as a block below its key."""
    for key, value in mapping.items():
        if isinstance(value, dict) and value:
            lines.append("%s%s:" % (indent, key))
            block(lines, value, indent + "  ")
        elif isinstance(value, list) and value and all(isinstance(item, dict) for item in value):
            lines.append("%s%s:" % (indent, key))
            for item in value:
                lines.append("%s  - %s" % (indent, flow(item)))
        else:
            lines.append("%s%s: %s" % (indent, key, flow(value)))


def string_knob(knobs, name):
    value = knobs.get(name, "")
    if not isinstance(value, str):
        raise Refused("the resolved configuration states no string %s" % name)
    return value


def manifest(facts, knobs):
    target = {"board": string_knob(knobs, "KICKOS_BOARD")}
    if string_knob(knobs, "KICKOS_CHIP"):
        target["chip"] = string_knob(knobs, "KICKOS_CHIP")
    target["arch"] = string_knob(knobs, "KICKOS_ARCH")
    target["cores"] = knob(knobs, "KICKOS_NUM_CORES")
    target["kernel_cores"] = knob(knobs, "KICKOS_KERNEL_CORES")
    target["isolated_cores"] = Hex(knob(knobs, "KICKOS_ISOLATED_CORES", 0))
    if facts["amp"] is not None:
        target["amp"] = {
            "node": facts["amp"]["node"],
            "nodes": knob(knobs, "KICKOS_AMP_NODES"),
            "ports": facts["amp"]["ports"],
        }

    # The rule the kernel applies whether or not the build enforces: it rounds every arena block and
    # checks every device window by it. A translating build has none, its page being the chip file's.
    seams = facts["seams"]
    protection = {"enforced": knob(knobs, "KICKOS_MEMORY_ENFORCED") != 0, "window_rule": "none"}
    if knob(knobs, "KICKOS_HAVE_ASPACE", 0):
        pass
    elif "min_region" in seams and seams["min_region"] != 0:
        protection["window_rule"] = "granule"
        if seams["region_pow2"]:
            protection["window_rule"] = "pow2"
        protection["smallest_window"] = seams["min_region"]
    elif "min_region" in seams:
        protection["window_rule"] = "granule"
        protection["smallest_window"] = seams["no_unit_granule"]
    elif target["arch"] == "sim":
        # The sim's arch_mpu_min_region is the page size of the host it runs on, which is this one.
        protection["window_rule"] = "granule"
        protection["smallest_window"] = mmap.PAGESIZE
    else:
        raise Refused("the build states no arch_mpu_min_region seam for arch %s" % target["arch"])
    protection["thread_windows"] = knob(knobs, WINDOWS_KNOB)

    pools = {}
    for name in knobs:
        if is_pool(name):
            pools[name] = knob(knobs, name)

    threads = {
        "priority": facts["priority"],
        "min_stack": knob(knobs, "KICKOS_MIN_STACK_SIZE"),
        "user_stack": knob(knobs, "KICKOS_USER_STACK_SIZE"),
        "idle_stack": knob(knobs, "KICKOS_IDLE_STACK_SIZE"),
        "root_stack": knob(knobs, "KICKOS_ROOT_STACK_SIZE"),
        "stack_align": facts["stack_align"],
        "stack_stride": facts["stack_stride"],
    }

    document = {
        "version": MANIFEST_VERSION,
        "abi": facts["abi"],
        "target": target,
        "protection": protection,
        "pools": pools,
        "threads": threads,
    }
    if facts["descriptions"] is not None:
        document["descriptions"] = {
            "chip": facts["descriptions"]["chip"],
            "board": facts["descriptions"]["board"],
        }
    if facts["default"] is not None:
        document["default"] = {"composition": facts["default"]}
    document["drivers"] = catalogue(facts["drivers"])
    return document


def catalogue(drivers):
    """The catalogue by driver name, whose order a table's `driver` index counts in."""
    return {name: {field: drivers[name][field] for field in DRIVER_FIELDS} for name in sorted(drivers)}


def render(document):
    lines = ["# GENERATED at configure by tools/manifest/genmanifest.py. Edits are overwritten."]
    block(lines, document, "")
    return "\n".join(lines) + "\n"


def main(argv):
    if len(argv) != 2:
        print("usage: genmanifest.py <facts.json> <manifest.yaml>", file=sys.stderr)
        return 2
    try:
        with open(argv[0], encoding="utf-8") as stream:
            facts = json.load(stream)
        text = render(manifest(facts, read_config(facts["config"])))
    except (OSError, ValueError, KeyError, Refused) as error:
        print("genmanifest.py: %s" % error, file=sys.stderr)
        return 1
    try:
        with open(argv[1], encoding="utf-8") as stream:
            if stream.read() == text:
                return 0
    except OSError:
        pass
    with open(argv[1], "w", encoding="utf-8") as stream:
        stream.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
