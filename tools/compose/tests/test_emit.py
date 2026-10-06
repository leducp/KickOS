# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

import os
import shutil
import subprocess
import sys
import tempfile
import types
import unittest

from kickos_compose import descriptions, emit, partition, supply
from kickos_compose.subset import Report
from kickos_compose.__main__ import REFUSED
from test_arms import (
    ARM64_AMP, ARM64_PAIR, C6_AMP, C6_PAIR, MANIFESTS, PLATFORM, RP_AMP, RP_PAIR, SYSTEMS, TREE, VIRTIO, mutate, read,
    write,
)

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

    def test_a_driver_task_carries_its_ring_block_and_whether_it_takes_the_console(self):
        refusals, table, path, manifest = self.table("xmc4800-relax.yaml")
        console, spi, sensor = table.tasks[0], table.tasks[1], table.tasks[2]
        self.assertEqual((console.block, console.console), (1024, True))
        self.assertEqual((spi.block, spi.console), (0, False))
        self.assertEqual((sensor.block, sensor.console), (0, False))
        source = emit.render(table, "xmc4800-relax.yaml")
        self.assertEqual(source.count(".flags = KOS_TABLE_TASK_CONSOLE,"), 1)
        self.assertIn(".block = 1024,", source)

    def test_a_driver_s_lines_are_emitted_in_the_order_of_its_line_roles(self):
        refusals, table, path, manifest = self.table(
            "xmc4800-relax.yaml", [("    lines: { irq: /dev/usic0/sr1 }\n", "    lines: { err: /dev/usic0/sr2, irq: /dev/usic0/sr1 }\n")],
            [("    windows: [regs]\n    lines: [irq]\n    threads:\n      - { name: bus,",
              "    windows: [regs]\n    lines: [irq, err]\n    threads:\n      - { name: bus,")])
        self.assertEqual(refusals, [])
        lines = [(g.name, g.line) for g in self.grants_of(table, "spi0") if g.kind == "line"]
        self.assertEqual(lines, [("irq", 85), ("err", 86)])

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
        self.assertEqual([(g.kind, g.name, g.cap_slot, g.window, g.flags) for g in health],
                         [("notification", "/init/events", 0, None, []), ("status", "/init/status", None, 0, ["ro"]),
                          ("region", "/shm/history", None, 1, ["ro"])])
        # Its own two records, rounded as a region is.
        self.assertEqual(health[1].size, 32)
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

    def test_the_init_lowers_itself_above_the_lowest_unless_the_composition_states_its_priority(self):
        refusals, table, path, manifest = self.table("qemu-arm64.yaml")
        self.assertEqual(refusals, [])
        self.assertEqual(table.init_priority, 2)
        source, asserts, fragment, gate = self.system("qemu-arm64.yaml")
        self.assertIn("\n        .init_priority = 2,\n", source)
        narrowed = [("  priority: [1, 31]\n", "  priority: [4, 31]\n")]
        refusals, table, path, manifest = self.table("qemu-arm64.yaml", manifest_edits=narrowed)
        self.assertEqual(refusals, [])
        self.assertEqual(table.init_priority, 5)
        edits = [("heap: 65536\n", "heap: 65536\ninit: { priority: 7 }\n")]
        refusals, table, path, manifest = self.table("qemu-arm64.yaml", edits)
        self.assertEqual(refusals, [])
        self.assertEqual(table.init_priority, 7)
        source, asserts, fragment, gate = self.system("qemu-arm64.yaml", edits)
        self.assertIn("\n        .init_priority = 7,\n", source)

    def test_a_refused_composition_emits_nothing(self):
        edits = [("devices: [/dev/rtc]", "devices: [/dev/rtc9]")]
        refusals, table, path, manifest = self.table("qemu-arm64.yaml", edits)
        self.assertNotEqual(refusals, [])
        self.assertEqual(table, None)
        report, text = emit.emit(path, manifest)
        self.assertEqual(text, None)

    def test_a_table_layout_the_tool_does_not_emit_is_refused(self):
        edits = [("  table: 6\n", "  table: 5\n")]
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

    def system(self, name, edits=()):
        refusals, table, path, manifest = self.table(name, edits)
        self.assertEqual(refusals, [])
        report, texts = emit.emit_system(path, manifest)
        self.assertEqual([str(r) for r in report.refusals], [])
        return texts

    def test_the_table_defines_the_symbol_kickos_kernel_requires(self):
        source, asserts, fragment, gate = self.system("qemu-arm64.yaml")
        self.assertIn("\nchar const kickos_link_one_system_target = 1;\n", source)

    def test_each_reserved_stack_pays_the_thread_local_carve(self):
        source, asserts, fragment, gate = self.system("qemu-arm64.yaml")
        stacks = [line for line in asserts.splitlines() if line.endswith("__kickos_tls_carve,")]
        self.assertEqual(stacks, ["ASSERT(4096 >= 2816 + __kickos_tls_carve,",
                                  "ASSERT(8192 >= 2816 + __kickos_tls_carve,",
                                  "ASSERT(4096 >= 2816 + __kickos_tls_carve,"])
        self.assertIn("task `app`'s 8192-byte stack cannot hold the linked image's thread-local block", asserts)
        # A translating board places no task's stack in a region arena.
        self.assertNotIn("__kickos_ram_end", asserts)

    def test_a_masked_stack_is_the_chip_scripts_to_check_and_the_arena_is_replayed(self):
        source, asserts, fragment, gate = self.system("xmc4800-relax.yaml")
        self.assertNotIn("__kickos_tls_carve", asserts)
        placed = [line for line in asserts.splitlines() if line.startswith("__kickos_system_arena_")]
        self.assertEqual(placed[0], "__kickos_system_arena_0 = ALIGN(__kickos_ram_start, 0x200) + 0x200;")
        self.assertTrue(placed[1].startswith("__kickos_system_arena_1 = ALIGN(__kickos_system_arena_0, "))
        self.assertEqual(asserts.count("<= __kickos_ram_end,"), len(placed))
        self.assertIn("the arena cannot hold root's boot stack", asserts)
        self.assertIn("task `sensor`'s default stack", asserts)

    def system_heap(self, edits, manifest_edits=()):
        refusals, table, path, manifest = self.table("qemu-arm64.yaml", edits, manifest_edits)
        self.assertEqual(refusals, [])
        report, texts = emit.emit_system(path, manifest)
        return texts

    def test_a_composition_naming_no_board_takes_what_it_leaves_out_from_the_board_default(self):
        default = os.path.join("boards", "qemu-arm64", "composition.yaml")
        os.makedirs(os.path.dirname(os.path.join(self.scratch, default)), exist_ok=True)
        write(os.path.join(self.scratch, default), mutate(read(os.path.join(TREE, default)),
                                                          [("heap: 65536\n", "heap: 12288\n"),
                                                           ("    stack: 20480\n", "    stack: 24576\n")])[0])
        refusals, table, path, manifest = self.table(
            "qemu-arm64.yaml", [("board: qemu-arm64\n", ""), ("heap: 65536\n", ""), ("    stack: 8192\n", "")],
            [("drivers:\n  xmcssc:", "default:\n  composition: %s\ndrivers:\n  xmcssc:" % default)])
        self.assertEqual(refusals, [])
        self.assertEqual([(t.name, t.stack) for t in table.tasks if t.name == "app"], [("app", 24576)])
        report, (source, asserts, fragment, gate) = emit.emit_system(path, manifest)
        self.assertIn("set(KICKOS_COMPOSE_HEAP 12288)\n", fragment)

    def test_a_composition_naming_no_board_states_each_ceiling(self):
        default = os.path.join("boards", "qemu-arm64", "composition.yaml")
        os.makedirs(os.path.dirname(os.path.join(self.scratch, default)), exist_ok=True)
        write(os.path.join(self.scratch, default), read(os.path.join(TREE, default)))
        refusals, table, path, manifest = self.table(
            "qemu-arm64.yaml", [("board: qemu-arm64\n", ""), ("    priority: 8\n    ceiling: 8\n", "    priority: 8\n")],
            [("drivers:\n  xmcssc:", "default:\n  composition: %s\ndrivers:\n  xmcssc:" % default)])
        self.assertEqual([r.split(": ")[1] for r in refusals], ["form.missing"])
        self.assertIn("task `app` runs an `entry`, so it needs `ceiling`", refusals[0])

    def test_a_heap_of_zero_stays_zero(self):
        source, asserts, fragment, gate = self.system_heap([("heap: 65536\n", "heap: 0\n")])
        self.assertIn("set(KICKOS_COMPOSE_HEAP 0)\n", fragment)
        self.assertNotIn("_kickos_heap_limit", asserts)

    def test_the_heap_is_carried_to_the_link(self):
        source, asserts, fragment, gate = self.system_heap([("heap: 65536\n", "heap: 4096\n")])
        self.assertIn("set(KICKOS_COMPOSE_HEAP 4096)\n", fragment)
        self.assertIn("ASSERT(_kickos_heap_limit - _kickos_heap_start >= 4096,", asserts)

    def test_the_pad_is_checked_against_the_heap(self):
        refusals, table, path, manifest = self.table("xmc4800-relax.yaml")
        report, (source, asserts, fragment, gate) = emit.emit_system(path, manifest)
        self.assertIn("ASSERT(_kickos_heap_limit - _kickos_heap_start >= 16384,", asserts)
        self.assertIn("state a smaller `heap` in %s, or link a kernel package built to carve more, whose "
                      "KICKOS_APPDATA_SIZE is that span where the heap is the app window's pad" % path, asserts)
        self.assertIn("link exactly one system target, and define no KICKOS_USER_HEAP_SIZE of the app's own", asserts)

    def test_the_asserts_cite_the_composition_by_its_path(self):
        refusals, table, path, manifest = self.table("qemu-arm64.yaml")
        report, (source, asserts, fragment, gate) = emit.emit_system(path, manifest)
        self.assertIn("so its `stack` in %s would have to grow" % path, asserts)
        self.assertIn("is not the 65536-byte heap of %s, which" % path, asserts)
        self.assertIn("the 65536-byte heap of %s, its `heap`, is more than" % path, asserts)

    def test_each_entry_is_declared_at_its_line_in_the_composition(self):
        refusals, table, path, manifest = self.table("qemu-arm64.yaml")
        report, (source, asserts, fragment, gate) = emit.emit_system(path, manifest, "system.c")
        lines = source.splitlines()
        composition = read(path).splitlines()
        declared = [n for n, line in enumerate(lines) if line.startswith("extern void ")]
        self.assertEqual(len(declared), 3)
        for n in declared:
            entry = lines[n][len("extern void "):lines[n].index("(")]
            self.assertEqual(lines[n - 1], "#line %d \"%s\"" % (composition.index("    entry: " + entry) + 1, path))
        resumed = declared[-1] + 1
        self.assertEqual(lines[resumed], "#line %d \"system.c\"" % (resumed + 2))

    def test_the_asserts_spell_the_chip_scripts_symbols_with_the_abis_prefix(self):
        prefixed = [('  symbol_prefix: ""\n', "  symbol_prefix: _\n")]
        source, asserts, fragment, gate = self.system_heap((), prefixed)
        self.assertIn("ASSERT(4096 >= 2816 + ___kickos_tls_carve,", asserts)
        self.assertIn("ASSERT(__kickos_heap_limit - __kickos_heap_start >= 65536,", asserts)
        refusals, table, path, manifest = self.table("xmc4800-relax.yaml", manifest_edits=prefixed)
        report, (source, asserts, fragment, gate) = emit.emit_system(path, manifest)
        self.assertIn("__kickos_system_arena_0 = ALIGN(___kickos_ram_start, 0x200) + 0x200;", asserts)
        self.assertIn("ASSERT(__kickos_system_arena_0 <= ___kickos_ram_end,", asserts)
        source, asserts, fragment, gate = self.system("xmc4800-relax.yaml")
        self.assertIn("ALIGN(__kickos_ram_start, 0x200)", asserts)

    def test_a_hosted_image_brings_no_chip_script_asserts(self):
        report = Report()
        path = os.path.join(TREE, "platform", "sim", "chip.yaml")
        chip = descriptions.check_chip(path, read(path), report)
        self.assertEqual([str(r) for r in report.refusals], [])
        admitted = types.SimpleNamespace(chip=chip, heap=16384, manifest=None, tasks=[], shared=[],
                                         translating=False)
        asserts = emit.render_asserts(admitted, "composition.yaml")
        self.assertIn("ASSERT(KICKOS_USER_HEAP_SIZE == 16384,", asserts)
        for symbol in ("_kickos_heap_limit", "__kickos_ram_start", "__kickos_tls_carve"):
            with self.subTest(symbol=symbol):
                self.assertNotIn(symbol, asserts)

    def test_the_link_refuses_a_heap_symbol_other_than_the_systems(self):
        source, asserts, fragment, gate = self.system_heap([("heap: 65536\n", "heap: 0\n")])
        self.assertIn("ASSERT(KICKOS_USER_HEAP_SIZE == 0,", asserts)
        source, asserts, fragment, gate = self.system_heap(())
        self.assertIn("ASSERT(KICKOS_USER_HEAP_SIZE == 65536,", asserts)

    def test_the_fragment_names_the_drivers_and_the_packaged_main(self):
        source, asserts, fragment, gate = self.system("xmc4800-relax.yaml")
        self.assertIn('set(KICKOS_COMPOSE_DRIVERS "xmcuartirq;xmcssc")\n', fragment)
        self.assertIn('set(KICKOS_COMPOSE_CLIENTS "kickos_spi_proxy")\n', fragment)
        self.assertIn("set(KICKOS_COMPOSE_MAIN 0)\n", fragment)
        source, asserts, fragment, gate = self.system("qemu-arm64.yaml", [("entry: app_main", "entry: kickos_main")])
        self.assertIn('set(KICKOS_COMPOSE_DRIVERS "")\n', fragment)
        self.assertIn('set(KICKOS_COMPOSE_CLIENTS "")\n', fragment)
        self.assertIn("set(KICKOS_COMPOSE_MAIN 1)\n", fragment)

    def test_the_fragment_lists_each_client_once_in_the_order_the_drivers_first_name_it(self):
        edits = [("    client: []\n", "    client: [kickos_zeta, kickos_spi_proxy]\n"),
                 ("client: [kickos_spi_proxy]\n", "client: [kickos_spi_proxy, kickos_alpha]\n")]
        refusals, table, path, manifest = self.table("xmc4800-relax.yaml", manifest_edits=edits)
        self.assertEqual(refusals, [])
        report, (source, asserts, fragment, gate) = emit.emit_system(path, manifest)
        self.assertEqual([str(r) for r in report.refusals], [])
        self.assertIn('set(KICKOS_COMPOSE_DRIVERS "xmcuartirq;xmcssc")\n', fragment)
        self.assertIn('set(KICKOS_COMPOSE_CLIENTS "kickos_zeta;kickos_spi_proxy;kickos_alpha")\n', fragment)

    def emit_cli(self, path, manifest, outputs):
        env = dict(os.environ)
        env["PYTHONPATH"] = os.path.join(TREE, "tools", "compose")
        return subprocess.run([sys.executable, "-m", "kickos_compose", "emit", path, "--manifest", manifest,
                               "-o", outputs[0], "--asserts", outputs[1], "--fragment", outputs[2]],
                              env=env, capture_output=True, text=True)

    def test_a_refused_system_leaves_none_of_its_three_files(self):
        refusals, table, path, manifest = self.table("qemu-arm64.yaml")
        outputs = [os.path.join(self.scratch, name) for name in ("system.c", "system.ld", "system.cmake")]
        run = self.emit_cli(path, manifest, outputs)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual([os.path.isfile(output) for output in outputs], [True, True, True])
        refusals, table, path, manifest = self.table("qemu-arm64.yaml", [("devices: [/dev/rtc]", "devices: [/dev/rtc9]")])
        run = self.emit_cli(path, manifest, outputs)
        self.assertEqual(run.returncode, REFUSED)
        self.assertEqual([os.path.isfile(output) for output in outputs], [False, False, False])

    def test_only_a_refusal_exits_refused(self):
        refusals, table, path, manifest = self.table("qemu-arm64.yaml", [("devices: [/dev/rtc]", "devices: [/dev/rtc9]")])
        outputs = [os.path.join(self.scratch, name) for name in ("system.c", "system.ld", "system.cmake")]
        self.assertEqual(self.emit_cli(path, manifest, outputs).returncode, REFUSED)
        missing = os.path.join(self.scratch, "no-manifest.yaml")
        run = self.emit_cli(path, missing, outputs)
        self.assertNotIn(run.returncode, (0, REFUSED), run.stdout + run.stderr)

    def cost(self, name, edits=()):
        path = os.path.join(self.systems, "cost-" + name)
        write(path, mutate(read(os.path.join(SYSTEMS, name)), edits)[0])
        manifest = os.path.join(self.scratch, "manifest.yaml")
        write(manifest, MANIFESTS[name])
        env = dict(os.environ)
        env["PYTHONPATH"] = os.path.join(TREE, "tools", "compose")
        return subprocess.run([sys.executable, "-m", "kickos_compose", "cost", path, "--manifest", manifest],
                              env=env, capture_output=True, text=True)

    def test_cost_prints_what_the_init_spends(self):
        run = self.cost("qemu-x86_64.yaml")
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(run.stdout, "cap_slots 6\nendpoints 1\nnotifications 2\nthreads 3\ntasks 3\nirq_handles 0\ndomains 6\n"
                                     "reservations 6\nself_grants 2\n")

    def test_cost_of_a_refused_composition_prints_its_refusals(self):
        run = self.cost("qemu-x86_64.yaml", [("devices: [/dev/cmos_rtc]", "devices: [/dev/cmos_rtc9]")])
        self.assertEqual(run.returncode, REFUSED)
        self.assertIn("1 refusal(s) in 1 composition(s) read", run.stdout)

