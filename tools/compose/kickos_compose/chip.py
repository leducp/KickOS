# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The kernel's chip headers, tables and link values, written from a chip file for the built
# cluster, the board's console and LED pins from its board file, and the symbol compare that
# proves a generated header against a hand-written one.

import os
import re
import subprocess
import tempfile

from .descriptions import (
    check_board, check_chip, device_layout, device_symbols, line_macro, line_symbols, memory_layout,
    memory_symbol, reaches, unit_views,
)
from .subset import Report, read_utf8

FAMILIES = {
    "armv6m": "arm", "armv7m": "arm", "armv8a": "arm64", "rxv3": "rx", "rv32imac": "riscv",
    "rv64imac": "riscv",
    "lx6": "xtensa", "x86_64": "x86", "sim": "sim",
}
LIMITS = (
    ("count", "KICKOS_MAX_IRQ"),
    ("soft_only_from", "KICKOS_IRQ_SOFT_ONLY_BASE"),
    ("free_from", "KICKOS_IRQ_FREE_BASE"),
    ("vectors", "KICKOS_RX_INTB_ENTRIES"),
)
# The fields `interrupts` states in lines, which the built cluster's line_offset moves.
LINE_LIMITS = ("count", "soft_only_from", "free_from")
INCLUDE_OUTPUTS = ("chip_mmap.h", "chip_limits.h")
CHIP_OUTPUTS = ("irq.h", "chip_layout.h", "chip_tables.h", "chip.cmake", "board_pins.h", "board_buses.h")
COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
NAME_NUMBER = re.compile(r"[a-z]+?([0-9]+)")


class Failure(Exception):
    """A request the tool cannot serve, as against a chip file it refuses."""


class View:
    """The chip as one build sees it: the cluster it runs on, or None for the whole part."""

    def __init__(self, chip, cluster, board=None):
        self.chip = chip
        self.cluster = cluster
        self.offset = chip.line_offsets.get(cluster, 0)
        self.board = board

    def memory(self):
        if self.board is None:
            return self.chip.memory
        return self.chip.memory + self.board.memory

    def reaches(self, device):
        return reaches(device, self.cluster)

    def devices(self):
        return [device for device in self.chip.devices.values() if self.reaches(device)]

    def base(self, entry):
        """The memory entry's base in this view, or None where the view does not reach it."""
        if self.cluster is not None:
            return entry.bases.get(self.cluster)
        for base in entry.bases.values():
            return base
        return None

    def units(self):
        """The protection units this build's cores carry."""
        return [unit for view, unit in unit_views(self.chip, self.cluster)]


def read(path, report):
    """(Chip, Board) a chip file or a board file describes, the Board None for a chip file, or
    (None, None) once refused."""
    text = read_utf8(path, report)
    if text is None:
        return None, None
    if os.path.basename(path) == "chip.yaml":
        return check_chip(path, text, report), None
    board = check_board(path, text, report, {}, {})
    if board is None:
        return None, None
    board.name = os.path.splitext(os.path.basename(path))[0]
    return board.chip, board


def view_of(chip, arch, board=None):
    if arch not in FAMILIES:
        raise Failure("`%s` is no architecture the kernel builds; they are %s" % (arch, ", ".join(FAMILIES)))
    if not chip.multi_arch:
        if chip.arch != arch:
            raise Failure("chip `%s` is `%s`, not `%s`" % (chip.name, chip.arch, arch))
        return View(chip, None, board)
    clusters = [cluster for cluster, cluster_arch in chip.cluster_arch.items() if cluster_arch == arch]
    if len(clusters) != 1:
        raise Failure("chip `%s` has no one cluster of arch `%s`; its clusters are %s"
                      % (chip.name, arch, ", ".join("%s (%s)" % item for item in chip.cluster_arch.items())))
    return View(chip, clusters[0], board)


def namespace(chip):
    return chip.namespace or "kickos::%s" % chip.name


def line_enum(chip):
    return chip.line_enum or "irq_num"


