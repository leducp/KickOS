# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Each golden composition's table, emitted twice by the command line under two hash seeds and
# compiled on the host with <kickos/sys/table.h> and table_walker.c. The two emits are
# byte-identical, and the walker's dump of the compiled table is the tool's dump of its model, so
# the compiler checks the layout and the emitter together. tests/static/check_platform.sh runs it:
#
#   round_trip.py <host C compiler> <the build's generated include directory>

import os
import shutil
import subprocess
import sys
import tempfile
import unittest

from kickos_compose import emit
from test_arms import MANIFESTS, PLATFORM, SYSTEMS, TREE, VIRTIO, mutate, read, write

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "..")

# A golden system granting what none of the three does: an instance's window and line, an
# uncached region, authority, a task whose return ends the system, and the init's priority.
VARIANTS = [
    ("qemu-arm64.yaml", "qemu-arm64-variant.yaml", [
        ("devices: [/dev/rtc]", VIRTIO % "bus_master, coarse_gate" + "\n    lines: { irq: /dev/virtio/3/irq }"),
        ("cache: cached", "cache: uncached"),
        ("ends: never", "ends: health"),
        ("heap: 65536\n", "heap: 65536\ninit: { priority: 5 }\n"),
        ("    watches: [sensor]\n", "    watches: [sensor]\n    authority: [system, memory]\n"),
    ]),
]

# Hand-written from the chip files and the compositions, never from the tool: each is (record,
# what picks one record out, what that record shows), the walker's dump of the compiled table
# being what they are read from.
EXPECTED = {
    "xmc4800-relax.yaml": [
        ("grant", {"path": "/dev/usic0/ch0"}, {"kind": "window", "base": "0x40030000", "size": "0x200",
                                              "privs": "0+3", "window": "none"}),
        ("grant", {"path": "/dev/usic0/ch1"}, {"kind": "window", "base": "0x40030200", "size": "0x200",
                                              "privs": "0+3"}),
        ("priv", {"#": "0"}, {"offset": "0x10", "width": "4"}),
        ("priv", {"#": "1"}, {"offset": "0x14", "width": "4"}),
        ("priv", {"#": "2"}, {"offset": "0x40", "width": "4"}),
        ("grant", {"kind": "endpoint_use", "path": "/svc/spi0"}, {"target": "1", "cap_slot": "cap0+0"}),
        ("grant", {"kind": "endpoint_use", "path": "/svc/sensor"}, {"target": "2"}),
        ("task", {"name": "health"}, {"watches": "2+2", "authority": "-"}),
        ("ref", {"#": "2"}, {"task": "1"}),
        ("ref", {"#": "3"}, {"task": "2"}),
        ("task", {"name": "console"}, {"driver": "1", "entry": "xmcuartirq_console_start", "stack": "0",
                                       "block": "1024", "flags": "console"}),
        ("task", {"name": "spi0"}, {"driver": "0", "restart_max": "3", "block": "0", "flags": "-"}),
        ("task", {"name": "sensor"}, {"block": "0", "flags": "-"}),
        ("task", {"#": "0"}, {"name": "console"}),
        ("string", {"offset": "0"}, {"text": "console"}),
        ("string", {"offset": "8"}, {"text": "regs"}),
        ("string", {"offset": "13"}, {"text": "/dev/usic0/ch0"}),
        ("region", {"name": "/shm/history"}, {"size": "0x40", "flags": "-"}),
        ("grant", {"kind": "region", "flags": "-"}, {"window": "0", "target": "0"}),
        ("grant", {"kind": "status"}, {"path": "/init/status", "flags": "ro", "cap_slot": "none", "window": "0",
                                       "size": "0x20"}),
        ("grant", {"kind": "region", "flags": "ro"}, {"window": "1"}),
    ],
    "qemu-arm64.yaml": [
        ("task", {"name": "app"}, {"core_mask": "0x4"}),
        ("task", {"name": "sensor"}, {"core_mask": "0x2"}),
        ("task", {"name": "health"}, {"core_mask": "0x0"}),
        ("grant", {"path": "/dev/rtc"}, {"base": "0x9010000", "size": "0x1000", "window": "0"}),
        ("region", {"name": "/shm/history"}, {"size": "0x1000"}),
        ("grant", {"kind": "region", "flags": "-"}, {"window": "1"}),
        ("grant", {"kind": "status"}, {"window": "0", "size": "0x1000"}),
        ("grant", {"kind": "region", "flags": "ro"}, {"window": "1"}),
    ],
    "qemu-x86_64.yaml": [
        ("header", {}, {"init_priority": "2"}),
        ("grant", {"path": "/dev/cmos_rtc"}, {"kind": "ports", "base": "0x70", "size": "0x2", "privs": "0+1",
                                             "window": "0"}),
        ("priv", {"#": "0"}, {"offset": "0x0", "width": "1"}),
    ],
    "qemu-arm64-variant.yaml": [
        ("task", {"name": "health"}, {"authority": "memory,system"}),
        ("grant", {"path": "/dev/virtio/31"}, {"base": "0xA003E00", "size": "0x200", "window": "1"}),
        ("grant", {"kind": "line"}, {"window": "none", "cap_slot": "cap0+0"}),
        ("grant", {"kind": "line"}, {"path": "/dev/virtio/3/irq", "line": "51", "line_index": "0"}),
        ("header", {}, {"flags": "ends_task", "ends_task": "2", "init_priority": "5"}),
        ("region", {"#": "0"}, {"flags": "uncached"}),
    ],
}

