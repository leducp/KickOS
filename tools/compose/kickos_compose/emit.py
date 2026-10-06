# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The table an admitted composition is emitted as: one C source of designated initializers over
# <kickos/sys/table.h> (docs/design-m10-composition.md, "The emitted table"), and the canonical
# dump of the same model that tools/compose/tests/round_trip.py compares a compiled table with.

import os

from .composition import AUTHORITIES, Cache, admit_composition, amp_port, read_composition, region_size
from .descriptions import check_chip, host_runs
from .manifest import read_manifest
from .partition import admit_partition, derive_gate, place_regions
from .subset import File, Report, line_of
from .supply import arena_blocks, ram_align, ram_size, ring_block, status_block

# The most entries of one kind, whose last index is then 0xFFFE.
COUNT_LIMIT = 0xFFFF
# The largest value a 16-bit field that can hold KOS_TABLE_NONE (0xFFFF) carries.
FIELD_LIMIT = 0xFFFE
STRINGS_LIMIT = 0xFFFFFFFF
EVENTS = "/init/events"
STATUS = "/init/status"
CAPABILITY_KINDS = ("endpoint_serve", "endpoint_use", "notification", "line", "port")
WINDOW_KINDS = ("window", "ports", "region", "status")
# A privileged register's width is the kernel call that writes it: a port register one byte
# through kos_port_reg_write, a memory register a 32-bit word through kos_periph_reg_write.
WIDTHS = {"port": 1, "mem": 4}
GRANT_FLAGS = {"ro": "KOS_WINDOW_RO", "uncached": "KOS_WINDOW_UNCACHED", "wait": "KOS_CAP_WAIT",
               "signal": "KOS_CAP_SIGNAL"}
REGION_FLAGS = {"uncached": "KOS_MEM_NOCACHE", "partition": "KOS_TABLE_REGION_PARTITION"}
AUTHORITY_BITS = {name: "KOS_AUTH_%s" % name.upper() for name in AUTHORITIES}
ACCESS_SPELLED = {"r": "KICKOS_GATE_R", "w": "KICKOS_GATE_W", "x": "KICKOS_GATE_X"}
# The entry KickOS::main defines, which calls the app's main.
PACKAGED_MAIN = "kickos_main"


class Pool:
    """The deduplicated strings, at the offset of their first use."""

    def __init__(self):
        self.offsets = {}
        self.texts = []
        self.size = 0

    def add(self, text):
        # Admission matched every name against an ASCII pattern, which encode() relies on.
        if text not in self.offsets:
            self.offsets[text] = self.size
            self.texts.append(text)
            self.size = self.size + len(text.encode("ascii")) + 1
        return text


class TaskEntry:
    def __init__(self):
        self.name = None
        self.entry = None
        self.driver = None
        self.stack = 0
        self.block = 0
        self.priority = 0
        self.ceiling = 0
        self.restart_max = 0
        self.console = False
        self.block_uncached = False
        self.core_mask = 0
        self.authority = []
        self.first_grant = 0
        self.grant_count = 0
        self.cap_grant_count = 0
        self.first_use = 0
        self.use_count = 0
        self.first_watch = 0
        self.watch_count = 0


class GrantEntry:
    def __init__(self, kind, name, path):
        self.kind = kind
        self.flags = []
        self.cap_slot = None
        self.name = name
        self.path = path
        self.target = None
        self.window = None
        self.base = 0
        self.size = 0
        self.line_index = None
        self.line = None
        self.priv_first = 0
        self.priv_count = 0


class RegionEntry:
    def __init__(self, name, size, uncached, offset=None):
        self.name = name
        self.size = size
        self.uncached = uncached
        # A partition region's offset in the partition's user share, or None.
        self.offset = offset


class Table:
    def __init__(self, version):
        self.version = version
        self.ends_task = None
        self.init_priority = 0
        self.tasks = []
        self.grants = []
        self.refs = []
        self.privs = []
        self.regions = []
        self.strings = Pool()
        # Each entry symbol once, as (symbol, "task" or "driver"), in first-use order.
        self.externs = []
        # Each entry symbol's line in the composition, where the task first naming it names it.
        self.extern_lines = {}
        self.usb_device_console = False


