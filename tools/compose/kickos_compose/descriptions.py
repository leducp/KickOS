# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The chip and board description files under platform/: platform/<chip>/chip.yaml and
# platform/<chip>/<board>.yaml.

import os
import re

from ruamel.yaml.nodes import MappingNode

from .subset import (
    ACCESS, C_IDENTIFIER, FILE_NAME, FUNCTION, GPIO_FUNCTION, IDENTIFIER, LINE_ENUM, NAMESPACE, PIN, REGION,
    SELECTOR, File, Report, index_below, line_of,
)

UNITS = ("pmsav7", "pmsav8", "pmsav6", "pmp", "rxmpu", "sysmpu", "mprotect", "mmu", "none")

# The versions of the chip and board file formats this tool reads.
CHIP_VERSIONS = (1,)
BOARD_VERSIONS = (1,)
CHIP_FIELDS = (
    "version", "chip", "manual", "arch", "protection", "cores", "clusters_coherent", "partition_gate",
    "data_cache", "interrupts", "cycle_counter", "c", "reserved", "devices", "memory", "pins", "esptool_image",
)
PROTECTION_FIELDS = (
    "unit", "covers_devices", "memory_type", "page", "device_gate", "bus_gate", "privilege", "io_ports", "driven",
)
CLUSTER_FIELDS = ("arch", "count", "smp", "protection", "line_offset")
DEVICE_FIELDS = (
    "window", "ports", "channels", "blocks", "count", "stride", "lines", "bus_master", "owner", "sysreg", "host",
    "privileged_registers", "cluster", "ref", "symbol", "gate_register",
)
MEMORY_FIELDS = ("size", "base", "at", "cluster", "arena", "link", "ref", "symbol")
INTERRUPT_FIELDS = ("count", "soft_only_from", "free_from", "vectors")
CYCLE_COUNTER_FIELDS = ("hz", "glitches")
# The kinds of `partition_gate`: a region gate per security mode, a register per peripheral, and
# one the tree programs nowhere.
GATE_KINDS = ("apm", "accessctrl", "rdc")
GATE_FIELDS = ("regions", "ranges", "memory", "kernel")
# The width of each architecture's addresses, which a window the generated headers name must fit.
ADDRESS_BITS = {
    "armv6m": 32, "armv7m": 32, "rv32imac": 32, "rxv3": 32, "lx6": 32, "armv8a": 64, "rv64imac": 64,
    "x86_64": 64, "sim": 64,
}
# Printable ASCII, and nothing that ends a C comment or continues a C++ one.
TEXT = re.compile(r"[ -~]*")
BOARD_FIELDS = (
    "version", "board", "chip", "console", "leds", "parts", "buses", "reserved_pins", "memory", "emulator",
)
EMULATOR_FIELDS = ("qemu", "machine", "gicv3_machine", "options", "ram_global")
# One word of an emulator's command line, with nothing a shell or a CMake list would split or expand.
WORD = re.compile(r"[A-Za-z0-9_.,=+:/-]+")
BOARD_MEMORY_FIELDS = ("size", "base", "cluster", "link", "ref", "symbol")
# A pin function stated as a mapping: the value its device's input-select register takes to pick
# this pin, where the device has one.
PIN_FUNCTION_FIELDS = ("function", "input_select", "ref")
# A level LED lights at its `active` level; an addressable one is sent its state as data.
LED_KINDS = ("level", "addressable")


class Window:
    """`count` instances `stride` apart, labelled `<label>/<k>` when there is more than one."""

    def __init__(self, space, label, base, size, reach, node, count=1, stride=0):
        self.space = space
        self.label = label
        self.base = base
        self.size = size
        self.reach = reach
        self.node = node
        self.count = count
        self.stride = stride

    def start(self, k):
        return self.base + k * self.stride

    def name(self, k):
        if self.count == 1:
            return self.label
        return "%s/%d" % (self.label, k)


class Device:
    def __init__(self, name, node):
        self.name = name
        self.node = node
        self.window = None
        self.ports = None
        self.channels = {}
        self.channel_windows = {}
        self.count = None
        self.stride = None
        self.lines = {}
        self.sources = {}
        self.owner = None
        self.sysreg = False
        self.host = False
        self.bus_master = False
        self.cluster = None
        # The offset of each privileged register, in the chip file's order.
        self.registers = []
        self.ref = None
        self.symbol = None
        # [Block], in the chip file's order.
        self.blocks = []
        # The offset of the register a per-peripheral partition gate assigns it by, or None.
        self.gate_register = None
        self.gate_register_node = None
        # {channel or line: Named}
        self.channel_names = {}
        self.line_names = {}


class Gate:
    """One unit of a region `partition_gate`."""

    def __init__(self, name):
        self.name = name
        # The regions it holds, the reset catch-all among them.
        self.regions = None
        # The device windows it fronts, and the memory it fronts for every node but node 0, each a
        # (base, size).
        self.ranges = []
        self.memory = []
        # What every node but node 0 has its kernel hold through it, each a (base, size, access).
        self.kernel = []


class PartitionGate:
    def __init__(self, kind):
        self.kind = kind
        # The device a per-peripheral gate is programmed through, or None.
        self.device = None
        # {name: Gate} of a region gate, in the chip file's order.
        self.gates = {}
        # {register offset: name} of a per-peripheral gate's registers no node is ever assigned.
        self.never_assigned = {}


class Named:
    """The `ref` and `symbol` an entry states, each None when it states none."""

    def __init__(self, ref=None, symbol=None):
        self.ref = ref
        self.symbol = symbol


class Block:
    def __init__(self, name, offset, named):
        self.name = name
        self.offset = offset
        self.ref = named.ref
        self.symbol = named.symbol


class Memory:
    def __init__(self, name, size, bases, named):
        self.name = name
        self.size = size
        self.host_placed = False
        # {cluster or PART: base}
        self.bases = bases
        self.ref = named.ref
        self.symbol = named.symbol
        # (region, access), or None
        self.link = None


class Board:
    def __init__(self, chip, console):
        self.chip = chip
        self.console = console
        # [(role, pin, selector)] of the console's pins, in the board file's order.
        self.console_pins = []
        # (pin, active, kind) of the LED the kernel owns, or None; active is None when addressable.
        self.kernel_led = None
        # [pin] the board reserves, in the board file's order.
        self.reserved = []
        # [Memory] soldered on the board, in the board file's order.
        self.memory = []
        # {pin: what the board spends it on}
        self.wired = {}
        # [Bus], in the board file's order.
        self.buses = []
        # {field: value} of `emulator`, or None where no emulator runs the board.
        self.emulator = None


class Bus:
    def __init__(self, name, path, function_prefix, pins, chip_selects):
        self.name = name
        self.path = path
        self.function_prefix = function_prefix
        self.pins = pins
        self.chip_selects = chip_selects


class Protection:
    def __init__(self, unit):
        self.unit = unit
        self.covers_devices = None
        self.memory_type = None
        self.page = None
        self.privilege = True
        self.io_ports = None
        self.driven = True


class Chip:
    def __init__(self, name, path):
        self.name = name
        self.path = path
        self.clusters = []
        self.multi_arch = False
        self.data_cache = True
        self.devices = {}
        # {cluster or PART: Protection} for each view whose unit is known.
        self.protection = {}
        # {cluster or PART: (base, size) of the memory entry its user arena is carved from}
        self.arena = {}
        # {cluster or PART: (kind, size, ranges)} for each view whose unit has a `device_gate`,
        # ranges being a list of (base, size), or None when the gate covers every window.
        self.gates = {}
        # {pin: {selector: function}}, or None when `pins` could not be read.
        self.pins = {}
        # {(pin, selector): (input_select, ref)} of each function stated as a mapping.
        self.pin_details = {}
        self.manual = None
        self.arch = None
        # {cluster: arch} for a part whose clusters carry their own
        self.cluster_arch = {}
        # {cluster: line_offset}
        self.line_offsets = {}
        # {field: value} and {field: ref} of `interrupts` and `cycle_counter`
        self.interrupts = {}
        self.cycle_counter = {}
        self.refs = {}
        self.namespace = None
        self.line_enum = None
        # The LED kind the chip's code drives as the kernel's.
        self.led_kind = "level"
        # [Memory], in the chip file's order.
        self.memory = []
        # [Window] of every device and memory entry, and {(view, region): node} of each link region.
        self.windows = []
        self.links = {}
        # {cluster or PART: whether its cores share one kernel's memory coherently}
        self.smp = {}
        # The PartitionGate, or None.
        self.partition_gate = None
        # The esptool elf2image options of the image its ROM boots, or None where the ROM boots none.
        self.esptool_image = None


# A part with no clusters reaches its windows as one view, named by None.
PART = None


