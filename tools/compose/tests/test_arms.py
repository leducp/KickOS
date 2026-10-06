# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

import ast
import os
import shutil
import sys
import tempfile
import unittest
from unittest import mock

from kickos_compose import composition, descriptions, emit, manifest, partition, subset, supply
from kickos_compose.composition import admit, region_size
from kickos_compose.descriptions import check_platform
from kickos_compose.manifest import check_manifests
from kickos_compose.subset import RULES

TREE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..")
PLATFORM = os.path.join(TREE, "platform")
SYSTEMS = os.path.join(TREE, "examples", "composition", "systems")
XMC_DEFAULT = os.path.join("boards", "xmc4800-relax", "composition.yaml")
SOURCES = tuple(os.path.realpath(module.__file__)
                for module in (composition, descriptions, emit, manifest, partition, subset, supply))


def mutate(text, edits):
    """The text with each (old, new) applied once, and the lines each new text spans."""
    lines = set()
    for old, new in edits:
        count = text.count(old)
        if count != 1:
            raise AssertionError("the mutation site %r occurs %d times in the control" % (old, count))
        at = text.index(old)
        text = text[:at] + new + text[at + len(old):]
        first = text[:at].count("\n") + 1
        for line in range(first, first + new.count("\n") + 1):
            lines.add(line)
    return text, lines


def run(paths):
    report, count = check_platform(paths)
    return report.refusals


def run_admit(paths, platform, manifest=None):
    report, count = admit(paths, platform, manifest)
    return report.refusals


def run_manifest(paths):
    report, count = check_manifests(paths)
    return report.refusals


BIG = "1" * 5000
# Thirty-two tasks above `health` in the x86 golden, each watched with `sensor` by WATCHES_33.
WATCHED = ("\n  - name: health\n", "".join("\n  - name: w%d\n    entry: w%d_main\n    stack: 4096\n    priority: 7\n    ceiling: 7\n"
                                         % (n, n) for n in range(32)) + "\n  - name: health\n")
WATCHES_33 = ", ".join(["sensor"] + ["w%d" % n for n in range(32)])
WATCHES_32 = ", ".join(["w%d" % n for n in range(32)])

ARMS = [
    ("chip.line-order", "xmc4800/chip.yaml", [("      sr1: 85\n      sr2: 86\n", "      sr2: 86\n      sr1: 85\n")], False),
    (None, "rx72m/chip.yaml", [("      rxi: { number: 86", "      rx0: { number: 86")], False),
    ("form.unknown-field", "stm32f411/chip.yaml", [("chip: stm32f411\n", "chip: stm32f411\nvendor: st\n")], False),
    ("form.unknown-field", "stm32f411/chip.yaml",
     [("protection: { unit: pmsav7, covers_devices: true, memory_type: true }",
       "protection: { unit: pmsav7, covers_devices: true, memory_type: true, granule: 32 }")], False),
    ("form.unknown-field", "mk64f/chip.yaml",
     [("  uart1: { window: [0x4006B000, 0x20],", "  uart1: { window: [0x4006B000, 0x20], irq: 31,")], False),
    ("form.unknown-field", "xmc4800/chip.yaml",
     [("ch0: { window: [0x40030000, 0x200] }", "ch0: { window: [0x40030000, 0x200], fifo: 64 }")], False),
    ("form.unknown-field", "imx8mp/chip.yaml", [("    smp: true\n", "    smp: true\n    big: true\n")], False),
    ("form.unknown-field", "mk64f/chip.yaml", [("    per_thread: false\n", "    per_thread: false\n    slots: 2\n")], False),
    ("form.unknown-field", "esp32c6/chip.yaml", [("traps: false }", "traps: false, latch: true }")], False),
    ("form.unknown-field", "imx8mp/chip.yaml", [("ocram: { size: 0x90000,", "ocram: { size: 0x90000, ecc: true,")], False),
    ("form.unknown-field", "imx8mp/chip.yaml",
     [("partition_gate: { kind: rdc, device: rdc }", "partition_gate: { kind: rdc, device: rdc, domains: 4 }")], False),
    ("chip.gate-straddle", "esp32c6/chip.yaml", [("ranges: [[0x60000000, 0xB0000]]", "ranges: [[0x60000800, 0xAF800]]")],
     True),
    ("chip.gate-register", "rp2350/chip.yaml", [("    gate_register: 0xA0\n", "    gate_register: 0xA4\n")], False),
    ("chip.gate-register", "rp2350/chip.yaml", [("    gate_register: 0x48\n", "    gate_register: 0xA0\n")], False),
    ("form.unknown-field", "q35/chip.yaml",
     [("cores: { count: [1, 12], smp: true }", "cores: { count: [1, 12], smp: true, threads: 2 }")], False),
    ("form.unknown-field", "stm32f411/f411disco.yaml", [("chip: stm32f411\n", "chip: stm32f411\nrevision: 2\n")], False),
    ("form.unknown-field", "xmc4800/xmc4800-relax.yaml",
     [("  device: /dev/usic0/ch0\n", "  device: /dev/usic0/ch0\n  baud: 115200\n")], False),
    ("form.unknown-field", "mk64f/frdmk64f.yaml",
     [("blue: { pin: PTB21, active: low }", "blue: { pin: PTB21, active: low, colour: blue }")], False),
    ("form.unknown-field", "stm32f411/f411disco.yaml",
     [("    chip_select: PE3\n", "    chip_select: PE3\n    address: 0x6B\n")], False),
    ("form.unknown-field", "mk64f/frdmk64f.yaml",
     [("    chip_selects: [PTC4]\n", "    chip_selects: [PTC4]\n    speed: 1000000\n")], False),
    ("form.boolean", "stm32f411/chip.yaml", [("covers_devices: true,", "covers_devices: yes,")], False),
    ("form.boolean", "stm32f411/chip.yaml", [("covers_devices: true,", "covers_devices: no,")], False),
    ("form.boolean", "stm32f411/chip.yaml", [("covers_devices: true,", "covers_devices: on,")], False),
    ("form.boolean", "stm32f411/chip.yaml", [("covers_devices: true,", "covers_devices: off,")], False),
    ("form.boolean", "stm32f411/chip.yaml", [("covers_devices: true,", "covers_devices: True,")], False),
    ("form.boolean", "virt_arm64/chip.yaml", [("    bus_master: true\n", "    bus_master: TRUE\n")], False),
    ("form.anchor", "stm32f411/chip.yaml", [("  spi1: { window", "  spi1: &bus { window")], False),
    (("form.anchor", "form.alias"), "stm32f411/f411disco.yaml",
     [("  pins: { tx: PA2, rx: PA3 }", "  pins: &serial { tx: PA2, rx: PA3 }"),
      ("    pins: [PA5, PA6, PA7]", "    pins: *serial")], False),
    ("form.alias", "stm32f411/f411disco.yaml", [("    pins: [PA5, PA6, PA7]", "    pins: *serial")], False),
    ("form.merge-key", "xmc4800/xmc4800-relax.yaml",
     [("  led2: { pin: P5.8, active: high }", "  led2: { pin: P5.8, active: high, <<: { owner: kernel } }")], False),
    ("form.duplicate-key", "stm32f411/chip.yaml", [("lines: { global: 35 }", "lines: { global: 35, global: 39 }")], False),
    ("form.duplicate-key", "stm32f411/chip.yaml", [("arch: armv7m\n", "arch: armv7m\narch: armv7m\n")], False),
    ("form.tag", "stm32f411/chip.yaml", [("chip: stm32f411", "chip: !!str stm32f411")], False),
    ("form.directive", "stm32f411/chip.yaml", [("# SPDX", "%YAML 1.1\n---\n# SPDX")], False),
    ("form.documents", "stm32f411/f411disco.yaml", [("version: 1\n", "version: 1\n---\nversion: 1\n")], False),
    ("form.syntax", "stm32f411/chip.yaml", [("lines: { global: 35 }, ref", "lines: { global: 35 , ref")], True),
    ("form.directive", "stm32f411/chip.yaml", [("# SPDX", "%TAG !e! tag:example.com,2000:\n---\n# SPDX")], False),
    ("form.directive", "stm32f411/chip.yaml", [("# SPDX", "%FOO bar\n---\n# SPDX")], False),
    ("form.anchor", "stm32f411/chip.yaml", [("chip: stm32f411", "chip: &name stm32f411")], False),
    ("form.anchor", "stm32f411/f411disco.yaml", [("pins: [PA5, PA6, PA7]", "pins: &spi [PA5, PA6, PA7]")], False),
    ("form.anchor", "stm32f411/chip.yaml", [("arch: armv7m", "&key arch: armv7m")], False),
    ("form.tag", "stm32f411/chip.yaml", [("lines: { global: 35 }", "lines: !!map { global: 35 }")], False),
    ("form.tag", "stm32f411/f411disco.yaml", [("pins: [PA5, PA6, PA7]", "pins: !!seq [PA5, PA6, PA7]")], False),
    ("form.type", "stm32f411/chip.yaml", [("lines: { global: 35 }", "lines: [35]")], False),
    ("form.type", "stm32f411/chip.yaml", [("lines: { global: 35 }", "lines: { [global]: 35 }")], False),
    ("form.type", "stm32f411/f411disco.yaml", [("pins: [PA5, PA6, PA7]", "pins: PA5")], False),
    ("form.type", "stm32f411/chip.yaml", [("[0x40004400, 0x20]", "[0x40004400, 0x20, 4]")], False),
    ("form.type", "stm32f411/chip.yaml", [("covers_devices: true,", "covers_devices: 1,")], False),
    ("form.range", "stm32f411/chip.yaml", [("[0x40004400, 0x20]", "[0x40004400, 0x100000000]")], False),
    ("form.range", "stm32f411/chip.yaml", [("[0x40004400, 0x20]", "[0x10000000000000000, 0x20]")], False),
    ("form.range", "stm32f411/chip.yaml", [("lines: { global: 35 }", "lines: { global: 65536 }")], False),
    ("form.range", "virt_arm64/chip.yaml", [("    count: 32\n", "    count: 0x10000\n")], False),
    ("form.range", "q35/chip.yaml", [("{ index: 0x0 }", "{ index: 0x10000 }")], False),
    ("form.range", "stm32f411/chip.yaml", [("version: 1\n", "version: 65536\n")], False),
    ("form.range", "stm32f411/chip.yaml", [("[0x40004400, 0x20]", "[0xFFFFFFFFFFFFFFF0, 0x20]")], False),
    ("form.range", "virt_arm64/chip.yaml", [("[0x0A000000, 0x200]", "[0xFFFFFFFFFFFFF000, 0x200]")], False),
    ("form.range", "stm32f411/chip.yaml", [("base: 0x20000000, size", "base: 0xFFFFFFFFFFFF0000, size")], False),
    ("chip.address-width", "stm32f411/chip.yaml", [("[0x40004400, 0x20]", "[0xFFFFFFFFFFFFFFE0, 0x20]")], False),
    ("chip.address-width", "stm32f411/chip.yaml", [("base: 0x20000000, size: 0x20000", "base: 0xFFFF0000, size: 0x20000")],
     False),
    (None, "stm32f411/chip.yaml", [("[0x40004400, 0x20]", "[0xFFFFFFE0, 0x20]")], False),
    (None, "virt_arm64/chip.yaml", [("[0x09030000, 0x1000]", "[0xFFFFFFFFFFFFE000, 0x1000]")], False),
    ("chip.reserved", "virt_rv32/chip.yaml", [("owner: kernel, ", "")], True),
    (None, "virt_rv32/chip.yaml", [("owner: kernel, ", ""), ("\ndevices:\n", "\nreserved: none\n\ndevices:\n")], False),
    ("chip.reserved", "xmc4800/chip.yaml", [("\ndevices:\n", "\nreserved: none\n\ndevices:\n")], False),
    ("form.enum", "virt_rv32/chip.yaml", [("\ndevices:\n", "\nreserved: all\n\ndevices:\n")], False),
    ("form.text", "xmc4800/chip.yaml", [('ref: "Table 7-2 Memory Map, p.7-5"', 'ref: "Table 7-2 */ p.7-5"')], False),
    ("form.text", "xmc4800/chip.yaml", [('ref: "Table 7-2 Memory Map, p.7-5"', 'ref: "Table 7-2 \\\\ p.7-5"')], False),
    ("form.text", "xmc4800/chip.yaml", [('ref: "Table 7-2 Memory Map, p.7-5"', 'ref: "Table 7-2, p.7\u20135"')], False),
    ("form.text", "xmc4800/chip.yaml", [('manual: "XMC4700', 'manual: "\\nXMC4700')], False),
    (None, "virt_arm64/chip.yaml", [("    count: 32\n", "    count: 0xFFFF\n")], False),
    (None, "imx8mp/imx8mp-evk.yaml",
     [("dram: { base: 0x40000000, size: 0x4000000,", "dram: { base: 0x40000000, size: 0x100000000,")], False),
    ("form.inapplicable", "esp32c6/chip.yaml", [("data_cache: false\n", "data_cache: false\nclusters_coherent: false\n")], False),
    ("form.inapplicable", "q35/chip.yaml", [("memory_type: true, io_ports: true }", "memory_type: true, io_ports: true, driven: false }")],
     False),
    ("form.inapplicable", "esp32c6/chip.yaml", [("  hp:\n", "  hp:\n    line_offset: 0\n")], False),
    ("form.missing", "stm32f411/chip.yaml", [("unit: pmsav7, covers_devices: true,", "unit: pmsav7,")], False),
    ("form.missing", "stm32f411/chip.yaml", [(", memory_type: true }", " }")], False),
    ("form.missing", "esp32c6/chip.yaml", [("      memory_type: false\n", "")], True),
    (None, "esp32c6/chip.yaml", [("{ unit: none, privilege: false }", "{ unit: none, privilege: false, memory_type: false }")],
     False),
    ("form.missing", "virt_rv64/chip.yaml", [("arch: rv64imac\n", "")], True),
    ("form.missing", "virt_rv64/chip.yaml",
     [("protection: { unit: mmu, page: 0x1000, covers_devices: true, memory_type: false }\n", "")], True),
    ("form.missing", "esp32c6/chip.yaml", [("arch: rv32imac\n", "")], True),
    ("form.missing", "virt_arm64/chip.yaml", [("    count: 32\n", "")], True),
    ("form.missing", "virt_arm64/chip.yaml", [("    stride: 0x200\n", "")], True),
    ("form.missing", "virt_arm64/chip.yaml", [("    window: [0x0A000000, 0x200]\n", "")], True),
    ("form.missing", "imx8mp/chip.yaml", [("ocram: { size: 0x90000, at: { a53: 0x00900000, m7: 0x20200000 },",
                                           "ocram: { size: 0x90000,")], False),
    ("form.missing", "imx8mp/chip.yaml", [("at: { a53: 0x00900000, m7: 0x20200000 }", "at: {}")], False),
    ("form.missing", "mk64f/chip.yaml", [("    per_thread: false\n", "")], True),
    ("form.missing", "imx8mp/chip.yaml", [("partition_gate: { kind: rdc, device: rdc }", "partition_gate: { device: rdc }")], False),
    ("form.exclusive", "imx8mp/chip.yaml", [("chip: imx8mp\n", "chip: imx8mp\narch: armv8a\n")], True),
    ("form.type", "stm32f411/chip.yaml", [("[0x40004400, 0x20]", "[usart, 0x20]")], False),
    ("form.type", "stm32f411/chip.yaml", [("[0x40004400, 0x20]", "['0x40004400', 0x20]")], False),
    ("form.type", "stm32f411/chip.yaml", [("chip: stm32f411", "chip: { name: stm32f411 }")], False),
    ("form.type", "stm32f411/f411disco.yaml", [("  device: /dev/usart2\n", "  device:\n")], False),
    ("form.integer", "stm32f411/chip.yaml", [("[0x40004400, 0x20]", "[0x40004400, 0o40]")], False),
    ("form.integer", "stm32f411/chip.yaml", [("[0x40004400, 0x20]", "[0x40004400, 032]")], False),
    ("form.integer", "stm32f411/chip.yaml", [("[0x40004400, 0x20]", "[0x4000_4400, 0x20]")], False),
    ("form.integer", "stm32f411/chip.yaml", [("lines: { global: 35 }", "lines: { global: 3.8e1 }")], False),
    ("form.integer", "stm32f411/chip.yaml", [("lines: { global: 35 }", "lines: { global: -38 }")], False),
    ("form.integer", "stm32f411/chip.yaml", [("lines: { global: 35 }", "lines: { global: 0b100110 }")], False),
    ("form.path", "stm32f411/f411disco.yaml", [("device: /dev/usart2", "device: dev/usart2")], False),
    ("form.name", "stm32f411/chip.yaml", [("arch: armv7m", "arch: ARMv7-M")], False),
    ("form.name", "stm32f411/chip.yaml", [("lines: { global: 35 }", "lines: { Global: 35 }")], False),
    ("form.name", "mk64f/chip.yaml", [("PTD3: { alt2: dspi0.sin,", "PTD3: { alt2: dspi0,")], False),
    ("form.name", "stm32f411/chip.yaml", [("PD12: { gpio: gpio.3.12 }", "PD12: { gpio: gpio.d.12 }")], False),
    ("form.name", "stm32f411/chip.yaml", [("PD12: { gpio: gpio.3.12 }", "PD12: { gpio: gpio }")], False),
    ("form.enum", "stm32f411/chip.yaml", [("unit: pmsav7", "unit: pmsav9")], False),
    ("form.enum", "stm32f411/f411disco.yaml", [("active: high", "active: on")], False),
    ("form.enum", "stm32f411/f411disco.yaml", [("active: high, owner: kernel", "active: high, owner: user")], False),
    ("form.missing", "stm32f411/chip.yaml", [("chip: stm32f411\n", "")], True),
    ("form.missing", "stm32f411/chip.yaml", [("version: 1\n", "")], True),
    ("form.missing", "stm32f411/f411disco.yaml", [("  device: /dev/usart2\n", "")], True),
    ("form.missing", "virt_rv32/qemu-riscv.yaml", [("  semihosting: true\n", "  semihosting: false\n")], False),
    ("form.exclusive", "virt_rv32/qemu-riscv.yaml",
     [("  semihosting: true\n", "  semihosting: true\n  device: /dev/clint\n")], False),
    ("form.inapplicable", "virt_rv32/qemu-riscv.yaml",
     [("  semihosting: true\n", "  semihosting: true\n  pins: { tx: PA2 }\n")], False),
    ("form.missing", "stm32f411/f411disco.yaml", [("ld4: { pin: PD12, active: high", "ld4: { active: high")], False),
    ("form.missing", "imx8mp/chip.yaml", [("ocram: { size: 0x90000, at:", "ocram: { at:")], False),
    ("form.missing", "virt_rv64/chip.yaml", [("unit: mmu, page: 0x1000,", "unit: mmu,")], False),
    ("form.missing", "imx8mp/chip.yaml", [("    line_offset: 32\n", "")], True),
    ("form.version", "stm32f411/chip.yaml", [("version: 1\n", "version: 2\n")], False),
    ("form.version", "stm32f411/f411disco.yaml", [("version: 1\n", "version: 0\n")], False),
    ("form.exclusive", "q35/chip.yaml", [("com1: { ports:", "com1: { window: [0x1000, 8], ports:")], False),
    ("board.pin-reused", "rp2040/picopi.yaml", [("pin: GPIO25,", "pin: GPIO0,")], False),
    (None, "stm32f411/f411disco.yaml",
     [("    pins: [PA5, PA6, PA7]\n", "    pins: [PA5, PA6, PA7]\n  baro:\n    bus: /dev/spi1\n    chip_select: PC13\n"
                                     "    pins: [PA5, PA6, PA7]\n")], False),
    ("board.pin-reused", "stm32f411/f411disco.yaml",
     [("    pins: [PA5, PA6, PA7]\n", "    pins: [PA5, PA6, PA7]\n  baro:\n    bus: /dev/spi1\n    chip_select: PE3\n"
                                     "    pins: [PA5, PA6, PA7]\n")], False),
    ("board.pin-signal", "stm32f302/f302nucleo.yaml", [("pins: { tx: PA2, rx: PA3 }", "pins: { tx: PA3, rx: PA2 }")], False),
    ("board.pin-signal", "stm32f411/f411disco.yaml", [("pins: { tx: PA2, rx: PA3 }", "pins: { tx: PA3, rx: PA2 }")], False),
    ("board.memory-overlap", "rp2040/picopi.yaml", [("    base: 0x10000000\n", "    base: 0x20000000\n")], False),
    ("board.link-duplicate", "rp2040/picopi.yaml",
     [("link: { region: FLASH, access: rx }", "link: { region: RAM, access: rx }")], False),
    ("board.symbol-collision", "rp2040/picopi.yaml", [("  xip_flash:\n", "  sram:\n")], False),
    ("form.exclusive", "sim/chip.yaml",
     [("  console:\n    host: true\n", "  console:\n    host: true\n    window: [0x1000, 0x100]\n")], False),
    ("form.inapplicable", "stm32f103/chip.yaml", [("  usart1:\n", "  console: { host: true }\n  usart1:\n")], False),
    ("form.inapplicable", "sim/chip.yaml",
     [("arena: { size: 0x200000, arena: true }",
       "arena: { size: 0x200000, arena: true, link: { region: RAM, access: rwx } }")], False),
    ("form.missing", "sim/chip.yaml", [("  arena: { size: 0x200000, arena: true }\n",
                                        "  arena: { size: 0x200000, arena: true }\n  scratch: { size: 0x1000 }\n")], False),
    ("form.exclusive", "imx8mp/chip.yaml", [("ocram: { size: 0x90000,", "ocram: { size: 0x90000, base: 0x900000,")], False),
    ("form.exclusive", "imx8mp/chip.yaml", [("ocram: { size: 0x90000,", "ocram: { size: 0x90000, cluster: a53,")], False),
    ("form.exclusive", "imx8mp/chip.yaml",
     [("clusters_coherent: false\n", "clusters_coherent: false\nprotection: { unit: mmu, page: 0x1000, covers_devices: true }\n")], False),
    ("form.exclusive", "stm32f411/chip.yaml",
     [("  spi1: { window", "  spi1: { sysreg: true, window")], False),
    ("chip.name-mismatch", "stm32f411/chip.yaml", [("chip: stm32f411", "chip: stm32f401")], False),
    ("chip.name-collision", "xmc4800/chip.yaml", [("      sr1: 85\n", "      ch1: 85\n")], False),
    ("chip.overlap", "stm32f411/chip.yaml", [("[0x40013000, 0x20]", "[0x40004410, 0x20]")], False),
    ("chip.overlap", "xmc4800/chip.yaml", [("[0x40030200, 0x200]", "[0x40030100, 0x200]")], False),
    ("chip.overlap", "q35/chip.yaml", [("ports: [0x70, 2]", "ports: [0x3FF, 2]")], False),
    ("chip.overlap", "virt_arm64/chip.yaml", [("    stride: 0x200\n", "    stride: 0x100\n")], True),
    ("chip.overlap", "imx8mp/chip.yaml",
     [("mu1_b: { window: [0x30AB0000, 0x10000]", "mu1_b: { window: [0x30860000, 0x10000]")], False),
    ("chip.overlap", "imx8mp/chip.yaml", [("m7: 0x20200000", "m7: 0x30AB0000")], False),
    (None, "imx8mp/chip.yaml", [("mu1_b: { window: [0x30AB0000, 0x10000]", "mu1_b: { window: [0x30AA0000, 0x10000]")], False),
    (None, "imx8mp/chip.yaml", [("mu1_b: { window: [0x30AB0000, 0x10000]", "mu1_b: { window: [0x00900000, 0x10000]")], False),
    (None, "stm32f411/chip.yaml", [("[0x40013000, 0x20]", "[0x40004420, 0x20]")], False),
    ("chip.cluster-unknown", "imx8mp/chip.yaml", [("cluster: m7, lines", "cluster: m4, lines")], False),
    ("chip.cluster-unknown", "imx8mp/chip.yaml", [("m7: 0x20200000", "m4: 0x20200000")], False),
    ("chip.cluster-unknown", "stm32f411/chip.yaml", [("  spi1: { window", "  spi1: { cluster: m4, window")], False),
    ("chip.kernel-window", "stm32f411/chip.yaml", [("\ndevices:\n", "\ndevices:\n  iwdg: { owner: kernel }\n")], False),
    ("chip.memory-required", "q35/chip.yaml",
     [("unit: mmu, page: 0x1000, covers_devices: true,", "unit: pmp, covers_devices: true,")], True),
    ("chip.arena", "stm32f411/chip.yaml", [("arena: true", "arena: false")], True),
    ("chip.arena", "stm32f411/chip.yaml",
     [("\nmemory:\n", "\nmemory:\n  ccm: { base: 0x10000000, size: 0x10000, arena: true }\n")], True),
    ("chip.arena", "q35/chip.yaml",
     [("\ndevices:\n", "\nmemory:\n  dram: { base: 0x80000000, size: 0x1000, arena: true }\n\ndevices:\n")], False),
    ("chip.page-unit", "stm32f411/chip.yaml",
     [("memory_type: true }", "memory_type: true, page: 0x1000 }")], False),
    ("chip.core-count", "q35/chip.yaml", [("count: [1, 12]", "count: [12, 1]")], False),
    ("chip.core-count", "q35/chip.yaml", [("count: [1, 12]", "count: [0, 12]")], False),
    ("chip.zero-size", "virt_arm64/chip.yaml", [("    stride: 0x200\n", "    stride: 0\n")], False),
    ("chip.zero-size", "stm32f411/chip.yaml", [("size: 0x20000, arena", "size: 0, arena")], False),
    ("chip.zero-size", "mk64f/chip.yaml", [("    size: 0x1000\n", "    size: 0\n")], False),
    ("form.type", "mk64f/chip.yaml", [("[[0x40000000, 0x80000], [", "[[0x40000000], [")], False),
    ("form.type", "mk64f/chip.yaml", [("ranges: [[0x40000000, 0x80000], [0x40080000, 0x7F000]]", "ranges: 0x40000000")], False),
    ("chip.zero-size", "mk64f/chip.yaml", [("[0x40080000, 0x7F000]", "[0x40080000, 0]")], False),
    ("form.range", "mk64f/chip.yaml", [("[0x40080000, 0x7F000]", "[0x40080000, 0x100000000]")], False),
    ("form.range", "mk64f/chip.yaml", [("[0x40080000, 0x7F000]", "[0xFFFFFFFFFFFFF000, 0x7F000]")], False),
    ("form.missing", "mk64f/chip.yaml", [("ranges: [[0x40000000, 0x80000], [0x40080000, 0x7F000]]", "ranges: []")], False),
    ("chip.gate-straddle", "mk64f/chip.yaml", [("[[0x40000000, 0x80000], [", "[[0x40000000, 0x6A010], [")], True),
    (None, "mk64f/chip.yaml", [("[[0x40000000, 0x80000], [", "[[0x40000000, 0x6A020], [")], False),
    ("form.range", "stm32f411/chip.yaml", [("lines: { global: 35 }", "lines: { global: %s }" % BIG)], False),
    ("chip.device-unknown", "stm32f411/chip.yaml", [("PE3: { gpio: gpio.4.3 }", "PE3: { gpio: gpio.%s.3 }" % BIG)], False),
    ("board.device-unknown", "virt_arm64/qemu-arm64.yaml", [("device: /dev/uart0", "device: /dev/virtio/%s" % BIG)], False),
    ("chip.zero-size", "virt_rv64/chip.yaml", [("page: 0x1000,", "page: 0,")], False),
    ("chip.page-size", "virt_rv64/chip.yaml", [("page: 0x1000,", "page: 0x1800,")], False),
    ("chip.register-outside", "q35/chip.yaml", [("{ index: 0x0 }", "{ index: 0x2 }")], False),
    ("chip.register-outside", "xmc4800/chip.yaml", [("ccr: 0x040 }", "ccr: 0x200 }")], False),
    ("chip.register-outside", "imx8mp/chip.yaml",
     [("sysreg: true }", "sysreg: true, privileged_registers: { ctl: 0x0 } }")], False),
    (None, "q35/chip.yaml", [("{ index: 0x0 }", "{ index: 0x1 }")], False),
    ("chip.zero-size", "stm32f411/chip.yaml", [("[0x40004400, 0x20]", "[0x40004400, 0]")], False),
    ("chip.zero-size", "virt_arm64/chip.yaml", [("    count: 32\n", "    count: 0\n")], False),
    ("chip.device-unknown", "mk64f/chip.yaml", [("dspi0.sin", "dspi1.sin")], False),
    ("chip.device-unknown", "imx8mp/chip.yaml", [("device: rdc }", "device: rdc2 }")], False),
    ("chip.device-unknown", "mk64f/chip.yaml", [("PTC4: { gpio: gpio.2.4 }", "PTC4: { gpio: gpioc.4 }")], False),
    ("chip.device-unknown", "xmc4800/chip.yaml", [("P5.8: { gpio: port.5.8 }", "P5.8: { gpio: gpio.5.8 }")], False),
    ("chip.device-unknown", "stm32f411/chip.yaml", [("PE3: { gpio: gpio.4.3 }", "PE3: { gpio: gpio.5.3 }")], False),
    ("chip.device-unknown", "stm32f411/chip.yaml", [("PE3: { gpio: gpio.4.3 }", "PE3: { gpio: gpio.3 }")], False),
    ("chip.device-unknown", "esp32c6/chip.yaml", [("GPIO8: { gpio: gpio.8 }", "GPIO8: { gpio: gpio.0.8 }")], False),
    (None, "stm32f411/chip.yaml", [("PE3: { gpio: gpio.4.3 }", "PE3: { gpio: gpioh.3 }")], False),
    ("board.name-mismatch", "stm32f411/f411disco.yaml", [("board: f411disco", "board: blackpill")], False),
    ("board.chip-unknown", "stm32f411/f411disco.yaml", [("chip: stm32f411", "chip: stm32f412")], False),
    ("board.device-unknown", "stm32f411/f411disco.yaml", [("device: /dev/usart2", "device: /dev/usart3")], False),
    ("board.device-unknown", "xmc4800/xmc4800-relax.yaml", [("device: /dev/usic0/ch0", "device: /dev/usic0")], False),
    ("board.device-unknown", "xmc4800/xmc4800-relax.yaml", [("device: /dev/usic0/ch0", "device: /dev/usic0/sr0")], False),
    ("board.device-unknown", "virt_arm64/qemu-arm64.yaml", [("device: /dev/uart0", "device: /dev/virtio/32")], False),
    ("board.device-unknown", "stm32f411/f411disco.yaml", [("bus: /dev/spi1", "bus: /dev/spi2")], False),
    ("board.device-unknown", "virt_arm64/qemu-arm64.yaml", [("device: /dev/uart0", "device: /dev/virtio/\u00b2")], False),
    (None, "virt_arm64/qemu-arm64.yaml", [("device: /dev/uart0", "device: /dev/virtio/31")], False),
    ("board.pin-unknown", "stm32f411/f411disco.yaml", [("rx: PA3", "rx: PA4")], False),
    ("board.pin-unknown", "stm32f411/f411disco.yaml", [("pin: PD12", "pin: PD13")], False),
    ("board.pin-unknown", "stm32f411/f411disco.yaml", [("chip_select: PE3", "chip_select: PE4")], False),
    ("board.pin-unknown", "mk64f/frdmk64f.yaml", [("chip_selects: [PTC4]", "chip_selects: [PTC5]")], False),
    ("board.pin-unknown", "esp32c6/esp32c6-wroom.yaml", [("GPIO15: strapping", "GPIO14: strapping")], False),
    ("board.pin-unknown", "virt_arm64/qemu-arm64.yaml",
     [("  device: /dev/uart0\n", "  device: /dev/uart0\n  pins: { tx: TX0 }\n")], False),
    ("board.pin-reused", "xmc4800/xmc4800-relax.yaml", [("pin: P5.8,", "pin: P1.4,")], False),
    ("board.pin-function", "stm32f411/f411disco.yaml", [("tx: PA2", "tx: PC13")], False),
    ("board.led-owner", "xmc4800/xmc4800-relax.yaml",
     [("led2: { pin: P5.8, active: high }", "led2: { pin: P5.8, active: high, owner: kernel }")], False),
    ("form.inapplicable", "esp32/chip.yaml", [("  GPIO2: { gpio: gpio.2 }", "  GPIO2: { gpio: { function: gpio.2 } }")], False),
    ("form.missing", "rp2040/chip.yaml", [("GPIO0: { f2: uart0.tx,", "GPIO0: { f2: { input_select: 1 },")], False),
    (None, "rp2040/chip.yaml", [("GPIO0: { f2: uart0.tx,", "GPIO0: { f2: { function: uart0.tx, input_select: 1, ref: \"p.1\" },")],
     False),
    (("board.pin-function", "board.pin-reused"), "mk64f/frdmk64f.yaml", [("[PTD1, PTD2, PTD3]", "[PTD1, PTD2, PTB16]")],
     False),
    ("board.pin-function", "xmc4800/xmc4800-relax.yaml", [("device: /dev/usic0/ch0", "device: /dev/usic0/ch1")], True),
    ("board.pin-function", "stm32f411/f411disco.yaml", [("pins: [PA5, PA6, PA7]", "pins: [PA5, PA6, PC13]")], False),
    ("board.reserved-pin-used", "esp32c6/esp32c6-wroom.yaml", [("led2: { pin: GPIO8,", "led2: { pin: GPIO9,")], False),
    ("form.inapplicable", "esp32c6/esp32c6-wroom.yaml", [("kind: addressable,", "kind: addressable, active: high,")],
     False),
    ("form.missing", "esp32/esp32-wroom.yaml", [("pin: GPIO2, active: high,", "pin: GPIO2,")], False),
    ("board.led-kind", "esp32c6/esp32c6-wroom.yaml", [("kind: addressable,", "kind: level, active: high,")], False),
    ("board.led-kind", "esp32/esp32-wroom.yaml", [("pin: GPIO2, active: high,", "pin: GPIO2, kind: addressable,")],
     False),
    ("form.enum", "esp32c6/esp32c6-wroom.yaml", [("kind: addressable,", "kind: rgb,")], False),
    (None, "esp32/esp32-wroom.yaml", [("pin: GPIO2, active: high,", "pin: GPIO2, kind: level, active: high,")], False),
    ("chip.block-outside", "stm32f411/chip.yaml",
     [("  rcc: { window: [0x40023800, 0x400],", "  rcc: { window: [0x40023800, 0x400], blocks: { cr: 0x400 },")], False),
    (None, "stm32f411/chip.yaml",
     [("  rcc: { window: [0x40023800, 0x400],", "  rcc: { window: [0x40023800, 0x400], blocks: { cr: 0x3FC },")], False),
    ("chip.block-outside", "xmc4800/chip.yaml",
     [('    ref: "Table 18-21, p.18-157"\n    channels:\n      ch0: { window: [0x40030000,',
       '    ref: "Table 18-21, p.18-157"\n    blocks: { status: 0x0 }\n    channels:\n      ch0: { window: [0x40030000,')],
     False),
    (None, "stm32f411/chip.yaml",
     [("  rcc: { window: [0x40023800, 0x400],",
       "  rcc: { window: [0x40023800, 0x400], symbol: RCC_REGS, blocks: { cr: { offset: 0x4, ref: p.2, "
       "symbol: RCC_CR } },")], False),
    ("form.unknown-field", "stm32f411/chip.yaml",
     [("  rcc: { window: [0x40023800, 0x400],", "  rcc: { window: [0x40023800, 0x400], blocks: { cr: { offset: 0x4, "
       "size: 4 } },")], False),
    ("form.missing", "stm32f411/chip.yaml",
     [("  rcc: { window: [0x40023800, 0x400],", "  rcc: { window: [0x40023800, 0x400], blocks: { cr: { ref: p.2 } },")],
     False),
    ("chip.line-range", "stm32f411/chip.yaml", [("lines: { global: 35 }", "lines: { global: 86 }")], False),
    (None, "stm32f411/chip.yaml", [("lines: { global: 35 }", "lines: { global: 85 }")], False),
    ("chip.line-range", "virt_arm64/chip.yaml", [("    lines: { irq: 48 }\n", "    lines: { irq: 288 }\n")], False),
    (None, "virt_arm64/chip.yaml", [("    lines: { irq: 48 }\n", "    lines: { irq: 287 }\n")], False),
    ("chip.line-range", "esp32c6/chip.yaml", [("soft_only_from: 26", "soft_only_from: 32")], False),
    (None, "esp32c6/chip.yaml", [("soft_only_from: 26", "soft_only_from: 31")], False),
    ("chip.line-range", "virt_arm64/chip.yaml", [("free_from: 200", "free_from: 288")], False),
    (None, "virt_arm64/chip.yaml", [("free_from: 200", "free_from: 287")], False),
    (None, "stm32f411/chip.yaml",
     [("lines: { global: 35 }", "lines: { global: { number: 35, ref: p.3, symbol: SPI1_IRQ } }")], False),
    ("form.missing", "stm32f411/chip.yaml", [("lines: { global: 35 }", "lines: { global: { ref: p.3 } }")], False),
    ("form.missing", "stm32f411/chip.yaml", [('manual: "RM0383 STM32F411xC/E Reference Manual, Rev 4 (May 2025)"\n', "")],
     True),
    ("form.missing", "stm32f411/chip.yaml", [('interrupts: { count: { value: 86, ref: "Table 37 Vector table, p.205" } }\n', "")],
     True),
    ("form.missing", "stm32f411/chip.yaml", [('interrupts: { count: { value: 86, ref: "Table 37 Vector table, p.205" } }',
                                          "interrupts: { }")], False),
    ("form.missing", "stm32f411/chip.yaml", [("count: { value: 86,", "count: {")], False),
    ("chip.link-duplicate", "xmc4800/chip.yaml",
     [("\n# Pins are", "  dsram2: { base: 0x20020000, size: 0x20000, link: { region: RAM, access: rwx } }\n\n# Pins are")],
     False),
    (None, "xmc4800/chip.yaml",
     [("\n# Pins are", "  dsram2: { base: 0x20020000, size: 0x20000, link: { region: RAM2, access: rw } }\n\n# Pins are")],
     False),
    ("form.name", "xmc4800/chip.yaml", [("link: { region: RAM, access: rwx }", "link: { region: RAM, access: wr }")], False),
    ("form.name", "xmc4800/chip.yaml", [("link: { region: RAM, access: rwx }", "link: { region: ram, access: rw }")], False),
    ("chip.symbol-collision", "stm32f411/chip.yaml",
     [("  spi1: { window: [0x40013000, 0x20],", "  spi1: { window: [0x40013000, 0x20], symbol: USART2_BASE,")], False),
    ("chip.symbol-collision", "stm32f411/chip.yaml",
     [("lines: { global: 35 }", "lines: { global: { number: 35, symbol: USART2_IRQ } }")], False),
    ("chip.symbol-collision", "stm32f411/chip.yaml",
     [("  sram: { base: 0x20000000,", "  sram: { symbol: TIM2_BASE, base: 0x20000000,")], False),
    ("chip.symbol-collision", "stm32f411/chip.yaml",
     [("lines: { global: 35 }", "lines: { global: { number: 35, symbol: SPI_BASE } }"),
      ("  tim2:\n", "  line_spi: { window: [0x40013400, 0x20] }\n  tim2:\n")],
     False),
    (None, "stm32f411/chip.yaml",
     [("lines: { global: 35 }", "lines: { global: { number: 35, symbol: SPI_LINE } }"),
      ("  tim2:\n", "  line_spi: { window: [0x40013400, 0x20] }\n  tim2:\n")],
     False),
    ("chip.symbol-collision", "stm32f411/chip.yaml",
     [("  sram: { base: 0x20000000,", "  gpio0: { base: 0x30000000, size: 0x1000 }\n  sram: { base: 0x20000000,")], False),
    (None, "stm32f411/chip.yaml",
     [("  sram: { base: 0x20000000,", "  gpio9: { base: 0x30000000, size: 0x1000 }\n  sram: { base: 0x20000000,")], False),
    (None, "stm32f411/chip.yaml",
     [("chip: stm32f411\n", "chip: stm32f411\nc: { namespace: kickos::stm, line_enum: \"irq_num : int\" }\n"
       "cycle_counter: { hz: { value: 0, ref: p.4 }, glitches: { value: true, ref: p.5 } }\n")], False),
    ("form.name", "stm32f411/chip.yaml", [("chip: stm32f411\n", "chip: stm32f411\nc: { namespace: \"stm::x\" }\n")],
     False),
    ("form.name", "stm32f411/chip.yaml",
     [("chip: stm32f411\n", "chip: stm32f411\nc: { line_enum: \"irq_num: int\" }\n")], False),
    ("form.unknown-field", "stm32f411/chip.yaml", [("chip: stm32f411\n", "chip: stm32f411\nc: { prefix: STM }\n")],
     False),
    ("form.boolean", "stm32f411/chip.yaml", [("chip: stm32f411\n", "chip: stm32f411\ncycle_counter: { glitches: yes }\n")],
     False),
    ("form.name", "stm32f411/chip.yaml",
     [("  spi1: { window: [0x40013000, 0x20],", "  spi1: { window: [0x40013000, 0x20], symbol: 1SPI,")], False),
]