def arch_of(view):
    if view.cluster is not None:
        return view.chip.cluster_arch[view.cluster]
    return view.chip.arch


def hex_u(value):
    return "0x%Xu" % value


def comment(ref, style):
    if ref is None:
        return ""
    if style == "c":
        return " /* %s */" % ref
    return " // %s" % ref


def mmap_symbols(view):
    """[(symbol, value, ref)] of chip_mmap.h."""
    symbols = []
    for device in view.devices():
        symbols.extend(device_symbols(device))
    for entry in view.memory():
        base = view.base(entry)
        if base is not None:
            symbols.append((memory_symbol(entry), base, entry.ref))
    return symbols


def irq_symbols(view):
    """[(symbol, line, ref)] of irq.h, each line moved by the cluster's offset."""
    symbols = []
    for device in view.devices():
        for symbol, number, ref in line_symbols(device):
            symbols.append((symbol, number + view.offset, ref))
    return symbols


def limit_values(view):
    """[(macro, value, ref)] of chip_limits.h."""
    chip = view.chip
    values = []
    for field, macro in LIMITS:
        if field in chip.interrupts and chip.interrupts[field] is not None:
            value = chip.interrupts[field]
            if field in LINE_LIMITS:
                value = value + view.offset
            values.append((macro, value, chip.refs.get(field)))
    if "hz" in chip.cycle_counter:
        values.append(("KICKOS_CHIP_CYCCNT_HZ", chip.cycle_counter["hz"], chip.refs.get("hz")))
    if chip.cycle_counter.get("glitches"):
        values.append(("KICKOS_CHIP_CYCCNT_GLITCHES", 1, chip.refs.get("glitches")))
    values.append(("KICKOS_CHIP_DCACHE", int(chip.data_cache), None))
    values.append(("KICKOS_CHIP_DOORBELL_SEAT", int(chip.doorbell_seat), None))
    return values


def opening(view, style):
    chip = view.chip
    banner = "Generated by kickos_compose chip from platform/%s/chip.yaml" % chip.name
    if view.board is not None and view.board.memory:
        banner += " and platform/%s/%s.yaml" % (chip.name, view.board.name)
    if style == "c":
        return "/* %s */\n/* %s */\n" % (banner, chip.manual)
    if style == "cmake":
        return "# %s\n" % banner
    return "// %s\n// %s\n" % (banner, chip.manual)


def emit_mmap(view):
    chip = view.chip
    lines = [opening(view, "cxx"),
             "\n#ifndef KICKOS_CHIP_MMAP_H\n#define KICKOS_CHIP_MMAP_H\n\n#include <stdint.h>\n\n",
             "namespace %s::mmap\n{\n" % namespace(chip)]
    for symbol, value, ref in mmap_symbols(view):
        lines.append("    constexpr uintptr_t %s = %s;%s\n" % (symbol, hex_u(value), comment(ref, "cxx")))
    lines.append("}\n\n#endif\n")
    return "".join(lines)


def irq_guard(view):
    return "KICKOS_ARCH_%s_CHIP_%s_IRQ_H" % (FAMILIES[arch_of(view)].upper(), view.chip.name.upper())


def emit_irq(view):
    chip = view.chip
    guard = irq_guard(view)
    lines = [opening(view, "cxx"), "\n#ifndef %s\n#define %s\n\n" % (guard, guard),
             "namespace %s::irq\n{\n    enum %s\n    {\n" % (namespace(chip), line_enum(chip))]
    for symbol, number, ref in irq_symbols(view):
        lines.append("        %s = %d,%s\n" % (symbol, number, comment(ref, "cxx")))
    lines.append("    };\n}\n\n#endif\n")
    return "".join(lines)


def emit_limits(view):
    lines = [opening(view, "c"), "\n#ifndef KICKOS_CHIP_LIMITS_H\n#define KICKOS_CHIP_LIMITS_H\n\n"]
    for macro, value, ref in limit_values(view):
        lines.append("#define %s %d%s\n" % (macro, value, comment(ref, "c")))
    lines.append("\n#endif\n")
    return "".join(lines)