def check_chip(path, text, report):
    """The chip a board resolves against, or None when its file cannot be read at all."""
    f = File(path, report)
    root = f.load(text)
    if root is None:
        return None
    top = f.fields(root, "the chip file", CHIP_FIELDS, ("version", "chip", "manual", "interrupts", "devices"))
    if top is None or not f.version(top, "the chip file", CHIP_VERSIONS):
        return None
    folder = os.path.basename(os.path.dirname(os.path.abspath(path)))
    if "chip" in top:
        name = f.name(top["chip"], "`chip`", FILE_NAME)
        if name is not None and name != folder:
            f.refuse(top["chip"], "chip.name-mismatch",
                     "`chip: %s` sits in platform/%s/, which names it `%s`" % (name, folder, folder))
    chip = Chip(folder, path)

    for flag in ("clusters_coherent", "data_cache"):
        if flag in top:
            setting = f.boolean(top[flag], "`%s`" % flag)
            if flag == "data_cache" and setting is False:
                chip.data_cache = False
    if "esptool_image" in top:
        chip.esptool_image = words(f, top["esptool_image"], "`esptool_image`")
    if "arch" in top:
        chip.arch = f.name(top["arch"], "`arch`", IDENTIFIER)
    if "manual" in top:
        chip.manual = check_text(f, top["manual"], "`manual`")
    if "c" in top:
        check_c(f, top["c"], chip)
    if "cycle_counter" in top:
        counter = f.fields(top["cycle_counter"], "`cycle_counter`", CYCLE_COUNTER_FIELDS, ())
        if counter is not None:
            if "hz" in counter:
                chip.cycle_counter["hz"], chip.refs["hz"] = check_valued(
                    f, counter["hz"], "`cycle_counter` hz", lambda node, what: f.integer(node, what, 32))
            if "glitches" in counter:
                chip.cycle_counter["glitches"], chip.refs["glitches"] = check_valued(
                    f, counter["glitches"], "`cycle_counter` glitches", f.boolean)

    translates = check_cores(f, top, root, chip)
    if "clusters_coherent" in top and not chip.multi_arch:
        f.refuse(top["clusters_coherent"], "form.inapplicable",
                 "`clusters_coherent` is for a part whose clusters each carry their own `arch`")
    devices = {}
    if "devices" in top:
        devices = f.mapping(top["devices"], "`devices`")
    windows = []
    if devices is not None:
        for device_name, (key, value) in devices.items():
            device = check_device(f, chip, key, value, windows)
            if device is not None:
                chip.devices[device_name] = device

    if "partition_gate" in top:
        chip.partition_gate = check_partition_gate(f, top["partition_gate"], chip, devices)

    check_memory(f, top, root, chip, translates, windows)
    if "pins" in top:
        check_chip_pins(f, top["pins"], chip, devices)
    check_overlaps(f, chip, windows)
    check_gate_edges(f, chip, windows)
    chip.windows = windows
    if "interrupts" in top:
        check_interrupts(f, top["interrupts"], chip)
    check_symbols(f, chip)
    check_reserved(f, top, root, chip)
    check_address_width(f, chip, windows)
    return chip


def check_partition_gate(f, node, chip, devices):
    gate = f.fields(node, "`partition_gate`", ("kind", "device", "gates", "never_assigned"), ("kind",))
    if gate is None or "kind" not in gate:
        return None
    kind = f.enum(gate["kind"], "`partition_gate` kind", GATE_KINDS)
    if kind is None:
        return None
    found = PartitionGate(kind)
    named = []
    if "device" in gate:
        found.device = f.name(gate["device"], "`partition_gate` device", IDENTIFIER)
        named.append((found.device, gate["device"]))
    units = {}
    if "gates" in gate:
        units = f.mapping(gate["gates"], "`partition_gate` gates") or {}
    for name, (key, value) in units.items():
        what = "partition gate `%s`" % name
        named.append((f.name(key, what, IDENTIFIER), key))
        values = f.fields(value, what, GATE_FIELDS, ("regions", "ranges"))
        if values is None:
            continue
        unit = Gate(name)
        if "regions" in values:
            unit.regions, _ = check_valued(f, values["regions"], "%s regions" % what,
                                           lambda item, label: check_size(f, item, label, 8))
        for field in ("ranges", "memory"):
            if field in values:
                setattr(unit, field, check_gate_ranges(f, values[field], "%s %s range" % (what, field)) or [])
        items = []
        if "kernel" in values:
            items = f.sequence(values["kernel"], "%s kernel" % what) or []
        for item in items:
            held = f.fields(item, "%s kernel range" % what, ("window", "access", "ref"), ("window", "access"))
            if held is None or "window" not in held or "access" not in held:
                continue
            window = check_range(f, held["window"], "%s kernel window" % what, "size")
            access = f.name(held["access"], "%s kernel access" % what, ACCESS)
            if "ref" in held:
                check_text(f, held["ref"], "%s kernel ref" % what)
            if window is not None and access is not None:
                unit.kernel.append((window[0], window[1], access))
        found.gates[name] = unit
    for target, at in named:
        if target is not None and devices is not None and target not in devices:
            f.refuse(at, "chip.device-unknown",
                     "`partition_gate` names `%s`, which is no device of this chip" % target)
    never = {}
    if "never_assigned" in gate:
        never = f.mapping(gate["never_assigned"], "`partition_gate` never_assigned") or {}
    for name, (key, value) in never.items():
        f.name(key, "`partition_gate` never_assigned", IDENTIFIER)
        offset = f.integer(value, "`partition_gate` never_assigned `%s`" % name, 16)
        if offset is not None:
            found.never_assigned[offset] = name
    claimed = {}
    for name, device in chip.devices.items():
        register = device.gate_register
        if register is None:
            continue
        if register in found.never_assigned:
            f.refuse(device.gate_register_node, "chip.gate-register",
                     "device `%s` names gate register 0x%X, which is %s's, and `partition_gate` never_assigned "
                     "names it" % (name, register, found.never_assigned[register]))
        elif register in claimed:
            f.refuse(device.gate_register_node, "chip.gate-register",
                     "device `%s` names gate register 0x%X, which device `%s` names too, so one register would "
                     "assign both" % (name, register, claimed[register]))
        else:
            claimed[register] = name
    return found


def check_valued(f, node, what, read):
    """(value, ref): `node` read as a value, or a mapping of its `value` and the `ref` beside it."""
    if isinstance(node, MappingNode):
        values = f.fields(node, what, ("value", "ref"), ("value",))
        if values is None or "value" not in values:
            return None, None
        return read(values["value"], what), check_ref(f, values)
    return read(node, what), None


def host_runs(chip):
    """Whether the host runs the part, which its `mprotect` unit says."""
    return any(protection.unit == "mprotect" for protection in chip.protection.values())


def protecting_views(chip):
    """The views whose unit is a region unit or `mmu`."""
    return [view for view, protection in chip.protection.items() if protection.unit not in (None, "none")]


def check_reserved(f, top, root, chip):
    """A protecting part holds a window for life, or says `reserved: none`."""
    stated_none = False
    if "reserved" in top:
        stated_none = f.enum(top["reserved"], "`reserved`", ("none",)) == "none"
    for view in protecting_views(chip):
        held = [device.name for device in chip.devices.values()
                if device.owner == "kernel" and (device.window is not None or device.channel_windows)
                and view in device_reach(chip, device.cluster)]
        where = "the part"
        if view is not PART:
            where = "cluster `%s`" % view
        if held and stated_none:
            f.refuse(top["reserved"], "chip.reserved",
                     "`reserved: none`, and %s holds /dev/%s for life" % (where, held[0]))
            return
        if not held and not stated_none:
            f.refuse(root, "chip.reserved",
                     "%s protects memory and the kernel holds no window of it; mark the devices the kernel "
                     "holds `owner: kernel`, or state `reserved: none`" % where)
            return


def check_address_width(f, chip, windows):
    """A window past the addresses its view's architecture reaches."""
    for w in windows:
        if w.space != "mem":
            continue
        for view in w.reach:
            arch = chip.cluster_arch.get(view, chip.arch)
            bits = ADDRESS_BITS.get(arch)
            end = w.start(w.count - 1) + w.size
            if bits is not None and end > 1 << bits:
                f.refuse(w.node, "chip.address-width", "%s ends at 0x%X, past the %d-bit addresses of `%s`"
                         % (w.name(w.count - 1), end, bits, arch))
                break


def check_text(f, node, what):
    """A string the generated headers carry in a comment."""
    text = f.string(node, what)
    if text is None:
        return None
    if not TEXT.fullmatch(text) or "*/" in text or "\\" in text:
        f.refuse(node, "form.text",
                 "%s `%s` is not printable ASCII free of `*/` and backslashes, which a C comment carries"
                 % (what, text.encode("ascii", "backslashreplace").decode("ascii")))
        return None
    return text