COMPOSITION_ARMS = [
    ("form.unknown-field", "xmc4800-relax.yaml", [("ends: never\n", "ends: never\nlog: verbose\n")], False),
    ("form.unknown-field", "xmc4800-relax.yaml", [("    priority: 12\n", "    priority: 12\n    weight: 1\n")], False),
    ("form.unknown-field", "qemu-arm64.yaml", [("    size: 64\n", "    size: 64\n    align: 8\n")], False),
    ("form.unknown-field", "qemu-arm64.yaml", [("restart: { max: 3 }", "restart: { max: 3, delay: 1 }")], False),
    ("form.missing", "qemu-arm64.yaml", [("stdout: kernel\n", "")], True),
    ("form.missing", "qemu-arm64.yaml", [("ends: never\n", "")], True),
    ("form.missing", "qemu-x86_64.yaml", [("    priority: 8\n", "")], True),
    ("form.missing", "qemu-x86_64.yaml", [("    entry: app_main\n", "")], True),
    ("form.missing", "qemu-x86_64.yaml", [("    stack: 8192\n", "")], True),
    ("form.missing", "qemu-arm64.yaml", [("    cache: cached # shared by tasks of this image only, on coherent memory\n", "")],
     True),
    ("form.missing", "qemu-arm64.yaml", [("restart: { max: 3 }", "restart: {}")], False),
    ("form.version", "qemu-arm64.yaml", [("version: 1\n", "version: 2\n")], False),
    ("form.inapplicable", "xmc4800-relax.yaml", [("    driver: xmcssc\n", "    driver: xmcssc\n    stack: 1024\n")], False),
    ("form.inapplicable", "xmc4800-relax.yaml", [("board: xmc4800-relax\n", "board: xmc4800-relax\ncluster: m4\n")], False),
    ("form.exclusive", "xmc4800-relax.yaml", [("    driver: xmcssc\n", "    driver: xmcssc\n    entry: spi_main\n")], False),
    ("form.name", "qemu-x86_64.yaml", [("entry: app_main", "entry: app-main")], False),
    (None, "qemu-x86_64.yaml", [("entry: app_main", "entry: App_Main2")], False),
    ("form.name", "xmc4800-relax.yaml", [("driver: xmcssc", "driver: XmcSsc")], False),
    ("form.name", "qemu-x86_64.yaml", [("  - name: app\n", "  - name: App\n")], False),
    ("form.name", "qemu-arm64.yaml", [("ends: never", "ends: Never")], False),
    ("form.type", "qemu-arm64.yaml", [("uses: [/svc/sensor]", "uses: /svc/sensor")], False),
    ("form.path", "qemu-arm64.yaml", [("devices: [/dev/rtc]", "devices: [dev/rtc]")], False),
    ("form.path", "xmc4800-relax.yaml", [("stdout: /svc/console", "stdout: console")], False),
    ("form.duplicate-key", "qemu-arm64.yaml",
     [("maps: { /shm/history: ro }", "maps: { /shm/history: ro, /shm/history: rw }")], False),
    ("form.range", "qemu-x86_64.yaml", [("    priority: 8\n", "    priority: 256\n")], False),
    ("form.range", "qemu-x86_64.yaml", [("    stack: 8192\n", "    stack: 0x100000000\n")], False),
    ("form.range", "qemu-arm64.yaml", [("restart: { max: 3 }", "restart: { max: 256 }")], False),
    ("form.range", "qemu-arm64.yaml", [("    size: 64\n", "    size: 0x100000000\n")], False),
    ("form.range", "qemu-arm64.yaml", [("heap: 65536\n", "heap: 0x100000000\n")], False),
    ("form.range", "qemu-arm64.yaml", [("    core: 2\n", "    core: 32\n")], False),
    ("form.range", "qemu-arm64.yaml", [("    core: 2\n", "    core: 0x10000\n")], False),
    (None, "qemu-arm64.yaml", [("    core: 2\n", "    core: 31\n")], False),
    (None, "qemu-arm64.yaml", [("heap: 65536\n", "heap: 0xFFFFFFFF\n")], False),
    ("form.missing", "qemu-arm64.yaml", [("heap: 65536\n", "")], True),
    (None, "qemu-arm64.yaml", [("heap: 65536\n", "heap: 0\n")], False),
    ("form.enum", "qemu-arm64.yaml", [("cache: cached", "cache: write_back")], False),
    (None, "qemu-arm64.yaml", [("cache: cached", "cache: uncached")], False),
    ("form.enum", "qemu-arm64.yaml", [("maps: { /shm/history: ro }", "maps: { /shm/history: rx }")], False),
    ("form.enum", "qemu-arm64.yaml", [("    core: 2\n", "    core: 2\n    authority: [root]\n")], False),
    (None, "qemu-arm64.yaml",
     [("    core: 2\n", "    core: 2\n    authority: [memory, pinmux, pstate, irq, system, console, tasks]\n")], False),
    ("form.enum", "qemu-arm64.yaml", [("ends: never\n", "ends: never\naccepts: [no_cache]\n")], False),
    ("form.duplicate-entry", "qemu-arm64.yaml", [("devices: [/dev/rtc]", "devices: [/dev/rtc, /dev/rtc]")], False),
    ("form.duplicate-entry", "qemu-arm64.yaml", [("uses: [/svc/sensor]", "uses: [/svc/sensor, /svc/sensor]")], False),
    ("form.duplicate-entry", "qemu-arm64.yaml", [("watches: [sensor]", "watches: [sensor, sensor]")], False),
    ("form.duplicate-entry", "qemu-arm64.yaml", [("    core: 2\n", "    core: 2\n    authority: [memory, memory]\n")], False),
    ("form.duplicate-entry", "qemu-arm64.yaml",
     [("ends: never\n", "ends: never\naccepts: [no_protection, no_protection]\n")], False),
    ("form.scope", "qemu-arm64.yaml", [("ends: never\n", "ends: never\naccepts: [bus_master]\n")], False),
    ("form.scope", "qemu-arm64.yaml", [("    core: 2\n", "    core: 2\n    accepts: [no_protection]\n")], False),
    (None, "qemu-arm64.yaml",
     [("ends: never\n", "ends: never\naccepts: [no_protection, no_privilege_split, cached_incoherent]\n")], False),
    (None, "qemu-arm64.yaml",
     [("    core: 2\n", "    core: 2\n    accepts: [device_not_isolated, coarse_gate, bus_master]\n")], False),
    ("name.board-unknown", "qemu-arm64.yaml", [("board: qemu-arm64", "board: qemu-arm32")], False),
    ("name.board-unknown", "qemu-arm64.yaml", [("board: qemu-arm64", "board: chip")], False),
    ("name.device-unknown", "qemu-arm64.yaml", [("devices: [/dev/rtc]", "devices: [/dev/virtio/03]")], False),
    ("name.device-unknown", "qemu-arm64.yaml", [("devices: [/dev/rtc]", "devices: [/dev/virtio/%s]" % BIG)], False),
    ("name.line-unknown", "qemu-arm64.yaml",
     [("devices: [/dev/rtc]", "devices: [/dev/rtc]\n    lines: { irq: /dev/virtio/%s/irq }" % BIG)], False),
    ("form.range", "qemu-x86_64.yaml", [("    priority: 8\n", "    priority: %s\n" % BIG)], False),
    ("name.device-unknown", "qemu-arm64.yaml", [("devices: [/dev/rtc]", "devices: [/dev/rtc9]")], False),
    ("name.device-unknown", "qemu-arm64.yaml", [("devices: [/dev/rtc]", "devices: [/sys/rtc]")], False),
    ("name.device-unknown", "qemu-arm64.yaml", [("devices: [/dev/rtc]", "devices: [/dev/virtio]")], False),
    ("name.device-unknown", "qemu-arm64.yaml", [("devices: [/dev/rtc]", "devices: [/dev/virtio/32]")], False),
    ("name.device-unknown", "qemu-arm64.yaml", [("devices: [/dev/rtc]", "devices: [/dev/rtc/0]")], False),
    ("name.device-unknown", "xmc4800-relax.yaml", [("devices: [/dev/usic0/ch1]", "devices: [/dev/usic0]")], False),
    ("name.device-unknown", "xmc4800-relax.yaml", [("devices: [/dev/usic0/ch1]", "devices: [/dev/usic0/ch2]")], False),
    ("name.device-unknown", "xmc4800-relax.yaml", [("devices: [/dev/usic0/ch1]", "devices: [/dev/usic0/sr1]")], False),
    ("name.device-unknown", "qemu-x86_64.yaml", [("devices: [/dev/cmos_rtc]", "devices: [/dev/cmos_rtc/0]")], False),
    (None, "qemu-arm64.yaml",
     [("devices: [/dev/rtc]", "devices: [/dev/rtc, /dev/virtio/31, /dev/gpio]\n    accepts: [bus_master, coarse_gate]")],
     False),
    ("name.line-unknown", "xmc4800-relax.yaml", [("lines: { irq: /dev/usic0/sr1 }", "lines: { irq: /dev/usic0/sr9 }")], False),
    ("name.line-unknown", "xmc4800-relax.yaml", [("lines: { irq: /dev/usic0/sr1 }", "lines: { irq: /dev/usic0/ch1 }")], False),
    ("name.line-unknown", "xmc4800-relax.yaml",
     [("lines: { irq: /dev/usic0/sr1 }", "lines: { irq: /dev/usic0/ch1/sr1 }")], False),
    ("name.line-unknown", "qemu-arm64.yaml",
     [("devices: [/dev/rtc]", "devices: [/dev/rtc]\n    lines: { irq: /dev/virtio/irq }")], False),
    ("name.line-unknown", "qemu-arm64.yaml",
     [("devices: [/dev/rtc]", "devices: [/dev/rtc]\n    lines: { irq: /dev/virtio/32/irq }")], False),
    ("name.line-unknown", "qemu-arm64.yaml",
     [("devices: [/dev/rtc]", "devices: [/dev/rtc]\n    lines: { alarm: /dev/rtc/0/alarm }")], False),
    (None, "qemu-arm64.yaml",
     [("devices: [/dev/rtc]", "devices: [/dev/rtc]\n    lines: { alarm: /dev/rtc/alarm, irq: /dev/virtio/31/irq }")],
     False),
    ("name.namespace", "qemu-x86_64.yaml", [("    uses: [/svc/sensor]\n", "    uses: [/svc/sensor]\n    serves: /shm/app\n")],
     False),
    ("name.namespace", "qemu-x86_64.yaml", [("uses: [/svc/sensor]", "uses: [/svc/Sensor]")], False),
    ("name.namespace", "qemu-arm64.yaml", [("maps: { /shm/history: ro }", "maps: { /svc/history: ro }")], False),
    ("name.namespace", "xmc4800-relax.yaml", [("stdout: /svc/console", "stdout: /dev/usic0/ch0")], False),
    (("name.namespace", "order.undeclared"), "qemu-arm64.yaml",
     [("  - name: /shm/history\n", "  - name: /svc/history\n")], True),
    ("name.reserved", "qemu-x86_64.yaml", [("uses: [/svc/sensor]", "uses: [/svc/sensor, /init/events]")], False),
    ("name.reserved", "qemu-x86_64.yaml", [("stdout: kernel", "stdout: /init")], False),
    ("name.reserved", "qemu-x86_64.yaml", [("  - name: app\n", "  - name: never\n")], False),
    ("name.duplicate", "qemu-x86_64.yaml", [("  - name: app\n", "  - name: sensor\n")], False),
    ("name.duplicate", "qemu-arm64.yaml", [("    core: 2\n", "    core: 2\n    serves: /svc/sensor\n")], False),
    ("name.duplicate", "qemu-arm64.yaml",
     [("    cache: cached #", "    cache: cached\n  - name: /shm/history\n    size: 32\n    cache: uncached #")], False),
    ("order.undeclared", "qemu-x86_64.yaml", [("uses: [/svc/sensor]", "uses: [/svc/sensors]")], False),
    (("form.inapplicable", "order.forward"), "xmc4800-relax.yaml",
     [("    serves: /svc/spi0\n", "    serves: /svc/spi0\n    uses: [/svc/sensor]\n")],
     False),
    ("order.forward", "xmc4800-relax.yaml", [("uses: [/svc/spi0]", "uses: [/svc/spi0, /svc/sensor]")], False),
    ("order.undeclared", "qemu-x86_64.yaml", [("watches: [sensor]", "watches: [sensors]")], False),
    ("order.forward", "qemu-x86_64.yaml", [("    serves: /svc/sensor\n", "    serves: /svc/sensor\n    watches: [health]\n")],
     False),
    ("order.forward", "qemu-x86_64.yaml", [("watches: [sensor]", "watches: [sensor, health]")], False),
    ("order.undeclared", "qemu-arm64.yaml", [("maps: { /shm/history: ro }", "maps: { /shm/histories: ro }")], False),
    ("order.undeclared", "qemu-arm64.yaml", [("ends: never", "ends: main")], False),
    ("restart.ends", "qemu-arm64.yaml", [("ends: never", "ends: sensor")], True),
    (None, "qemu-arm64.yaml", [("ends: never", "ends: sensor"), ("    restart: { max: 3 }\n", "")], False),
    (None, "qemu-arm64.yaml", [("ends: never", "ends: health")], False),
    (None, "xmc4800-relax.yaml", [("    priority: 12\n", "    priority: 12\n    restart: { max: 1 }\n")], False),
    (None, "xmc4800-relax.yaml", [("    priority: 12\n", "    priority: 12\n    restart: { max: 0 }\n")], False),
    ("encoding.watches", "qemu-x86_64.yaml", [WATCHED, ("watches: [sensor]", "watches: [%s]" % WATCHES_33)], False),
    (None, "qemu-x86_64.yaml", [WATCHED, ("watches: [sensor]", "watches: [%s]" % WATCHES_32)], False),
    ("order.undeclared", "xmc4800-relax.yaml", [("stdout: /svc/console", "stdout: /svc/uart")], False),
    ("ownership.device", "qemu-x86_64.yaml",
     [("    uses: [/svc/sensor]\n", "    uses: [/svc/sensor]\n    devices: [/dev/cmos_rtc]\n")], False),
    ("ownership.device", "qemu-arm64.yaml", [("    core: 2\n", "    core: 2\n    devices: [/dev/rtc]\n")], False),
    (None, "qemu-arm64.yaml", [("    core: 2\n", "    core: 2\n    devices: [/dev/gpio]\n")], False),
    (None, "qemu-x86_64.yaml", [("    uses: [/svc/sensor]\n", "    uses: [/svc/sensor]\n    devices: [/dev/com2]\n")], False),
    ("ownership.line", "xmc4800-relax.yaml", [("lines: { irq: /dev/usic0/sr1 }", "lines: { irq: /dev/usic0/sr0 }")], False),
    ("ownership.line", "xmc4800-relax.yaml",
     [("lines: { irq: /dev/usic0/sr1 }", "lines: { irq: /dev/usic0/sr1, rx: /dev/usic0/sr1 }")], False),
    (None, "xmc4800-relax.yaml",
     [("lines: { irq: /dev/usic0/sr1 }", "lines: { irq: /dev/usic0/sr1, rx: /dev/usic0/sr2 }")], False),
    ("ownership.line", "qemu-arm64.yaml",
     [("devices: [/dev/rtc]", "devices: [/dev/rtc]\n    lines: { a: /dev/virtio/3/irq }"),
      ("    core: 2\n", "    core: 2\n    lines: { b: /dev/virtio/3/irq }\n")], False),
    (None, "qemu-arm64.yaml",
     [("devices: [/dev/rtc]", "devices: [/dev/rtc]\n    lines: { a: /dev/virtio/3/irq }"),
      ("    core: 2\n", "    core: 2\n    lines: { b: /dev/virtio/4/irq }\n")], False),
    ("ownership.gate", "qemu-arm64.yaml",
     [("devices: [/dev/rtc]", "devices: [/dev/rtc, /dev/virtio/0]"), ("    core: 2\n", "    core: 2\n    devices: [/dev/virtio/1]\n")],
     False),
    (None, "qemu-arm64.yaml",
     [("devices: [/dev/rtc]", "devices: [/dev/rtc, /dev/virtio/0]"), ("    core: 2\n", "    core: 2\n    devices: [/dev/virtio/8]\n")],
     False),
    ("ownership.kernel", "xmc4800-relax.yaml", [("devices: [/dev/usic0/ch1]", "devices: [/dev/usic0/ch1, /dev/scu]")], False),
    ("form.inapplicable", "xmc4800-relax.yaml", [("    serves: /svc/spi0\n", "    serves: /svc/spi0\n    watches: [console]\n")], False),
    ("name.entry", "qemu-x86_64.yaml", [("entry: app_main", "entry: switch")], False),
    ("name.entry", "qemu-x86_64.yaml", [("entry: app_main", "entry: xor_eq")], False),
    ("name.entry", "qemu-x86_64.yaml", [("entry: app_main", "entry: kickos_table")], False),
    ("name.entry", "qemu-x86_64.yaml", [("entry: app_main", "entry: kickos_table_image")], False),
    ("name.entry", "qemu-x86_64.yaml", [("entry: app_main", "entry: kos_app")], False),
    ("name.entry", "qemu-x86_64.yaml", [("entry: app_main", "entry: KOS_APP")], False),
    ("name.entry", "qemu-x86_64.yaml", [("entry: app_main", "entry: KICKOS_APP")], False),
    (None, "qemu-x86_64.yaml", [("entry: app_main", "entry: kickos_main")], False),
    ("ownership.kernel", "qemu-x86_64.yaml", [("devices: [/dev/cmos_rtc]", "devices: [/dev/cmos_rtc, /dev/pit]")], False),
    ("ownership.console", "qemu-x86_64.yaml", [("devices: [/dev/cmos_rtc]", "devices: [/dev/cmos_rtc, /dev/com1]")], False),
    ("ownership.kernel", "qemu-arm64.yaml", [("devices: [/dev/rtc]", "devices: [/dev/timer]")], False),
    ("ownership.kernel", "qemu-arm64.yaml",
     [("devices: [/dev/rtc]", "devices: [/dev/rtc]\n    lines: { tick: /dev/timer/el1_phys }")], False),
    ("ownership.console", "qemu-arm64.yaml", [("devices: [/dev/rtc]", "devices: [/dev/rtc, /dev/uart0]")], False),
    (None, "xmc4800-relax.yaml", [("devices: [/dev/usic0/ch0]", "devices: [/dev/usic0/ch0] # stdout's own")], False),
    (("ownership.console", "ownership.device"), "xmc4800-relax.yaml",
     [("    uses: [/svc/sensor]\n", "    uses: [/svc/sensor]\n    devices: [/dev/usic0/ch0]\n")], False),
    (("ownership.console", "ownership.device"), "xmc4800-relax.yaml",
     [("devices: [/dev/usic0/ch1]", "devices: [/dev/usic0/ch1, /dev/usic0/ch0]")], False),
    ("ownership.console", "xmc4800-relax.yaml",
     [("devices: [/dev/usic0/ch0]", "devices: []"),
      ("    uses: [/svc/sensor]\n", "    uses: [/svc/sensor]\n    devices: [/dev/usic0/ch0]\n")], False),
]