def layout_values(view):
    """[(macro, value, ref)] of chip_layout.h: each memory entry's and device window's base and size."""
    values = []
    for entry in view.memory():
        base = view.base(entry)
        if base is not None:
            values.extend(memory_layout(entry, base))
        elif entry.host_placed:
            values.append(memory_layout(entry, 0)[1])
    for device in view.devices():
        values.extend(device_layout(device))
    return values


def emit_layout(view):
    lines = [opening(view, "c"), "\n#ifndef KICKOS_CHIP_LAYOUT_H\n#define KICKOS_CHIP_LAYOUT_H\n\n"]
    for macro, value, ref in layout_values(view):
        lines.append("#define %s 0x%X%s\n" % (macro, value, comment(ref, "c")))
    for symbol, number, ref in irq_symbols(view):
        lines.append("#define %s %d%s\n" % (line_macro(symbol), number, comment(ref, "c")))
    for entry in view.memory():
        base = view.base(entry)
        if base is None or entry.link is None:
            continue
        region, access = entry.link
        lines.append("#define KICKOS_LINK_%s %s (%s) : ORIGIN = 0x%X, LENGTH = 0x%X\n"
                     % (region, region, access, base, entry.size))
    lines.append("\n#endif\n")
    return "".join(lines)


def window_rows(device):
    """[(base, size)] a device's windows cover, a repeat whose instances touch being one row."""
    if device.window is not None and device.count is not None:
        base, size = device.window
        if device.stride == size:
            return [(base, size * device.count)]
        return [(base + k * device.stride, size) for k in range(device.count)]
    if device.window is not None:
        return [device.window]
    return list(device.channel_windows.values())


def table_rows(view):
    """{table: [(base, size, ref)]} of chip_tables.h."""
    units = view.units()
    translating = any(unit.unit == "mmu" for unit in units)
    ports = any(unit.io_ports for unit in units)
    tables = {"reserved_blocks": [], "window_apertures": [], "bus_master_apertures": [], "port_apertures": []}
    for device in view.devices():
        kernel = device.owner == "kernel"
        rows = [(base, size, device.ref) for base, size in window_rows(device)]
        if kernel:
            tables["reserved_blocks"].extend(rows)
        elif translating:
            tables["window_apertures"].extend(rows)
        if device.bus_master and not kernel:
            tables["bus_master_apertures"].extend(rows)
        if device.ports is not None and not kernel and ports:
            tables["port_apertures"].append((device.ports[0], device.ports[1], device.ref))
    return tables


def emit_tables(view):
    lines = [opening(view, "cxx"),
             "\n#ifndef KICKOS_CHIP_TABLES_H\n#define KICKOS_CHIP_TABLES_H\n\n"
             "#include <kickos/arch/arch.h>\n\nnamespace kickos::chip\n{\n"]
    for table, rows in table_rows(view).items():
        if not rows:
            lines.append("    inline constexpr struct arch_reserved_span %s{};\n" % table)
            continue
        lines.append("    inline constexpr struct arch_reserved_block %s_rows[] = {\n" % table)
        for base, size, ref in rows:
            lines.append("        {%s, %s},%s\n" % (hex_u(base), hex_u(size), comment(ref, "cxx")))
        lines.append("    };\n    inline constexpr struct arch_reserved_span %s{%s_rows};\n"
                     % (table, table))
    lines.append("}\n\n#endif\n")
    return "".join(lines)


def bus_selector(chip, bus, pin):
    """The mux value of the selector at which `pin` carries the bus's device."""
    selectors = [selector for selector, function in chip.pins[pin].items()
                 if selector != "gpio" and function.startswith(bus.function_prefix + ".")]
    if len(selectors) != 1:
        raise Failure("bus `%s` pin `%s` carries `%s` at the selectors %s, and a bus pin names one"
                      % (bus.name, pin, bus.path, ", ".join("`%s`" % s for s in selectors)))
    value = selector_value(selectors[0])
    if value is None:
        raise Failure("bus `%s` pin `%s` carries `%s` at `%s`, which names no mux value"
                      % (bus.name, pin, bus.path, selectors[0]))
    return selectors[0], value


