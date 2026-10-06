# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The chip and board description files under platform/: platform/<chip>/chip.yaml and
# platform/<chip>/<board>.yaml.

import os

from .subset import (
    FILE_NAME, FUNCTION, GPIO_FUNCTION, IDENTIFIER, PIN, SELECTOR, File, Report, index_below, line_of,
)

UNITS = ("pmsav7", "pmsav8", "pmsav6", "pmp", "rxmpu", "sysmpu", "mmu", "none")

# The versions of the chip and board file formats this tool reads.
CHIP_VERSIONS = (1,)
BOARD_VERSIONS = (1,)
CHIP_FIELDS = (
    "version", "chip", "arch", "protection", "cores", "clusters_coherent", "partition_gate",
    "data_cache", "devices", "memory", "pins",
)
PROTECTION_FIELDS = (
    "unit", "covers_devices", "memory_type", "page", "device_gate", "bus_gate", "privilege", "io_ports",
)
CLUSTER_FIELDS = ("arch", "count", "smp", "protection", "line_offset")
DEVICE_FIELDS = (
    "window", "ports", "channels", "count", "stride", "lines", "bus_master", "owner", "sysreg",
    "privileged_registers", "cluster",
)
MEMORY_FIELDS = ("size", "base", "at", "cluster", "arena")
BOARD_FIELDS = ("version", "board", "chip", "console", "leds", "parts", "buses", "reserved_pins")


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
        self.bus_master = False
        self.cluster = None
        # The offset of each privileged register, in the chip file's order.
        self.registers = []


class Board:
    def __init__(self, chip, console):
        self.chip = chip
        self.console = console
        # {pin: what the board spends it on}
        self.wired = {}


class Protection:
    def __init__(self, unit):
        self.unit = unit
        self.covers_devices = None
        self.memory_type = None
        self.page = None
        self.privilege = True
        self.io_ports = None


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


# A part with no clusters reaches its windows as one view, named by None.
PART = None