COMPILER = None
INCLUDE = None


def records(dump):
    """Each line of a dump as (record, {field: value}), its index as `#`."""
    found = []
    for line in dump.splitlines():
        words = line.split(" ")
        if words[0] == "string":
            found.append(("string", {"offset": words[1], "text": " ".join(words[2:])}))
            continue
        fields = {}
        for word in words[1:]:
            if "=" in word:
                key, value = word.split("=", 1)
                fields[key] = value
            else:
                fields["#"] = word
        found.append((words[0], fields))
    return found


def stub_source(table):
    """Each entry the table names, defined, and the walker's map from its address to its name."""
    out = ["#include <kickos/sys/table.h>", "", "#include <stddef.h>", "#include <stdint.h>", "",
           "struct entry_symbol", "{", "    char const* name;", "    uintptr_t address;", "};", ""]
    # A distinct body each, so no two entries fold to one address.
    out.extend(["static volatile int entry_sink;", ""])
    for n, (symbol, kind) in enumerate(table.externs):
        if kind == "driver":
            out.append("int %s(struct kos_service_cfg const* cfg)" % symbol)
            out.extend(["{", "    (void)cfg;", "    return %d;" % (n + 1), "}", ""])
        else:
            out.append("void %s(kos_self_t const* self)" % symbol)
            out.extend(["{", "    (void)self;", "    entry_sink = %d;" % (n + 1), "}", ""])
    out.append("struct entry_symbol const entry_symbols[] = {")
    for symbol, kind in table.externs:
        out.append("    {\"%s\", (uintptr_t)&%s}," % (symbol, symbol))
    out.append("    {NULL, 0}};")
    return "\n".join(out) + "\n"


def emit_with_seed(path, manifest, output, seed):
    env = dict(os.environ)
    env["PYTHONHASHSEED"] = seed
    env["PYTHONPATH"] = TOOL
    return subprocess.run([sys.executable, "-m", "kickos_compose", "emit", path, "--manifest", manifest,
                           "-o", output], env=env, capture_output=True, text=True)