def bus_port_bit(chip, bus, pin):
    """(port, bit) of a bus pin, as kernel_pins numbers them."""
    if "gpio" not in chip.pins[pin]:
        raise Failure("bus `%s` pin `%s` has no `gpio` function to name its port" % (bus.name, pin))
    return pinmux_port_bit(chip, pin)


def emit_buses(view):
    chip = view.chip
    banner = "Generated by kickos_compose chip from platform/%s/chip.yaml" % chip.name
    buses = []
    if view.board is not None:
        banner += " and platform/%s/%s.yaml" % (chip.name, view.board.name)
        buses = [bus for bus in view.board.buses if view.reaches(chip.devices[bus.path.split("/")[2]])]
    lines = ["// %s\n// %s\n" % (banner, chip.manual),
             "\n#ifndef KICKOS_BOARD_BUSES_H\n#define KICKOS_BOARD_BUSES_H\n\n#include <stdint.h>\n\n",
             "namespace %s::board\n{\n" % namespace(chip),
             "    // port is the port kos_pinmux_set names the pin by, select its selector's mux value, as\n"
             "    // in board_pins.h.\n"
             "    struct gpio_pin\n    {\n        uint32_t port;\n        uint32_t bit;\n    };\n\n"
             "    struct bus_pin\n    {\n        uint32_t port;\n        uint32_t bit;\n"
             "        uint32_t select;\n    };\n\n"
             "    struct bus\n    {\n        char const* name;\n        uintptr_t base;\n"
             "        bus_pin const* pins;\n        uint32_t pin_count;\n"
             "        gpio_pin const* chip_selects;\n        uint32_t chip_select_count;\n    };\n"]
    rows = []
    for bus in buses:
        pins = "nullptr"
        if bus.pins:
            pins = "%s_pins" % bus.name
            lines.append("\n    inline constexpr bus_pin %s[] = {\n" % pins)
            for pin in bus.pins:
                port, bit = bus_port_bit(chip, bus, pin)
                selector, value = bus_selector(chip, bus, pin)
                ref = "%s %s: %s" % (pin, selector, chip.pins[pin][selector])
                lines.append("        {%du, %du, %du},%s\n" % (port, bit, value, comment(ref, "c")))
            lines.append("    };\n")
        selects = "nullptr"
        if bus.chip_selects:
            selects = "%s_chip_selects" % bus.name
            lines.append("    inline constexpr gpio_pin %s[] = {\n" % selects)
            for pin in bus.chip_selects:
                port, bit = bus_port_bit(chip, bus, pin)
                lines.append("        {%du, %du},%s\n" % (port, bit, comment(pin, "c")))
            lines.append("    };\n")
        window = window_of(chip, bus.path)
        if window is None:
            raise Failure("bus `%s` device `%s` has no window of its own" % (bus.name, bus.path))
        rows.append('        {"%s", %s, %s, %du, %s, %du},\n'
                    % (bus.name, hex_u(window[0]), pins, len(bus.pins), selects, len(bus.chip_selects)))
    if rows:
        lines.append("\n    inline constexpr bus bus_rows[] = {\n")
        lines.extend(rows)
        lines.append("    };\n    inline constexpr bus const* buses = bus_rows;\n")
    else:
        lines.append("\n    inline constexpr bus const* buses = nullptr;\n")
    lines.append("    inline constexpr uint32_t bus_count = %du;\n}\n\n#endif\n" % len(rows))
    return "".join(lines)