def device_symbols(device):
    """[(symbol, value, ref)] of the device's windows, their sizes, blocks and stride, in the chip
    file's order."""
    up = device.name.upper()
    symbols = []
    if device.window is not None and device.count is not None:
        symbols.append((device.symbol or up + "0_BASE", device.window[0], device.ref))
        symbols.append((up + "_STRIDE", device.stride, device.ref))
        symbols.append((up + "_SIZE", device.window[1], device.ref))
    elif device.window is not None or device.ports is not None:
        window = device.window or device.ports
        symbols.append((device.symbol or up + "_BASE", window[0], device.ref))
        symbols.append((up + "_SIZE", window[1], device.ref))
    for channel, window in device.channel_windows.items():
        named = device.channel_names[channel]
        symbol = named.symbol or "%s_%s_BASE" % (up, channel.upper())
        symbols.append((symbol, window[0], named.ref or device.ref))
        symbols.append(("%s_%s_SIZE" % (up, channel.upper()), window[1], named.ref or device.ref))
    for block in device.blocks:
        symbol = block.symbol or "%s_%s_BASE" % (up, block.name.upper())
        symbols.append((symbol, device.window[0] + block.offset, block.ref))
    return symbols


def device_layout(device):
    """[(macro, value, ref)] of the device's chip_layout.h definitions."""
    name = device.name.upper()
    values = []
    if device.window is not None and device.count is not None:
        values.append(("KICKOS_LAYOUT_%s0_BASE" % name, device.window[0], device.ref))
        values.append(("KICKOS_LAYOUT_%s_STRIDE" % name, device.stride, None))
        values.append(("KICKOS_LAYOUT_%s_SIZE" % name, device.window[1], None))
    elif device.window is not None:
        values.append(("KICKOS_LAYOUT_%s_BASE" % name, device.window[0], device.ref))
        values.append(("KICKOS_LAYOUT_%s_SIZE" % name, device.window[1], None))
    for channel, window in device.channel_windows.items():
        values.append(("KICKOS_LAYOUT_%s_%s_BASE" % (name, channel.upper()), window[0],
                       device.channel_names[channel].ref or device.ref))
    return values


def memory_layout(entry, base):
    """[(macro, value, ref)] of the memory entry's chip_layout.h definitions."""
    name = entry.name.upper()
    return [("KICKOS_LAYOUT_%s_BASE" % name, base, entry.ref),
            ("KICKOS_LAYOUT_%s_SIZE" % name, entry.size, None)]


def line_macro(symbol):
    """The chip_layout.h definition of the line whose C name is `symbol`."""
    return "KICKOS_LAYOUT_LINE_" + symbol


def memory_symbol(entry):
    return entry.symbol or entry.name.upper() + "_BASE"


def line_symbols(device):
    """[(symbol, source number, ref)] of the device's lines, in the chip file's order."""
    symbols = []
    for line, number in device.sources.items():
        named = device.line_names[line]
        symbols.append((named.symbol or "%s_%s" % (device.name.upper(), line.upper()), number, named.ref))
    return symbols


def check_symbols(f, chip):
    """Two entries whose C names are one name, in the mmap namespace, the line enum or the layout."""
    names = {}
    lines = {}
    layout = {}
    for device in chip.devices.values():
        for symbol, value, ref in device_symbols(device):
            refuse_collision(f, names, symbol, device.node, "/dev/%s" % device.name)
        for symbol, value, ref in line_symbols(device):
            fresh = symbol not in lines
            refuse_collision(f, lines, symbol, device.node, "/dev/%s" % device.name)
            if fresh:
                refuse_collision(f, layout, line_macro(symbol), device.node, "/dev/%s" % device.name)
        for symbol, value, ref in device_layout(device):
            refuse_collision(f, layout, symbol, device.node, "/dev/%s" % device.name)
    for entry in chip.memory:
        refuse_collision(f, names, memory_symbol(entry), entry.node, "memory `%s`" % entry.name)
        for symbol, value, ref in memory_layout(entry, 0):
            refuse_collision(f, layout, symbol, entry.node, "memory `%s`" % entry.name)


def refuse_collision(f, seen, symbol, node, what):
    if symbol in seen:
        f.refuse(node, "chip.symbol-collision",
                 "%s is named `%s` in C, as %s on line %d is"
                 % (what, symbol, seen[symbol][1], line_of(seen[symbol][0])))
        return
    seen[symbol] = (node, what)


def check_ref(f, values):
    """The `ref` among `values`, or None."""
    if "ref" not in values:
        return None
    return check_text(f, values["ref"], "`ref`")


def check_named(f, values, what):
    """The `ref` and `symbol` among `values`."""
    named = Named(check_ref(f, values))
    if "symbol" in values:
        named.symbol = f.name(values["symbol"], "%s symbol" % what, C_IDENTIFIER)
    return named


def check_c(f, node, chip):
    values = f.fields(node, "`c`", ("namespace", "line_enum", "led"), ())
    if values is None:
        return
    if "namespace" in values:
        chip.namespace = f.name(values["namespace"], "`c` namespace", NAMESPACE)
    if "line_enum" in values:
        chip.line_enum = f.name(values["line_enum"], "`c` line_enum", LINE_ENUM)
    if "led" in values:
        chip.led_kind = f.enum(values["led"], "`c` led", LED_KINDS)


def check_interrupts(f, node, chip):
    """`count` lines a controller has, the two thresholds below it, and every device's line under it."""
    values = f.fields(node, "`interrupts`", INTERRUPT_FIELDS, ("count",))
    if values is None:
        return
    for field in INTERRUPT_FIELDS:
        if field in values:
            chip.interrupts[field], chip.refs[field] = check_valued(
                f, values[field], "`interrupts` %s" % field, lambda n, what: f.integer(n, what, 16))
    count = chip.interrupts.get("count")
    if count is None:
        return
    for field in ("soft_only_from", "free_from"):
        value = chip.interrupts.get(field)
        if value is not None and value >= count:
            f.refuse(values[field], "chip.line-range",
                     "`interrupts` %s %d is past the %d lines `count` gives" % (field, value, count))
    for device in chip.devices.values():
        for line, number in device.sources.items():
            if number >= count:
                f.refuse(device.lines[line], "chip.line-range",
                         "line `/dev/%s/%s` %d is past the %d lines `interrupts` count gives"
                         % (device.name, line, number, count))


def check_protection(f, node, what, chip, view):
    """True when the unit translates, False when it does not, None when unknown."""
    values = f.fields(node, what, PROTECTION_FIELDS, ("unit",))
    if values is None or "unit" not in values:
        return None
    unit = f.enum(values["unit"], "%s unit" % what, UNITS)
    protection = Protection(unit)
    for field in ("covers_devices", "memory_type"):
        if unit is not None and unit != "none" and field not in values:
            f.refuse(node, "form.missing", "%s needs `%s`" % (what, field))
    for flag in ("covers_devices", "memory_type", "privilege", "io_ports", "driven"):
        if flag in values:
            setattr(protection, flag, f.boolean(values[flag], "%s `%s`" % (what, flag)))
    if "driven" in values and unit in ("mmu", "none"):
        f.refuse(values["driven"], "form.inapplicable",
                 "%s says whether a build drives its unit, which only a region unit leaves to the port" % what)
    if unit == "mmu" and "page" not in values:
        f.refuse(node, "form.missing", "%s needs `page`, the unit translating" % what)
    if "page" in values:
        page = check_size(f, values["page"], "%s `page`" % what, 32)
        if page is not None and page & (page - 1):
            f.refuse(values["page"], "chip.page-size", "%s `page` 0x%X is not a power of two" % (what, page))
            page = None
        if page is not None and unit is not None and unit != "mmu":
            f.refuse(values["page"], "chip.page-unit", "%s has a `page`, which only a translating unit has" % what)
        protection.page = page
    if unit is not None:
        chip.protection[view] = protection
    if "device_gate" in values:
        gate = f.fields(values["device_gate"], "%s `device_gate`" % what, ("kind", "size", "per_thread", "ranges"),
                        ("kind", "size", "per_thread"))
        if gate is not None:
            kind = None
            size = None
            ranges = None
            if "ranges" in gate:
                ranges = check_gate_ranges(f, gate["ranges"], "%s `device_gate` range" % what)
            if "kind" in gate:
                kind = f.name(gate["kind"], "`device_gate` kind", IDENTIFIER)
            if "size" in gate:
                size = check_size(f, gate["size"], "`device_gate` size", 32)
            if kind is not None and size is not None and (ranges is not None or "ranges" not in gate):
                chip.gates[view] = (kind, size, ranges)
            if "per_thread" in gate:
                f.boolean(gate["per_thread"], "`device_gate` per_thread")
    if "bus_gate" in values:
        gate = f.fields(values["bus_gate"], "%s `bus_gate`" % what, ("kind", "per_thread", "traps"),
                        ("kind", "per_thread", "traps"))
        if gate is not None:
            if "kind" in gate:
                f.name(gate["kind"], "`bus_gate` kind", IDENTIFIER)
            for flag in ("per_thread", "traps"):
                if flag in gate:
                    f.boolean(gate[flag], "`bus_gate` %s" % flag)
    if unit is None:
        return None
    return unit == "mmu"