# A manifest as tools/manifest/genmanifest.py writes it, beside a copy of platform/.
MANIFEST = """version: 1
abi:
  table: 6
  cap_reserved: 2
  symbol_prefix: ""
target:
  board: xmc4800-relax
  chip: xmc4800
  arch: armv7m
  cores: 2
  kernel_cores: 1
  isolated_cores: 0x0
  amp:
    node: 0
    nodes: 2
    ports: [[0, 2], [1, 3]]
    share: 0x4000
    share_cache: uncached
protection:
  enforced: true
  window_rule: pow2
  smallest_window: 32
  thread_windows: 4
  fault_isolation: true
pools:
  KICKOS_MAX_THREADS: 8
  KICKOS_TASK_ENDPOINT_BUDGET: 4
  KICKOS_MAX_SPAWN_GRANTS: 6
  KICKOS_CAP_TABLE_SUPPLY: 16
threads:
  priority: [1, 31]
  min_stack: 960
  user_stack: 4096
  idle_stack: 512
  root_stack: 4096
  stack_align: 16
  stack_stride: 4096
init:
  status_record_size: 8
  private_record_size: 64
  free_regions: 5
descriptions:
  chip: platform/xmc4800/chip.yaml
  board: platform/xmc4800/xmc4800-relax.yaml
drivers:
  xmcssc:
    windows: [regs]
    lines: [irq]
    threads:
      - { name: bus, priority: 0, stack: default, caps: 3, badged: 0 }
    endpoints: 1
    notifications: 1
    block: none
    block_cache: cached
    posture: retain
    barrier: none
    console: false
    start: xmc_spi0_start
    receiver: bus
    client: [kickos_spi_proxy]
  xmcuartirq:
    windows: [regs]
    lines: [irq]
    threads:
      - { name: uartirq, priority: 1, stack: default, caps: 2, badged: 0 }
      - { name: service, priority: 0, stack: default, caps: 2, badged: 1 }
    endpoints: 1
    notifications: 1
    block: 1024
    block_cache: cached
    posture: handover
    barrier: 1
    console: true
    start: xmcuartirq_console_start
    receiver: service
    client: []
"""

MANIFEST_DEFAULT = ("drivers:\n  xmcssc:", "default:\n  composition: boards/xmc4800-relax/composition.yaml\ndrivers:\n  xmcssc:")
MANIFEST_AMP = "  amp:\n    node: 0\n    nodes: 2\n    ports: [[0, 2], [1, 3]]\n    share: 0x4000\n    share_cache: uncached\n"

XMC_TARGET = ("  board: xmc4800-relax\n  chip: xmc4800\n  arch: armv7m\n  cores: 2\n  kernel_cores: 1\n"
              "  isolated_cores: 0x0\n  amp:\n    node: 0\n    nodes: 2\n    ports: [[0, 2], [1, 3]]\n"
              "    share: 0x4000\n    share_cache: uncached\n")
XMC_PROTECTION = ("protection:\n  enforced: true\n  window_rule: pow2\n  smallest_window: 32\n  thread_windows: 4\n"
                  "  fault_isolation: true\n")
XMC_POOLS = ("  KICKOS_MAX_THREADS: 8\n  KICKOS_TASK_ENDPOINT_BUDGET: 4\n  KICKOS_MAX_SPAWN_GRANTS: 6\n"
             "  KICKOS_CAP_TABLE_SUPPLY: 16\n")
XMC_THREADS = "  min_stack: 960\n  user_stack: 4096\n  idle_stack: 512\n  root_stack: 4096\n  stack_align: 16\n  stack_stride: 4096\n"
XMC_DESCRIPTIONS = "  chip: platform/xmc4800/chip.yaml\n  board: platform/xmc4800/xmc4800-relax.yaml\n"
# A node of a two-node partition that names no crossing and states a user share.
SHARE_AMP = "  amp:\n    node: 0\n    nodes: 2\n    ports: []\n    share: 0x4000\n    share_cache: cached\n"
XMC_SHARE = [("  kernel_cores: 1\n  isolated_cores: 0x0\n", "  kernel_cores: 1\n  isolated_cores: 0x0\n" + SHARE_AMP)]
ARM64_SHARE = [("  kernel_cores: 4\n  isolated_cores: 0x0\n", "  kernel_cores: 1\n  isolated_cores: 0x0\n" + SHARE_AMP)]
ARM64_UNPINNED = [("    core: 1\n", ""), ("    core: 2\n", "")]
TRANSLATING = "protection:\n  enforced: true\n  window_rule: none\n  thread_windows: 4\n  fault_isolation: true\n"


def pools_of(threads, tasks, domains, endpoints, endpoint_budget, supply):
    """The pools a kernel build of the fleet's exports, as their figures."""
    return ("  KICKOS_MAX_THREADS: %d\n  KICKOS_MAX_TASKS: %d\n  KICKOS_MAX_DOMAINS: %d\n  KICKOS_MAX_SEMAPHORES: 16\n"
            "  KICKOS_MAX_MUTEXES: 8\n  KICKOS_MAX_ENDPOINTS: %d\n  KICKOS_MAX_IRQ_HANDLES: 8\n  KICKOS_MAX_NOTIFY: 8\n"
            "  KICKOS_TASK_SEMAPHORE_BUDGET: 15\n  KICKOS_TASK_MUTEX_BUDGET: 7\n  KICKOS_TASK_ENDPOINT_BUDGET: %d\n"
            "  KICKOS_TASK_IRQ_HANDLE_BUDGET: 7\n  KICKOS_TASK_NOTIFY_BUDGET: 7\n  KICKOS_MAX_SPAWN_GRANTS: 6\n"
            "  KICKOS_CAP_TABLE_SUPPLY: %d\n  KICKOS_RAM_OWNER_SLOTS: 48\n  KICKOS_ASPACE_RANGES: 64\n"
            % (threads, tasks, domains, endpoints, endpoint_budget, supply))


REGION_POOLS = pools_of(8, 10, 10, 5, 4, 16)
ARM64_POOLS = pools_of(16, 18, 20, 4, 3, 16)
Q35_POOLS = pools_of(32, 18, 20, 16, 12, 32)
C6_POOLS = pools_of(16, 18, 18, 4, 3, 16)


def manifest_of(board, chip, arch, protection, cores, pools, stacks):
    """MANIFEST as the kernel build of `board` writes it, with `cores` kernel cores and `stacks`
    (min, user, idle, root, the masked-SP stride or none)."""
    return mutate(MANIFEST, [
        (XMC_TARGET, "  board: %s\n  chip: %s\n  arch: %s\n  cores: %d\n  kernel_cores: %d\n  isolated_cores: 0x0\n"
         % (board, chip, arch, cores, cores)),
        (XMC_PROTECTION, protection),
        (XMC_POOLS, pools),
        (XMC_THREADS, "  min_stack: %d\n  user_stack: %d\n  idle_stack: %d\n  root_stack: %d\n  stack_align: 16\n"
         "  stack_stride: %s\n" % stacks),
        (XMC_DESCRIPTIONS, "  chip: platform/%s/chip.yaml\n  board: platform/%s/%s.yaml\n" % (chip, chip, board)),
    ])[0]