def emit_cmake(view):
    units = view.units()
    regions = sorted({unit.unit for unit in units if unit.unit not in ("mmu", "none") and unit.driven is not False})
    if len(regions) > 1:
        raise Failure("chip `%s`'s cores carry the region units %s; one build drives one"
                      % (view.chip.name, ", ".join(regions)))
    translating = "OFF"
    if any(unit.unit == "mmu" for unit in units):
        translating = "ON"
    links = []
    lengths = {}
    for entry in view.memory():
        base = view.base(entry)
        if base is None or entry.link is None:
            continue
        region = entry.link[0]
        lengths[region] = entry.size
        links.append("set(KICKOS_CHIP_LINK_%s_ORIGIN 0x%X)\nset(KICKOS_CHIP_LINK_%s_LENGTH 0x%X)\n"
                     % (region, base, region, entry.size))
    privilege = "OFF"
    if not units or any(unit.privilege is not False for unit in units):
        privilege = "ON"
    seat = "OFF"
    if view.chip.doorbell_seat:
        seat = "ON"
    facts = ["set(KICKOS_CHIP_PRIVILEGE %s)\n" % privilege,
             "set(KICKOS_CHIP_RESERVED_BLOCKS %d)\n" % len(table_rows(view)["reserved_blocks"]),
             "set(KICKOS_CHIP_DOORBELL_SEAT %s)\n" % seat]
    if view.chip.esptool_image is not None:
        facts.append("set(KICKOS_CHIP_ESPTOOL_IMAGE \"%s\")\n" % ";".join(view.chip.esptool_image))
    if view.board is not None and view.board.emulator is not None:
        facts.extend(emulator_cmake(view, view.board.emulator, lengths))
    return "%s%s%sset(KICKOS_CHIP_REGION_UNIT \"%s\")\nset(KICKOS_CHIP_TRANSLATES %s)\n" % (
        opening(view, "cmake"), "".join(links), "".join(facts), "".join(regions), translating)


def emulator_cmake(view, emulator, lengths):
    """The QEMU binary, machine and options of a board an emulator runs, as chip.cmake sets them."""
    options = list(emulator["options"])
    if "ram_global" in emulator:
        if "RAM" not in lengths:
            raise Failure("the board's emulator sets `%s` to the RAM link region's length, and chip `%s` links "
                          "no RAM" % (emulator["ram_global"], view.chip.name))
        options.extend(["-global", "%s=%d" % (emulator["ram_global"], lengths["RAM"])])
    out = ["set(KICKOS_QEMU_BINARY \"%s\")\n" % emulator["qemu"],
           "set(KICKOS_QEMU_MACHINE \"%s\")\n" % emulator["machine"],
           "set(KICKOS_QEMU_OPTIONS \"%s\")\n" % ";".join(options)]
    if "gicv3_machine" in emulator:
        out.append("set(KICKOS_QEMU_GICV3_MACHINE \"%s\")\n" % emulator["gicv3_machine"])
    return out


def selector_value(selector):
    """The value a console pin's mux field takes for `selector`: the number it ends in, a
    one-letter selector's place from `a`, or None for a selector of more letters and no number,
    which names no mux value."""
    number = NAME_NUMBER.fullmatch(selector)
    if number is not None:
        return int(number.group(1))
    if len(selector) == 1:
        return ord(selector) - ord("a")
    return None


def instance_base(device, index):
    if index is None:
        return device.window[0]
    return device.window[0] + index * device.stride


def window_of(chip, path):
    """(base, size) of the window a /dev path names, or None where it names none."""
    parts = path.split("/")
    device = chip.devices[parts[2]]
    if len(parts) == 3:
        return device.window
    leaf = parts[3]
    if leaf in device.channel_windows:
        return device.channel_windows[leaf]
    if leaf in device.channels:
        return None
    return instance_base(device, int(leaf)), device.window[1]


def port_bit(chip, pin):
    """(port device, instance or None, bit) of a pin's `gpio` function, which admission requires
    of every pin board_pins.h names."""
    parts = chip.pins[pin]["gpio"].split(".")
    index = None
    if len(parts) == 3:
        index = int(parts[1])
    return chip.devices[parts[0]], index, int(parts[-1])