def emit(path, manifest_path):
    """(the Report, the emitted C source or None once refused)."""
    report, table = table_of(path, manifest_path)
    if table is None:
        return report, None
    return report, render(table, os.path.basename(path), path)


def emit_system(path, manifest_path, output="table.c", partition=None):
    """(the Report, None once refused, or the (C source, asserts script, CMake fragment, gate C source
    or None) a system target is built from); `output` is the name the C source is compiled under.
    With `partition`, the node compositions in node order, the composition is the one at this
    build's node index, admitted with every other."""
    if partition is not None:
        report, found = admit_partition(partition, manifest_path, node_of(manifest_path))
        if found is None:
            return report, None
        path = partition[found.manifest.amp_node]
        admitted = found.admitted[found.manifest.amp_node]
        rows = found.rows
    else:
        report, admitted = admitted_of(path, manifest_path)
        if admitted is None:
            return report, None
        rows = single_gate(File(path, report), admitted)
        if report.refusals:
            return report, None
    table = build(admitted)
    if not check_table(File(path, report), admitted.root, table):
        return report, None
    source = os.path.basename(path)
    gate = None
    if emits_gate(admitted.manifest, admitted.chip):
        gate = render_gate(rows, source)
    return report, (render(table, source, path, output), render_asserts(admitted, path),
                    render_fragment(admitted, table, source, gate is not None), gate)


def node_of(manifest_path):
    """The node index the manifest at `manifest_path` states, or None."""
    manifest = read_manifest(manifest_path, Report())
    if not manifest:
        return None
    return manifest.amp_node


def emits_gate(manifest, chip):
    """Whether this build's system target carries the gate assignment: node 0 of a partition, or
    a build outside any partition of a chip stating a partition gate."""
    return manifest.amp_node == 0 or (manifest.amp_node is None and chip.partition_gate is not None)


def single_gate(f, admitted):
    """The gate rows of one composition, the node its manifest states holding its grants."""
    manifest = admitted.manifest
    own = manifest.amp_node or 0
    grants = [(own, grant) for task in admitted.tasks for grant in task.grants if grant.space == "mem"]

    def refuse(k, grant, message):
        node = admitted.root
        if grant is not None:
            node = grant.node
        f.refuse(node, "partition.gate-budget", message)

    return derive_gate(admitted.chip, manifest, grants, manifest.amp_nodes or 1, refuse)


def emit_gate(manifest_path):
    """(the Report, the gate C source of a build linking no composition, or None once refused or
    where the build carries none)."""
    report = Report()
    manifest = read_manifest(manifest_path, report)
    if not manifest:
        return report, None
    chip = None
    if manifest.descriptions is not None:
        chip = check_chip(manifest.descriptions[0], read_text(manifest.descriptions[0]), report)
    if report.refusals:
        return report, None
    if chip is None:
        if manifest.amp_node == 0:
            return report, render_gate([], os.path.basename(manifest_path))
        return report, None
    if not emits_gate(manifest, chip):
        return report, None

    def refuse(k, grant, message):
        report.refuse(manifest_path, 1, "partition.gate-budget", message)

    rows = derive_gate(chip, manifest, [], manifest.amp_nodes or 1, refuse)
    if report.refusals:
        return report, None
    return report, render_gate(rows, os.path.basename(manifest_path))


def read_text(path):
    with open(path, encoding="utf-8") as stream:
        return stream.read()


def render_gate(rows, source):
    """The gate assignment as C over <kickos/sys/partition_gate.h>, `source` naming what it came from."""
    out = [
        "// SPDX-License-Identifier: CECILL-C",
        "// Copyright (c) 2026 Philippe Leduc",
        "//",
        "// GENERATED by kickos_compose from %s; edits are overwritten by the next emit." % printable(source),
        "",
        "#include <kickos/sys/partition_gate.h>",
        "",
    ]
    if not rows:
        out.append("struct kickos_gate_row const kickos_gate_rows[1] = {{0}};")
    else:
        out.append("struct kickos_gate_row const kickos_gate_rows[%d] = {" % len(rows))
        for row in rows:
            access = [ACCESS_SPELLED[letter] for letter in row.access]
            out.append("    { .base = 0x%Xull, .size = 0x%Xull, .gate = %d, .reg = 0x%Xu, .node = %d, .access = %s },"
                       % (row.base, row.size, row.gate, row.register, row.node, " | ".join(access)))
        out.append("};")
    out.append("uint16_t const kickos_gate_row_count = %d;" % len(rows))
    return "\n".join(out) + "\n"