# Each golden system's control manifest, as its preset writes it.
MANIFESTS = {
    "xmc4800-relax.yaml": manifest_of("xmc4800-relax", "xmc4800", "armv7m", XMC_PROTECTION, 1, REGION_POOLS,
                                        (960, 4096, 512, 4096, "4096")),
    "qemu-arm64.yaml": manifest_of("qemu-arm64", "virt_arm64", "armv8a", TRANSLATING, 4, ARM64_POOLS,
                                     (2816, 12288, 4096, 20480, "none")),
    "qemu-x86_64.yaml": manifest_of("qemu-x86_64", "q35", "x86_64", TRANSLATING, 2, Q35_POOLS,
                                      (3328, 65536, 65536, 65536, "none")),
}
XMC_MANIFEST = MANIFESTS["xmc4800-relax.yaml"]
K64F_MANIFEST = manifest_of("frdmk64f", "mk64f", "armv7m", XMC_PROTECTION.replace("pow2", "granule"), 1, REGION_POOLS,
                            (960, 8192, 512, 8192, "8192"))
C6_MANIFEST = manifest_of("esp32c6-wroom", "esp32c6", "rv32imac", XMC_PROTECTION.replace("32", "8"), 1, C6_POOLS,
                          (1024, 8192, 512, 8192, "none"))
IMX_MANIFEST = manifest_of("imx8mp-evk", "imx8mp", "armv8a", TRANSLATING, 1, ARM64_POOLS, (2816, 12288, 4096, 20480, "none"))
RV64_MANIFEST = manifest_of("qemu-riscv64", "virt_rv64", "rv64imac", TRANSLATING, 1, ARM64_POOLS, (2304, 12288, 4096, 20480, "none"))
XMC_ENDPOINTS_2 = XMC_MANIFEST.replace("  KICKOS_TASK_ENDPOINT_BUDGET: 4\n", "  KICKOS_TASK_ENDPOINT_BUDGET: 2\n")
XMC_ENDPOINTS_3 = XMC_MANIFEST.replace("  KICKOS_TASK_ENDPOINT_BUDGET: 4\n", "  KICKOS_TASK_ENDPOINT_BUDGET: 3\n")
XMC_ARENA = "    size: 0x20000\n    arena: true\n"
UNENFORCED = [("  enforced: true\n", "  enforced: false\n")]
NO_PROTECTION = ("ends: never\n", "ends: never\naccepts: [no_protection]\n")
VIRTIO = "devices: [/dev/rtc, /dev/virtio/31]\n    accepts: [%s]"
PORTS = ("    maps: { /shm/history: rw }\n", "    maps: { /shm/history: rw }\n    devices: [/dev/port/3, /dev/port/4, /dev/port/6]\n")
SENSOR_ALARM = ("devices: [/dev/rtc]", "devices: [/dev/rtc]\n    lines: { alarm: /dev/rtc/alarm }")
DRIVERS_UNNOTIFIED = [("    notifications: 1\n    block: none\n", "    notifications: 0\n    block: none\n"),
                      ("    notifications: 1\n    block: 1024\n", "    notifications: 0\n    block: 1024\n")]
SENSOR_LINES = ("    maps: { /shm/history: rw }\n", "    maps: { /shm/history: rw }\n    lines: { a: /dev/usic0/sr2, b: /dev/usic0/sr3 }\n")

# A packaged driver of one window role, and a task running it on q35 over `device`.
PORT_DRIVER = [("drivers:\n", "drivers:\n  portdrv:\n    windows: [regs]\n    lines: []\n    threads:\n"
                "      - { name: io, priority: 0, stack: default, caps: 1, badged: 0 }\n    endpoints: 1\n"
                "    notifications: 0\n    block: none\n    block_cache: cached\n    posture: retain\n    barrier: none\n"
                "    console: false\n    start: portdrv_start\n    receiver: io\n    client: []\n")]
def com2_driver(device):
    return ("    maps: { /shm/history: ro }\n",
            "    maps: { /shm/history: ro }\n\n  - name: uart\n    driver: portdrv\n    devices: [%s]\n"
            "    priority: 11\n" % device)

# A golden system against its control manifest, each with edits: (rules, system, edits, manifest
# edits, anywhere).
ADMISSION_ARMS = [
    ("encoding.window", "xmc4800-relax.yaml", [], [("  smallest_window: 32\n", "  smallest_window: 0x400\n")], True),
    ("encoding.window", "xmc4800-relax.yaml", [NO_PROTECTION],
     UNENFORCED + [("  smallest_window: 32\n", "  smallest_window: 0x400\n")], True),
    (None, "xmc4800-relax.yaml", [NO_PROTECTION], UNENFORCED + [("  smallest_window: 32\n", "  smallest_window: 0x200\n")],
     False),
    (None, "xmc4800-relax.yaml", [], [("  smallest_window: 32\n", "  smallest_window: 0x200\n")], False),
    ("encoding.window", "xmc4800-relax.yaml", [],
     [("window_rule: pow2", "window_rule: granule"), ("  smallest_window: 32\n", "  smallest_window: 0x400\n")], True),
    (None, "xmc4800-relax.yaml", [],
     [("window_rule: pow2", "window_rule: granule"), ("  smallest_window: 32\n", "  smallest_window: 0x100\n")], False),
    ("encoding.budget", "xmc4800-relax.yaml", [PORTS], [("  thread_windows: 4\n", "  thread_windows: 3\n")], True),
    (None, "xmc4800-relax.yaml", [PORTS], [], False),
    ("encoding.budget", "xmc4800-relax.yaml", [NO_PROTECTION, PORTS],
     UNENFORCED + [("  thread_windows: 4\n", "  thread_windows: 3\n")], True),
    ("encoding.budget", "qemu-arm64.yaml", [("    maps: { /shm/history: rw }\n", "")],
     [("  thread_windows: 4\n", "  thread_windows: 1\n")], True),
    (None, "qemu-arm64.yaml", [("    maps: { /shm/history: rw }\n", ""), ("    watches: [sensor]\n", "")],
     [("  thread_windows: 4\n", "  thread_windows: 1\n")], False),
    ("supply.init-windows", "xmc4800-relax.yaml", [], [("  free_regions: 5\n", "  free_regions: 2\n")], True),
    (None, "xmc4800-relax.yaml", [], [("  free_regions: 5\n", "  free_regions: 3\n")], False),
    ("supply.init-windows", "xmc4800-relax.yaml", [NO_PROTECTION],
     UNENFORCED + [("  free_regions: 5\n", "  free_regions: 2\n")], True),
    (None, "xmc4800-relax.yaml", [NO_PROTECTION], UNENFORCED + [("  free_regions: 5\n", "  free_regions: 3\n")], False),
    (None, "qemu-arm64.yaml", [], [("  free_regions: 5\n", "  free_regions: 0\n")], False),
    ("enforcement.port-bank", "xmc4800-relax.yaml",
     [("    maps: { /shm/history: rw }\n", "    maps: { /shm/history: rw }\n    devices: [/dev/port/1]\n")], [], False),
    (None, "xmc4800-relax.yaml",
     [("    maps: { /shm/history: rw }\n", "    maps: { /shm/history: rw }\n    devices: [/dev/port/1]\n    accepts: [coarse_gate]\n")],
     [], False),
    ("enforcement.unneeded", "xmc4800-relax.yaml",
     [("    maps: { /shm/history: rw }\n", "    maps: { /shm/history: rw }\n    devices: [/dev/port/3]\n    accepts: [coarse_gate]\n")],
     [], False),
    ("enforcement.no-protection", "xmc4800-relax.yaml", [], UNENFORCED, True),
    (None, "xmc4800-relax.yaml", [NO_PROTECTION], UNENFORCED, False),
    (None, "xmc4800-relax.yaml", [("ends: never\n", "ends: never\naccepts: [no_protection]\n")], [], False),
    ("enforcement.unneeded", "qemu-arm64.yaml", [("ends: never\n", "ends: never\naccepts: [no_protection]\n")], [], False),
    ("enforcement.bus-master", "qemu-arm64.yaml",
     [("devices: [/dev/rtc]", VIRTIO % "coarse_gate"), ("ends: never\n", "ends: never\naccepts: [cached_incoherent]\n")], [],
     False),
    ("encoding.page-shared", "qemu-arm64.yaml",
     [("devices: [/dev/rtc]", VIRTIO % "bus_master"), ("ends: never\n", "ends: never\naccepts: [cached_incoherent]\n")], [],
     False),
    ("memory.cached-incoherent", "qemu-arm64.yaml", [("devices: [/dev/rtc]", VIRTIO % "bus_master, coarse_gate")], [], True),
    (None, "qemu-arm64.yaml",
     [("devices: [/dev/rtc]", VIRTIO % "bus_master, coarse_gate"), ("ends: never\n", "ends: never\naccepts: [cached_incoherent]\n")],
     [], False),
    (None, "qemu-arm64.yaml",
     [("devices: [/dev/rtc]", VIRTIO % "bus_master, coarse_gate"), ("cache: cached", "cache: uncached")], [], False),
    ("enforcement.unneeded", "qemu-arm64.yaml", [("devices: [/dev/rtc]", "devices: [/dev/rtc]\n    accepts: [bus_master]")], [],
     False),
    ("enforcement.unneeded", "qemu-arm64.yaml", [("ends: never\n", "ends: never\naccepts: [cached_incoherent]\n")], [], False),
    (None, "qemu-arm64.yaml", [("cache: cached", "cache: uncached")], [], False),
    (None, "qemu-x86_64.yaml", [("cache: cached", "cache: uncached")], [], False),
    ("name.driver-unknown", "xmc4800-relax.yaml", [("    driver: xmcssc\n", "    driver: xmcspi\n")], [], False),
    ("driver.line-role", "xmc4800-relax.yaml",
     [("lines: { irq: /dev/usic0/sr1 }", "lines: { irq: /dev/usic0/sr1, rx: /dev/usic0/sr2 }")], [], False),
    ("driver.line-role", "xmc4800-relax.yaml", [("    lines: { irq: /dev/usic0/sr1 }\n", "")], [], True),
    ("driver.line-name", "xmc4800-relax.yaml", [("lines: { irq: /dev/usic0/sr1 }", "lines: { sr2: /dev/usic0/sr1 }")],
     [("  xmcssc:\n    windows: [regs]\n    lines: [irq]\n", "  xmcssc:\n    windows: [regs]\n    lines: [sr2]\n")], False),
    (None, "xmc4800-relax.yaml", [("lines: { irq: /dev/usic0/sr1 }", "lines: { sr1: /dev/usic0/sr1 }")],
     [("  xmcssc:\n    windows: [regs]\n    lines: [irq]\n", "  xmcssc:\n    windows: [regs]\n    lines: [sr1]\n")], False),
    ("driver.window-role", "xmc4800-relax.yaml",
     [("devices: [/dev/usic0/ch1]", "devices: [/dev/usic0/ch1, /dev/port/3]")], [], False),
    ("driver.window-role", "xmc4800-relax.yaml", [("    devices: [/dev/usic0/ch1]\n", "")], [], True),
    ("driver.authority", "xmc4800-relax.yaml", [("    driver: xmcssc\n", "    driver: xmcssc\n    authority: [irq]\n")], [],
     False),
    ("driver.port-window", "qemu-x86_64.yaml", [com2_driver("/dev/com2")], PORT_DRIVER, False),
    (None, "qemu-x86_64.yaml", [com2_driver("/dev/hpet")], PORT_DRIVER, False),
    ("restart.no-isolation", "xmc4800-relax.yaml", [], [("  fault_isolation: true\n", "  fault_isolation: false\n")],
     True),
    (None, "xmc4800-relax.yaml", [("    priority: 11\n    restart: { max: 3 }\n", "    priority: 11\n")],
     [("  fault_isolation: true\n", "  fault_isolation: false\n")], False),
    (None, "xmc4800-relax.yaml", [("    stack: 4096\n    priority: 8\n", "    stack: 4096\n    priority: 8\n    authority: [irq]\n")],
     [], False),
    ("scheduling.init-priority-range", "xmc4800-relax.yaml", [("heap: 16384\n", "heap: 16384\ninit: { priority: 32 }\n")], [],
     False),
    ("scheduling.init-priority-range", "xmc4800-relax.yaml", [("heap: 16384\n", "heap: 16384\ninit: { priority: 0 }\n")], [],
     False),
    (None, "xmc4800-relax.yaml", [("heap: 16384\n", "heap: 16384\ninit: { priority: 31 }\n")], [], False),
    (("scheduling.init-priority-range", "scheduling.priority", "scheduling.ceiling"), "xmc4800-relax.yaml", [],
     [("  priority: [1, 31]\n", "  priority: [1, 1]\n")], True),
    (None, "xmc4800-relax.yaml", [("heap: 16384\n", "heap: 16384\ninit: { priority: 1 }\n")], [], False),
    ("form.unknown-field", "xmc4800-relax.yaml",
     [("heap: 16384\n", "heap: 16384\ninit: { priority: 5, stack: 4096 }\n")], [], False),
    ("form.type", "xmc4800-relax.yaml", [("heap: 16384\n", "heap: 16384\ninit: 5\n")], [], False),
    ("scheduling.priority", "xmc4800-relax.yaml", [("    priority: 12\n", "    priority: 31\n")], [], False),
    (None, "xmc4800-relax.yaml", [("    priority: 12\n", "    priority: 30\n")], [], False),
    ("scheduling.priority", "xmc4800-relax.yaml", [("    priority: 8\n", "    priority: 0\n")], [], False),
    (None, "xmc4800-relax.yaml", [("    priority: 8\n", "    priority: 1\n")], [], False),
    ("scheduling.ceiling", "xmc4800-relax.yaml", [("    stack: 4096\n    priority: 8\n    ceiling: 8\n", "    stack: 4096\n    priority: 8\n    ceiling: 7\n")],
     [], False),
    (("scheduling.ceiling", "scheduling.stdout-priority"), "xmc4800-relax.yaml",
     [("    stack: 4096\n    priority: 8\n    ceiling: 8\n", "    stack: 4096\n    priority: 8\n    ceiling: 32\n")], [], False),
    ("scheduling.stdout-priority", "xmc4800-relax.yaml",
     [("    stack: 4096\n    priority: 8\n    ceiling: 8\n", "    stack: 4096\n    priority: 8\n    ceiling: 13\n")], [], False),
    (None, "xmc4800-relax.yaml", [("    stack: 4096\n    priority: 8\n    ceiling: 8\n", "    stack: 4096\n    priority: 8\n    ceiling: 12\n")],
     [], False),
    (None, "qemu-x86_64.yaml", [("    priority: 8\n    ceiling: 8\n", "    priority: 8\n    ceiling: 31\n")], [], False),
    ("form.missing", "xmc4800-relax.yaml", [("    stack: 4096\n    priority: 8\n    ceiling: 8\n", "    stack: 4096\n    priority: 8\n")],
     [], True),
    ("form.inapplicable", "xmc4800-relax.yaml", [("    priority: 11\n    restart: { max: 3 }\n", "    priority: 11\n    ceiling: 12\n    restart: { max: 3 }\n")],
     [], False),
    ("scheduling.stdout-priority", "xmc4800-relax.yaml", [("    priority: 10\n    ceiling: 10\n", "    priority: 13\n    ceiling: 13\n")], [], False),
    (None, "xmc4800-relax.yaml", [("    priority: 10\n    ceiling: 10\n", "    priority: 12\n    ceiling: 12\n")], [], False),
    ("scheduling.stdout-order", "xmc4800-relax.yaml",
     [("\ntasks:\n", "\ntasks:\n  - name: early\n    entry: early_main\n    stack: 2048\n    priority: 5\n    ceiling: 5\n")], [], False),
    (("ownership.console", "scheduling.console-driver"), "xmc4800-relax.yaml", [("stdout: /svc/console", "stdout: kernel")],
     [], True),
    ("scheduling.console-driver", "xmc4800-relax.yaml", [], [("    console: true\n", "    console: false\n")], True),
    ("supply.pool", "xmc4800-relax.yaml", [], [("  KICKOS_MAX_THREADS: 8\n", "  KICKOS_MAX_THREADS: 5\n")], True),
    (None, "xmc4800-relax.yaml", [], [("  KICKOS_MAX_THREADS: 8\n", "  KICKOS_MAX_THREADS: 6\n")], False),
    ("supply.pool", "xmc4800-relax.yaml", [], [("  KICKOS_MAX_ENDPOINTS: 5\n", "  KICKOS_MAX_ENDPOINTS: 2\n")], True),
    ("supply.pool", "xmc4800-relax.yaml", [], [("  KICKOS_MAX_TASKS: 10\n", "  KICKOS_MAX_TASKS: 6\n")], True),
    (None, "xmc4800-relax.yaml", [], [("  KICKOS_MAX_TASKS: 10\n", "  KICKOS_MAX_TASKS: 7\n")], False),
    ("supply.budget", "xmc4800-relax.yaml", [], [("  KICKOS_TASK_ENDPOINT_BUDGET: 4\n", "  KICKOS_TASK_ENDPOINT_BUDGET: 2\n")],
     True),
    (None, "xmc4800-relax.yaml", [], [("  KICKOS_TASK_ENDPOINT_BUDGET: 4\n", "  KICKOS_TASK_ENDPOINT_BUDGET: 3\n")], False),
    ("supply.budget", "xmc4800-relax.yaml", [], [("  KICKOS_TASK_NOTIFY_BUDGET: 7\n", "  KICKOS_TASK_NOTIFY_BUDGET: 2\n")],
     True),
    (None, "xmc4800-relax.yaml", [], [("  KICKOS_TASK_NOTIFY_BUDGET: 7\n", "  KICKOS_TASK_NOTIFY_BUDGET: 3\n")], False),
    ("supply.budget", "xmc4800-relax.yaml", [SENSOR_LINES], [("  KICKOS_TASK_IRQ_HANDLE_BUDGET: 7\n", "  KICKOS_TASK_IRQ_HANDLE_BUDGET: 1\n")],
     True),
    (None, "xmc4800-relax.yaml", [SENSOR_LINES], [("  KICKOS_TASK_IRQ_HANDLE_BUDGET: 7\n", "  KICKOS_TASK_IRQ_HANDLE_BUDGET: 2\n")],
     False),
    ("supply.pool", "xmc4800-relax.yaml", [], [("  KICKOS_MAX_DOMAINS: 10\n", "  KICKOS_MAX_DOMAINS: 2\n")], True),
    (None, "xmc4800-relax.yaml", [], [("  KICKOS_MAX_DOMAINS: 10\n", "  KICKOS_MAX_DOMAINS: 3\n")], False),
    ("supply.pool", "qemu-arm64.yaml", [], [("  KICKOS_MAX_DOMAINS: 20\n", "  KICKOS_MAX_DOMAINS: 5\n")], True),
    (None, "qemu-arm64.yaml", [], [("  KICKOS_MAX_DOMAINS: 20\n", "  KICKOS_MAX_DOMAINS: 6\n")], False),
    ("supply.pool", "qemu-arm64.yaml", [], [("  KICKOS_MAX_TASKS: 18\n", "  KICKOS_MAX_TASKS: 7\n")], True),
    (None, "qemu-arm64.yaml", [], [("  KICKOS_MAX_TASKS: 18\n", "  KICKOS_MAX_TASKS: 8\n")], False),
    ("supply.pool", "xmc4800-relax.yaml", [], [("  KICKOS_MAX_NOTIFY: 8\n", "  KICKOS_MAX_NOTIFY: 3\n")], True),
    (None, "xmc4800-relax.yaml", [], [("  KICKOS_MAX_NOTIFY: 8\n", "  KICKOS_MAX_NOTIFY: 4\n")], False),
    ("supply.pool", "xmc4800-relax.yaml", [], [("  KICKOS_MAX_IRQ_HANDLES: 8\n", "  KICKOS_MAX_IRQ_HANDLES: 1\n")], True),
    (None, "xmc4800-relax.yaml", [], [("  KICKOS_MAX_IRQ_HANDLES: 8\n", "  KICKOS_MAX_IRQ_HANDLES: 2\n")], False),
    ("supply.pool", "xmc4800-relax.yaml", [], [("  KICKOS_MAX_ENDPOINTS: 5\n", "  KICKOS_MAX_ENDPOINTS: 4\n"),
                                                ("  isolated_cores: 0x0\n", "  isolated_cores: 0x0\n" + MANIFEST_AMP),
                                                ("  KICKOS_TASK_ENDPOINT_BUDGET: 4\n", "  KICKOS_TASK_ENDPOINT_BUDGET: 5\n")],
     True),
    (None, "xmc4800-relax.yaml", [], [("  KICKOS_MAX_ENDPOINTS: 5\n", "  KICKOS_MAX_ENDPOINTS: 4\n")], False),
    ("supply.cap-table", "qemu-arm64.yaml", [], [("  KICKOS_CAP_TABLE_SUPPLY: 16\n", "  KICKOS_CAP_TABLE_SUPPLY: 5\n")], True),
    (None, "qemu-arm64.yaml", [("    watches: [sensor]\n", "")], [("  KICKOS_CAP_TABLE_SUPPLY: 16\n", "  KICKOS_CAP_TABLE_SUPPLY: 5\n")],
     False),
    (None, "qemu-arm64.yaml", [], [("  KICKOS_CAP_TABLE_SUPPLY: 16\n", "  KICKOS_CAP_TABLE_SUPPLY: 6\n")], False),
    ("supply.cap-table", "qemu-arm64.yaml", [SENSOR_ALARM], [("  KICKOS_CAP_TABLE_SUPPLY: 16\n", "  KICKOS_CAP_TABLE_SUPPLY: 6\n")],
     True),
    (None, "qemu-arm64.yaml", [SENSOR_ALARM], [("  KICKOS_CAP_TABLE_SUPPLY: 16\n", "  KICKOS_CAP_TABLE_SUPPLY: 7\n")], False),
    ("supply.stack", "xmc4800-relax.yaml", [("    stack: 2048\n    priority: 9\n", "    stack: 2056\n    priority: 9\n")], [], False),
    (None, "xmc4800-relax.yaml", [("    stack: 2048\n    priority: 9\n", "    stack: 2064\n    priority: 9\n")], [], False),
    ("supply.stack", "xmc4800-relax.yaml", [("    stack: 2048\n    priority: 9\n", "    stack: 4112\n    priority: 9\n")], [], False),
    (None, "xmc4800-relax.yaml", [("    stack: 2048\n    priority: 9\n", "    stack: 4096\n    priority: 9\n")], [], False),
    (None, "qemu-arm64.yaml", [("    stack: 4096\n    priority: 9\n", "    stack: 0x10000\n    priority: 9\n")], [], False),
    ("scheduling.line-core", "xmc4800-relax.yaml", [], [("  cores: 1\n  kernel_cores: 1\n", "  cores: 2\n  kernel_cores: 2\n")], True),
    (None, "xmc4800-relax.yaml", [("    priority: 12\n", "    priority: 12\n    core: 0\n"), ("    priority: 11\n", "    priority: 11\n    core: 1\n")],
     [("  cores: 1\n  kernel_cores: 1\n", "  cores: 2\n  kernel_cores: 2\n")], False),
    ("scheduling.core", "xmc4800-relax.yaml", [("    uses: [/svc/spi0]\n", "    uses: [/svc/spi0]\n    core: 1\n")], [], False),
    (None, "xmc4800-relax.yaml", [("    uses: [/svc/spi0]\n", "    uses: [/svc/spi0]\n    core: 0\n")], [], False),
    (None, "xmc4800-relax.yaml", [("    priority: 10\n    ceiling: 10\n", "    priority: 13\n    ceiling: 13\n")], [("    receiver: service\n", "    receiver: uartirq\n")],
     False),
    ("name.entry", "xmc4800-relax.yaml", [("entry: app_main", "entry: xmc_spi0_start")], [], False),
    ("supply.spawn-grants", "xmc4800-relax.yaml", [], [("  KICKOS_MAX_SPAWN_GRANTS: 6\n", "  KICKOS_MAX_SPAWN_GRANTS: 1\n")],
     True),
    ("supply.spawn-grants", "xmc4800-relax.yaml", [], [("  KICKOS_MAX_SPAWN_GRANTS: 6\n", "  KICKOS_MAX_SPAWN_GRANTS: 2\n")],
     True),
    (None, "xmc4800-relax.yaml", [], [("  KICKOS_MAX_SPAWN_GRANTS: 6\n", "  KICKOS_MAX_SPAWN_GRANTS: 3\n")], False),
    ("supply.cap-table", "xmc4800-relax.yaml", [], [("  KICKOS_CAP_TABLE_SUPPLY: 16\n", "  KICKOS_CAP_TABLE_SUPPLY: 10\n")], True),
    (None, "xmc4800-relax.yaml", [], [("  KICKOS_CAP_TABLE_SUPPLY: 16\n", "  KICKOS_CAP_TABLE_SUPPLY: 11\n")], False),
    (None, "xmc4800-relax.yaml", [], DRIVERS_UNNOTIFIED + [("  KICKOS_CAP_TABLE_SUPPLY: 16\n", "  KICKOS_CAP_TABLE_SUPPLY: 9\n")],
     False),
    ("supply.cap-table", "xmc4800-relax.yaml", [],
     DRIVERS_UNNOTIFIED + [("  KICKOS_CAP_TABLE_SUPPLY: 16\n", "  KICKOS_CAP_TABLE_SUPPLY: 8\n")], True),
    ("supply.cap-table", "xmc4800-relax.yaml", [],
     [("caps: 2, badged: 1 }", "caps: 2, badged: 2 }"), ("  KICKOS_CAP_TABLE_SUPPLY: 16\n", "  KICKOS_CAP_TABLE_SUPPLY: 11\n")],
     True),
    ("supply.cap-table", "xmc4800-relax.yaml", [],
     [("caps: 2, badged: 1 }", "caps: 2, badged: 0 }"), ("  KICKOS_CAP_TABLE_SUPPLY: 16\n", "  KICKOS_CAP_TABLE_SUPPLY: 10\n")],
     True),
    (None, "xmc4800-relax.yaml", [],
     [("caps: 2, badged: 1 }", "caps: 2, badged: 0 }"), ("  KICKOS_CAP_TABLE_SUPPLY: 16\n", "  KICKOS_CAP_TABLE_SUPPLY: 11\n")],
     False),
    ("supply.stack", "xmc4800-relax.yaml", [], [("  min_stack: 960\n", "  min_stack: 4096\n")], True),
    (None, "xmc4800-relax.yaml", [], [("  min_stack: 960\n", "  min_stack: 2048\n")], False),
    ("supply.arena", "xmc4800-relax.yaml", [], [("  user_stack: 4096\n", "  user_stack: 0x10000\n"),
                                                 ("  stack_stride: 4096\n", "  stack_stride: 0x10000\n")], True),
    (None, "xmc4800-relax.yaml", [], [("  user_stack: 4096\n", "  user_stack: 0x2000\n")], False),
    (None, "xmc4800-relax.yaml", [("heap: 16384\n", "heap: 0x18000\n")], [], False),
    ("supply.reservations", "xmc4800-relax.yaml", [], [("  KICKOS_RAM_OWNER_SLOTS: 48\n", "  KICKOS_RAM_OWNER_SLOTS: 3\n")], True),
    (None, "xmc4800-relax.yaml", [], [("  KICKOS_RAM_OWNER_SLOTS: 48\n", "  KICKOS_RAM_OWNER_SLOTS: 4\n")], False),
    # The partition's user share takes one slot more, seated in root before the init runs.
    ("supply.reservations", "xmc4800-relax.yaml", [],
     XMC_SHARE + [("  KICKOS_RAM_OWNER_SLOTS: 48\n", "  KICKOS_RAM_OWNER_SLOTS: 4\n")], True),
    (None, "xmc4800-relax.yaml", [], XMC_SHARE + [("  KICKOS_RAM_OWNER_SLOTS: 48\n", "  KICKOS_RAM_OWNER_SLOTS: 5\n")],
     False),
    (None, "xmc4800-relax.yaml", [NO_PROTECTION], UNENFORCED + [("  KICKOS_RAM_OWNER_SLOTS: 48\n", "  KICKOS_RAM_OWNER_SLOTS: 3\n")],
     False),
    ("supply.ranges", "qemu-arm64.yaml", [], [("  KICKOS_ASPACE_RANGES: 64\n", "  KICKOS_ASPACE_RANGES: 8\n")], True),
    (None, "qemu-arm64.yaml", [], [("  KICKOS_ASPACE_RANGES: 64\n", "  KICKOS_ASPACE_RANGES: 9\n")], False),
    # The init's space holds the partition's user share, seated in root before the init runs.
    (None, "qemu-arm64.yaml", ARM64_UNPINNED, [("  KICKOS_ASPACE_RANGES: 64\n", "  KICKOS_ASPACE_RANGES: 9\n")], False),
    ("supply.ranges", "qemu-arm64.yaml", ARM64_UNPINNED,
     ARM64_SHARE + [("  KICKOS_ASPACE_RANGES: 64\n", "  KICKOS_ASPACE_RANGES: 9\n")], True),
    (None, "qemu-arm64.yaml", ARM64_UNPINNED,
     ARM64_SHARE + [("  KICKOS_ASPACE_RANGES: 64\n", "  KICKOS_ASPACE_RANGES: 10\n")], False),
    ("supply.size", "qemu-arm64.yaml", [("    size: 64\n", "    size: 0xFFFFFFFF\n")], [], False),
    ("manifest.window", "xmc4800-relax.yaml", [], [("  window_rule: pow2\n  smallest_window: 32\n", "  window_rule: none\n")],
     True),
    ("form.range", "qemu-arm64.yaml", [("    size: 64\n", "    size: 0\n")], [], False),
    (None, "qemu-arm64.yaml", [("    size: 64\n", "    size: 1\n")], [], False),
    (None, "qemu-arm64.yaml", [("    size: 64\n", "    size: 0xFFFFF000\n")], [], False),
    ("scheduling.core", "qemu-arm64.yaml", [], [("  kernel_cores: 4\n", "  kernel_cores: 2\n")], True),
    (None, "qemu-arm64.yaml", [], [("  kernel_cores: 4\n", "  kernel_cores: 3\n")], False),
    ("scheduling.line-core", "qemu-arm64.yaml",
     [("    watches: [sensor]\n", "    watches: [sensor]\n    lines: { irq: /dev/gpio/global }\n")], [], False),
    (None, "qemu-arm64.yaml",
     [("    watches: [sensor]\n", "    watches: [sensor]\n    core: 3\n    lines: { irq: /dev/gpio/global }\n")], [], False),
]