def check_gate_ranges(f, node, what):
    """The `[base, size]` ranges, or None when one is refused."""
    items = f.sequence(node, "%ss" % what)
    if items is None:
        return None
    if not items:
        f.refuse(node, "form.missing", "%ss lists none; a gate over every device window leaves `ranges` out" % what)
        return None
    ranges = []
    for item in items:
        pair = check_range(f, item, what, "size")
        if pair is not None:
            ranges.append(pair)
    if len(ranges) != len(items):
        return None
    return ranges


def check_count_range(f, node, what):
    pair = f.pair(node, what, "lo", "hi", (16, 16))
    if pair is None:
        return
    lo, hi = pair
    if lo < 1 or hi < lo:
        f.refuse(node, "chip.core-count", "%s `[%d, %d]` is no range of cores" % (what, lo, hi))


def check_cores(f, top, root, chip):
    """Whether each view translates: {cluster or PART: bool}, leaving out what is unknown."""
    translates = {}
    cores = None
    if "cores" in top:
        cores = f.mapping(top["cores"], "`cores`")
    per_cluster = bool(cores) and "count" not in cores and "smp" not in cores
    if cores is not None and not per_cluster:
        values = f.select(cores, top["cores"], "`cores`", ("count", "smp"), ("count",))
        if "count" in values:
            check_count_range(f, values["count"], "`cores` count")
        if "smp" in values:
            chip.smp[PART] = f.boolean(values["smp"], "`cores` smp") is True
    if not per_cluster:
        if "arch" not in top:
            f.refuse(root, "form.missing", "the chip file needs `arch`, or one per cluster in `cores`")
        if "protection" not in top:
            f.refuse(root, "form.missing", "the chip file needs `protection`, or one per cluster in `cores`")
        else:
            unit = check_protection(f, top["protection"], "`protection`", chip, PART)
            if unit is not None:
                translates[PART] = unit
        return translates

    if "protection" in top:
        f.refuse(top["protection"], "form.exclusive",
                 "`protection` is given per cluster in `cores`, so not for the whole part")
    for cluster_name, (key, value) in cores.items():
        cluster = f.name(key, "cluster", IDENTIFIER)
        what = "cluster `%s`" % cluster_name
        values = f.fields(value, what, CLUSTER_FIELDS, ("protection",))
        if cluster is not None:
            chip.clusters.append(cluster)
        if values is None:
            continue
        if "arch" in values:
            arch = f.name(values["arch"], "%s `arch`" % what, IDENTIFIER)
            if cluster is not None and arch is not None:
                chip.cluster_arch[cluster] = arch
            chip.multi_arch = True
            if "arch" in top:
                f.refuse(values["arch"], "form.exclusive",
                         "%s has an `arch`, and so does the whole part" % what)
            if "line_offset" not in values:
                f.refuse(value, "form.missing",
                         "%s carries its own `arch`, so it needs `line_offset`" % what)
        elif "arch" not in top:
            f.refuse(value, "form.missing", "%s needs `arch`, the part naming none" % what)
        if "line_offset" in values:
            offset = f.integer(values["line_offset"], "%s `line_offset`" % what, 16)
            if cluster is not None and offset is not None:
                chip.line_offsets[cluster] = offset
            if "arch" not in values:
                f.refuse(values["line_offset"], "form.inapplicable",
                         "%s has a `line_offset`, which only a cluster with its own `arch` has" % what)
        if "count" in values:
            check_count_range(f, values["count"], "%s count" % what)
        if "smp" in values:
            smp = f.boolean(values["smp"], "%s smp" % what)
            if cluster is not None:
                chip.smp[cluster] = smp is True
        if "protection" in values:
            unit = check_protection(f, values["protection"], "%s protection" % what, chip, cluster)
            if unit is not None and cluster is not None:
                translates[cluster] = unit
    return translates


def check_size(f, node, what, bits):
    size = f.integer(node, what, bits)
    if size is not None and size == 0:
        f.refuse(node, "chip.zero-size", "%s is zero" % what)
        return None
    return size


def check_end(f, node, what, end):
    if end > 1 << 64:
        f.refuse(node, "form.range", "%s ends at 0x%X, past the 64-bit address space" % (what, end))
        return False
    return True


def check_range(f, node, what, second):
    """A `[base, size]`, base 64 bits wide and size 32."""
    pair = f.pair(node, what, "base", second, (64, 32))
    if pair is None:
        return None
    if pair[1] == 0:
        f.refuse(node, "chip.zero-size", "%s has a zero %s" % (what, second))
        return None
    if not check_end(f, node, what, pair[0] + pair[1]):
        return None
    return pair


def check_cluster_ref(f, chip, node, what):
    name = f.name(node, what, IDENTIFIER)
    if name is None:
        return None
    if name not in chip.clusters:
        known = "this part has no clusters"
        if chip.clusters:
            known = "its clusters are %s" % ", ".join(chip.clusters)
        f.refuse(node, "chip.cluster-unknown", "%s `%s` names no cluster; %s" % (what, name, known))
        return None
    return name


def device_reach(chip, cluster):
    if cluster is not None:
        return (cluster,)
    if chip.clusters:
        return tuple(chip.clusters)
    return (PART,)


def check_line_order(f, lines, name):
    """A line's index is its position among its device's lines, so where a device's lines are all
    one prefix and a number, the line numbered k sits at position k."""
    if len(lines) < 2:
        return
    numbered = [re.fullmatch(r"([a-z_]*[a-z_])([0-9]+)", line) for line in lines]
    if any(m is None for m in numbered) or len(set(m.group(1) for m in numbered)) != 1:
        return
    for position, (line, m) in enumerate(zip(lines, numbered)):
        if int(m.group(2)) != position:
            f.refuse(lines[line], "chip.line-order",
                     "line `/dev/%s/%s` is at position %d among its device's lines, and a line's index is its "
                     "position, so `%s%d` must sit at position %d" % (name, line, position, m.group(1),
                                                                      int(m.group(2)), int(m.group(2))))