def admitted_of(path, manifest_path):
    """(the Report, the Admitted composition at `path` or None once refused)."""
    report = Report()
    manifest = read_manifest(manifest_path, report)
    if not manifest:
        return report, None
    text = read_composition(path, report)
    if text is None:
        return report, None
    admitted = admit_composition(path, text, None, report, Cache(), manifest)
    if admitted is None or report.refusals:
        return report, None
    place_regions([File(path, report)], [admitted], manifest)
    if report.refusals:
        return report, None
    return report, admitted


def table_of(path, manifest_path):
    """(the Report, the Table of the composition at `path` or None once refused)."""
    report, admitted = admitted_of(path, manifest_path)
    if admitted is None:
        return report, None
    table = build(admitted)
    if not check_table(File(path, report), admitted.root, table):
        return report, None
    return report, table


def build(admitted):
    manifest = admitted.manifest
    catalogue = list(manifest.drivers)
    table = Table(manifest.table)
    pool = table.strings
    served = {}
    index = {}
    for task in admitted.tasks:
        index[task.name] = task.index
        if task.serves is not None and amp_port(task.serves[0]) is None:
            served[task.serves[0]] = task.index
    regions = list(admitted.shared)
    if admitted.ends.value != "never":
        table.ends_task = index[admitted.ends.value]
    table.init_priority = admitted.init_priority
    if admitted.stdout != "kernel":
        table.usb_device_console = admitted.stdout.catalogue.usb_device
    runs = {}

    for task in admitted.tasks:
        entry = TaskEntry()
        entry.name = pool.add(task.name)
        symbol = (task.entry_name, "task")
        if task.driver is not None:
            entry.driver = catalogue.index(task.driver)
            symbol = (task.catalogue.start, "driver")
            entry.block = ring_block(task) or 0
            entry.console = task.catalogue.console
            entry.block_uncached = task.catalogue.block_cache == "uncached"
        entry.entry = symbol[0]
        if symbol not in table.externs:
            table.externs.append(symbol)
            table.extern_lines[symbol[0]] = line_of(task.nodes.get("entry", task.nodes.get("driver")))
        if task.driver is None:
            entry.stack = task.stack
            entry.authority = [name for name in AUTHORITIES if name in task.authority]
        entry.priority = task.priority
        entry.ceiling = task.priority
        if task.ceiling is not None:
            entry.ceiling = task.ceiling
        if task.driver is not None and task.catalogue is not None and task.catalogue.threads:
            entry.ceiling = task.priority + max(offset for _, offset, _ in task.catalogue.threads)
        entry.restart_max = task.restart_max
        if task.core is not None:
            entry.core_mask = 1 << task.core
        entry.first_grant = len(table.grants)
        windows = []

        # A packaged driver's Descriptor places what each of its threads receives, so its grants
        # record neither a slot nor a window's place.
        def grant(kind, name, path):
            made = GrantEntry(kind, pool.add(name), pool.add(path))
            if kind in CAPABILITY_KINDS and task.driver is None:
                made.cap_slot = entry.cap_grant_count
                entry.cap_grant_count = entry.cap_grant_count + 1
            if kind in WINDOW_KINDS and task.driver is None:
                made.window = len(windows)
                windows.append(made)
            table.grants.append(made)
            return made

        for field in task.nodes:
            if field == "serves" and amp_port(task.serves[0]) is not None:
                crossing(grant, task.serves[0], "wait", manifest)
            elif field == "serves":
                grant("endpoint_serve", task.serves[0], task.serves[0]).target = task.index
            elif field == "uses":
                for used, node in task.uses:
                    if amp_port(used) is not None:
                        crossing(grant, used, "signal", manifest)
                    else:
                        grant("endpoint_use", used, used).target = served[used]
            elif field == "watches" and task.watches:
                grant("notification", EVENTS, EVENTS)
                made = grant("status", STATUS, STATUS)
                made.flags.append("ro")
                made.size = region_size(status_block(task, manifest), admitted.chip, admitted.cluster, manifest)
            elif field == "devices":
                for k, held in enumerate(task.grants):
                    name = held.path
                    if task.driver is not None:
                        name = task.catalogue.windows[k]
                    kind = "window"
                    if held.space == "port":
                        kind = "ports"
                    made = grant(kind, name, held.path)
                    made.base = held.base
                    made.size = held.size
                    device = held.device
                    if device.name not in runs:
                        runs[device.name] = (len(table.privs), len(device.registers))
                        for offset in device.registers:
                            table.privs.append((offset, WIDTHS[held.space]))
                    if device.registers:
                        made.priv_first, made.priv_count = runs[device.name]
            elif field == "maps":
                for mapped, node in task.maps:
                    made = grant("region", mapped, mapped)
                    made.target = regions.index(mapped)
                    if task.modes[mapped] == "ro":
                        made.flags.append("ro")
                    if admitted.shared[mapped].cache == "uncached":
                        made.flags.append("uncached")
            elif field == "lines":
                # A packaged driver's lines in the order of its line roles, which its descriptor
                # numbers its lines in.
                lines = task.lines
                if task.driver is not None:
                    lines = sorted(task.lines, key=lambda held: task.catalogue.lines.index(held.name))
                for line in lines:
                    made = grant("line", line.name, line.path)
                    made.line_index = line.index
                    made.line = line.source

        entry.grant_count = len(table.grants) - entry.first_grant
        entry.first_use = len(table.refs)
        local = [used for used, node in task.uses if amp_port(used) is None]
        table.refs.extend(served[used] for used in local)
        entry.use_count = len(local)
        entry.first_watch = len(table.refs)
        table.refs.extend(index[name] for name, node in task.watches)
        entry.watch_count = len(task.watches)
        table.tasks.append(entry)

    for path, region in admitted.shared.items():
        size = region_size(region.size, admitted.chip, admitted.cluster, manifest)
        table.regions.append(RegionEntry(pool.add(path), size, region.cache == "uncached",
                                         admitted.offsets.get(path)))
    return table