class Partitioned(unittest.TestCase):
    """What a partition's node compositions emit: crossings, placed regions and the gate rows."""

    def setUp(self):
        self.scratch = tempfile.mkdtemp(prefix="kickos-partition-")
        shutil.copytree(PLATFORM, os.path.join(self.scratch, "platform"))
        self.manifest = os.path.join(self.scratch, "manifest.yaml")

    def tearDown(self):
        shutil.rmtree(self.scratch)

    def nodes(self, texts, manifest, edits=None):
        write(self.manifest, manifest)
        paths = []
        for k, text in enumerate(texts):
            path = os.path.join(self.scratch, "systems", "node%d.yaml" % k)
            write(path, mutate(text, (edits or {}).get(k, []))[0])
            paths.append(path)
        return paths

    def rows(self, texts, manifest, edits=None):
        report, found = partition.admit_partition(self.nodes(texts, manifest, edits), self.manifest, 0)
        self.assertEqual([str(r) for r in report.refusals], [])
        return [(r.gate, r.node, r.base, r.size, r.access, r.register) for r in found.rows]

    def test_a_crossing_is_a_port_grant_and_orders_nothing(self):
        paths = self.nodes(ARM64_PAIR, ARM64_AMP)
        report, texts = emit.emit_system(None, self.manifest, "table.c", paths)
        self.assertEqual([str(r) for r in report.refusals], [])
        source = texts[0]
        self.assertIn(".kind = KOS_GRANT_PORT,", source)
        self.assertIn(".flags = KOS_CAP_SIGNAL,", source)
        self.assertIn(".use_count = 0,", source)
        self.assertIn(".offset = 0x0u, .flags = KOS_MEM_NOCACHE | KOS_TABLE_REGION_PARTITION", source)
        self.assertIn("set(KICKOS_COMPOSE_GATE 1)", texts[2])
        self.assertIn("kickos_gate_row_count = 0;", texts[3])

    def test_partition_regions_are_placed_in_node_0_s_declaration_order(self):
        second = ("  - name: /shm/book\n", "  - name: /shm/log\n    size: 0x1001\n    cache: uncached\n"
                  "    partition: true\n  - name: /shm/book\n")
        reordered = ("    partition: true\ntasks:", "    partition: true\n  - name: /shm/log\n    size: 0x1001\n"
                     "    cache: uncached\n    partition: true\ntasks:")
        paths = self.nodes(ARM64_PAIR, ARM64_AMP, {0: [second], 1: [reordered]})
        report, found = partition.admit_partition(paths, self.manifest, 0)
        self.assertEqual([str(r) for r in report.refusals], [])
        self.assertEqual(found.admitted[0].offsets, {"/shm/log": 0, "/shm/book": 0x2000})
        self.assertEqual(found.admitted[1].offsets, {"/shm/log": 0, "/shm/book": 0x2000})

    def test_the_c6_rows_give_each_node_its_grants_and_nothing_else(self):
        hp = ("    uses: [/amp/3]\n", "    uses: [/amp/3]\n    devices: [/dev/timg0]\n")
        lp = ("    serves: /amp/3\n", "    serves: /amp/3\n    devices: [/dev/timg1]\n")
        rows = self.rows(C6_PAIR, C6_AMP, {0: [hp], 1: [lp]})
        self.assertEqual(rows, [
            (0, 0, 0x60008000, 0x1000, "rw", 0),
            (0, 1, 0x40878000, 0x8000, "rw", 0), (0, 1, 0x60009000, 0x1000, "rw", 0),
            (0, 1, 0x4083C000, 0x3C000, "rwx", 0),
            (1, 1, 0x600B0000, 0x400, "rw", 0), (1, 1, 0x600B0C00, 0x400, "rw", 0),
            (1, 1, 0x70000000, 0x400, "rwx", 0),
        ])

    def test_the_rp2350_rows_name_each_granted_register(self):
        hp = ("    uses: [/amp/3]\n", "    uses: [/amp/3]\n    devices: [/dev/uart0]\n")
        self.assertEqual(self.rows(RP_PAIR, RP_AMP, {0: [hp]}), [(0, 0, 0x40070000, 0x4000, "rw", 0xA0)])

    def test_only_node_0_and_a_chip_with_a_gate_emit_one(self):
        write(self.manifest, MANIFESTS["qemu-arm64.yaml"])
        report, text = emit.emit_gate(self.manifest)
        self.assertEqual((report.refusals, text), ([], None))
        write(self.manifest, ARM64_AMP)
        report, text = emit.emit_gate(self.manifest)
        self.assertIn("kickos_gate_row_count = 0;", text)
        write(self.manifest, C6_AMP.replace("    node: 0\n", "    node: 1\n"))
        report, text = emit.emit_gate(self.manifest)
        self.assertEqual((report.refusals, text), ([], None))
        write(self.manifest, mutate(C6_AMP, [("  amp:\n    node: 0\n    nodes: 2\n    ports: [[0, 2], [1, 3]]\n"
                                              "    share: 0x4000\n    share_cache: cached\n"
                                              "    slices: [0x40800000, 0x3C000]\n    window: [0x40878000, 0x8000]\n", "")])[0])
        report, text = emit.emit_gate(self.manifest)
        self.assertIn("kickos_gate_row_count = 0;", text)

    def test_the_command_line_refuses_a_partition_as_it_refuses_a_composition(self):
        paths = self.nodes(ARM64_PAIR, ARM64_AMP, {1: [("    serves: /amp/3\n", "")]})
        env = dict(os.environ)
        env["PYTHONPATH"] = os.path.join(TREE, "tools", "compose")
        run = subprocess.run([sys.executable, "-m", "kickos_compose", "partition"] + paths
                             + ["--manifest", self.manifest, "--node", "0"], env=env, capture_output=True, text=True)
        self.assertEqual(run.returncode, REFUSED, run.stdout + run.stderr)
        self.assertIn("partition.unserved", run.stdout)


class Reserved(unittest.TestCase):
    """The init's private block and its reservations name the same shared regions."""

    def test_a_region_admitted_no_size_has_no_record_and_no_reservation(self):
        class Region:
            def __init__(self, size):
                self.size = size
                self.partition = False

        class Manifest:
            status_record_size = 8
            private_record_size = 64
            stack_stride = 4096

        shared = {"/shm/a": Region(64), "/shm/b": Region(None)}
        reserved = supply.init_reservations([], shared, Manifest())
        self.assertEqual(reserved, [64, 64])
        self.assertEqual(supply.private_block([], shared, Manifest()), 64)


if __name__ == "__main__":
    unittest.main()