# Minimal pairs on MANIFEST.
MANIFEST_ARMS = [
    ("form.unknown-field", [("version: 1\n", "version: 1\nkernel: 0.5.1\n")], False),
    ("form.unknown-field", [("  table: 6\n", "  table: 6\n  lookups: 1\n")], False),
    ("form.unknown-field", [("  arch: armv7m\n", "  arch: armv7m\n  fpu: true\n")], False),
    ("form.unknown-field", [("    nodes: 2\n", "    nodes: 2\n    peers: [1]\n")], False),
    ("form.unknown-field", [("  enforced: true\n", "  enforced: true\n  unit: pmsav7\n")], False),
    ("form.unknown-field", [("  KICKOS_MAX_THREADS: 8\n", "  KICKOS_MAX_THREADS: 8\n  KICKOS_HEAP_SIZE: 4096\n")], False),
    ("form.unknown-field", [("  KICKOS_MAX_THREADS: 8\n", "  KICKOS_MAX_THREADS: 8\n  KICKOS_MAX_THREAD_WINDOWS: 4\n")], False),
    ("form.unknown-field", [("  user_stack: 4096\n", "  user_stack: 4096\n  policies: [fifo]\n")], False),
    ("form.unknown-field", [("  board: platform/xmc4800/xmc4800-relax.yaml\n",
                             "  board: platform/xmc4800/xmc4800-relax.yaml\n  memory: arena\n")], False),
    ("form.unknown-field", [("    block: 1024\n", "    block: 1024\n    block_flags: 0\n")], False),
    ("form.unknown-field", [("{ name: bus, priority: 0, stack: default, caps: 3, badged: 0 }",
                             "{ name: bus, priority: 0, stack: default, caps: 3, badged: 0, core: 0 }")], False),
    ("form.missing", [("  arch: armv7m\n", "")], True),
    ("form.missing", [("pools:\n  KICKOS_MAX_THREADS: 8\n  KICKOS_TASK_ENDPOINT_BUDGET: 4\n"
                       "  KICKOS_MAX_SPAWN_GRANTS: 6\n  KICKOS_CAP_TABLE_SUPPLY: 16\n", "")], True),
    ("form.missing", [("    console: false\n", "")], True),
    ("form.missing", [("stack: default, caps: 3, badged: 0 }", "caps: 3, badged: 0 }")], False),
    ("form.missing", [("stack: default, caps: 3, badged: 0 }", "stack: default, caps: 3 }")], False),
    ("manifest.bound", [("caps: 2, badged: 1 }", "caps: 2, badged: 3 }")], False),
    (None, [("caps: 2, badged: 1 }", "caps: 2, badged: 2 }")], False),
    ("form.missing", [("stack: default, caps: 3, badged: 0 }", "stack: default, badged: 0 }")],
     False),
    ("form.missing", [("    receiver: bus\n", "")], True),
    ("manifest.receiver", [("    receiver: bus\n", "    receiver: irq\n")], False),
    ("form.missing", [("    client: [kickos_spi_proxy]\n", "")], True),
    ("form.name", [("client: [kickos_spi_proxy]", "client: [kickos-spi-proxy]")], False),
    ("form.duplicate-entry", [("client: [kickos_spi_proxy]", "client: [kickos_spi_proxy, kickos_spi_proxy]")], False),
    (None, [("client: [kickos_spi_proxy]", "client: [Kickos_spi_proxy, _kickos_spi_proxy]")], False),
    ("form.type", [("client: [kickos_spi_proxy]", "client: kickos_spi_proxy")], False),
    ("form.name", [("client: [kickos_spi_proxy]", 'client: [""]')], False),
    ("form.missing", [("  stack_stride: 4096\n", "")], True),
    ("form.missing", [('  symbol_prefix: ""\n', "")], True),
    ("form.name", [('  symbol_prefix: ""\n', "  symbol_prefix: x\n")], False),
    (None, [('  symbol_prefix: ""\n', "  symbol_prefix: _\n")], False),
    ("manifest.bound", [("  stack_stride: 4096\n", "  stack_stride: 3000\n")], False),
    (None, [("  stack_stride: 4096\n", "  stack_stride: none\n")], False),
    ("manifest.bound", [("  stack_align: 16\n", "  stack_align: 24\n")], False),
    ("manifest.bound", [("  idle_stack: 512\n", "  idle_stack: 0\n")], False),
    ("form.missing", [("  smallest_window: 32\n", "")], True),
    ("form.inapplicable", [("window_rule: pow2", "window_rule: none")], True),
    (("form.inapplicable", "form.missing"), [("  window_rule: pow2\n", "")], True),
    (None, [("  window_rule: pow2\n  smallest_window: 32\n", "  window_rule: none\n")], False),
    ("form.missing", [("  window_rule: pow2\n  smallest_window: 32\n", "")], True),
    ("form.missing", [("  enforced: true\n  window_rule: pow2\n  smallest_window: 32\n", "  enforced: false\n")], True),
    ("form.enum", [("window_rule: pow2", "window_rule: napot")], False),
    ("form.enum", [("posture: retain", "posture: publish")], False),
    ("form.version", [("version: 1\n", "version: 2\n")], False),
    ("form.version", [("  table: 6\n", "  table: 5\n")], False),
    ("manifest.block-cache", [("    block: none\n    block_cache: cached\n", "    block: none\n    block_cache: uncached\n")],
     False),
    (None, [("    block: 1024\n    block_cache: cached\n", "    block: 1024\n    block_cache: uncached\n")], False),
    ("form.enum", [("    block: 1024\n    block_cache: cached\n", "    block: 1024\n    block_cache: nocache\n")], False),
    ("form.missing", [("    start: xmc_spi0_start\n", "")], True),
    ("form.name", [("start: xmc_spi0_start", "start: xmc-spi0-start")], False),
    ("form.boolean", [("  enforced: true\n", "  enforced: yes\n")], False),
    ("form.type", [("    block: 1024\n", "    block: ring\n")], False),
    ("manifest.bound", [("    block: 1024\n", "    block: 1000\n")], False),
    ("manifest.bound", [("    block: 1024\n", "    block: 0\n")], False),
    (None, [("    block: 1024\n", "    block: 2048\n")], False),
    ("form.type", [("stack: default, caps: 2, badged: 0 }\n      - { name: service", "stack: big, caps: 2, badged: 0 }\n      - { name: service")],
     False),
    ("form.type", [("    barrier: 1\n", "    barrier: after\n")], False),
    ("form.type", [("  KICKOS_MAX_THREADS: 8\n", "  KICKOS_MAX_THREADS: eight\n")], False),
    ("form.type", [("  chip: platform/xmc4800/chip.yaml\n", "  chip: [platform/xmc4800/chip.yaml]\n")], False),
    ("form.integer", [("    endpoints: 1\n    notifications: 1\n    block: 1024\n",
                       "    endpoints: 1\n    notifications: 1\n    block: 1_024\n")], False),
    ("form.integer", [("{ name: uartirq, priority: 1,", "{ name: uartirq, priority: -1,")], False),
    ("form.range", [("  table: 6\n", "  table: 0x10000\n")], False),
    ("form.range", [("  thread_windows: 4\n", "  thread_windows: 256\n")], False),
    ("form.range", [("  isolated_cores: 0x0\n", "  isolated_cores: 0x100000000\n")], False),
    ("form.name", [("  board: xmc4800-relax\n", "  board: XMC4800\n")], False),
    ("form.name", [("    lines: [irq]\n    threads:\n      - { name: uartirq",
                    "    lines: [IRQ]\n    threads:\n      - { name: uartirq")], False),
    ("form.name", [("  xmcssc:\n", "  XmcSsc:\n")], False),
    (None, [("    block: 1024\n", "    block: none\n"), ("    barrier: 1\n", "    barrier: none\n")], False),
    (None, [("stack: default, caps: 2, badged: 0 }\n      - { name: service",
             "stack: 2048, caps: 2, badged: 0 }\n      - { name: service")], False),
    (None, [("    barrier: 1\n", "    barrier: 2\n")], False),
    (None, [(MANIFEST[MANIFEST.index("drivers:\n"):], "drivers: {}\n")], False),
    (None, [("descriptions:\n  chip: platform/xmc4800/chip.yaml\n  board: platform/xmc4800/xmc4800-relax.yaml\n",
             "")], False),
    (("manifest.cores", "manifest.amp"), [("  kernel_cores: 1\n", "  kernel_cores: 3\n")], False),
    (("manifest.cores", "manifest.amp"), [("  kernel_cores: 1\n", "  kernel_cores: 0\n")], False),
    ("manifest.amp", [("  kernel_cores: 1\n", "  kernel_cores: 2\n")], False),
    (None, [("  kernel_cores: 1\n", "  kernel_cores: 2\n"), (MANIFEST_AMP, "")], False),
    ("manifest.cores", [("  isolated_cores: 0x0\n", "  isolated_cores: 0x1\n")], False),
    ("manifest.cores", [("  isolated_cores: 0x0\n", "  isolated_cores: 0x2\n")], False),
    (None, [("  kernel_cores: 1\n  isolated_cores: 0x0\n", "  kernel_cores: 2\n  isolated_cores: 0x2\n"), (MANIFEST_AMP, "")],
     False),
    ("manifest.amp", [("    node: 0\n", "    node: 2\n")], False),
    ("manifest.amp", [("[[0, 2], [1, 3]]", "[[0, 2], [2, 3]]")], False),
    ("form.missing", [("    share: 0x4000\n", "")], True),
    ("form.range", [("    share: 0x4000\n", "    share: 0x100000000\n")], False),
    ("form.missing", [("    share_cache: uncached\n", "")], True),
    ("form.enum", [("    share_cache: uncached\n", "    share_cache: writeback\n")], False),
    ("manifest.window", [("  smallest_window: 32\n", "  smallest_window: 48\n")], False),
    ("manifest.window", [("  smallest_window: 32\n", "  smallest_window: 0\n")], False),
    (None, [("window_rule: pow2", "window_rule: granule")], False),
    ("manifest.priority", [("  priority: [1, 31]\n", "  priority: [0, 31]\n")], False),
    ("manifest.priority", [("  priority: [1, 31]\n", "  priority: [9, 8]\n")], False),
    ("manifest.bound", [("  status_record_size: 8\n", "  status_record_size: 0\n")], False),
    ("manifest.bound", [("  private_record_size: 64\n", "  private_record_size: 0\n")], False),
    ("form.missing", [("init:\n  status_record_size: 8\n  private_record_size: 64\n  free_regions: 5\n", "")],
     True),
    ("form.missing", [("  free_regions: 5\n", "")], True),
    ("form.range", [("  free_regions: 5\n", "  free_regions: 256\n")], False),
    (None, [("  status_record_size: 8\n", "  status_record_size: 0xFFFF\n")], False),
    ("form.missing", [("  status_record_size: 8\n", "")], True),
    ("form.missing", [("  private_record_size: 64\n", "")], True),
    ("form.unknown-field", [("  private_record_size: 64\n", "  private_record_size: 64\n  stack: 4096\n")], False),
    ("form.range", [("  status_record_size: 8\n", "  status_record_size: 0x10000\n")], False),
    ("manifest.barrier", [("    barrier: 1\n", "    barrier: 3\n")], False),
    ("manifest.barrier", [("    barrier: 1\n", "    barrier: 0\n")], False),
    ("manifest.barrier", [("    barrier: none\n", "    barrier: 1\n")], False),
    ("manifest.console", [("    console: false\n", "    console: true\n")], False),
    (None, [("    console: false\n", "    console: true\n"), ("posture: retain", "posture: handover")], False),
    (None, [("  enforced: true\n", "  enforced: false\n")], False),
    (None, [("  enforced: true\n", "  enforced: false\n"), ("window_rule: pow2", "window_rule: granule")], False),
    (None, [("  enforced: true\n  window_rule: pow2\n  smallest_window: 32\n",
             "  enforced: false\n  window_rule: none\n")], False),
    ("manifest.bound", [("  thread_windows: 4\n", "  thread_windows: 0\n")], False),
    ("manifest.bound", [("  min_stack: 960\n", "  min_stack: 0\n")], False),
    ("manifest.bound", [("  user_stack: 4096\n", "  user_stack: 0\n")], False),
    (None, [("    lines: [irq]\n    threads:\n      - { name: bus",
             "    lines: [\"off\", \"no\"]\n    threads:\n      - { name: bus")], False),
    ("form.duplicate-entry", [("    lines: [irq]\n    threads:\n      - { name: uartirq",
                               "    lines: [irq, irq]\n    threads:\n      - { name: uartirq")], False),
    ("form.duplicate-entry", [("{ name: uartirq, priority: 1,", "{ name: service, priority: 1,")], True),
    ("manifest.description-path", [("  chip: platform/xmc4800/chip.yaml\n", "  chip: platform/stm32f411/chip.yaml\n")], False),
    ("manifest.description-path", [("  board: platform/xmc4800/xmc4800-relax.yaml\n",
                                    "  board: ../platform/xmc4800/xmc4800-relax.yaml\n")], False),
    ("manifest.description-path", [("  chip: xmc4800\n", "")], True),
    ("manifest.description-unknown", [("  board: xmc4800-relax\n", "  board: xmc4800-kit\n"),
                                      ("  board: platform/xmc4800/xmc4800-relax.yaml\n",
                                       "  board: platform/xmc4800/xmc4800-kit.yaml\n")], True),
    (None, [MANIFEST_DEFAULT], False),
    ("manifest.default-path", [(MANIFEST_DEFAULT[0], MANIFEST_DEFAULT[1].replace("xmc4800-relax", "f411disco"))], False),
    ("manifest.default-path", [("descriptions:\n  chip: platform/xmc4800/chip.yaml\n"
                                "  board: platform/xmc4800/xmc4800-relax.yaml\n", ""), MANIFEST_DEFAULT], False),
    ("form.missing", [(MANIFEST_DEFAULT[0], "default: {}\n" + MANIFEST_DEFAULT[0])], False),
]


def arm_fault(rules, refusals, lines, anywhere):
    """Why the refusals do not satisfy the arm, or None when they do."""
    if rules is None:
        if refusals:
            return "a mutation the rules admit was refused"
        return None
    if isinstance(rules, str):
        rules = (rules,)
    if not refusals:
        return "nothing refused"
    found = sorted(set(r.rule for r in refusals))
    if found != sorted(rules):
        return "refused under %s" % ", ".join(found)
    if not anywhere:
        for refusal in refusals:
            if refusal.line not in lines:
                return "refused on line %d, off the mutation" % refusal.line
    return None