def crossing(grant, path, right, manifest):
    """The port grant of crossing `path`: the port, its server's node and the right the init delegates."""
    port = amp_port(path)
    made = grant("port", path, path)
    made.flags.append(right)
    made.base = port
    made.target = [node for node, listed in manifest.amp_list if listed == port][0]


def check_table(f, root, table):
    """Whether every count and every field that can hold KOS_TABLE_NONE fits the table."""
    drivers = [task.driver for task in table.tasks if task.driver is not None]
    lines = [grant.line_index for grant in table.grants if grant.line_index is not None]
    rows = [
        ("holds %d tasks", len(table.tasks), COUNT_LIMIT),
        ("holds %d grants", len(table.grants), COUNT_LIMIT),
        ("holds %d refs", len(table.refs), COUNT_LIMIT),
        ("holds %d privileged registers", len(table.privs), COUNT_LIMIT),
        ("holds %d regions", len(table.regions), COUNT_LIMIT),
        ("holds %d bytes of strings", table.strings.size, STRINGS_LIMIT),
        ("names catalogue driver %d", max(drivers + [0]), FIELD_LIMIT),
        ("names line %d of a device", max(lines + [0]), FIELD_LIMIT),
    ]
    fits = True
    for what, value, limit in rows:
        if value > limit:
            f.refuse(root, "encoding.table",
                     "the composition's table %s, past the %d it carries" % (what % value, limit))
            fits = False
    return fits


def printable(text):
    """`text` as printable ASCII, every other character escaped."""
    out = []
    for character in text:
        if " " <= character <= "~" and character != "\\":
            out.append(character)
        else:
            out.append(character.encode("unicode_escape").decode("ascii"))
    return "".join(out)


def c_string(text):
    """`text` inside a C string literal."""
    return printable(text).replace("\"", "\\\"")


def none_or(value):
    if value is None:
        return "KOS_TABLE_NONE"
    return "%d" % value


def bits(names, spelled):
    if not names:
        return "0"
    return " | ".join(spelled[name] for name in names)