def check_device(f, chip, key, value, windows):
    name = f.name(key, "device", IDENTIFIER)
    if name is None:
        return None
    what = "device `%s`" % name
    device = Device(name, key)
    values = f.fields(value, what, DEVICE_FIELDS, ())
    if values is None:
        return device

    if "cluster" in values:
        device.cluster = check_cluster_ref(f, chip, values["cluster"], "%s cluster" % what)
    reach = device_reach(chip, device.cluster)

    shapes = [field for field in ("window", "ports", "channels") if field in values]
    for extra in shapes[1:]:
        f.refuse(values[extra], "form.exclusive",
                 "%s has both `%s` and `%s`; it is one of the three" % (what, shapes[0], extra))
    spans = []
    if "window" in values:
        device.window = check_range(f, values["window"], "%s window" % what, "size")
        if device.window is not None:
            spans.append(("its window", device.window[1]))
    if "ports" in values and len(shapes) == 1:
        device.ports = check_range(f, values["ports"], "%s ports" % what, "count")
        if device.ports is not None:
            spans.append(("its port range", device.ports[1]))
            windows.append(Window("port", "/dev/%s" % name, device.ports[0], device.ports[1], reach,
                                  values["ports"]))
    if "channels" in values and len(shapes) == 1:
        channels = f.mapping(values["channels"], "%s channels" % what)
        if channels is not None:
            for channel_name, (ckey, cvalue) in channels.items():
                channel = f.name(ckey, "%s channel" % what, IDENTIFIER)
                cwhat = "channel `%s/%s`" % (name, channel_name)
                cvalues = f.fields(cvalue, cwhat, ("window", "ref", "symbol"), ("window",))
                if channel is None or cvalues is None or "window" not in cvalues:
                    continue
                window = check_range(f, cvalues["window"], "%s window" % cwhat, "size")
                device.channels[channel] = ckey
                device.channel_names[channel] = check_named(f, cvalues, cwhat)
                if window is not None:
                    device.channel_windows[channel] = window
                    spans.append(("the window of channel `%s`" % channel, window[1]))
                    windows.append(Window("mem", "/dev/%s/%s" % (name, channel), window[0], window[1],
                                          reach, cvalues["window"]))

    if "count" in values or "stride" in values:
        if "count" not in values:
            f.refuse(value, "form.missing", "%s has a `stride`, so it needs `count`" % what)
        elif "stride" not in values:
            f.refuse(value, "form.missing", "%s has a `count`, so it needs `stride`" % what)
        elif "window" not in values:
            f.refuse(value, "form.missing", "%s repeats at a `stride`, so it needs the `window` it repeats" % what)
        else:
            device.count = check_size(f, values["count"], "%s count" % what, 16)
            device.stride = check_size(f, values["stride"], "%s stride" % what, 32)
    if device.window is not None and len(shapes) == 1:
        base, size = device.window
        if device.count is not None and device.stride is not None:
            if check_repeat(f, values, what, base, size, device.count, device.stride):
                windows.append(Window("mem", "/dev/%s" % name, base, size, reach, values["window"],
                                      device.count, device.stride))
        elif "count" not in values:
            windows.append(Window("mem", "/dev/%s" % name, base, size, reach, values["window"]))

    if "lines" in values:
        lines = f.mapping(values["lines"], "%s lines" % what)
        if lines is not None:
            for line_name, (lkey, lvalue) in lines.items():
                line = f.name(lkey, "%s line" % what, IDENTIFIER)
                lwhat = "line `%s/%s`" % (name, line_name)
                named = Named()
                if isinstance(lvalue, MappingNode):
                    lvalues = f.fields(lvalue, lwhat, ("number", "ref", "symbol"), ("number",))
                    if lvalues is None or "number" not in lvalues:
                        continue
                    named = check_named(f, lvalues, lwhat)
                    lvalue = lvalues["number"]
                number = f.integer(lvalue, lwhat, 16)
                if line is not None and number is not None:
                    device.lines[line] = lkey
                    device.sources[line] = number
                    device.line_names[line] = named
    check_line_order(f, device.lines, name)
    for line, lkey in device.lines.items():
        if line in device.channels:
            f.refuse(lkey, "chip.name-collision",
                     "`/dev/%s/%s` names both a channel and a line of %s" % (name, line, what))

    for flag in ("bus_master", "sysreg", "host"):
        if flag in values:
            if f.boolean(values[flag], "%s `%s`" % (what, flag)):
                setattr(device, flag, True)
    if device.sysreg and shapes:
        f.refuse(values["sysreg"], "form.exclusive",
                 "%s is reached through system registers alone, so it has no `%s`" % (what, shapes[0]))
    if device.host and (shapes or device.sysreg):
        f.refuse(values["host"], "form.exclusive",
                 "%s is reached through the host, so it has no `%s`" % (what, (shapes + ["sysreg"])[0]))
    if device.host and not host_runs(chip):
        f.refuse(values["host"], "form.inapplicable",
                 "%s is reached through the host, and only a part the host runs, whose unit is `mprotect`, "
                 "has such a device" % what)
    if "owner" in values:
        device.owner = f.enum(values["owner"], "%s owner" % what, ("kernel",))
    if device.owner == "kernel" and "window" not in values and "ports" not in values and not device.sysreg:
        f.refuse(key, "chip.kernel-window",
                 "%s is kernel-owned, so it states its `window` or `ports`, or `sysreg: true`" % what)

    named = check_named(f, values, what)
    device.ref = named.ref
    device.symbol = named.symbol
    if "blocks" in values:
        check_blocks(f, values["blocks"], device, what, shapes, spans)

    if "privileged_registers" in values:
        registers = f.mapping(values["privileged_registers"], "%s privileged_registers" % what)
        if registers is not None:
            for register, (rkey, rvalue) in registers.items():
                f.name(rkey, "%s register" % what, IDENTIFIER)
                rwhat = "register `%s` of %s" % (register, what)
                offset = f.integer(rvalue, rwhat, 16)
                if offset is not None:
                    check_register(f, rvalue, rwhat, offset, shapes, spans)
                    device.registers.append(offset)
    if "gate_register" in values:
        device.gate_register = f.integer(values["gate_register"], "%s gate_register" % what, 16)
        device.gate_register_node = values["gate_register"]
    return device


def check_blocks(f, node, device, what, shapes, spans):
    blocks = f.mapping(node, "%s blocks" % what)
    if blocks is None:
        return
    for block_name, (bkey, bvalue) in blocks.items():
        block = f.name(bkey, "%s block" % what, IDENTIFIER)
        bwhat = "block `%s` of %s" % (block_name, what)
        named = Named()
        if isinstance(bvalue, MappingNode):
            bvalues = f.fields(bvalue, bwhat, ("offset", "ref", "symbol"), ("offset",))
            if bvalues is None or "offset" not in bvalues:
                continue
            named = check_named(f, bvalues, bwhat)
            bvalue = bvalues["offset"]
        offset = f.integer(bvalue, bwhat, 32)
        if block is None or offset is None:
            continue
        if shapes != ["window"]:
            f.refuse(bvalue, "chip.block-outside", "%s at 0x%X lies in no window; a block is part of a "
                     "device's one `window`" % (bwhat, offset))
            continue
        if spans and offset >= spans[0][1]:
            f.refuse(bvalue, "chip.block-outside",
                     "%s at 0x%X lies outside its window, 0x%X long" % (bwhat, offset, spans[0][1]))
            continue
        device.blocks.append(Block(block, offset, named))


def check_repeat(f, values, what, base, size, count, stride):
    """Whether `count` instances of the window `stride` apart neither overlap nor wrap."""
    if count > 1 and stride < size:
        f.refuse(values["stride"], "chip.overlap",
                 "%s repeats every 0x%X, so its window of 0x%X overlaps the next instance" % (what, stride, size))
        return False
    return check_end(f, values["window"], "%s instance %d" % (what, count - 1), base + (count - 1) * stride + size)


def check_register(f, node, what, offset, shapes, spans):
    if not shapes:
        f.refuse(node, "chip.register-outside", "%s at 0x%X lies in no window; the device has none" % (what, offset))
        return
    for label, size in spans:
        if offset >= size:
            f.refuse(node, "chip.register-outside",
                     "%s at 0x%X lies outside %s, 0x%X long" % (what, offset, label, size))
            return


def check_memory(f, top, root, chip, translates, windows):
    carving = []
    if PART in translates and not translates[PART]:
        carving.append(PART)
    for cluster in chip.clusters:
        if cluster in translates and not translates[cluster]:
            carving.append(cluster)
    if "memory" not in top:
        if carving:
            f.refuse(root, "chip.memory-required",
                     "the chip file needs `memory`, %s" % carving_prose(carving))
        return
    entries = f.mapping(top["memory"], "`memory`")
    if entries is None:
        return
    views = [PART]
    if chip.clusters:
        views = chip.clusters
    known = all(view in translates for view in views)
    arenas = {}
    for view in carving:
        arenas[view] = []
    # {(view, region): the node that named it first}
    links = chip.links
    for entry_name, (key, value) in entries.items():
        name = f.name(key, "memory entry", IDENTIFIER)
        what = "memory `%s`" % entry_name
        values = f.fields(value, what, MEMORY_FIELDS, ("size",))
        if name is None or values is None:
            continue
        size = None
        if "size" in values:
            size = check_size(f, values["size"], "%s size" % what, 64)
        if "base" in values and "at" in values:
            f.refuse(values["at"], "form.exclusive", "%s has both `base` and `at`" % what)
            continue
        if "base" not in values and "at" not in values:
            if not host_placed(f, values, chip):
                f.refuse(value, "form.missing", "%s needs `base`, or `at` with one base per cluster, unless it "
                         "is the arena of a part the host runs, which the host places" % what)
                continue
            if "link" in values:
                f.refuse(values["link"], "form.inapplicable",
                         "%s has no `base`, the host placing it, so the link has no region to place there" % what)
            if size is not None:
                entry = Memory(name, size, {PART: None}, check_named(f, values, what))
                entry.node = key
                entry.host_placed = True
                chip.memory.append(entry)
                if PART in arenas:
                    arenas[PART].append((values["arena"], (None, size)))
            continue
        if "at" in values and "cluster" in values:
            f.refuse(values["cluster"], "form.exclusive",
                     "%s names its clusters in `at`, so it has no `cluster`" % what)
            continue
        bases = {}
        if "base" in values:
            cluster = None
            if "cluster" in values:
                cluster = check_cluster_ref(f, chip, values["cluster"], "%s cluster" % what)
                if cluster is None:
                    continue
            base = f.integer(values["base"], "%s base" % what, 64)
            for view in device_reach(chip, cluster):
                bases[view] = (base, values["base"])
        else:
            at = f.mapping(values["at"], "%s at" % what)
            if at is None:
                continue
            if not at:
                f.refuse(values["at"], "form.missing", "%s `at` names no cluster" % what)
                continue
            for cluster_name, (ckey, cvalue) in at.items():
                cluster = check_cluster_ref(f, chip, ckey, "%s at" % what)
                base = f.integer(cvalue, "%s base for `%s`" % (what, cluster_name), 64)
                if cluster is not None:
                    bases[cluster] = (base, cvalue)
        if size is not None and all(base is not None for base, node in bases.values()):
            entry = Memory(name, size, {view: base for view, (base, node) in bases.items()},
                           check_named(f, values, what))
            entry.node = key
            if "link" in values:
                entry.link = check_link(f, values["link"], what, links, bases)
            chip.memory.append(entry)
        arena = False
        if "arena" in values:
            arena = f.boolean(values["arena"], "%s arena" % what)
        for view, (base, node) in bases.items():
            if base is not None and size is not None and check_end(f, node, what, base + size):
                windows.append(Window("mem", "memory `%s`" % name, base, size, (view,), node))
        if not arena or not known:
            continue
        carved = False
        for view in bases:
            if view in arenas:
                arenas[view].append((values["arena"], (bases[view][0], size)))
                carved = True
        if not carved:
            f.refuse(values["arena"], "chip.arena",
                     "%s is marked `arena`, but no core that reaches it carves one" % what)
    if not known:
        return
    for view in carving:
        marks = [mark for mark, span in arenas[view]]
        if len(marks) == 1:
            chip.arena[view] = arenas[view][0][1]
            continue
        where = "the part"
        if view is not PART:
            where = "cluster `%s`" % view
        if not marks:
            f.refuse(top["memory"], "chip.arena",
                     "no `memory` entry %s reaches is marked `arena: true`" % where)
        for mark in marks[1:]:
            f.refuse(mark, "chip.arena",
                     "a second `arena` for %s, which carves its arena from exactly one" % where)