def exact_fault(expected, refusals):
    """`expected` holds (path, line, rule), or (path, line, rule, text the message must carry)."""
    found = sorted((r.path, r.line, r.rule) for r in refusals)
    if found != sorted(entry[:3] for entry in expected):
        return "refused as %s" % found
    for entry in expected:
        if len(entry) == 4 and not any((r.path, r.line, r.rule) == entry[:3] and entry[3] in r.message
                                       for r in refusals):
            return "the %s refusal on line %d does not name %s" % (entry[2], entry[1], entry[3])
    return None


def read(path):
    with open(path, encoding="utf-8") as stream:
        return stream.read()


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as stream:
        stream.write(text)


def line_starting(path, prefix):
    starts = [n + 1 for n, line in enumerate(read(path).split("\n")) if line.startswith(prefix)]
    if len(starts) != 1:
        raise AssertionError("%d lines of %s begin with %r" % (len(starts), path, prefix))
    return starts[0]


def no_document(root):
    board = os.path.join(root, "stm32f411", "f411disco.yaml")
    write(board, "# a board yet to be described\n")
    return run([board]), [(board, 1, "form.documents")]


def stray_file(root):
    stray = os.path.join(root, "stm32f411", "notes.txt")
    write(stray, "a board note\n")
    return run([root]), [(stray, 1, "form.layout")]


def nested_chip(root):
    nested = os.path.join(root, "stm32f411", "b", "chip.yaml")
    write(nested, read(os.path.join(root, "stm32f411", "chip.yaml")).replace("chip: stm32f411", "chip: b"))
    return run([nested]), [(nested, 1, "form.layout")]


def deep_board(root):
    deep = os.path.join(root, "stm32f411", "boards", "f411disco.yaml")
    write(deep, read(os.path.join(root, "stm32f411", "f411disco.yaml")))
    return run([root]), [(deep, 1, "form.layout")]


def missing_path(root):
    missing = os.path.join(root, "stm32f411", "absent.yaml")
    return run([missing]), [(missing, 1, "form.layout")]


def unreadable_file(root):
    chip = os.path.join(root, "stm32f411", "chip.yaml")
    with open(chip, "wb") as stream:
        stream.write(b"chip: \xff\n")
    return run([chip]), [(chip, 1, "form.unreadable")]


def unreadable_chip(root):
    with open(os.path.join(root, "stm32f411", "chip.yaml"), "wb") as stream:
        stream.write(b"chip: \xff\n")
    board = os.path.join(root, "stm32f411", "f411disco.yaml")
    return run([board]), [(board, line_starting(board, "chip: "), "board.chip-unreadable")]


def chip_folder(root):
    moved = os.path.join(root, "mk64f", "f411disco.yaml")
    shutil.move(os.path.join(root, "stm32f411", "f411disco.yaml"), moved)
    return run([moved]), [(moved, line_starting(moved, "chip: "), "board.chip-folder")]


def board_collision(root):
    twin = os.path.join(root, "mk64f", "f411disco.yaml")
    write(twin, read(os.path.join(root, "mk64f", "frdmk64f.yaml")).replace("board: frdmk64f", "board: f411disco"))
    board = os.path.join(root, "stm32f411", "f411disco.yaml")
    return run([twin, board]), [(board, line_starting(board, "board: "), "board.name-collision")]


def board_alone_broken_chip(root):
    chip = os.path.join(root, "stm32f411", "chip.yaml")
    write(chip, read(chip) + "extra: 1\n")
    board = os.path.join(root, "stm32f411", "f411disco.yaml")
    return run([board]), [(chip, line_starting(chip, "extra: "), "form.unknown-field")]


def console_on_instance(root):
    board = os.path.join(root, "stm32f411", "f411disco.yaml")
    write(board, read(board).replace("device: /dev/usart2", "device: /dev/gpio/0"))
    line = line_starting(board, "  pins: ")
    return run([os.path.join(root, "stm32f411", "chip.yaml"), board]), [
        (board, line, "board.pin-function"), (board, line, "board.pin-function")]


def without_gpio(chip, edits, board, prefix, pin=None):
    """A pin the chip lists without `gpio`, named where the board needs one, the refusal naming
    `pin` where given."""
    def scenario(root):
        chip_path = os.path.join(root, chip)
        board_path = os.path.join(root, board)
        write(chip_path, mutate(read(chip_path), edits)[0])
        expected = (board_path, line_starting(board_path, prefix), "board.pin-not-gpio")
        if pin is not None:
            expected += ("`%s`" % pin,)
        return run([chip_path, board_path]), [expected]
    return scenario


def systems_of(root):
    return os.path.join(os.path.dirname(root), "systems")


def golden(root, name):
    return os.path.join(systems_of(root), name)


def absent_composition(root):
    path = golden(root, "absent.yaml")
    return run_admit([path], root), [(path, 1, "form.layout")]


def unreadable_composition(root):
    path = golden(root, "qemu-arm64.yaml")
    with open(path, "wb") as stream:
        stream.write(b"board: \xff\n")
    return run_admit([path], root), [(path, 1, "form.unreadable")]


def unreadable_board(root):
    board = os.path.join(root, "virt_arm64", "qemu-arm64.yaml")
    with open(board, "wb") as stream:
        stream.write(b"board: \xff\n")
    return run_admit([golden(root, "qemu-arm64.yaml")], root), [(board, 1, "form.unreadable")]


def ambiguous_board(root):
    shutil.copy(os.path.join(root, "virt_arm64", "qemu-arm64.yaml"), os.path.join(root, "q35", "qemu-arm64.yaml"))
    path = golden(root, "qemu-arm64.yaml")
    return run_admit([path], root), [(path, line_starting(path, "board: "), "name.board-ambiguous")]


def broken_board(root):
    board = os.path.join(root, "virt_arm64", "qemu-arm64.yaml")
    write(board, read(board) + "colour: green\n")
    return run_admit([golden(root, "qemu-arm64.yaml")], root), [
        (board, line_starting(board, "colour: "), "form.unknown-field")]


IMX_BOARD = """version: 1
board: imx8mp-evk
chip: imx8mp
console:
  device: /dev/uart1
"""

IMX_SYSTEM = """version: 1
board: imx8mp-evk
cluster: a53
stdout: kernel
ends: never
heap: 0
tasks:
  - name: audio
    entry: audio_main
    stack: 4096
    priority: 9
    ceiling: 9
    devices: [/dev/sai5, /dev/sai6]
    lines: { irq: /dev/sai5/shared }
"""

K64F_SYSTEM = """version: 1
board: frdmk64f
stdout: kernel
ends: never
heap: 0
tasks:
  - name: spi
    entry: spi_main
    stack: 2048
    priority: 9
    ceiling: 9
    devices: [/dev/dspi0]
    accepts: [device_not_isolated, coarse_gate]
  - name: leds
    entry: leds_main
    stack: 1024
    priority: 8
    ceiling: 8
    devices: [/dev/gpio/1] # leds
    accepts: [device_not_isolated, coarse_gate]
"""

C6_SYSTEM = """version: 1
board: esp32c6-wroom
stdout: kernel
ends: never
heap: 0
accepts: [no_protection, no_privilege_split]
shared:
  - name: /shm/state
    size: 16
    cache: uncached
tasks:
  - name: blink
    entry: blink_main
    stack: 1024
    priority: 9
    ceiling: 9
    devices: [/dev/gpio]
    maps: { /shm/state: rw }
"""

RV64_SYSTEM = """version: 1
board: qemu-riscv64
stdout: kernel
ends: never
heap: 0
shared:
  - name: /shm/state
    size: 16
    cache: cached
tasks:
  - name: clock
    entry: clock_main
    stack: 4096
    priority: 9
    ceiling: 9
    devices: [/dev/rtc]
    maps: { /shm/state: rw }
"""


def composed(name, text, edits, expect, board=None, platform_edits=(), names=None, manifest=None):
    """A composition written beside the golden ones, and admitted against `manifest` when one is
    given. `expect` names each refusal by the line it begins and its rule, as (prefix, rule) in the
    composition or (platform file, prefix, rule), and `names` is text every refusal's message
    carries."""
    def scenario(root):
        if board is not None:
            write(os.path.join(root, board[0]), board[1])
        for target, target_edits in platform_edits:
            target_path = os.path.join(root, target)
            write(target_path, mutate(read(target_path), target_edits)[0])
        path = golden(root, name)
        write(path, mutate(text, edits)[0])
        expected = []
        for entry in expect:
            where = path
            if len(entry) == 3:
                where = os.path.join(root, entry[0])
            found = (where, line_starting(where, entry[-2]), entry[-1])
            if names is not None:
                found = found + (names,)
            expected.append(found)
        manifest_path = None
        if manifest is not None:
            manifest_path = os.path.join(os.path.dirname(root), "manifest.yaml")
            write(manifest_path, manifest)
        return run_admit([path], root, manifest_path), expected
    scenario.__name__ = "composed_%s" % name
    return scenario


def on_imx(edits, expect, manifest=None):
    return composed("imx8mp-evk.yaml", IMX_SYSTEM, edits, expect, ("imx8mp/imx8mp-evk.yaml", IMX_BOARD), manifest=manifest)


def on_k64f(edits, expect, chip_edits=(), names=None, manifest=None):
    platform_edits = ()
    if chip_edits:
        platform_edits = (("mk64f/chip.yaml", chip_edits),)
    return composed("frdmk64f.yaml", K64F_SYSTEM, edits, expect, platform_edits=platform_edits, names=names,
                    manifest=manifest)


def on_arm64(edits, expect, chip_edits, manifest=None):
    return composed("qemu-arm64.yaml", read(os.path.join(SYSTEMS, "qemu-arm64.yaml")), edits, expect,
                    platform_edits=(("virt_arm64/chip.yaml", chip_edits),), manifest=manifest)


# The C6 golden naming no board and leaving its heap, accepts and stack to the board's default.
BOARD_LESS_C6 = [("board: esp32c6-wroom\n", ""), ("heap: 0\n", ""), ("accepts: [no_protection, no_privilege_split]\n", ""),
                 ("    stack: 1024\n", "")]


def defaulted(manifest, board):
    """`manifest` naming `board`'s default composition."""
    return mutate(manifest, [(MANIFEST_DEFAULT[0], MANIFEST_DEFAULT[1].replace("xmc4800-relax", board))])[0]


def on_c6(edits, expect, chip_edits=(), names=None, manifest=C6_MANIFEST):
    platform_edits = ()
    if chip_edits:
        platform_edits = (("esp32c6/chip.yaml", chip_edits),)
    return composed("esp32c6-wroom.yaml", C6_SYSTEM, edits, expect, platform_edits=platform_edits, names=names,
                    manifest=manifest)


C6BLINK = read(os.path.join(TREE, "user", "apps", "esp32c6-wroom", "c6blink", "system.yaml"))
C6BLINK_COARSE = ("    authority: [pinmux]\n", "    authority: [pinmux]\n    accepts: [coarse_gate]\n")


def c6blink(edits, expect, names=None):
    """c6blink's own composition over the C6 board's default."""
    return composed("esp32c6-wroom.yaml", C6BLINK, edits, expect, names=names,
                    manifest=defaulted(C6_MANIFEST, "esp32c6-wroom"))


ARM64_ALONE = """version: 1
board: qemu-arm64
stdout: kernel
ends: never
heap: 0
tasks:
    - name: probe
      entry: probe_main
      stack: 4096
      priority: 9
      ceiling: 9
      devices: [/dev/rtc, /dev/gpio, /dev/virtio/31, /dev/virtio/23]
      accepts: [bus_master, coarse_gate]
"""


ARM64_WATCHER = """version: 1
board: qemu-arm64
stdout: kernel
ends: never
heap: 0
shared:
  - name: /shm/state
    size: 64
    cache: uncached
tasks:
    - name: probe
      entry: probe_main
      stack: 4096
      priority: 9
      ceiling: 9
    - name: watcher
      entry: watcher_main
      stack: 4096
      priority: 8
      ceiling: 8
      devices: [/dev/rtc, /dev/gpio, /dev/virtio/31, /dev/virtio/23]
      accepts: [bus_master, coarse_gate]
      watches: [probe]
      maps: { /shm/state: rw }
"""

ARM64_DRIVER = """version: 1
board: qemu-arm64
stdout: kernel
ends: never
heap: 0
tasks:
    - name: spi
      driver: xmcssc
      devices: [/dev/gpio]
      lines: { irq: /dev/gpio/global }
      serves: /svc/spi
      priority: 11
      core: 1
"""
# xmcssc with three threads and a ring block: its space holds seven ranges, the init's six.
SSC_WIDE = [("      - { name: bus, priority: 0, stack: default, caps: 3, badged: 0 }\n",
             "      - { name: bus, priority: 0, stack: default, caps: 3, badged: 0 }\n"
             "      - { name: rx, priority: 0, stack: default, caps: 1, badged: 0 }\n"
             "      - { name: tx, priority: 0, stack: default, caps: 1, badged: 0 }\n"),
            ("    notifications: 1\n    block: none\n", "    notifications: 1\n    block: 1024\n")]


def on_arm64_watcher(ranges, expect):
    """A watcher whose space holds nine ranges, its image's two, its stack, four devices, its map
    and its /init/status, while the init's holds eight."""
    manifest = mutate(MANIFESTS["qemu-arm64.yaml"],
                      [("  KICKOS_ASPACE_RANGES: 64\n", "  KICKOS_ASPACE_RANGES: %d\n" % ranges),
                       ("  thread_windows: 4\n", "  thread_windows: 6\n")])[0]
    return composed("qemu-arm64.yaml", ARM64_WATCHER, [], expect, manifest=manifest)


def on_arm64_driver(ranges, expect):
    manifest = mutate(MANIFESTS["qemu-arm64.yaml"],
                      SSC_WIDE + [("  KICKOS_ASPACE_RANGES: 64\n", "  KICKOS_ASPACE_RANGES: %d\n" % ranges)])[0]
    return composed("qemu-arm64.yaml", ARM64_DRIVER, [], expect, manifest=manifest)


def on_arm64_alone(ranges, expect):
    """One task whose space holds seven ranges, its image's two, its stack and four windows, while
    the init's holds six, its image's two, its stack, its two blocks and the task's stack."""
    manifest = MANIFESTS["qemu-arm64.yaml"].replace("  KICKOS_ASPACE_RANGES: 64\n", "  KICKOS_ASPACE_RANGES: %d\n" % ranges)
    return composed("qemu-arm64.yaml", ARM64_ALONE, [], expect, manifest=manifest)


def on_rv64(edits, expect):
    return composed("qemu-riscv64.yaml", RV64_SYSTEM, edits, expect, manifest=RV64_MANIFEST)


def on_xmc(edits, expect, chip_edits, manifest, names=None):
    return composed("xmc4800-relax.yaml", read(os.path.join(SYSTEMS, "xmc4800-relax.yaml")), edits, expect,
                    platform_edits=(("xmc4800/chip.yaml", chip_edits),), manifest=manifest, names=names)


K64F_SCRATCH = [("  dspi0: { window: [0x4002C000, 0x40],",
                 "  scratch: { window: [0x4002C800, 0x40] }\n  dspi0: { window: [0x4002C000, 0x40],")]
K64F_BESIDE_PIT = [("  dspi0: { window: [0x4002C000, 0x40],",
                    "  scratch: { window: [0x40037800, 0x40] }\n  dspi0: { window: [0x4002C000, 0x40],")]
K64F_UNRANGED = [("    ranges: [[0x40000000, 0x80000], [0x40080000, 0x7F000]]\n", "")]
K64F_LEDS = "# leds\n    accepts: [device_not_isolated, coarse_gate]\n"
K64F_LEDS_ISOLATED = (K64F_LEDS, "# leds\n    accepts: [device_not_isolated]\n")
# A packaged driver with an uncached ring block and nothing else, and a K64F task running it.
BLOCK_DRIVER = ("drivers:\n", "drivers:\n  blkdrv:\n    windows: []\n    lines: []\n    threads:\n"
                "      - { name: io, priority: 0, stack: default, caps: 1, badged: 0 }\n    endpoints: 1\n"
                "    notifications: 0\n    block: 1024\n    block_cache: uncached\n    posture: retain\n"
                "    barrier: 1\n    console: false\n    start: blkdrv_start\n    receiver: io\n    client: []\n")
K64F_BLOCK_TASK = ("tasks:\n", "tasks:\n  - name: blk\n    driver: blkdrv\n    serves: /svc/blk\n    priority: 9\n")
K64F_SHARED = ("stdout: kernel\n", "stdout: kernel\nshared:\n  - name: /shm/state\n    size: 16\n    cache: uncached\n")
K64F_CACHED = [("\ndata_cache: false\n", "\n")]
ARM64_SCRATCH = [("  gpio: { window", "  scratch: { window: [0x09020000, 0x100] }\n  gpio: { window")]
ARM64_UNCACHED = [("cores: { count", "data_cache: false\ncores: { count")]
C6_BANK = ("    maps: { /shm/state: rw }\n", "    maps: { /shm/state: rw }\n    accepts: [coarse_gate]\n")
K64F_UNENFORCED = mutate(K64F_MANIFEST, UNENFORCED)[0]
XMC_UNENFORCED = mutate(XMC_MANIFEST, UNENFORCED)[0]
# A region build whose seam states no unit: the kernel rounds and encodes at 16 bytes.
NO_UNIT = UNENFORCED + [("  smallest_window: 32\n", "  smallest_window: 16\n")]
K64F_NO_UNIT = mutate(K64F_MANIFEST, NO_UNIT)[0]
XMC_NO_UNIT = mutate(XMC_MANIFEST, NO_UNIT + [("window_rule: pow2", "window_rule: granule"),
                                              ("  stack_stride: 4096\n", "  stack_stride: none\n")])[0]
K64F_UNGUARDED = [("ends: never\n", "ends: never\naccepts: [no_protection]\n"),
                  ("[/dev/dspi0]\n    accepts: [device_not_isolated, coarse_gate]", "[/dev/dspi0]")]
XMC_ODD_REGION = [NO_PROTECTION, ("    size: 64\n", "    size: 0x41\n")]
# A heap past the arena: the link places it, below the arena.
HEAP = ("heap: 16384\n", "heap: 0x18000\n")
C6_FREE = C6_MANIFEST.replace("  free_regions: 5\n", "  free_regions: %d\n")
# Masked stacks of a user stack the stride rounds up: each block is the user stack's size on a
# stride's alignment.
XMC_ODD_STACKS = mutate(XMC_MANIFEST, [("window_rule: pow2", "window_rule: granule"),
                                       ("  user_stack: 4096\n", "  user_stack: 0x1100\n"),
                                       ("  stack_stride: 4096\n", "  stack_stride: 0x2000\n")])[0]
C6_OWNER_SLOTS = C6_MANIFEST.replace("  KICKOS_RAM_OWNER_SLOTS: 48\n", "  KICKOS_RAM_OWNER_SLOTS: %d\n")
# Two watchers of blink, each reserving and self-granting a status block of its own.
C6_WATCHERS = [("    maps: { /shm/state: rw }\n",
                "    maps: { /shm/state: rw }\n"
                "  - name: first\n    entry: first_main\n    stack: 1024\n    priority: 8\n    ceiling: 8\n    watches: [blink]\n"
                "  - name: second\n    entry: second_main\n    stack: 1024\n    priority: 8\n    ceiling: 8\n    watches: [blink]\n")]
LP_PROTECTED = [("{ unit: none, privilege: false }", "{ unit: pmp, covers_devices: true, memory_type: false }")]


def diagnostic_app(name, grants, expect):
    """The golden XMC system plus one task granting what the app `name` grants, which the SPI
    bus service's task already holds."""
    def scenario(root):
        path = golden(root, "xmc4800-relax.yaml")
        text = read(path)
        task = "\n  - name: %s\n    entry: %s_main\n    stack: 2048\n    priority: 10\n    ceiling: 10\n%s" % (name, name, grants)
        write(path, text + task)
        first = text.count("\n") + 1
        expected = []
        for n, line in enumerate(task.split("\n")):
            for prefix, rule in expect:
                if line.startswith(prefix):
                    expected.append((path, first + n, rule, "task `spi0`"))
        return run_admit([path], root), expected
    scenario.__name__ = "diagnostic_app_%s" % name
    return scenario


def manifest_beside(root):
    path = os.path.join(os.path.dirname(root), "manifest.yaml")
    write(path, MANIFEST)
    return path


def unreadable_manifest(root):
    path = manifest_beside(root)
    with open(path, "wb") as stream:
        stream.write(b"version: \xff\n")
    return run_manifest([path]), [(path, 1, "form.unreadable")]


def manifest_broken_board(root):
    board = os.path.join(root, "xmc4800", "xmc4800-relax.yaml")
    write(board, read(board) + "colour: green\n")
    return run_manifest([manifest_beside(root)]), [(board, line_starting(board, "colour: "), "form.unknown-field")]


def manifest_missing_default(root):
    shutil.rmtree(os.path.join(os.path.dirname(root), "boards"))
    path = os.path.join(os.path.dirname(root), "manifest.yaml")
    write(path, mutate(MANIFEST, [MANIFEST_DEFAULT])[0])
    return run_manifest([path]), [(path, line_starting(path, "  composition: "), "manifest.default-unknown")]


def manifest_missing_chip(root):
    os.remove(os.path.join(root, "xmc4800", "chip.yaml"))
    path = manifest_beside(root)
    return run_manifest([path]), [(path, line_starting(path, "  chip: platform/"), "manifest.description-unknown")]


def edited_platform_beside(root):
    """A platform tree that would refuse, passed beside a manifest whose own descriptions admit."""
    manifest_path = os.path.join(os.path.dirname(root), "manifest.yaml")
    write(manifest_path, XMC_MANIFEST)
    edited = os.path.join(os.path.dirname(root), "edited")
    shutil.copytree(root, edited)
    chip = os.path.join(edited, "xmc4800", "chip.yaml")
    write(chip, mutate(read(chip), [(XMC_ARENA, XMC_ARENA.replace("size: 0x20000", "size: 0x8000"))])[0])
    return run_admit([golden(root, "xmc4800-relax.yaml")], edited, manifest_path), []


def table_limits(counts, fields, strings, names):
    """The XMC golden's table against narrower limits than the format's: every row past them is
    refused, each naming its row."""
    def scenario(root):
        path = golden(root, "xmc4800-relax.yaml")
        manifest_path = os.path.join(os.path.dirname(root), "manifest.yaml")
        write(manifest_path, MANIFESTS["xmc4800-relax.yaml"])
        with mock.patch.object(emit, "COUNT_LIMIT", counts), mock.patch.object(emit, "FIELD_LIMIT", fields), \
                mock.patch.object(emit, "STRINGS_LIMIT", strings):
            report, table = emit.table_of(path, manifest_path)
        line = line_starting(path, "version: ")
        return report.refusals, [(path, line, "encoding.table", name) for name in names]
    scenario.__name__ = "table_limits_%d_%d_%d" % (counts, fields, strings)
    return scenario