def render(table, source, composition=None, output="table.c"):
    """The table's C source, `source` naming the composition it came from. Given the composition's
    path, each entry's declaration is reported at its line there, and what follows at its own line
    in `output`, the name the source is compiled under."""
    pool = table.strings
    out = [
        "// SPDX-License-Identifier: CECILL-C",
        "// Copyright (c) 2026 Philippe Leduc",
        "//",
        "// GENERATED by kickos_compose emit from %s; edits are overwritten by the next emit."
        % printable(source),
        "",
        "#include <kickos/sys/abi.h>",
        "#include <kickos/sys/table.h>",
        "",
        "#include <stddef.h>",
        "",
        "_Static_assert(KICKOS_TABLE_VERSION == %d, \"this table is emitted as layout %d, which "
        "<kickos/sys/table.h> is not\");" % (table.version, table.version),
        "",
    ]
    for symbol, kind in table.externs:
        if composition is not None:
            out.append("#line %d \"%s\"" % (table.extern_lines[symbol], c_string(composition)))
        if kind == "driver":
            out.append("extern int %s(struct kos_driver_instance* instance);" % symbol)
        else:
            out.append("extern void %s(kos_self_t const* self);" % symbol)
    if table.externs and composition is not None:
        out.append("#line %d \"%s\"" % (len(out) + 2, c_string(output)))
    if table.externs:
        out.append("")

    arrays = [
        ("task", "struct kos_table_task", len(table.tasks)),
        ("grant", "struct kos_table_grant", len(table.grants)),
        ("ref", "struct kos_table_ref", len(table.refs)),
        ("priv", "struct kos_table_priv", len(table.privs)),
        ("region", "struct kos_table_region", len(table.regions)),
        ("strings", "char", pool.size),
    ]
    present = [array for array in arrays if array[2] > 0]
    out.append("struct kickos_table_image")
    out.append("{")
    out.append("    struct kos_table_header header;")
    for member, kind, count in present:
        out.append("    %s %s[%d];" % (kind, member, count))
    out.append("};")
    out.append("")
    end = "sizeof(struct kos_table_header)"
    for member, kind, count in present:
        out.append("_Static_assert(offsetof(struct kickos_table_image, %s) == %s," % (member, end))
        out.append("               \"the table's arrays follow one another with no gap\");")
        end = "offsetof(struct kickos_table_image, %s) + %d * sizeof(%s)" % (member, count, kind)
    if present:
        out.append("")
    slots = [grant.cap_slot for grant in table.grants if grant.cap_slot is not None]
    if slots:
        out.append("_Static_assert(KOS_SPAWN_DELEGATED_CAP0 + %d < KOS_TABLE_NONE," % max(slots))
        out.append("               \"a capability slot of this table reads as none\");")
        out.append("")

    flags = "0"
    if table.ends_task is not None:
        flags = "KOS_TABLE_ENDS_TASK"
    out.append("static struct kickos_table_image const kickos_table_image = {")
    out.append("    .header = {")
    out.append("        .magic = KOS_TABLE_MAGIC,")
    out.append("        .version = KICKOS_TABLE_VERSION,")
    out.append("        .flags = %s," % flags)
    out.append("        .ends_task = %s," % none_or(table.ends_task))
    out.append("        .task_count = %d," % len(table.tasks))
    out.append("        .grant_count = %d," % len(table.grants))
    out.append("        .ref_count = %d," % len(table.refs))
    out.append("        .priv_count = %d," % len(table.privs))
    out.append("        .region_count = %d," % len(table.regions))
    out.append("        .strings_size = %d," % pool.size)
    out.append("        .init_priority = %d," % table.init_priority)
    out.append("    },")
    if table.tasks:
        out.append("    .task = {")
        for n, task in enumerate(table.tasks):
            entry = ".task = %s" % task.entry
            if task.driver is not None:
                entry = ".driver = %s" % task.entry
            out.append("        [%d] = {" % n)
            out.append("            .name = %d," % pool.offsets[task.name])
            out.append("            .block = %d," % task.block)
            out.append("            .entry = { %s }," % entry)
            out.append("            .driver = %s," % none_or(task.driver))
            out.append("            .stack = %d," % task.stack)
            out.append("            .ceiling = %d," % task.ceiling)
            out.append("            .priority = %d," % task.priority)
            out.append("            .restart_max = %d," % task.restart_max)
            flags = []
            if task.console:
                flags.append("KOS_TABLE_TASK_CONSOLE")
            if task.block_uncached:
                flags.append("KOS_TABLE_TASK_BLOCK_UNCACHED")
            if flags:
                out.append("            .flags = %s," % " | ".join(flags))
            out.append("            .core_mask = 0x%Xu," % task.core_mask)
            out.append("            .authority = %s," % bits(task.authority, AUTHORITY_BITS))
            out.append("            .first_grant = %d," % task.first_grant)
            out.append("            .grant_count = %d," % task.grant_count)
            out.append("            .cap_grant_count = %d," % task.cap_grant_count)
            out.append("            .first_use = %d," % task.first_use)
            out.append("            .use_count = %d," % task.use_count)
            out.append("            .first_watch = %d," % task.first_watch)
            out.append("            .watch_count = %d," % task.watch_count)
            out.append("        },")
        out.append("    },")
    if table.grants:
        out.append("    .grant = {")
        for n, grant in enumerate(table.grants):
            slot = "KOS_TABLE_NONE"
            if grant.cap_slot is not None:
                slot = "KOS_SPAWN_DELEGATED_CAP0 + %d" % grant.cap_slot
            out.append("        [%d] = {" % n)
            out.append("            .kind = KOS_GRANT_%s," % grant.kind.upper())
            out.append("            .flags = %s," % bits(grant.flags, GRANT_FLAGS))
            out.append("            .cap_slot = %s," % slot)
            out.append("            .name = %d," % pool.offsets[grant.name])
            out.append("            .path = %d," % pool.offsets[grant.path])
            out.append("            .target = %s," % none_or(grant.target))
            out.append("            .window = %s," % none_or(grant.window))
            out.append("            .base = 0x%Xu," % grant.base)
            out.append("            .size = 0x%Xu," % grant.size)
            out.append("            .line_index = %s," % none_or(grant.line_index))
            out.append("            .line = %s," % none_or(grant.line))
            out.append("            .priv_first = %d," % grant.priv_first)
            out.append("            .priv_count = %d," % grant.priv_count)
            out.append("        },")
        out.append("    },")
    if table.refs:
        out.append("    .ref = {")
        for n, task in enumerate(table.refs):
            out.append("        [%d] = { .task = %d }," % (n, task))
        out.append("    },")
    if table.privs:
        out.append("    .priv = {")
        for n, (offset, width) in enumerate(table.privs):
            out.append("        [%d] = { .offset = 0x%Xu, .width = %d }," % (n, offset, width))
        out.append("    },")
    if table.regions:
        out.append("    .region = {")
        for n, region in enumerate(table.regions):
            flags = []
            if region.uncached:
                flags.append("uncached")
            offset = 0
            if region.offset is not None:
                flags.append("partition")
                offset = region.offset
            out.append("        [%d] = { .name = %d, .size = 0x%Xu, .offset = 0x%Xu, .flags = %s },"
                       % (n, pool.offsets[region.name], region.size, offset, bits(flags, REGION_FLAGS)))
        out.append("    },")
    if pool.texts:
        # The literal's own terminator is the last string's, so the array holds the pool exactly.
        out.append("    .strings =")
        for n, text in enumerate(pool.texts):
            nul = "\\0"
            if n == len(pool.texts) - 1:
                nul = ""
            out.append("        \"%s%s\"" % (text.replace("\\", "\\\\").replace("\"", "\\\""), nul))
    out.append("};")
    out.append("")
    out.append("struct kos_table_header const* const kickos_table = &kickos_table_image.header;")
    out.append("char const kickos_link_one_system_target = 1;")
    out.append("char const kickos_usb_device_console = %d;" % int(table.usb_device_console))
    return "\n".join(out) + "\n"