def check_chip(path, text, report):
    """The chip a board resolves against, or None when its file cannot be read at all."""
    f = File(path, report)
    root = f.load(text)
    if root is None:
        return None
    top = f.fields(root, "the chip file", CHIP_FIELDS, ("version", "chip", "devices"))
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
    if "arch" in top:
        f.name(top["arch"], "`arch`", IDENTIFIER)

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
        gate = f.fields(top["partition_gate"], "`partition_gate`", ("kind", "device"), ("kind", "device"))
        if gate is not None:
            if "kind" in gate:
                f.name(gate["kind"], "`partition_gate` kind", IDENTIFIER)
            if "device" in gate:
                target = f.name(gate["device"], "`partition_gate` device", IDENTIFIER)
                if target is not None and devices is not None and target not in devices:
                    f.refuse(gate["device"], "chip.device-unknown",
                             "`partition_gate` names `%s`, which is no device of this chip" % target)

    check_memory(f, top, root, chip, translates, windows)
    if "pins" in top:
        check_chip_pins(f, top["pins"], chip, devices)
    check_overlaps(f, chip, windows)
    check_gate_edges(f, chip, windows)
    return chip


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
    for flag in ("covers_devices", "memory_type", "privilege", "io_ports"):
        if flag in values:
            setattr(protection, flag, f.boolean(values[flag], "%s `%s`" % (what, flag)))
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
            f.boolean(values["smp"], "`cores` smp")
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
            f.name(values["arch"], "%s `arch`" % what, IDENTIFIER)
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
            f.integer(values["line_offset"], "%s `line_offset`" % what, 16)
            if "arch" not in values:
                f.refuse(values["line_offset"], "form.inapplicable",
                         "%s has a `line_offset`, which only a cluster with its own `arch` has" % what)
        if "count" in values:
            check_count_range(f, values["count"], "%s count" % what)
        if "smp" in values:
            f.boolean(values["smp"], "%s smp" % what)
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
                cvalues = f.fields(cvalue, cwhat, ("window",), ("window",))
                if channel is None or cvalues is None or "window" not in cvalues:
                    continue
                window = check_range(f, cvalues["window"], "%s window" % cwhat, "size")
                device.channels[channel] = ckey
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
                number = f.integer(lvalue, "line `%s/%s`" % (name, line_name), 16)
                if line is not None and number is not None:
                    device.lines[line] = lkey
                    device.sources[line] = number
    for line, lkey in device.lines.items():
        if line in device.channels:
            f.refuse(lkey, "chip.name-collision",
                     "`/dev/%s/%s` names both a channel and a line of %s" % (name, line, what))

    for flag in ("bus_master", "sysreg"):
        if flag in values:
            if f.boolean(values[flag], "%s `%s`" % (what, flag)):
                setattr(device, flag, True)
    if device.sysreg and shapes:
        f.refuse(values["sysreg"], "form.exclusive",
                 "%s is reached through system registers alone, so it has no `%s`" % (what, shapes[0]))
    if "owner" in values:
        device.owner = f.enum(values["owner"], "%s owner" % what, ("kernel",))
    if device.owner == "kernel" and "window" not in values and "ports" not in values and not device.sysreg:
        f.refuse(key, "chip.kernel-window",
                 "%s is kernel-owned, so it states its `window` or `ports`, or `sysreg: true`" % what)

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
    return device


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
            f.refuse(value, "form.missing", "%s needs `base`, or `at` with one base per cluster" % what)
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
    """A device window a `device_gate` range covers only in part."""
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
    console_path = None

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
                        check_board_pin(f, chip, rvalue, "console pin `%s`" % role, device, used)

    if "leds" in top:
        leds = f.mapping(top["leds"], "`leds`")
        if leds is not None:
            for led_name, (key, value) in leds.items():
                f.name(key, "led", IDENTIFIER)
                what = "led `%s`" % led_name
                values = f.fields(value, what, ("pin", "active", "owner"), ("pin", "active"))
                if values is None:
                    continue
                if "pin" in values:
                    check_gpio_pin(f, chip, values["pin"], "%s pin" % what, used)
                if "active" in values:
                    f.enum(values["active"], "%s active level" % what, ("high", "low"))
                if "owner" in values:
                    f.enum(values["owner"], "%s owner" % what, ("kernel",))

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
                    check_pin_list(f, chip, values["pins"], "%s pin" % what, bus, used)

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
                if "pins" in values:
                    check_pin_list(f, chip, values["pins"], "%s pin" % what, device, used)
                if "chip_selects" in values:
                    items = f.sequence(values["chip_selects"], "%s chip selects" % what)
                    for item in items or ():
                        check_gpio_pin(f, chip, item, "%s chip select" % what, used)

    reserved = None
    if "reserved_pins" in top:
        reserved = f.mapping(top["reserved_pins"], "`reserved_pins`")
        if reserved is not None:
            for pin_name, (key, value) in reserved.items():
                pin = f.name(key, "reserved pin", PIN)
                f.string(value, "the reason pin `%s` is reserved" % pin_name)
                if pin is not None:
                    chip_pin(f, chip, key, "reserved pin", pin)
            for pin, node, what in used:
                if pin in reserved:
                    f.refuse(node, "board.reserved-pin-used",
                             "%s is `%s`, which the board reserves on line %d"
                             % (what, pin, line_of(reserved[pin][0])))
    board = Board(chip, console_path)
    for pin, node, what in used:
        board.wired.setdefault(pin, what)
    if "reserved_pins" in top and reserved is not None:
        for pin_name, (key, value) in reserved.items():
            board.wired.setdefault(pin_name, "reserved pin")
    return board


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
        if device.ports is not None:
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


def check_board_pin(f, chip, node, what, device, used):
    pin = f.name(node, what, PIN)
    if pin is None:
        return
    used.append((pin, node, what))
    functions = chip_pin(f, chip, node, what, pin)
    if functions is None or device is None:
        return
    for selector, function in functions.items():
        if selector != "gpio" and function.startswith(device + "."):
            return
    f.refuse(node, "board.pin-function",
             "%s `%s` has no function of `%s` on chip `%s`" % (what, pin, device.replace(".", "/"), chip.name))


def check_gpio_pin(f, chip, node, what, used):
    pin = f.name(node, what, PIN)
    if pin is None:
        return
    used.append((pin, node, what))
    functions = chip_pin(f, chip, node, what, pin)
    if functions is not None and "gpio" not in functions:
        f.refuse(node, "board.pin-not-gpio",
                 "%s `%s` has no `gpio` function on chip `%s`" % (what, pin, chip.name))


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