U0C1 = "    devices: [/dev/usic0/ch1]\n"
VIRTIO_WIDE = [("    count: 32\n", "    count: 0xFFFF\n")]


def amp_of(text, kernel_cores, ports, share, cache, slices, window):
    """`text`, a manifest, as node 0's build of a partition of two writes it."""
    return mutate(text, [("  kernel_cores: %d\n  isolated_cores: 0x0\n" % kernel_cores,
                          "  kernel_cores: 1\n  isolated_cores: 0x0\n  amp:\n    node: 0\n    nodes: 2\n"
                          "    ports: %s\n    share: %s\n    share_cache: %s\n    slices: %s\n    window: %s\n"
                          % (ports, share, cache, slices, window))])[0]


ARM64_AMP = amp_of(MANIFESTS["qemu-arm64.yaml"], 4, "[[0, 2], [1, 3], [1, 4]]", "0x200000", "uncached",
                   "[0x40000000, 0x4000000]", "[0x48000000, 0x400000]")
C6_AMP = amp_of(C6_MANIFEST, 1, "[[0, 2], [1, 3]]", "0x4000", "cached", "[0x40800000, 0x3C000]",
                "[0x40878000, 0x8000]")
RP_MANIFEST = manifest_of("pizero2350", "rp2350", "armv7m", XMC_PROTECTION, 1, REGION_POOLS,
                          (960, 8192, 512, 8192, "none"))
RP_AMP = amp_of(RP_MANIFEST, 1, "[[0, 2], [1, 3]]", "0x4000", "cached", "[0x20000000, 0x3C000]",
                "[0x20078000, 0x8000]")
IMX_AMP = amp_of(manifest_of("imx8mp-evk", "imx8mp", "armv8a", XMC_PROTECTION.replace("enforced: true", "enforced: false"),
                             1, ARM64_POOLS, (2816, 12288, 4096, 20480, "none")),
                 1, "[[0, 2], [1, 3]]", "0x10000", "cached", "[0x40000000, 0x4000000]", "[0x48000000, 0x20000]")

PING = """version: 1
board: %s
stdout: kernel
ends: main
heap: 0
%sshared:
  - name: /shm/book
    size: 64
    cache: %s
    partition: true
tasks:
  - name: main
    entry: ping_main
    stack: %d
    priority: 9
    ceiling: 9
    uses: [/amp/3]
    maps: { /shm/book: rw }
"""

PONG = """version: 1
board: %s
stdout: kernel
ends: never
heap: 0
%sshared:
  - name: /shm/book
    size: 64
    cache: %s
    partition: true
tasks:
  - name: serve
    entry: serve_main
    stack: %d
    priority: 9
    ceiling: 9
    serves: /amp/3
    maps: { /shm/book: rw }
"""

C6_ACCEPTS = "accepts: [no_protection, no_privilege_split]\n"
ARM64_PAIR = (PING % ("qemu-arm64", "", "uncached", 8192), PONG % ("qemu-arm64", "", "uncached", 8192))
C6_PAIR = (PING % ("esp32c6-wroom", C6_ACCEPTS, "cached", 1024), PONG % ("esp32c6-wroom", C6_ACCEPTS, "cached", 1024))
RP_PAIR = (PING % ("pizero2350", "", "cached", 1024), PONG % ("pizero2350", "", "cached", 1024))
IMX_PAIR = (PING % ("imx8mp-evk", "cluster: a53\naccepts: [no_protection]\n", "cached", 8192),
            PONG % ("imx8mp-evk", "cluster: m7\naccepts: [no_protection]\n", "cached", 8192))
IMX_ACCEPT = ("accepts: [no_protection]", "accepts: [no_protection, cached_incoherent]")
SPARE = ("    maps: { /shm/book: rw }\n", "    maps: { /shm/book: rw }\n  - name: spare\n    entry: spare_main\n"
         "    stack: 8192\n    priority: 8\n    ceiling: 8\n    serves:  /amp/3\n")
C6_HP_SMALL = [("regions: { value: 16,", "regions: { value: 2,")]
# A node composition with neither crossing nor partition region.
LONE = [("shared:\n  - name: /shm/book\n    size: 64\n    cache: cached\n    partition: true\n", ""),
        ("    uses: [/amp/3]\n", ""), ("    maps: { /shm/book: rw }\n", "")]


def partitioned(name, texts, edits, expect, manifest, platform_edits=(), paths=None):
    """Node compositions written beside the golden ones and admitted together against `manifest`,
    node 0's. `edits` maps a node to its (old, new) list; `expect` names each refusal as (node, the
    prefix of its line, rule), a node of None naming the manifest's first line."""
    def scenario(root):
        for target, target_edits in platform_edits:
            target_path = os.path.join(root, target)
            write(target_path, mutate(read(target_path), target_edits)[0])
        written = []
        for k, text in enumerate(texts):
            path = golden(root, "%s-node%d.yaml" % (name, k))
            write(path, mutate(text, edits.get(k, []))[0])
            written.append(path)
        manifest_path = os.path.join(os.path.dirname(root), "manifest.yaml")
        write(manifest_path, manifest)
        expected = []
        for k, prefix, rule in expect:
            if k is None:
                expected.append((manifest_path, 1, rule))
            else:
                expected.append((written[k], line_starting(written[k], prefix), rule))
        if paths is not None:
            return paths(written, manifest_path), expected
        report, found = partition.admit_partition(written, manifest_path, 0)
        return report.refusals, expected
    scenario.__name__ = "partitioned_%s" % name
    return scenario


def single_emit(written, manifest_path):
    report, texts = emit.emit_system(written[0], manifest_path)
    return report.refusals


def gate_emit(written, manifest_path):
    report, text = emit.emit_gate(manifest_path)
    return report.refusals


def three_nodes(written, manifest_path):
    report, found = partition.admit_partition(written + [written[1]], manifest_path, 0)
    return report.refusals


SCENARIOS = [
    ("form.unreadable", unreadable_manifest),
    ("form.unknown-field", manifest_broken_board),
    ("manifest.description-unknown", manifest_missing_chip),
    ("manifest.default-unknown", manifest_missing_default),
    ("form.documents", no_document),
    ("form.layout", stray_file),
    ("form.layout", nested_chip),
    ("form.layout", deep_board),
    ("form.layout", missing_path),
    ("form.unreadable", unreadable_file),
    ("board.chip-unreadable", unreadable_chip),
    ("board.chip-folder", chip_folder),
    ("board.name-collision", board_collision),
    ("form.unknown-field", board_alone_broken_chip),
    ("board.pin-function", console_on_instance),
    ("board.pin-not-gpio", without_gpio("stm32f411/chip.yaml", [("PD12: { gpio: gpio.3.12 }", "PD12: {}")],
                                        "stm32f411/f411disco.yaml", "  ld4: ")),
    ("board.pin-not-gpio", without_gpio("stm32f411/chip.yaml", [("PE3: { gpio: gpio.4.3 }", "PE3: {}")],
                                        "stm32f411/f411disco.yaml", "    chip_select: ")),
    ("board.pin-not-gpio", without_gpio("mk64f/chip.yaml", [("PTC4: { gpio: gpio.2.4 }", "PTC4: {}")],
                                        "mk64f/frdmk64f.yaml", "    chip_selects: ")),
    ("board.pin-not-gpio", without_gpio("stm32f411/chip.yaml", [("PA2: { af7: usart2.tx, gpio: gpio.0.2 }",
                                                                 "PA2: { af7: usart2.tx }")],
                                        "stm32f411/f411disco.yaml", "  pins: { tx", "PA2")),
    ("board.pin-not-gpio", without_gpio("esp32/chip.yaml", [("GPIO6: { gpio: gpio.6 }", "GPIO6: {}")],
                                        "esp32/esp32-wroom.yaml", "  GPIO6: ", "GPIO6")),
    ("form.layout", absent_composition),
    ("form.unreadable", unreadable_composition),
    ("form.unreadable", unreadable_board),
    ("name.board-ambiguous", ambiguous_board),
    ("form.unknown-field", broken_board),
    (None, on_imx([], [])),
    ("form.missing", on_imx([("cluster: a53\n", "")], [("version: ", "form.missing")])),
    ("name.cluster-unknown", on_imx([("cluster: a53", "cluster: m4")], [("cluster: ", "name.cluster-unknown")])),
    ("name.device-unknown", on_imx([("[/dev/sai5, /dev/sai6]", "[/dev/sai5, /dev/sai6, /dev/mu1_b]")],
                                   [("    devices: ", "name.device-unknown")])),
    ("ownership.kernel", on_imx([("[/dev/sai5, /dev/sai6]", "[/dev/sai5, /dev/sai6, /dev/mu1_a]")],
                                [("    devices: ", "ownership.kernel")])),
    ("ownership.line", on_imx([("    lines: { irq: /dev/sai5/shared }\n",
                                "    lines: { irq: /dev/sai5/shared }\n  - name: echo\n    entry: echo_main\n"
                                "    stack: 4096\n    priority: 8\n    ceiling: 8\n    lines: { irq: /dev/sai6/shared }\n")],
                              [("    lines: { irq: /dev/sai6", "ownership.line")])),
    (None, on_k64f([], [])),
    (None, on_k64f([("[/dev/dspi0]", "[/dev/gpio/0]")], [])),
    ("ownership.gate", on_k64f([("[/dev/gpio/1] # leds", "[/dev/scratch] # leds")],
                               [("    devices: [/dev/scratch]", "ownership.gate")], K64F_SCRATCH)),
    ("ownership.gate", on_k64f([("[/dev/dspi0]", "[/dev/gpio/0]")], [("    devices: [/dev/gpio/1]", "ownership.gate")],
                               K64F_UNRANGED)),
    ("ownership.gate", on_k64f([("[/dev/gpio/1] # leds", "[/dev/scratch] # leds")],
                               [("    devices: [/dev/scratch]", "ownership.gate")], K64F_BESIDE_PIT, "`pit`")),
    ("ownership.kernel", on_arm64([("devices: [/dev/rtc]", "devices: [/dev/rtc]\n    lines: { alarm: /dev/rtc/alarm }")],
                                  [("    lines: { alarm", "ownership.kernel")],
                                  [("lines: { alarm: 34 }", "lines: { alarm: 30 }")])),
    ("form.name", on_arm64([], [("virt_arm64/chip.yaml", "  Rtc: ", "form.name")],
                           [("  rtc: { window", "  Rtc: { window")])),
    ("ownership.device", on_k64f([("[/dev/dspi0]", "[/dev/gpio/1]")],
                                 [("    devices: [/dev/gpio/1] #", "ownership.device")])),
    (None, on_k64f([], [], manifest=K64F_MANIFEST)),
    ("enforcement.unneeded", on_k64f([("[/dev/gpio/1] # leds", "[/dev/gpio/0] # leds"),
                                      (K64F_LEDS, "# leds\n    accepts: [coarse_gate, device_not_isolated]\n")],
                                     [("    accepts: [coarse_gate", "enforcement.unneeded")], manifest=K64F_MANIFEST)),
    ("enforcement.device-not-isolated",
     on_k64f([(K64F_LEDS, "# leds\n    accepts: [coarse_gate]\n")],
             [("    devices: [/dev/gpio/1]", "enforcement.device-not-isolated")], manifest=K64F_MANIFEST)),
    ("enforcement.port-bank", on_k64f([K64F_LEDS_ISOLATED], [("    devices: [/dev/gpio/1]", "enforcement.port-bank")],
                                      manifest=K64F_MANIFEST, names="console pin `rx`")),
    (None, on_k64f([("[/dev/gpio/1] # leds", "[/dev/gpio/4] # leds"), K64F_LEDS_ISOLATED], [], manifest=K64F_MANIFEST)),
    ("enforcement.coarse-gate",
     on_k64f([("[/dev/dspi0]\n    accepts: [device_not_isolated, coarse_gate]", "[/dev/dspi0]\n    accepts: [device_not_isolated]")],
             [("    devices: [/dev/dspi0]", "enforcement.coarse-gate")], manifest=K64F_MANIFEST)),
    ("encoding.window", on_k64f([], [("    devices: [/dev/dspi0]", "encoding.window")],
                                [("[0x4002C000, 0x40]", "[0x4002C000, 0x44]")], manifest=K64F_MANIFEST)),
    ("encoding.window", on_k64f([], [("    devices: [/dev/dspi0]", "encoding.window")],
                                [("[0x4002C000, 0x40]", "[0x4002C010, 0x40]")], manifest=K64F_MANIFEST)),
    (None, on_k64f([], [], [("[0x4002C000, 0x40]", "[0x4002C020, 0x40]")], manifest=K64F_MANIFEST)),
    (None, on_k64f([], [], [("[0x4002C000, 0x40]", "[0x4002C001, 0x41]")],
                   manifest=K64F_MANIFEST.replace("smallest_window: 32", "smallest_window: 1"))),
    ("encoding.window", on_k64f([], [("    devices: [/dev/dspi0]", "encoding.window")], [("[0x4002C000, 0x40]", "[0x4002C000, 0x10]")],
                                manifest=K64F_MANIFEST.replace("granule", "pow2"))),
    (None, on_k64f([], [], [("[0x4002C000, 0x40]", "[0x4002C000, 0x20]")], manifest=K64F_MANIFEST.replace("granule", "pow2"))),
    ("memory.uncached", on_k64f([K64F_SHARED], [("    cache: uncached", "memory.uncached")], K64F_CACHED, manifest=K64F_MANIFEST)),
    ("memory.uncached", on_k64f([K64F_BLOCK_TASK], [("    driver: blkdrv", "memory.uncached")], K64F_CACHED,
                                manifest=mutate(K64F_MANIFEST, [BLOCK_DRIVER])[0])),
    (None, on_k64f([K64F_BLOCK_TASK], [], manifest=mutate(K64F_MANIFEST, [BLOCK_DRIVER])[0])),
    (None, on_k64f([K64F_SHARED], [], manifest=K64F_MANIFEST)),
    (None, on_k64f([K64F_SHARED, ("ends: never\n", "ends: never\naccepts: [no_protection]\n"),
                    ("[/dev/dspi0]\n    accepts: [device_not_isolated, coarse_gate]", "[/dev/dspi0]"),
                    (K64F_LEDS, "# leds\n")], [], K64F_CACHED, manifest=K64F_UNENFORCED)),
    (None, on_k64f([("ends: never\n", "ends: never\naccepts: [no_protection]\n"),
                    ("[/dev/dspi0]\n    accepts: [device_not_isolated, coarse_gate]", "[/dev/dspi0]")],
                   [], manifest=K64F_UNENFORCED)),
    ("enforcement.unneeded", on_k64f([("ends: never\n", "ends: never\naccepts: [no_protection]\n"),
                                      ("[/dev/dspi0]\n    accepts: [device_not_isolated, coarse_gate]", "[/dev/dspi0]"),
                                      (K64F_LEDS, "# leds\n    accepts: [device_not_isolated, coarse_gate, bus_master]\n")],
                                     [("    accepts: [device_not", "enforcement.unneeded")],
                                     manifest=K64F_UNENFORCED, names="`bus_master`, which nothing here needs")),
    ("name.device-unknown", on_k64f([("[/dev/gpio/1] # leds", "[/dev/gpio/5] # leds")],
                                    [("    devices: [/dev/gpio/5]", "name.device-unknown")], manifest=K64F_MANIFEST)),
    ("manifest.target", on_k64f([], [("board: ", "manifest.target")], manifest=XMC_MANIFEST)),
    (None, on_k64f([("board: frdmk64f\n", "")], [], manifest=K64F_MANIFEST)),
    (None, on_c6([("board: esp32c6-wroom\n", "")], [])),
    ("name.device-unknown", on_k64f([("board: frdmk64f\n", ""), ("[/dev/gpio/1] # leds", "[/dev/usic0/ch1] # leds")],
                                    [("    devices: [/dev/usic0", "name.device-unknown")], manifest=K64F_MANIFEST)),
    ("manifest.target", on_k64f([("board: frdmk64f\n", "")], [("version: ", "manifest.target")],
                                manifest=mutate(K64F_MANIFEST, [("descriptions:\n  chip: platform/mk64f/chip.yaml\n"
                                                                 "  board: platform/mk64f/frdmk64f.yaml\n", "")])[0])),
    (None, on_c6(BOARD_LESS_C6, [], manifest=defaulted(C6_MANIFEST, "esp32c6-wroom"))),
    ("form.missing", on_c6(BOARD_LESS_C6, [("version: ", "form.missing"), ("version: ", "enforcement.no-protection"),
                                           ("version: ", "enforcement.no-privilege-split"),
                                           ("  - name: blink", "form.missing")])),
    ("enforcement.no-privilege-split", on_c6([("board: esp32c6-wroom\n", ""), ("heap: 0\n", ""),
                                              ("[no_protection, no_privilege_split]", "[no_protection]")],
                                             [("version: ", "enforcement.no-privilege-split")], names="every cluster's",
                                             manifest=defaulted(C6_MANIFEST, "esp32c6-wroom"))),
    ("form.missing", on_k64f([("heap: 0\n", "")], [("version: ", "form.missing")], manifest=K64F_MANIFEST)),
    (None, on_imx([("board: imx8mp-evk\ncluster: a53\n", "")], [], manifest=defaulted(IMX_MANIFEST, "imx8mp-evk"))),
    ("form.missing", on_imx([("board: imx8mp-evk\ncluster: a53\n", "")],
                            [("version: ", "form.missing"), ("version: ", "manifest.window")], manifest=IMX_MANIFEST)),
    ("manifest.target", on_k64f([], [("board: ", "manifest.target")],
                                manifest=mutate(K64F_MANIFEST, [("  chip: mk64f\n  arch", "  chip: stm32f411\n  arch"),
                                                                ("descriptions:\n  chip: platform/mk64f/chip.yaml\n"
                                                                 "  board: platform/mk64f/frdmk64f.yaml\n", "")])[0])),
    (None, on_c6([], [])),
    ("enforcement.no-protection", on_c6([("[no_protection, no_privilege_split]", "[no_privilege_split]")],
                                        [("version: ", "enforcement.no-protection")], names="every cluster's")),
    ("enforcement.no-privilege-split", on_c6([("[no_protection, no_privilege_split]", "[no_protection]")],
                                             [("version: ", "enforcement.no-privilege-split")], names="every cluster's")),
    ("enforcement.unneeded", on_c6([C6_BANK], [("accepts: ", "enforcement.unneeded")], LP_PROTECTED,
                                   names="`no_privilege_split`")),
    ("enforcement.port-bank", on_c6([("accepts: [no_protection, no_privilege_split]\n", "")],
                                    [("    devices: [/dev/gpio]", "enforcement.port-bank")], LP_PROTECTED)),
    ("enforcement.unneeded", on_c6([C6_BANK], [("    accepts: [coarse_gate]", "enforcement.unneeded")], names="subsumes")),
    (None, c6blink([], [])),
    ("enforcement.unneeded", c6blink([C6BLINK_COARSE], [("    accepts: [coarse_gate]", "enforcement.unneeded")],
                                     names="subsumes")),
    (None, on_rv64([], [])),
    ("memory.uncached", on_rv64([("cache: cached", "cache: uncached")], [("    cache: uncached", "memory.uncached")])),
    ("encoding.page", on_arm64([("devices: [/dev/rtc]", "devices: [/dev/rtc, /dev/scratch]")],
                               [("    devices: [/dev/rtc, /dev/scratch]", "encoding.page")], ARM64_SCRATCH, MANIFESTS["qemu-arm64.yaml"])),
    (None, on_arm64([("devices: [/dev/rtc]", "devices: [/dev/rtc, /dev/scratch]")], [],
                    [("  gpio: { window", "  scratch: { window: [0x09020000, 0x1000] }\n  gpio: { window")],
                    MANIFESTS["qemu-arm64.yaml"])),
    (None, on_arm64([("devices: [/dev/rtc]", VIRTIO % "bus_master, coarse_gate")], [], ARM64_UNCACHED, MANIFESTS["qemu-arm64.yaml"])),
    ("enforcement.unneeded", on_arm64([("devices: [/dev/rtc]", VIRTIO % "bus_master, coarse_gate"),
                                       ("ends: never\n", "ends: never\naccepts: [cached_incoherent]\n")],
                                      [("accepts: [cached", "enforcement.unneeded")], ARM64_UNCACHED, MANIFESTS["qemu-arm64.yaml"])),
    (None, on_imx([], [], IMX_MANIFEST)),
    ("enforcement.bus-master", on_imx([("[/dev/sai5, /dev/sai6]", "[/dev/sai5, /dev/sai6, /dev/sdma1]")],
                                      [("    devices: ", "enforcement.bus-master")], IMX_MANIFEST)),
    (None, on_imx([("[/dev/sai5, /dev/sai6]", "[/dev/sai5, /dev/sai6, /dev/sdma1]\n    accepts: [bus_master]")], [],
                  IMX_MANIFEST)),
    ("memory.cached-incoherent",
     on_imx([("[/dev/sai5, /dev/sai6]", "[/dev/sai5, /dev/sai6, /dev/sdma1]\n    accepts: [bus_master]"),
             ("ends: never\n", "ends: never\nshared:\n  - name: /shm/ring\n    size: 64\n    cache: cached\n")],
            [("    cache: cached", "memory.cached-incoherent")], IMX_MANIFEST)),
    ("encoding.window", on_xmc([], [("    devices: [/dev/usic0/ch1]", "encoding.window")],
                               [("ch1: { window: [0x40030200, 0x200] }", "ch1: { window: [0x40030280, 0x100] }")], XMC_MANIFEST)),
    (None, on_xmc([], [], [("ch1: { window: [0x40030200, 0x200] }", "ch1: { window: [0x40030280, 0x100] }")],
                  XMC_MANIFEST.replace("window_rule: pow2", "window_rule: granule"))),
    ("form.range", on_arm64([("devices: [/dev/rtc]", "devices: [/dev/rtc]\n    lines: { irq: /dev/virtio/65488/irq }")],
                            [("    lines: { irq", "form.range")], VIRTIO_WIDE)),
    ("form.range", on_arm64([("devices: [/dev/rtc]", "devices: [/dev/rtc]\n    lines: { irq: /dev/virtio/65487/irq }")],
                            [("    lines: { irq", "form.range")], VIRTIO_WIDE)),
    (None, on_arm64([("devices: [/dev/rtc]", "devices: [/dev/rtc]\n    lines: { irq: /dev/virtio/65486/irq }")], [],
                    VIRTIO_WIDE)),
    ("encoding.table", table_limits(0, 0, 0, ["5 tasks", "13 grants", "4 refs", "3 privileged registers",
                                               "1 regions", "bytes of strings", "catalogue driver 1",
                                               "line 1 of a device"])),
    (None, table_limits(13, 1, 0xFFFFFFFF, [])),
    ("supply.budget", on_xmc([("    uses: [/svc/sensor]\n", "    uses: [/svc/sensor]\n    serves: /svc/app\n")],
                             [("version: ", "supply.budget")], [], XMC_ENDPOINTS_3, names="the init")),
    ("supply.budget", on_xmc([("    uses: [/svc/sensor]\n", "    uses: [/svc/sensor, /svc/spi0, /svc/console]\n")],
                             [("version: ", "supply.budget"), ("  - name: app", "supply.budget")], [], XMC_ENDPOINTS_2,
                             names="holds 3 endpoints")),
    ("supply.arena", on_xmc([], [("version: ", "supply.arena")], [(XMC_ARENA, XMC_ARENA.replace("size: 0x20000", "size: 0x8000"))],
                            XMC_MANIFEST)),
    ("supply.arena", on_xmc([], [("version: ", "supply.arena")],
                            [(XMC_ARENA, XMC_ARENA.replace("size: 0x20000", "size: 0x8800"))], XMC_MANIFEST)),
    (None, on_xmc([], [], [(XMC_ARENA, XMC_ARENA.replace("size: 0x20000", "size: 0x9000"))], XMC_MANIFEST)),
    (None, on_xmc([HEAP], [], [(XMC_ARENA, XMC_ARENA.replace("size: 0x20000", "size: 0x9000"))], XMC_MANIFEST)),
    ("supply.reservations", on_c6(C6_WATCHERS, [("version: ", "supply.reservations")], manifest=C6_OWNER_SLOTS % 6)),
    (None, on_c6(C6_WATCHERS, [], manifest=C6_OWNER_SLOTS % 7)),
    ("supply.ranges", on_arm64_alone(6, [("    - name: probe", "supply.ranges")])),
    (None, on_arm64_alone(7, [])),
    ("supply.ranges", on_arm64_watcher(8, [("    - name: watcher", "supply.ranges")])),
    (None, on_arm64_watcher(9, [])),
    ("supply.ranges", on_arm64_driver(6, [("    - name: spi", "supply.ranges")])),
    (None, on_arm64_driver(7, [])),
    ("supply.init-windows", on_c6(C6_WATCHERS, [("version: ", "supply.init-windows")], manifest=C6_FREE % 2)),
    (None, on_c6(C6_WATCHERS, [], manifest=C6_FREE % 3)),
    ("supply.arena", on_xmc([], [("version: ", "supply.arena")],
                            [(XMC_ARENA, XMC_ARENA.replace("size: 0x20000", "size: 0xF0E0"))], XMC_ODD_STACKS)),
    (None, on_xmc([], [], [(XMC_ARENA, XMC_ARENA.replace("size: 0x20000", "size: 0xF100"))], XMC_ODD_STACKS)),
    (None, edited_platform_beside),
    ("encoding.window", on_k64f(K64F_UNGUARDED, [("    devices: [/dev/dspi0]", "encoding.window")],
                                [("[0x4002C000, 0x40]", "[0x4002C008, 0x40]")], manifest=K64F_NO_UNIT)),
    (None, on_k64f(K64F_UNGUARDED, [], [("[0x4002C000, 0x40]", "[0x4002C010, 0x40]")], manifest=K64F_NO_UNIT)),
    ("supply.arena", on_xmc(XMC_ODD_REGION, [("version: ", "supply.arena")],
                            [(XMC_ARENA, XMC_ARENA.replace("size: 0x20000", "size: 0x67D0"))], XMC_NO_UNIT)),
    (None, on_xmc(XMC_ODD_REGION, [], [(XMC_ARENA, XMC_ARENA.replace("size: 0x20000", "size: 0x67E0"))], XMC_NO_UNIT)),
    ("ownership.device", diagnostic_app("xmcspi", U0C1 + "    lines: { irq: /dev/usic0/sr1 }\n",
                                        [("    devices: ", "ownership.device"), ("    lines: ", "ownership.line")])),
    ("ownership.device", diagnostic_app("xmccshold", U0C1, [("    devices: ", "ownership.device")])),
    ("ownership.device", diagnostic_app("pvprobe", U0C1, [("    devices: ", "ownership.device")])),
    ("ownership.device", diagnostic_app("inprstorm", U0C1, [("    devices: ", "ownership.device")])),
    (None, partitioned("arm64", ARM64_PAIR, {}, [], ARM64_AMP)),
    ("partition.device", partitioned("arm64", ARM64_PAIR,
                                     {0: [("    uses: [/amp/3]\n", "    uses: [/amp/3]\n    devices: [/dev/rtc]\n")],
                                      1: [("    serves: /amp/3\n", "    serves: /amp/3\n    devices: [/dev/rtc]\n")]},
                                     [(1, "    devices: ", "partition.device")], ARM64_AMP)),
    ("partition.port", partitioned("arm64", ARM64_PAIR, {0: [("uses: [/amp/3]", "uses: [/amp/9]")]},
                                   [(0, "    uses: ", "partition.port")], ARM64_AMP)),
    ("partition.port", partitioned("arm64", ARM64_PAIR, {0: [("uses: [/amp/3]", "uses: [/amp/1]")]},
                                   [(0, "    uses: ", "partition.port")], ARM64_AMP)),
    (None, partitioned("arm64", ARM64_PAIR, {0: [("uses: [/amp/3]", "uses: [/amp/0, /amp/3]")]}, [],
                       ARM64_AMP.replace("[[0, 2], [1, 3], [1, 4]]", "[[0, 2], [1, 0], [1, 3], [1, 4]]")
                       .replace("KICKOS_TASK_ENDPOINT_BUDGET: 3", "KICKOS_TASK_ENDPOINT_BUDGET: 4"))),
    ("partition.port", partitioned("arm64", ARM64_PAIR, {1: [("serves: /amp/3", "serves: /amp/2")]},
                                   [(1, "    serves: ", "partition.port")], ARM64_AMP)),
    ("partition.port", partitioned("arm64", ARM64_PAIR, {1: [SPARE]}, [(1, "    serves:  /amp/3", "partition.port")],
                                   ARM64_AMP)),
    ("partition.port", partitioned("arm64", ARM64_PAIR, {}, [(0, "    uses: ", "partition.port"),
                                                             (1, "    serves: ", "partition.port")],
                                   ARM64_AMP.replace("[[0, 2], [1, 3], [1, 4]]", "[[0, 3], [1, 3]]"))),
    ("partition.port", composed("xmc4800-relax.yaml", read(os.path.join(SYSTEMS, "xmc4800-relax.yaml")),
                                [("    serves: /svc/spi0\n", "    serves: /amp/3\n"), ("    uses: [/svc/spi0]\n", "")],
                                [("    serves: /amp/3", "partition.port")])),
    ("partition.unserved", partitioned("arm64", ARM64_PAIR, {1: [("    serves: /amp/3\n", "")]},
                                       [(0, "    uses: ", "partition.unserved")], ARM64_AMP)),
    ("partition.region", partitioned("arm64", ARM64_PAIR, {1: [("size: 64", "size: 128")]},
                                     [(1, "    size: ", "partition.region")], ARM64_AMP)),
    ("partition.region", partitioned("arm64", ARM64_PAIR, {0: [("size: 64", "size: 0x300000")],
                                                           1: [("size: 64", "size: 0x300000")]},
                                     [(0, "  - name: /shm/book", "partition.region"),
                                      (1, "  - name: /shm/book", "partition.region")], ARM64_AMP)),
    (None, partitioned("arm64", ARM64_PAIR, {0: [("size: 64", "size: 0x200000")], 1: [("size: 64", "size: 0x200000")]},
                       [], ARM64_AMP)),
    ("partition.region", partitioned("arm64", ARM64_PAIR, {0: [("cache: uncached", "cache: cached")]},
                                     [(0, "    cache: ", "partition.region")], ARM64_AMP)),
    ("partition.cached-incoherent", partitioned("imx", IMX_PAIR, {},
                                                [(0, "    cache: ", "partition.cached-incoherent"),
                                                 (1, "    cache: ", "partition.cached-incoherent")], IMX_AMP)),
    (None, partitioned("imx", IMX_PAIR, {0: [IMX_ACCEPT], 1: [IMX_ACCEPT]}, [], IMX_AMP)),
    ("partition.cached-incoherent", partitioned("imx", IMX_PAIR, {0: [IMX_ACCEPT]},
                                                [(1, "    cache: ", "partition.cached-incoherent")], IMX_AMP)),
    (None, partitioned("c6", C6_PAIR, {}, [], C6_AMP)),
    ("partition.gate-budget", partitioned("c6", C6_PAIR, {1: [("    serves: /amp/3\n",
                                                                "    serves: /amp/3\n    devices: [/dev/lp_uart]\n")]},
                                          [(1, "    devices: ", "partition.gate-budget")], C6_AMP)),
    (None, partitioned("c6", C6_PAIR, {1: [("    serves: /amp/3\n", "    serves: /amp/3\n    devices: [/dev/timg1]\n")]},
                       [], C6_AMP)),
    ("partition.gate-budget", partitioned("c6", C6_PAIR, {}, [(0, "version: ", "partition.gate-budget")], C6_AMP,
                                          [("esp32c6/chip.yaml", C6_HP_SMALL)])),
    ("partition.gate-budget", partitioned("c6", C6_PAIR, {0: LONE}, [(0, "version: ", "partition.gate-budget")],
                                          C6_AMP, [("esp32c6/chip.yaml", C6_HP_SMALL)], paths=single_emit)),
    (None, partitioned("c6", C6_PAIR, {0: LONE}, [], C6_AMP, paths=single_emit)),
    ("partition.lone", partitioned("c6", C6_PAIR, {0: [("    uses: [/amp/3]\n", "")]},
                                   [(0, "  - name: /shm/book", "partition.lone")], C6_AMP, paths=single_emit)),
    ("partition.lone", partitioned("c6", C6_PAIR, {0: [LONE[0], LONE[2]]},
                                   [(0, "    uses: ", "partition.lone")], C6_AMP, paths=single_emit)),
    ("partition.port", partitioned("c6", C6_PAIR, {1: [("    serves: /amp/3\n", "    serves: /amp/3\n    uses: [/amp/3]\n")]},
                                   [(1, "    uses: ", "partition.port")], C6_AMP)),
    ("partition.region", partitioned("arm64", ARM64_PAIR, {0: [("    uses: [/amp/3]\n", "")]},
                                     [(0, "  - name: /shm/book", "partition.region")], MANIFESTS["qemu-arm64.yaml"],
                                     paths=single_emit)),
    ("partition.gate-budget", partitioned("c6", C6_PAIR, {}, [(None, "", "partition.gate-budget")], C6_AMP,
                                          [("esp32c6/chip.yaml", C6_HP_SMALL)], paths=gate_emit)),
    (None, partitioned("rp", RP_PAIR, {0: [("    uses: [/amp/3]\n", "    uses: [/amp/3]\n    devices: [/dev/uart0]\n")]},
                       [], RP_AMP)),
    ("partition.gate-budget", partitioned("rp", RP_PAIR,
                                          {0: [("    uses: [/amp/3]\n", "    uses: [/amp/3]\n    devices: [/dev/uart0]\n")]},
                                          [(0, "    devices: ", "partition.gate-budget")], RP_AMP,
                                          [("rp2350/chip.yaml", [("    gate_register: 0xA0\n", "")])])),
    ("manifest.amp", partitioned("arm64", ARM64_PAIR, {}, [(0, "version: ", "manifest.amp")], ARM64_AMP,
                                 paths=three_nodes)),
]