def ld_text(text):
    """`text` inside a linker script's double-quoted string."""
    return printable(text).replace("\"", "'")


def render_asserts(admitted, composition):
    """The link-time asserts of docs/design-m10-target.md, section 6, an implicit script over the
    chip script's symbols, each spelled as the build's C ABI spells it, citing the composition by
    its path."""
    name = ld_text(composition)
    out = [
        "/* SPDX-License-Identifier: CECILL-C",
        " * Copyright (c) 2026 Philippe Leduc",
        " *",
        " * GENERATED by kickos_compose emit from %s; edits are overwritten by the next emit." % name,
        " */",
        "",
    ]
    hosted = host_runs(admitted.chip)
    if not hosted:
        out.extend(stack_asserts(admitted, name))
        out.extend(arena_asserts(admitted))
    out.extend(heap_asserts(admitted, name, hosted))
    return "\n".join(out) + "\n"


def c_symbol(manifest, symbol):
    return (manifest.symbol_prefix or "") + symbol


def stack_asserts(admitted, name):
    """Each reserved stack holds the image's thread-local carve above KICKOS_MIN_STACK_SIZE, where
    SP is not masked."""
    manifest = admitted.manifest
    out = []
    if manifest.stack_stride is not None or manifest.min_stack is None:
        return out
    carve = c_symbol(manifest, "__kickos_tls_carve")
    for task in admitted.tasks:
        if not task.entry or task.stack is None:
            continue
        out.append("ASSERT(%d >= %d + %s," % (task.stack, manifest.min_stack, carve))
        out.append("       \"KickOS: %s's %d-byte stack cannot hold the linked image's thread-local block, "
                   "__kickos_tls_carve, above the %d bytes of KICKOS_MIN_STACK_SIZE, so its `stack` in %s "
                   "would have to grow\")" % (ld_text(task.label()), task.stack, manifest.min_stack, name))
    return out


