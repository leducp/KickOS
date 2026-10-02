# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

import os
import shutil
import subprocess
import sys
import tempfile
import unittest

from kickos_compose import emit
from test_arms import MANIFESTS, PLATFORM, SYSTEMS, TREE, VIRTIO, mutate, read, write

sys.path.insert(0, os.path.join(TREE, "tools", "manifest"))
import genmanifest  # noqa: E402


class Emitted(unittest.TestCase):
    """What the model of each golden composition holds, against the chip files' own figures."""

    @classmethod
    def setUpClass(cls):
        cls.scratch = tempfile.mkdtemp(prefix="kickos-emit-")
        cls.root = os.path.join(cls.scratch, "platform")
        shutil.copytree(PLATFORM, cls.root)
        cls.systems = os.path.join(cls.scratch, "systems")
        shutil.copytree(SYSTEMS, cls.systems)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.scratch)

    def table(self, name, edits=(), manifest_edits=()):
        path = os.path.join(self.systems, "emitted-" + name)
        write(path, mutate(read(os.path.join(SYSTEMS, name)), edits)[0])
        manifest = os.path.join(self.scratch, "manifest.yaml")
        write(manifest, mutate(MANIFESTS[name], manifest_edits)[0])
        report, table = emit.table_of(path, manifest)
        return [str(r) for r in report.refusals], table, path, manifest

    def grants_of(self, table, task):
        entry = [t for t in table.tasks if t.name == task][0]
        return table.grants[entry.first_grant:entry.first_grant + entry.grant_count]

    def test_drivers_start_from_the_catalogue_and_take_no_slot(self):
        refusals, table, path, manifest = self.table("xmc4800-relax.yaml")
        self.assertEqual(refusals, [])
        console, spi = table.tasks[0], table.tasks[1]
        self.assertEqual((console.entry, console.driver, spi.entry, spi.driver),
                         ("xmcuartirq_console_start", 1, "xmc_spi0_start", 0))
        self.assertEqual((console.stack, console.cap_grant_count), (0, 0))
        grants = self.grants_of(table, "spi0")
        self.assertEqual([(g.kind, g.name, g.path, g.cap_slot) for g in grants],
                         [("window", "regs", "/dev/usic0/ch1", None), ("line", "irq", "/dev/usic0/sr1", None),
                          ("endpoint_serve", "/svc/spi0", "/svc/spi0", None)])
        self.assertEqual((grants[0].base, grants[0].size), (0x40030200, 0x200))
        self.assertEqual((grants[1].line, grants[1].line_index), (85, 1))
        self.assertEqual(table.externs[:2], [("xmcuartirq_console_start", "driver"), ("xmc_spi0_start", "driver")])

    def test_a_driver_task_restarts_and_holds_no_authority(self):
        refusals, table, path, manifest = self.table("xmc4800-relax.yaml")
        console, spi = table.tasks[0], table.tasks[1]
        self.assertEqual((console.restart_max, spi.restart_max), (0, 3))
        self.assertEqual((console.authority, spi.authority), ([], []))

    def test_the_catalogue_is_written_by_name(self):
        entry = dict((field, field) for field in genmanifest.DRIVER_FIELDS)
        written = genmanifest.catalogue({"xmcuartirq": entry, "xmcssc": entry, "f4uartirq": entry})
        self.assertEqual(list(written), ["f4uartirq", "xmcssc", "xmcuartirq"])

    def test_a_region_past_32_bits_emits_nothing(self):
        refusals, table, path, manifest = self.table("qemu-arm64.yaml", [("    size: 64\n", "    size: 0xFFFFF001\n")])
        self.assertEqual(table, None)
        self.assertEqual([r.split(": ")[1] for r in refusals], ["supply.size"])

    def test_memory_registers_are_words_shared_by_the_device(self):
        refusals, table, path, manifest = self.table("xmc4800-relax.yaml")
        self.assertEqual(table.privs, [(0x10, 4), (0x14, 4), (0x40, 4)])
        windows = [g for g in table.grants if g.kind == "window"]
        self.assertEqual([(g.path, g.priv_first, g.priv_count) for g in windows],
                         [("/dev/usic0/ch0", 0, 3), ("/dev/usic0/ch1", 0, 3)])

    def test_user_grants_number_their_capabilities_in_declaration_order(self):
        refusals, table, path, manifest = self.table("xmc4800-relax.yaml")
        sensor = self.grants_of(table, "sensor")
        self.assertEqual([(g.kind, g.cap_slot, g.target) for g in sensor],
                         [("endpoint_use", 0, 1), ("endpoint_serve", 1, 2), ("region", None, 0)])
        health = self.grants_of(table, "health")
        self.assertEqual([(g.kind, g.name, g.cap_slot, g.flags) for g in health],
                         [("notification", "/init/events", 0, []), ("region", "/shm/history", None, ["ro"])])
        entry = table.tasks[4]
        self.assertEqual(table.refs[entry.first_watch:entry.first_watch + entry.watch_count], [1, 2])
        self.assertEqual((entry.use_count, entry.cap_grant_count), (0, 1))
        self.assertEqual([(r.name, r.size, r.uncached) for r in table.regions], [("/shm/history", 64, False)])
        self.assertEqual(table.ends_task, None)

    def test_ports_and_their_byte_registers(self):
        refusals, table, path, manifest = self.table("qemu-x86_64.yaml")
        self.assertEqual(refusals, [])
        ports = self.grants_of(table, "sensor")[0]
        self.assertEqual((ports.kind, ports.base, ports.size, ports.priv_first, ports.priv_count),
                         ("ports", 0x70, 2, 0, 1))
        self.assertEqual(table.privs, [(0, 1)])
        self.assertEqual(table.tasks[0].core_mask, 1 << 1)

    def test_an_instance_window_and_line_count_from_their_device(self):
        edits = [("devices: [/dev/rtc]",
                  VIRTIO % "bus_master, coarse_gate" + "\n    lines: { irq: /dev/virtio/3/irq }"),
                 ("cache: cached", "cache: uncached"), ("ends: never", "ends: health"),
                 ("    watches: [sensor]\n", "    watches: [sensor]\n    authority: [system, memory]\n")]
        refusals, table, path, manifest = self.table("qemu-arm64.yaml", edits)
        self.assertEqual(refusals, [])
        sensor = self.grants_of(table, "sensor")
        virtio = [g for g in sensor if g.path == "/dev/virtio/31"][0]
        self.assertEqual((virtio.base, virtio.size), (0x0A000000 + 31 * 0x200, 0x200))
        line = [g for g in sensor if g.kind == "line"][0]
        self.assertEqual((line.name, line.line, line.line_index, line.cap_slot), ("irq", 48 + 3, 0, 0))
        self.assertEqual([g.kind for g in sensor if g.cap_slot == 1], ["endpoint_serve"])
        self.assertEqual([(r.size, r.uncached) for r in table.regions], [(0x1000, True)])
        region = [g for g in sensor if g.kind == "region"][0]
        self.assertEqual(region.flags, ["uncached"])
        self.assertEqual((table.ends_task, table.tasks[2].authority), (2, ["memory", "system"]))

    def test_a_refused_composition_emits_nothing(self):
        edits = [("devices: [/dev/rtc]", "devices: [/dev/rtc9]")]
        refusals, table, path, manifest = self.table("qemu-arm64.yaml", edits)
        self.assertNotEqual(refusals, [])
        self.assertEqual(table, None)
        report, text = emit.emit(path, manifest)
        self.assertEqual(text, None)

    def test_a_table_layout_the_tool_does_not_emit_is_refused(self):
        edits = [("  table: 1\n", "  table: 2\n")]
        refusals, table, path, manifest = self.table("qemu-arm64.yaml", manifest_edits=edits)
        self.assertEqual(table, None)
        self.assertEqual([r.split(": ")[1] for r in refusals], ["form.version"])

    def test_a_file_name_past_ascii_is_escaped(self):
        path = os.path.join(self.systems, "syst\u00e8me.yaml")
        write(path, read(os.path.join(SYSTEMS, "qemu-arm64.yaml")))
        manifest = os.path.join(self.scratch, "manifest.yaml")
        write(manifest, MANIFESTS["qemu-arm64.yaml"])
        output = os.path.join(self.scratch, "table.c")
        env = dict(os.environ)
        env["PYTHONPATH"] = os.path.join(TREE, "tools", "compose")
        run = subprocess.run([sys.executable, "-m", "kickos_compose", "emit", path, "--manifest", manifest,
                              "-o", output], env=env, capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn("// GENERATED by kickos_compose emit from syst\\xe8me.yaml;", read(output))

if __name__ == "__main__":
    unittest.main()