def refusal_sites():
    """Every `refuse` call in the tool, as (file, first line, last line)."""
    sites = set()
    for source in SOURCES:
        tree = ast.parse(read(source))
        for function in ast.walk(tree):
            if not isinstance(function, ast.FunctionDef) or function.name == "refuse":
                continue
            for node in ast.walk(function):
                if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute) and node.func.attr == "refuse":
                    sites.add((source, node.lineno, node.end_lineno))
    return sorted(sites)


def recording_sites():
    """Report.refuse, patched to stamp each refusal with the source line that made it."""
    original = subset.Report.refuse

    def refuse(report, path, line, rule, message):
        original(report, path, line, rule, message)
        frame = sys._getframe(1)
        if frame.f_code is subset.File.refuse.__code__:
            frame = frame.f_back
        report.refusals[-1].site = (os.path.realpath(frame.f_code.co_filename), frame.f_lineno)

    return mock.patch.object(subset.Report, "refuse", refuse)


def at_site(refusal, site):
    source, first, last = site
    return refusal.site[0] == source and first <= refusal.site[1] <= last


class Case:
    def __init__(self, label, rules, refusals, judge):
        self.label = label
        self.rules = rules
        self.refusals = refusals
        self.judge = judge


def run_cases():
    cases = []
    scratch = tempfile.mkdtemp(prefix="kickos-compose-")
    try:
        root = os.path.join(scratch, "platform")
        shutil.copytree(PLATFORM, root)
        systems = os.path.join(scratch, "systems")
        shutil.copytree(SYSTEMS, systems)
        os.makedirs(os.path.join(scratch, os.path.dirname(XMC_DEFAULT)))
        shutil.copyfile(os.path.join(TREE, XMC_DEFAULT), os.path.join(scratch, XMC_DEFAULT))
        goldens = [os.path.join(systems, name) for name in sorted(os.listdir(systems))]
        control = run([root]) + run_admit(goldens, root)
        for rules, target, edits, anywhere in ARMS:
            path = os.path.join(root, target)
            text = read(path)
            mutated, lines = mutate(text, edits)
            write(path, mutated)
            try:
                refusals = run([path])
            finally:
                write(path, text)
            judge = lambda found, rules=rules, lines=lines, anywhere=anywhere: arm_fault(rules, found, lines, anywhere)
            cases.append(Case("%s %s %r" % (rules, target, edits), rules, refusals, judge))
        for rules, target, edits, anywhere in COMPOSITION_ARMS:
            path = os.path.join(systems, target)
            text = read(path)
            mutated, lines = mutate(text, edits)
            write(path, mutated)
            try:
                refusals = run_admit([path], root)
            finally:
                write(path, text)
            judge = lambda found, rules=rules, lines=lines, anywhere=anywhere: arm_fault(rules, found, lines, anywhere)
            cases.append(Case("%s %s %r" % (rules, target, edits), rules, refusals, judge))
        manifest_path = os.path.join(scratch, "manifest.yaml")
        for name, text in sorted(MANIFESTS.items()):
            write(manifest_path, text)
            control = control + run_admit([os.path.join(systems, name)], root, manifest_path)
        for rules, target, edits, manifest_edits, anywhere in ADMISSION_ARMS:
            path = os.path.join(systems, target)
            text = read(path)
            mutated, lines = mutate(text, edits)
            write(path, mutated)
            write(manifest_path, mutate(MANIFESTS[target], manifest_edits)[0])
            try:
                refusals = run_admit([path], root, manifest_path)
            finally:
                write(path, text)
            judge = lambda found, rules=rules, lines=lines, anywhere=anywhere: arm_fault(rules, found, lines, anywhere)
            cases.append(Case("%s %s %r %r" % (rules, target, edits, manifest_edits), rules, refusals, judge))
        write(manifest_path, MANIFEST)
        control = control + run_manifest([manifest_path])
        for rules, edits, anywhere in MANIFEST_ARMS:
            mutated, lines = mutate(MANIFEST, edits)
            write(manifest_path, mutated)
            try:
                refusals = run_manifest([manifest_path])
            finally:
                write(manifest_path, MANIFEST)
            judge = lambda found, rules=rules, lines=lines, anywhere=anywhere: arm_fault(rules, found, lines, anywhere)
            cases.append(Case("%s manifest %r" % (rules, edits), rules, refusals, judge))
        for n, (rule, scenario) in enumerate(SCENARIOS):
            fresh = os.path.join(scratch, "fresh%d" % n, "platform")
            shutil.copytree(PLATFORM, fresh)
            shutil.copytree(SYSTEMS, systems_of(fresh))
            shutil.copytree(os.path.join(TREE, "boards"), os.path.join(os.path.dirname(fresh), "boards"))
            refusals, expected = scenario(fresh)
            judge = lambda found, expected=expected: exact_fault(expected, found)
            cases.append(Case("%s %s" % (rule, scenario.__name__), rule, refusals, judge))
    finally:
        shutil.rmtree(scratch)
    return control, cases


class Arms(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with recording_sites():
            cls.control, cls.cases = run_cases()

    def test_controls_pass(self):
        self.assertEqual([str(r) for r in self.control], [])

    def test_arms(self):
        for case in self.cases:
            with self.subTest(case=case.label):
                self.assertEqual(case.judge(case.refusals), None, [str(r) for r in case.refusals])

    def test_every_rule_has_an_arm(self):
        armed = set()
        for case in self.cases:
            if case.rules is None:
                continue
            if isinstance(case.rules, str):
                armed.add(case.rules)
            else:
                armed.update(case.rules)
        self.assertEqual(sorted(set(RULES) - armed), [])
        self.assertEqual(sorted(armed - set(RULES)), [])

    def test_every_refusal_site_reddens_an_arm(self):
        """Dropping what one `refuse` call reports must turn at least one green arm red."""
        unarmed = []
        for site in refusal_sites():
            reddened = False
            for case in self.cases:
                if case.rules is None or case.judge(case.refusals) is not None:
                    continue
                kept = [r for r in case.refusals if not at_site(r, site)]
                if case.judge(kept) is not None:
                    reddened = True
                    break
            if not reddened:
                unarmed.append("%s:%d" % (os.path.basename(site[0]), site[1]))
        self.assertEqual(unarmed, [])


class NeverAssigned(unittest.TestCase):
    def test_the_chip_file_and_the_chip_code_name_the_same_registers(self):
        report = descriptions.Report()
        path = os.path.join(PLATFORM, "rp2350", "chip.yaml")
        chip = descriptions.check_chip(path, read(path), report)
        self.assertEqual([str(r) for r in report.refusals], [])
        code = read(os.path.join(TREE, "arch", "arm", "chip", "rp2350", "regs", "accessctrl.h"))
        listed = code[code.index("NEVER_ASSIGNED[] = {"):]
        listed = listed[:listed.index("};")]
        constants = [int(word[:-1], 16) for word in listed.replace(",", " ").split() if word.startswith("0x")]
        self.assertEqual(sorted(constants), sorted(chip.partition_gate.never_assigned))


class Rounding(unittest.TestCase):
    def chip(self, chip):
        report = descriptions.Report()
        path = os.path.join(PLATFORM, chip, "chip.yaml")
        return descriptions.check_chip(path, read(path), report)

    def rule(self, rule, smallest):
        found = manifest.Manifest()
        found.enforced = True
        found.window_rule = rule
        found.smallest_window = smallest
        return found

    def test_shared_regions_round_up(self):
        xmc = self.chip("xmc4800")
        arm64 = self.chip("virt_arm64")
        self.assertEqual(region_size(64, xmc, None, self.rule("pow2", 32)), 64)
        self.assertEqual(region_size(65, xmc, None, self.rule("pow2", 32)), 128)
        self.assertEqual(region_size(8, xmc, None, self.rule("pow2", 32)), 32)
        self.assertEqual(region_size(65, xmc, None, self.rule("granule", 32)), 96)
        self.assertEqual(region_size(65, xmc, None, self.rule("none", None)), 65)
        self.assertEqual(region_size(64, arm64, None, self.rule("none", None)), 0x1000)
        self.assertEqual(region_size(0x1001, arm64, None, self.rule("none", None)), 0x2000)
        self.assertEqual(region_size(0x80000000, xmc, None, self.rule("pow2", 32)), 0x80000000)
        self.assertEqual(region_size(0x80000001, xmc, None, self.rule("pow2", 32)), 0x100000000)
        self.assertEqual(region_size(0xFFFFFFFF, xmc, None, self.rule("granule", 32)), 0x100000000)
        self.assertEqual(region_size(0xFFFFF001, arm64, None, self.rule("none", None)), 0x100000000)

    def test_a_region_build_with_no_unit_rounds_at_16(self):
        rule = self.rule("granule", 16)
        rule.stack_stride = None
        self.assertEqual((supply.ram_size(0x41, rule), supply.ram_align(0x41, rule)), (0x50, 16))
        self.assertEqual((supply.ram_size(8, rule), supply.ram_align(8, rule)), (16, 16))


class Versions(unittest.TestCase):
    """Each format reads the versions of its own list, so a version one format takes up is still
    refused in the other three."""

    FORMATS = (("chip", descriptions, "CHIP_VERSIONS"), ("board", descriptions, "BOARD_VERSIONS"),
               ("composition", composition, "COMPOSITION_VERSIONS"), ("manifest", manifest, "MANIFEST_VERSIONS"))

    def refused_at_2(self, scratch, root):
        """Each format's name, mapped to whether its file at version 2 is refused as form.version."""
        chip = os.path.join(root, "stm32f411", "chip.yaml")
        board = os.path.join(root, "stm32f411", "f411disco.yaml")
        system = os.path.join(scratch, "qemu-arm64.yaml")
        manifest_path = os.path.join(scratch, "manifest.yaml")
        files = {"chip": (chip, read(chip), lambda: run([chip])),
                 "board": (board, read(board), lambda: run([board])),
                 "composition": (system, read(os.path.join(SYSTEMS, "qemu-arm64.yaml")),
                                 lambda: run_admit([system], root)),
                 "manifest": (manifest_path, MANIFEST, lambda: run_manifest([manifest_path]))}
        refused = {}
        for name, (path, text, check) in files.items():
            write(path, mutate(text, [("version: 1\n", "version: 2\n")])[0])
            try:
                refused[name] = "form.version" in [r.rule for r in check()]
            finally:
                write(path, text)
        return refused

    def test_each_format_reads_its_own_versions(self):
        scratch = tempfile.mkdtemp(prefix="kickos-versions-")
        try:
            root = os.path.join(scratch, "platform")
            shutil.copytree(PLATFORM, root)
            for name, module, known in self.FORMATS:
                with self.subTest(format=name), mock.patch.object(module, known, (1, 2)):
                    want = {other: other != name for other, _, _ in self.FORMATS}
                    self.assertEqual(self.refused_at_2(scratch, root), want)
        finally:
            shutil.rmtree(scratch)


if __name__ == "__main__":
    unittest.main()