def host_placed(f, values, chip):
    """Whether a memory entry with no base is the arena a host places."""
    if not host_runs(chip) or chip.clusters or "arena" not in values:
        return False
    return getattr(values["arena"], "value", None) == "true"


def check_link(f, node, what, links, bases):
    """The (region, access) a memory entry links as, refused where a view already has the region."""
    values = f.fields(node, "%s link" % what, ("region", "access"), ("region", "access"))
    if values is None or "region" not in values or "access" not in values:
        return None
    region = f.name(values["region"], "%s link region" % what, REGION)
    access = f.name(values["access"], "%s link access" % what, ACCESS)
    if region is None or access is None:
        return None
    for view in bases:
        first = links.get((view, region))
        if first is not None:
            f.refuse(values["region"], "chip.link-duplicate",
                     "%s links as `%s`, which line %d already names" % (what, region, line_of(first)))
            return None
        links[(view, region)] = values["region"]
    return (region, access)


def carving_prose(carving):
    if carving == [PART]:
        return "since the part does not translate"
    names = " and ".join("`%s`" % c for c in carving)
    if len(carving) == 1:
        return "since cluster %s does not translate" % names
    return "since clusters %s do not translate" % names


def check_chip_pins(f, node, chip, devices):
    pins = f.mapping(node, "`pins`")
    if pins is None:
        chip.pins = None
        return
    for pin_name, (key, value) in pins.items():
        pin = f.name(key, "pin", PIN)
        functions = f.mapping(value, "pin `%s`" % pin_name)
        if pin is None or functions is None:
            continue
        chip.pins[pin] = {}
        for selector, (skey, svalue) in functions.items():
            f.name(skey, "pin `%s` selector" % pin_name, SELECTOR)
            pattern = FUNCTION
            if selector == "gpio":
                pattern = GPIO_FUNCTION
            if isinstance(svalue, MappingNode):
                svalue = check_pin_function(f, svalue, chip, pin_name, selector)
                if svalue is None:
                    continue
            function = f.name(svalue, "pin `%s` function" % pin_name, pattern)
            if function is None:
                continue
            device = function.split(".")[0]
            if devices is not None and device not in devices:
                f.refuse(svalue, "chip.device-unknown",
                         "pin `%s` function `%s` names no device of this chip" % (pin_name, function))
                continue
            if selector == "gpio" and not check_gpio_port(f, chip, svalue, pin_name, function):
                continue
            chip.pins[pin][selector] = function


def check_pin_function(f, node, chip, pin_name, selector):
    """The `function` node of a function stated as a mapping, its `input_select` and `ref` kept in
    chip.pin_details, or None when refused."""
    what = "pin `%s` function `%s`" % (pin_name, selector)
    values = f.fields(node, what, PIN_FUNCTION_FIELDS, ("function",))
    if values is None or "function" not in values:
        return None
    if selector == "gpio":
        f.refuse(node, "form.inapplicable", "%s is a port bit, which selects no input" % what)
        return None
    select = None
    if "input_select" in values:
        select = f.integer(values["input_select"], "%s input_select" % what, 32)
    chip.pin_details[(pin_name, selector)] = (select, check_ref(f, values))
    return values["function"]


def check_gpio_port(f, chip, node, pin_name, function):
    """Whether a `gpio` function names its port as the device's shape has it."""
    parts = function.split(".")
    device = chip.devices.get(parts[0])
    if device is None:
        return False
    if device.count is not None:
        if len(parts) == 3 and index_below(parts[1], device.count) is not None:
            return True
        f.refuse(node, "chip.device-unknown",
                 "pin `%s` function `%s` names no instance of device `%s`, which has %d, as "
                 "`%s.<instance>.<bit>`" % (pin_name, function, device.name, device.count, device.name))
        return False
    if len(parts) == 2:
        return True
    f.refuse(node, "chip.device-unknown",
             "pin `%s` function `%s` names an instance of device `%s`, which has none, so it is "
             "`%s.<bit>`" % (pin_name, function, device.name, device.name))
    return False


def check_overlaps(f, chip, windows):
    views = [PART]
    if chip.clusters:
        views = chip.clusters
    reported = set()
    for space in ("mem", "port"):
        for view in views:
            seen = sorted((w for w in windows if w.space == space and view in w.reach),
                          key=lambda w: (w.base, w.size))
            for i, later in enumerate(seen):
                for earlier in seen[:i]:
                    hit = overlap(earlier, later)
                    if hit is None:
                        continue
                    pair = (id(earlier), id(later))
                    if pair in reported:
                        continue
                    reported.add(pair)
                    where = ""
                    if view is not PART:
                        where = " in cluster `%s`" % view
                    kind = "window"
                    if space == "port":
                        kind = "port range"
                    k_earlier, k_later = hit
                    f.refuse(later.node, "chip.overlap",
                             "the %s of %s at 0x%X overlaps %s at 0x%X%s"
                             % (kind, later.name(k_later), later.start(k_later), earlier.name(k_earlier),
                                earlier.start(k_earlier), where))


def check_gate_edges(f, chip, windows):
    """A device window a `device_gate` or `partition_gate` range covers only in part."""
    for w in windows:
        if w.space != "mem" or not w.label.startswith("/dev/"):
            continue
        for view in w.reach:
            gate = chip.gates.get(view)
            if gate is None or gate[2] is None:
                continue
            edge = straddled(gate[2], w)
            if edge is not None:
                k, base, size = edge
                f.refuse(w.node, "chip.gate-straddle",
                         "the window of %s at 0x%X crosses the edge of the `device_gate` range [0x%X, 0x%X]"
                         % (w.name(k), w.start(k), base, size))
                break
        if chip.partition_gate is None:
            continue
        for gate in chip.partition_gate.gates.values():
            edge = straddled(gate.ranges, w)
            if edge is not None:
                k, base, size = edge
                f.refuse(w.node, "chip.gate-straddle",
                         "the window of %s at 0x%X crosses the edge of partition gate `%s`'s range [0x%X, 0x%X]"
                         % (w.name(k), w.start(k), gate.name, base, size))
                break


def straddled(ranges, w):
    """The first instance of `w` a range overlaps without holding it, as (k, base, size), or None."""
    for k in range(w.count):
        start = w.start(k)
        end = start + w.size
        for base, size in ranges:
            if base < end and start < base + size and not (base <= start and end <= base + size):
                return (k, base, size)
    return None


def overlap(a, b):
    """The first instances of `a` and `b` that overlap, as (k of a, k of b), or None."""
    if a.start(a.count - 1) + a.size <= b.base or b.start(b.count - 1) + b.size <= a.base:
        return None
    if a.count > b.count:
        hit = overlap(b, a)
        if hit is None:
            return None
        return (hit[1], hit[0])
    for k in range(a.count):
        j = instance_meeting(b, a.start(k), a.size)
        if j is not None:
            return (k, j)
    return None