class RoundTrip(unittest.TestCase):
    def test_every_golden_has_a_control_manifest(self):
        goldens = sorted(name for name in os.listdir(SYSTEMS) if name.endswith(".yaml"))
        self.assertNotEqual(goldens, [])
        self.assertEqual(goldens, sorted(MANIFESTS))

    def test_round_trip(self):
        scratch = tempfile.mkdtemp(prefix="kickos-table-")
        try:
            root = os.path.join(scratch, "platform")
            shutil.copytree(PLATFORM, root)
            systems = os.path.join(scratch, "systems")
            shutil.copytree(SYSTEMS, systems)
            cases = [(name, name) for name in sorted(MANIFESTS)]
            for base, name, edits in VARIANTS:
                write(os.path.join(systems, name), mutate(read(os.path.join(SYSTEMS, base)), edits)[0])
                cases.append((base, name))
            for base, name in cases:
                with self.subTest(composition=name):
                    self.round_trip(scratch, os.path.join(systems, name), MANIFESTS[base])
        finally:
            shutil.rmtree(scratch)

    def round_trip(self, scratch, path, manifest_text):
        work = os.path.join(scratch, os.path.basename(path)[:-len(".yaml")])
        os.makedirs(work)
        manifest = os.path.join(scratch, "manifest.yaml")
        write(manifest, manifest_text)

        outputs = []
        for seed in ("0", "1"):
            os.makedirs(os.path.join(work, "seed-" + seed))
            output = os.path.join(work, "seed-" + seed, "table.c")
            run = emit_with_seed(path, manifest, output, seed)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            outputs.append(read(output))
        self.assertEqual(outputs[0], outputs[1], "two emits of one composition differ")

        report, table = emit.table_of(path, manifest)
        self.assertEqual([str(r) for r in report.refusals], [])
        self.assertEqual(emit.render(table, os.path.basename(path), path), outputs[0])

        stubs = os.path.join(work, "stubs.c")
        write(stubs, stub_source(table))
        walker = os.path.join(work, "walker")
        compile_run = subprocess.run(
            [COMPILER, "-std=c11", "-pedantic-errors", "-Wall", "-Wextra", "-Werror",
             "-I", os.path.join(TREE, "user", "include"), "-I", os.path.join(TREE, "system", "include"),
             "-I", INCLUDE, os.path.join(work, "seed-0", "table.c"), stubs, os.path.join(HERE, "table_walker.c"),
             "-o", walker], capture_output=True, text=True)
        self.assertEqual(compile_run.returncode, 0, compile_run.stdout + compile_run.stderr)
        walk = subprocess.run([walker], capture_output=True, text=True)
        self.assertEqual(walk.returncode, 0, walk.stdout + walk.stderr)
        self.assertEqual(walk.stdout, emit.dump(table))
        name = os.path.basename(path)
        self.assertIn(name, EXPECTED)
        dumped = records(walk.stdout)
        for record, pick, want in EXPECTED[name]:
            picked = [fields for kind, fields in dumped
                      if kind == record and all(fields.get(k) == v for k, v in pick.items())]
            self.assertEqual(len(picked), 1, "%s %r picks %d records" % (record, pick, len(picked)))
            self.assertEqual({k: picked[0].get(k) for k in want}, want, "%s %r" % (record, pick))

    def test_the_walker_faults_a_task_field_no_emit_writes(self):
        scratch = tempfile.mkdtemp(prefix="kickos-table-")
        try:
            root = os.path.join(scratch, "platform")
            shutil.copytree(PLATFORM, root)
            manifest = os.path.join(scratch, "manifest.yaml")
            write(manifest, MANIFESTS["xmc4800-relax.yaml"])
            report, table = emit.table_of(os.path.join(SYSTEMS, "xmc4800-relax.yaml"), manifest)
            sensor = [task for task in table.tasks if task.name == "sensor"][0]
            rendered = emit.render(table, "xmc4800-relax.yaml")
            sensor.block = 64
            cases = [("control", rendered, 0),
                     ("a user task's ring block", emit.render(table, "xmc4800-relax.yaml"), 1),
                     ("an unknown task flag", rendered.replace(".flags = KOS_TABLE_TASK_CONSOLE,", ".flags = 2,"), 1)]
            stubs = os.path.join(scratch, "stubs.c")
            write(stubs, stub_source(table))
            for what, source, faults in cases:
                with self.subTest(case=what):
                    path = os.path.join(scratch, "table.c")
                    write(path, source)
                    walker = os.path.join(scratch, "walker")
                    built = subprocess.run(
                        [COMPILER, "-std=c11", "-pedantic-errors", "-Wall", "-Wextra", "-Werror",
                         "-I", os.path.join(TREE, "user", "include"), "-I", os.path.join(TREE, "system", "include"),
                         "-I", INCLUDE, path, stubs, os.path.join(HERE, "table_walker.c"), "-o", walker],
                        capture_output=True, text=True)
                    self.assertEqual(built.returncode, 0, built.stderr)
                    walk = subprocess.run([walker], capture_output=True, text=True)
                    self.assertEqual("faults=" in walk.stdout, faults != 0, walk.stdout[-200:])
        finally:
            shutil.rmtree(scratch)

    def test_an_entry_declared_otherwise_is_reported_at_its_line_in_the_composition(self):
        scratch = tempfile.mkdtemp(prefix="kickos-table-")
        try:
            shutil.copytree(PLATFORM, os.path.join(scratch, "platform"))
            path = os.path.join(scratch, "qemu-arm64.yaml")
            shutil.copyfile(os.path.join(SYSTEMS, "qemu-arm64.yaml"), path)
            manifest = os.path.join(scratch, "manifest.yaml")
            write(manifest, MANIFESTS["qemu-arm64.yaml"])
            source = os.path.join(scratch, "table.c")
            run = emit_with_seed(path, manifest, source, "0")
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            line = read(path).splitlines().index("    entry: sensor_main") + 1
            # The app declaring the entry as something else, as a missing or mistyped one would be.
            other = os.path.join(scratch, "other.h")
            write(other, "extern int sensor_main;\n")
            built = subprocess.run(
                [COMPILER, "-std=c11", "-I", os.path.join(TREE, "user", "include"),
                 "-I", os.path.join(TREE, "system", "include"), "-I", INCLUDE, "-include", other,
                 "-c", source, "-o", source + ".o"], capture_output=True, text=True)
            self.assertNotEqual(built.returncode, 0)
            self.assertIn("%s:%d:" % (path, line), built.stderr)
            # What follows the declarations is reported at its own line in the emitted source.
            self.assertNotIn("%s:%d:" % (path, line + 1), built.stderr)
        finally:
            shutil.rmtree(scratch)

    def test_a_slot_reading_as_none_does_not_compile(self):
        scratch = tempfile.mkdtemp(prefix="kickos-table-")
        try:
            root = os.path.join(scratch, "platform")
            shutil.copytree(PLATFORM, root)
            manifest = os.path.join(scratch, "manifest.yaml")
            write(manifest, MANIFESTS["qemu-arm64.yaml"])
            report, table = emit.table_of(os.path.join(SYSTEMS, "qemu-arm64.yaml"), manifest)
            granted = [grant for grant in table.grants if grant.cap_slot is not None][0]
            results = []
            for ordinal in (0xFFFD, 0xFFFE):
                granted.cap_slot = ordinal
                source = os.path.join(scratch, "table-%d.c" % ordinal)
                write(source, emit.render(table, "qemu-arm64.yaml"))
                results.append(subprocess.run(
                    [COMPILER, "-std=c11", "-pedantic-errors", "-Wall", "-Wextra", "-Werror",
                     "-I", os.path.join(TREE, "user", "include"), "-I", os.path.join(TREE, "system", "include"),
                     "-I", INCLUDE, "-c", source, "-o", source + ".o"], capture_output=True, text=True))
            self.assertEqual(results[0].returncode, 0, results[0].stderr)
            self.assertNotEqual(results[1].returncode, 0)
            self.assertIn("reads as none", results[1].stderr)
        finally:
            shutil.rmtree(scratch)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("usage: round_trip.py <host C compiler> <generated include directory>", file=sys.stderr)
        sys.exit(2)
    COMPILER = sys.argv[1]
    INCLUDE = sys.argv[2]
    if not os.path.isdir(INCLUDE):
        print("round_trip.py: no include directory %s" % INCLUDE, file=sys.stderr)
        sys.exit(2)
    unittest.main(argv=sys.argv[:1], verbosity=2)