def raw_port_number(chip, pin):
    device, index, bit = port_bit(chip, pin)
    if index is not None:
        return index
    number = NAME_NUMBER.fullmatch(device.name)
    if number is not None:
        return int(number.group(1))
    return None


def port_number(chip, pin):
    """The number of a pin's port: the instance its `gpio` function names where the port device
    repeats, else the number the device's name ends in, else None on a chip of one port. A chip
    whose port devices would share a number is refused."""
    owners = {}
    for other in chip.pins:
        if "gpio" not in chip.pins[other]:
            continue
        number = raw_port_number(chip, other)
        if number is None:
            number = 0
        name = port_bit(chip, other)[0].name
        if owners.setdefault(number, name) != name:
            raise Failure("chip `%s` numbers port devices `%s` and `%s` both %d"
                          % (chip.name, owners[number], name, number))
    return raw_port_number(chip, pin)


def pinmux_port_bit(chip, pin):
    """(port, bit) by which arch_pinmux_set names a pin, the port 0 on a chip of one port."""
    number = port_number(chip, pin)
    if number is None:
        number = 0
    return number, port_bit(chip, pin)[2]


def kernel_pins(board):
    """[(port, bit)] of the pins the kernel holds: the console's, its LED's, the reserved."""
    pins = [pin for role, pin, selector in board.console_pins]
    if board.kernel_led is not None:
        pins.append(board.kernel_led[0])
    pins.extend(board.reserved)
    return [pinmux_port_bit(board.chip, pin) for pin in pins]


def pin_values(chip, stem, pin):
    """[(macro, value, ref)] of the port a pin's `gpio` function names: its number, where the chip
    has more than one, the instance's base and the pin's bit."""
    device, index, bit = port_bit(chip, pin)
    values = []
    number = port_number(chip, pin)
    if number is not None:
        values.append((stem + "_PORT", number, None))
    values.append((stem + "_PORT_BASE", instance_base(device, index), None))
    values.append((stem + "_BIT", bit, None))
    return values


def board_values(view):
    """[(macro, value, ref)] of board_pins.h: the console device, each console pin's port, bit and
    the value its selector and input select take, and the kernel's LED."""
    board = view.board
    if board is None:
        return []
    chip = view.chip
    values = []
    if board.console is not None:
        window = window_of(chip, board.console)
        if window is not None:
            values.append(("KICKOS_BOARD_CONSOLE_BASE", window[0], board.console))
            values.append(("KICKOS_BOARD_CONSOLE_SIZE", window[1], None))
    for role, pin, selector in board.console_pins:
        stem = "KICKOS_BOARD_CONSOLE_%s" % role.upper()
        values.extend(pin_values(chip, stem, pin))
        select = selector_value(selector)
        if select is not None:
            values.append((stem + "_SELECT", select, "%s %s: %s" % (pin, selector, chip.pins[pin][selector])))
        detail = chip.pin_details.get((pin, selector))
        if detail is not None and detail[0] is not None:
            values.append((stem + "_INPUT_SELECT", detail[0], detail[1]))
    if board.kernel_led is not None:
        pin, active, kind = board.kernel_led
        values.extend(pin_values(chip, "KICKOS_BOARD_LED", pin))
        if kind == "addressable":
            values.append(("KICKOS_BOARD_LED_ADDRESSABLE", 1, None))
        else:
            low = 0
            if active == "low":
                low = 1
            values.append(("KICKOS_BOARD_LED_ACTIVE_LOW", low, None))
    return values