def arena_asserts(admitted):
    """On a region board, each block arch_ram_alloc places, from the linked arena's start, ends at or
    below its end; the cursor is a symbol of this script."""
    manifest = admitted.manifest
    out = []
    if admitted.translating:
        return out
    cursor = c_symbol(manifest, "__kickos_ram_start")
    end = c_symbol(manifest, "__kickos_ram_end")
    for n, (what, want, figure) in enumerate(arena_blocks(admitted.tasks, admitted.shared, manifest)):
        if not want:
            continue
        placed = "__kickos_system_arena_%d" % n
        out.append("%s = ALIGN(%s, 0x%X) + 0x%X;" % (placed, cursor, ram_align(want, manifest),
                                                  ram_size(want, manifest)))
        out.append("ASSERT(%s <= %s," % (placed, end))
        out.append("       \"KickOS: the arena cannot hold %s, placed as arch_ram_alloc places it after every "
                   "block before it, from __kickos_ram_start to __kickos_ram_end; %s, or a block before it, "
                   "would have to shrink, or the image's static footprint\")" % (ld_text(what), ld_text(figure)))
        cursor = placed
    return out


def heap_asserts(admitted, name, hosted):
    """KICKOS_USER_HEAP_SIZE is this system's heap, and the image carves at least that much where
    its link carves one."""
    manifest = admitted.manifest
    out = ["ASSERT(KICKOS_USER_HEAP_SIZE == %d," % admitted.heap,
           "       \"KickOS: KICKOS_USER_HEAP_SIZE is not the %d-byte heap of %s, which its system target "
           "defines at the link: link exactly one system target, and define no KICKOS_USER_HEAP_SIZE of the "
           "app's own\")" % (admitted.heap, name)]
    if admitted.heap and not hosted:
        out.append("ASSERT(%s - %s >= %d," % (c_symbol(manifest, "_kickos_heap_limit"),
                                              c_symbol(manifest, "_kickos_heap_start"), admitted.heap))
        out.append("       \"KickOS: the %d-byte heap of %s, its `heap`, is more than the image carves from "
                   "_kickos_heap_start to _kickos_heap_limit: state a smaller `heap` in %s, or link a kernel "
                   "package built to carve more, whose KICKOS_APPDATA_SIZE is that span where the heap is the app "
                   "window's pad\")" % (admitted.heap, name, name))
    return out