def instance_meeting(w, start, size):
    """The first instance of `w` that meets [start, start + size), its instances not overlapping."""
    k = 0
    if w.count > 1 and start > w.base:
        k = min((start - w.base) // w.stride, w.count - 1)
    for j in (k, k + 1):
        if j < w.count and w.start(j) < start + size and start < w.start(j) + w.size:
            return j
    return None


def check_board(path, text, report, chips, boards):
    """The Board, or None when its file cannot be read at all. `chips` maps a chip file's path to
    its Chip once read, so each is read once; `boards` maps each board name read so far to its
    file."""
    f = File(path, report)
    root = f.load(text)
    if root is None:
        return None
    top = f.fields(root, "the board file", BOARD_FIELDS, ("version", "board", "chip", "console"))
    if top is None or not f.version(top, "the board file", BOARD_VERSIONS):
        return None
    stem = os.path.splitext(os.path.basename(path))[0]
    if "board" in top:
        board = f.name(top["board"], "`board`", FILE_NAME)
        if board is not None and board != stem:
            f.refuse(top["board"], "board.name-mismatch",
                     "`board: %s` is in %s, which names it `%s`" % (board, os.path.basename(path), stem))
        if stem in boards:
            f.refuse(top["board"], "board.name-collision",
                     "board `%s` is also %s, and boards/%s/ holds one board" % (stem, boards[stem], stem))
        else:
            boards[stem] = path
    chip = None
    if "chip" in top:
        chip = resolve_chip(f, top["chip"], path, chips)
    used = []
    # {id(node): bus device} of each part's bus pin, which every part on that bus shares.
    bus_lines = {}
    console_path = None
    console_pins = []
    kernel_led = None

    if "console" in top:
        console = f.fields(top["console"], "`console`", ("device", "pins", "semihosting"), ())
        if console is not None:
            device = None
            semihosting = False
            if "semihosting" in console:
                semihosting = f.boolean(console["semihosting"], "console `semihosting`") is True
                if "device" in console:
                    f.refuse(console["semihosting"], "form.exclusive",
                             "`console` names both a `device` and `semihosting`; it is one of the two")
                elif "pins" in console:
                    f.refuse(console["pins"], "form.inapplicable",
                             "console `pins` wire a device, and a semihosting console has none")
            if "device" not in console and not semihosting:
                f.refuse(top["console"], "form.missing", "`console` needs `device`, or `semihosting: true`")
            if "device" in console:
                device = resolve_device(f, chip, console["device"], "the console device")
                if device is not None:
                    console_path = console["device"].value
            if "pins" in console and "semihosting" not in console:
                pins = f.mapping(console["pins"], "console pins")
                if pins is not None:
                    for role, (rkey, rvalue) in pins.items():
                        f.name(rkey, "console pin role", IDENTIFIER)
                        what = "console pin `%s`" % role
                        selector = check_board_pin(f, chip, rvalue, what, device, used, role)
                        if selector is not None and has_gpio(f, chip, rvalue, what, rvalue.value):
                            console_pins.append((role, rvalue.value, selector))

    if "leds" in top:
        leds = f.mapping(top["leds"], "`leds`")
        if leds is not None:
            for led_name, (key, value) in leds.items():
                f.name(key, "led", IDENTIFIER)
                what = "led `%s`" % led_name
                values = f.fields(value, what, ("pin", "kind", "active", "owner"), ("pin",))
                if values is None:
                    continue
                pin = None
                if "pin" in values:
                    pin = check_gpio_pin(f, chip, values["pin"], "%s pin" % what, used)
                kind = "level"
                if "kind" in values:
                    kind = f.enum(values["kind"], "%s kind" % what, LED_KINDS)
                active = None
                if kind == "addressable" and "active" in values:
                    f.refuse(values["active"], "form.inapplicable",
                             "%s is addressable, whose state is data and not a level, so it has no `active`" % what)
                elif kind == "level" and "active" not in values:
                    f.refuse(value, "form.missing", "%s needs `active`, the level that lights it" % what)
                elif "active" in values:
                    active = f.enum(values["active"], "%s active level" % what, ("high", "low"))
                owner = None
                if "owner" in values:
                    owner = f.enum(values["owner"], "%s owner" % what, ("kernel",))
                if owner is not None and kernel_led is not None:
                    f.refuse(values["owner"], "board.led-owner",
                             "%s is the kernel's, and led `%s` on line %d already is: the kernel drives one LED"
                             % (what, kernel_led[2], line_of(kernel_led[3])))
                elif owner is not None:
                    kernel_led = (pin, active, led_name, values["owner"], kind)
                    if chip is not None and kind is not None and kind != chip.led_kind:
                        f.refuse(values.get("kind", value), "board.led-kind",
                                 "%s is the kernel's and `kind: %s`, and chip `%s` drives the kernel's LED "
                                 "as `led: %s`" % (what, kind, chip.name, chip.led_kind))

    if "parts" in top:
        parts = f.mapping(top["parts"], "`parts`")
        if parts is not None:
            for part_name, (key, value) in parts.items():
                f.name(key, "part", IDENTIFIER)
                what = "part `%s`" % part_name
                values = f.fields(value, what, ("bus", "chip_select", "pins"), ("bus",))
                if values is None:
                    continue
                bus = None
                if "bus" in values:
                    bus = resolve_device(f, chip, values["bus"], "%s bus" % what)
                if "chip_select" in values:
                    check_gpio_pin(f, chip, values["chip_select"], "%s chip select" % what, used)
                if "pins" in values:
                    first = len(used)
                    check_pin_list(f, chip, values["pins"], "%s pin" % what, bus, used)
                    for pin, node, pin_what in used[first:]:
                        bus_lines[id(node)] = bus

    wired_buses = []
    if "buses" in top:
        buses = f.mapping(top["buses"], "`buses`")
        if buses is not None:
            for bus_name, (key, value) in buses.items():
                f.name(key, "bus", IDENTIFIER)
                what = "bus `%s`" % bus_name
                values = f.fields(value, what, ("device", "pins", "chip_selects"), ("device",))
                if values is None:
                    continue
                device = None
                if "device" in values:
                    device = resolve_device(f, chip, values["device"], "%s device" % what)
                first = len(used)
                if "pins" in values:
                    check_pin_list(f, chip, values["pins"], "%s pin" % what, device, used)
                pins = [pin for pin, node, pin_what in used[first:]]
                first = len(used)
                if "chip_selects" in values:
                    items = f.sequence(values["chip_selects"], "%s chip selects" % what)
                    for item in items or ():
                        check_gpio_pin(f, chip, item, "%s chip select" % what, used)
                chip_selects = [pin for pin, node, pin_what in used[first:]]
                if device is not None:
                    wired_buses.append(Bus(bus_name, f.path(values["device"], "%s device" % what), device,
                                           pins, chip_selects))

    reserved = None
    if "reserved_pins" in top:
        reserved = f.mapping(top["reserved_pins"], "`reserved_pins`")
        if reserved is not None:
            for pin_name, (key, value) in reserved.items():
                pin = f.name(key, "reserved pin", PIN)
                f.string(value, "the reason pin `%s` is reserved" % pin_name)
                if pin is not None and chip_pin(f, chip, key, "reserved pin", pin) is not None:
                    has_gpio(f, chip, key, "reserved pin", pin)
            for pin, node, what in used:
                if pin in reserved:
                    f.refuse(node, "board.reserved-pin-used",
                             "%s is `%s`, which the board reserves on line %d"
                             % (what, pin, line_of(reserved[pin][0])))
    seen = {}
    for pin, node, what in used:
        shared = bus_lines.get(id(node))
        if pin in seen and shared is not None and bus_lines.get(id(seen[pin][0])) == shared:
            continue
        if pin in seen:
            f.refuse(node, "board.pin-reused",
                     "%s is `%s`, which %s on line %d already spends" % (what, pin, seen[pin][1], line_of(seen[pin][0])))
        else:
            seen[pin] = (node, what)
    board = Board(chip, console_path)
    board.console_pins = console_pins
    if kernel_led is not None:
        pin, active, led_name, node, kind = kernel_led
        if pin is not None and kind is not None and (active is not None or kind == "addressable"):
            board.kernel_led = (pin, active, kind)
    board.buses = wired_buses
    if "memory" in top:
        check_board_memory(f, top["memory"], chip, board)
    if "emulator" in top:
        board.emulator = check_emulator(f, top["emulator"])
    for pin, node, what in used:
        board.wired.setdefault(pin, what)
    if "reserved_pins" in top and reserved is not None:
        for pin_name, (key, value) in reserved.items():
            board.wired.setdefault(pin_name, "reserved pin")
            if chip is not None and chip.pins is not None and pin_name in chip.pins:
                board.reserved.append(pin_name)
    return board


def words(f, node, what):
    """The words of the list at `node`, or None once refused."""
    items = f.sequence(node, what)
    if items is None:
        return None
    found = [f.name(item, "a word of %s" % what, WORD) for item in items]
    if None in found:
        return None
    return found


def check_emulator(f, node):
    """{field: value} of an `emulator`: the QEMU binary and machine, the machine under the GICv3
    posture, the options, and the QOM global set to the RAM link region's length; or None."""
    values = f.fields(node, "`emulator`", EMULATOR_FIELDS, ("qemu", "machine"))
    if values is None:
        return None
    emulator = {"options": []}
    for field in ("qemu", "machine", "gicv3_machine", "ram_global"):
        if field in values:
            emulator[field] = f.name(values[field], "emulator `%s`" % field, WORD)
    if "options" in values:
        emulator["options"] = words(f, values["options"], "emulator `options`")
    if None in emulator.values():
        return None
    return emulator


def check_board_memory(f, node, chip, board):
    """The memory soldered on the board, each entry a window of the chip's map the chip leaves free."""
    entries = f.mapping(node, "`memory`")
    if entries is None:
        return
    names = set()
    layout = set()
    if chip is not None:
        for device in chip.devices.values():
            names.update(symbol for symbol, value, ref in device_symbols(device))
            layout.update(symbol for symbol, value, ref in device_layout(device))
        for entry in chip.memory:
            names.add(memory_symbol(entry))
            layout.update(symbol for symbol, value, ref in memory_layout(entry, 0))
    links = {}
    windows = []
    for entry_name, (key, value) in entries.items():
        name = f.name(key, "memory entry", IDENTIFIER)
        what = "memory `%s`" % entry_name
        values = f.fields(value, what, BOARD_MEMORY_FIELDS, ("size", "base"))
        if name is None or values is None or "size" not in values or "base" not in values:
            continue
        size = check_size(f, values["size"], "%s size" % what, 64)
        base = f.integer(values["base"], "%s base" % what, 64)
        cluster = None
        if "cluster" in values and chip is not None:
            cluster = check_cluster_ref(f, chip, values["cluster"], "%s cluster" % what)
            if cluster is None:
                continue
        if size is None or base is None or not check_end(f, values["base"], what, base + size):
            continue
        entry = Memory(name, size, {}, check_named(f, values, what))
        entry.node = key
        if chip is None:
            continue
        reach = device_reach(chip, cluster)
        entry.bases = {view: base for view in reach}
        symbol = memory_symbol(entry)
        if symbol in names or any(macro in layout for macro, v, r in memory_layout(entry, 0)):
            f.refuse(key, "board.symbol-collision",
                     "%s is named `%s` in C, which chip `%s` already names" % (what, symbol, chip.name))
            continue
        names.add(symbol)
        layout.update(macro for macro, v, r in memory_layout(entry, 0))
        here = Window("mem", "board %s" % what, base, size, reach, values["base"])
        hits = [(w, overlap(w, here)) for w in chip.windows + windows
                if w.space == "mem" and set(w.reach) & set(reach)]
        hits = [(w, pair) for w, pair in hits if pair is not None]
        if hits:
            w, pair = hits[0]
            f.refuse(values["base"], "board.memory-overlap",
                     "%s at 0x%X overlaps %s at 0x%X" % (what, base, w.name(pair[0]), w.start(pair[0])))
            continue
        windows.append(here)
        if "link" in values:
            entry.link = check_board_link(f, values["link"], what, chip, links, reach)
        board.memory.append(entry)


def check_board_link(f, node, what, chip, links, reach):
    """The (region, access) a board's memory links as, refused where the chip or the board has the region."""
    values = f.fields(node, "%s link" % what, ("region", "access"), ("region", "access"))
    if values is None or "region" not in values or "access" not in values:
        return None
    region = f.name(values["region"], "%s link region" % what, REGION)
    access = f.name(values["access"], "%s link access" % what, ACCESS)
    if region is None or access is None:
        return None
    for view in reach:
        if (view, region) in chip.links or (view, region) in links:
            f.refuse(values["region"], "board.link-duplicate",
                     "%s links as `%s`, which chip `%s` or this board already links" % (what, region, chip.name))
            return None
    for view in reach:
        links[(view, region)] = values["region"]
    return (region, access)


def resolve_chip(f, node, path, chips):
    name = f.name(node, "`chip`", FILE_NAME)
    if name is None:
        return None
    here = os.path.dirname(os.path.abspath(path))
    chip_path = os.path.join(os.path.dirname(here), name, "chip.yaml")
    if not os.path.isfile(chip_path):
        f.refuse(node, "board.chip-unknown", "`chip: %s` names no file platform/%s/chip.yaml" % (name, name))
        return None
    if os.path.basename(here) != name:
        f.refuse(node, "board.chip-folder",
                 "a board on `%s` sits beside its chip file in platform/%s/" % (name, name))
    if chip_path not in chips:
        try:
            with open(chip_path, encoding="utf-8") as stream:
                text = stream.read()
        except (OSError, UnicodeDecodeError) as error:
            f.refuse(node, "board.chip-unreadable", "`chip: %s` names a file that cannot be read: %s" % (name, error))
            return None
        chips[chip_path] = check_chip(chip_path, text, f.report)
    return chips[chip_path]


def resolve_device(f, chip, node, what):
    """The function prefix a pin wired to this device must carry, as `usic0.ch0`, or None."""
    path = f.path(node, what)
    if path is None or chip is None:
        return None
    parts = path.split("/")
    device = None
    if len(parts) in (3, 4) and parts[1] == "dev":
        device = chip.devices.get(parts[2])
    if device is None:
        f.refuse(node, "board.device-unknown", "%s `%s` is no /dev path of chip `%s`" % (what, path, chip.name))
        return None
    if len(parts) == 3:
        if device.window is not None and device.count is None:
            return device.name
        if device.ports is not None or device.host:
            return device.name
        f.refuse(node, "board.device-unknown",
                 "%s `%s` names a device with no window of its own; name one of its %s"
                 % (what, path, instances_prose(device)))
        return None
    leaf = parts[3]
    if leaf in device.channels:
        return "%s.%s" % (device.name, leaf)
    if device.count is not None and index_below(leaf, device.count) is not None:
        return "%s.%s" % (device.name, leaf)
    f.refuse(node, "board.device-unknown", "%s `%s` is no channel or instance of device `%s`" % (what, path, device.name))
    return None


def instances_prose(device):
    if device.channels:
        return "channels"
    return "instances"


def check_pin_list(f, chip, node, what, device, used):
    items = f.sequence(node, "%ss" % what)
    if items is None:
        return
    for item in items:
        check_board_pin(f, chip, item, what, device, used)


def chip_pin(f, chip, node, what, pin):
    """The pin's functions on its chip, or None when unknown or refused."""
    if chip is None or chip.pins is None:
        return None
    if pin not in chip.pins:
        f.refuse(node, "board.pin-unknown", "%s `%s` is no pin chip `%s` lists" % (what, pin, chip.name))
        return None
    return chip.pins[pin]


def check_board_pin(f, chip, node, what, device, used, signal=None):
    """A pin wired to `device`, carrying `signal` of it where the role names one. The selector
    of that signal, or None."""
    pin = f.name(node, what, PIN)
    if pin is None:
        return None
    used.append((pin, node, what))
    functions = chip_pin(f, chip, node, what, pin)
    if functions is None or device is None:
        return None
    carried = [function for selector, function in functions.items()
               if selector != "gpio" and function.startswith(device + ".")]
    if not carried:
        f.refuse(node, "board.pin-function",
                 "%s `%s` has no function of `%s` on chip `%s`" % (what, pin, device.replace(".", "/"), chip.name))
        return None
    if signal is None:
        return None
    for selector, function in functions.items():
        if selector != "gpio" and function == "%s.%s" % (device, signal):
            return selector
    f.refuse(node, "board.pin-signal",
             "%s `%s` carries %s, not `%s.%s`: a console pin's role names the signal it carries"
             % (what, pin, ", ".join("`%s`" % c for c in carried), device, signal))
    return None


def check_gpio_pin(f, chip, node, what, used):
    """The pin, or None when it has no `gpio` function on its chip."""
    pin = f.name(node, what, PIN)
    if pin is None:
        return None
    used.append((pin, node, what))
    if chip_pin(f, chip, node, what, pin) is None or not has_gpio(f, chip, node, what, pin):
        return None
    return pin


def has_gpio(f, chip, node, what, pin):
    """Whether `pin`, a pin of its chip, has a `gpio` function, refused where it has none."""
    if "gpio" in chip.pins[pin]:
        return True
    f.refuse(node, "board.pin-not-gpio", "%s `%s` has no `gpio` function on chip `%s`" % (what, pin, chip.name))
    return False


def description_files(paths, report):
    files = []
    for path in paths:
        if os.path.isdir(path):
            for folder, subfolders, names in os.walk(path):
                subfolders.sort()
                for name in sorted(names):
                    files.append(os.path.join(folder, name))
        elif os.path.isfile(path):
            files.append(path)
        else:
            report.refuse(path, 1, "form.layout", "no such file or directory")
    return files


def check_platform(paths):
    report = Report()
    files = description_files(paths, report)
    chips = {}
    boards = []
    for path in files:
        platform = os.path.dirname(os.path.dirname(os.path.abspath(path)))
        if not path.endswith(".yaml") or os.path.basename(platform) != "platform":
            report.refuse(path, 1, "form.layout", "a description file is platform/<chip>/<name>.yaml")
            continue
        try:
            with open(path, encoding="utf-8") as stream:
                text = stream.read()
        except (OSError, UnicodeDecodeError) as error:
            report.refuse(path, 1, "form.unreadable", "the file cannot be read as UTF-8: %s" % error)
            continue
        if os.path.basename(path) == "chip.yaml":
            chips[os.path.abspath(path)] = check_chip(path, text, report)
        else:
            boards.append((path, text))
    names = {}
    for path, text in boards:
        check_board(path, text, report, chips, names)
    return report, len(files)