def emit_board(view):
    banner = "Generated by kickos_compose chip from platform/%s/chip.yaml" % view.chip.name
    if view.board is not None:
        banner += " and platform/%s/%s.yaml" % (view.chip.name, view.board.name)
    lines = ["/* %s */\n" % banner, "\n#ifndef KICKOS_BOARD_PINS_H\n#define KICKOS_BOARD_PINS_H\n\n"]
    for macro, value, ref in board_values(view):
        shown = "%d" % value
        if macro.endswith("_BASE") or macro.endswith("_SIZE"):
            shown = "0x%X" % value
        lines.append("#define %s %s%s\n" % (macro, shown, comment(ref, "c")))
    if view.board is not None:
        pins = "".join(" PIN(%d, %d)" % pin for pin in kernel_pins(view.board))
        lines.append("#define KICKOS_BOARD_KERNEL_PINS(PIN)%s /* PIN(port, bit) */\n" % pins)
        pins = "".join(" PIN(%d, %d)" % pinmux_port_bit(view.chip, pin)
                       for pin, functions in view.chip.pins.items() if "gpio" in functions)
        lines.append("#define KICKOS_CHIP_PINS(PIN)%s /* PIN(port, bit) */\n" % pins)
    lines.append("\n#endif\n")
    return "".join(lines)


def generate(view):
    """{output name: text}"""
    return {
        "chip_mmap.h": emit_mmap(view),
        "chip_limits.h": emit_limits(view),
        "irq.h": emit_irq(view),
        "chip_layout.h": emit_layout(view),
        "chip_tables.h": emit_tables(view),
        "chip.cmake": emit_cmake(view),
        "board_pins.h": emit_board(view),
        "board_buses.h": emit_buses(view),
    }


def write_if_changed(path, text):
    if os.path.isfile(path):
        with open(path, encoding="ascii") as stream:
            if stream.read() == text:
                return
    os.makedirs(os.path.dirname(path), exist_ok=True)
    fresh = path + ".new"
    with open(fresh, "w", encoding="ascii", newline="\n") as stream:
        stream.write(text)
    os.replace(fresh, path)


def write(outputs, include_dir, chip_dir):
    for name in INCLUDE_OUTPUTS:
        write_if_changed(os.path.join(include_dir, "kickos", name), outputs[name])
    for name in CHIP_OUTPUTS:
        write_if_changed(os.path.join(chip_dir, name), outputs[name])


PROBE = """
#include <type_traits>
#include <stdint.h>

namespace kickos_compare_absent
{
    struct absent
    {
    };
%s}

namespace kickos_compare
{
    template <typename T>
    constexpr bool constant(T const& v, unsigned long long want)
    {
        if constexpr (std::is_same_v<T, ::kickos_compare_absent::absent>)
        {
            return true;
        }
        else
        {
            return std::is_same_v<T, uintptr_t> and static_cast<unsigned long long>(v) == want;
        }
    }

    template <typename E, typename T>
    constexpr bool enumerator(T const& v, long long want)
    {
        if constexpr (std::is_same_v<T, ::kickos_compare_absent::absent>)
        {
            return true;
        }
        else
        {
            return std::is_same_v<T, E> and static_cast<long long>(v) == want;
        }
    }
}
"""


def compare_source(view, header, kind):
    """A C++ source asserting what `header` declares against the generated values."""
    chip = view.chip
    lines = ["#include \"%s\"\n" % os.path.abspath(header)]
    if kind == "chip_limits.h":
        for macro, value, ref in limit_values(view):
            lines.append("#if !defined(%s)\n#error \"%s is not defined\"\n" % (macro, macro))
            lines.append("#elif %s != %d\n#error \"%s is not %d\"\n#endif\n" % (macro, value, macro, value))
        return "".join(lines)
    if kind == "chip_mmap.h":
        symbols = mmap_symbols(view)
        scope = "%s::mmap" % namespace(chip)
    else:
        symbols = irq_symbols(view)
        scope = "%s::irq" % namespace(chip)
    absent = absent_from(view, header, kind)
    probes = "".join("    inline constexpr absent %s{};\n" % symbol for symbol in absent)
    lines.append(PROBE % probes)
    lines.append("namespace %s\n{\n    using namespace ::kickos_compare_absent;\n}\n" % scope)
    if kind == "chip_mmap.h":
        for symbol, value, ref in symbols:
            lines.append("static_assert(kickos_compare::constant(%s::%s, 0x%Xull), \"%s\");\n"
                         % (scope, symbol, value, symbol))
        return "".join(lines)
    enum = line_enum(chip)
    name = enum.split(" ")[0]
    lines.append("namespace kickos_compare_generated\n{\n    enum %s\n    {\n" % enum)
    for symbol, number, ref in symbols:
        lines.append("        %s = %d,\n" % (symbol, number))
    lines.append("    };\n}\n")
    lines.append("static_assert(std::is_same_v<std::underlying_type_t<%s::%s>, "
                 "std::underlying_type_t<kickos_compare_generated::%s>>, \"the underlying type of %s\");\n"
                 % (scope, name, name, name))
    for symbol, number, ref in symbols:
        lines.append("static_assert(kickos_compare::enumerator<%s::%s>(%s::%s, %d), \"%s\");\n"
                     % (scope, name, scope, symbol, number, symbol))
    return "".join(lines)