def render_fragment(admitted, table, source, gate=False):
    """The CMake fragment kickos_compose reads: the packaged drivers the composition names and the
    libraries their clients link, the driver serving stdout or `kernel`, whether it names
    KickOS::main's entry, and its heap."""
    drivers = []
    clients = []
    stdout = "kernel"
    if admitted.stdout != "kernel":
        stdout = admitted.stdout.driver
    for task in admitted.tasks:
        if task.driver is not None and task.driver not in drivers:
            drivers.append(task.driver)
            for client in task.catalogue.client:
                if client not in clients:
                    clients.append(client)
    main = 0
    if (PACKAGED_MAIN, "task") in table.externs:
        main = 1
    out = [
        "# SPDX-License-Identifier: CECILL-C",
        "# Copyright (c) 2026 Philippe Leduc",
        "#",
        "# GENERATED by kickos_compose emit from %s; edits are overwritten by the next emit." % printable(source),
        "",
        "set(KICKOS_COMPOSE_DRIVERS \"%s\")" % ";".join(drivers),
        "set(KICKOS_COMPOSE_CLIENTS \"%s\")" % ";".join(clients),
        "set(KICKOS_COMPOSE_STDOUT \"%s\")" % stdout,
        "set(KICKOS_COMPOSE_MAIN %d)" % main,
        "set(KICKOS_COMPOSE_HEAP %d)" % admitted.heap,
        "set(KICKOS_COMPOSE_GATE %d)" % int(gate),
    ]
    return "\n".join(out) + "\n"


def names_or_dash(names):
    if not names:
        return "-"
    return ",".join(names)


def none_text(value, form="%d"):
    if value is None:
        return "none"
    return form % value


def dump(table):
    """The canonical dump of the model, as tests/table_walker.c prints a compiled table."""
    pool = table.strings
    flags = []
    if table.ends_task is not None:
        flags.append("ends_task")
    out = ["header magic=ok version=%d flags=%s ends_task=%s tasks=%d grants=%d refs=%d privs=%d regions=%d "
           "strings=%d init_priority=%d"
           % (table.version, names_or_dash(flags), none_text(table.ends_task), len(table.tasks), len(table.grants),
              len(table.refs), len(table.privs), len(table.regions), pool.size, table.init_priority)]
    for n, task in enumerate(table.tasks):
        names = []
        if task.console:
            names.append("console")
        if task.block_uncached:
            names.append("block_uncached")
        flags = names_or_dash(names)
        out.append("task %d name=%s entry=%s driver=%s stack=%d block=%d priority=%d ceiling=%d restart_max=%d flags=%s "
                   "core_mask=0x%X authority=%s grants=%d+%d cap_grants=%d uses=%d+%d watches=%d+%d"
                   % (n, task.name, task.entry, none_text(task.driver), task.stack, task.block, task.priority,
                      task.ceiling, task.restart_max, flags, task.core_mask, names_or_dash(task.authority), task.first_grant, task.grant_count,
                      task.cap_grant_count, task.first_use, task.use_count, task.first_watch, task.watch_count))
    for n, grant in enumerate(table.grants):
        out.append("grant %d kind=%s flags=%s cap_slot=%s name=%s path=%s target=%s window=%s base=0x%X size=0x%X "
                   "line_index=%s line=%s privs=%d+%d"
                   % (n, grant.kind, names_or_dash(grant.flags), none_text(grant.cap_slot, "cap0+%d"), grant.name,
                      grant.path, none_text(grant.target), none_text(grant.window), grant.base, grant.size,
                      none_text(grant.line_index), none_text(grant.line), grant.priv_first, grant.priv_count))
    for n, task in enumerate(table.refs):
        out.append("ref %d task=%d" % (n, task))
    for n, (offset, width) in enumerate(table.privs):
        out.append("priv %d offset=0x%X width=%d" % (n, offset, width))
    for n, region in enumerate(table.regions):
        names = []
        if region.uncached:
            names.append("uncached")
        offset = 0
        if region.offset is not None:
            names.append("partition")
            offset = region.offset
        out.append("region %d name=%s size=0x%X offset=0x%X flags=%s"
                   % (n, region.name, region.size, offset, names_or_dash(names)))
    for text in pool.texts:
        out.append("string %d %s" % (pool.offsets[text], text))
    return "\n".join(out) + "\n"