def absent_from(view, header, kind):
    """The generated symbols `header` does not spell, which the compare leaves unasserted."""
    with open(header, encoding="utf-8") as stream:
        text = COMMENT.sub(" ", stream.read())
    return [name for name in compare_names(view, kind) if not re.search(r"\b%s\b" % re.escape(name), text)]


def compare_names(view, kind):
    if kind == "chip_limits.h":
        return [macro for macro, value, ref in limit_values(view)]
    if kind == "chip_mmap.h":
        return [symbol for symbol, value, ref in mmap_symbols(view)]
    return [symbol for symbol, value, ref in irq_symbols(view)]


def compared_kind(header):
    kind = os.path.basename(header)
    if kind not in ("chip_mmap.h", "irq.h", "chip_limits.h"):
        raise Failure("--compare takes a chip_mmap.h, irq.h or chip_limits.h, not %s" % kind)
    return kind


def compare(view, header, compiler):
    """None when `header` agrees with the generated values, else the compiler's diagnostics."""
    kind = compared_kind(header)
    with tempfile.TemporaryDirectory(prefix="kickos-compare-") as scratch:
        source = os.path.join(scratch, "compare.cc")
        with open(source, "w", encoding="ascii", newline="\n") as stream:
            stream.write(compare_source(view, header, kind))
        try:
            result = subprocess.run(compiler + ["-std=c++20", "-fsyntax-only", "-x", "c++", source],
                                    capture_output=True, text=True, timeout=300)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise Failure("the compiler %s did not run: %s" % (" ".join(compiler), error))
    if result.returncode != 0:
        return (result.stdout + result.stderr).strip() or "the compiler failed with no output"
    return None


def run(path, arch, include_dir=None, chip_dir=None, header=None, compiler=None):
    """(report, lines to print, exit status)"""
    report = Report()
    chip, board = read(path, report)
    if report.refusals or chip is None:
        return report, [], None
    view = view_of(chip, arch, board)
    outputs = generate(view)
    if header is None:
        write(outputs, include_dir, chip_dir)
        return report, [], 0
    kind = compared_kind(header)
    said = []
    absent = absent_from(view, header, kind)
    if not compare_names(view, kind):
        said.append("kickos_compose: the generated %s declares nothing to compare %s with" % (kind, header))
        return report, said, 1
    if len(absent) == len(compare_names(view, kind)):
        said.append("kickos_compose: %s declares none of the generated %s's symbols" % (header, kind))
        return report, said, 1
    failure = compare(view, header, compiler)
    if absent:
        said.append("kickos_compose: %s does not declare %s, which the generated %s adds"
                    % (header, ", ".join(absent), kind))
    if failure is not None:
        said.append("kickos_compose: %s disagrees with the generated %s:" % (header, kind))
        said.append(failure)
        return report, said, 1
    said.append("kickos_compose: %s agrees with the generated %s" % (header, kind))
    return report, said, 0
