# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The export manifest a kernel build writes (tools/manifest/genmanifest.py), and the chip and
# board files it names, relative to its own folder.

import os
import re

from ruamel.yaml.nodes import ScalarNode

from .descriptions import check_platform
from .manifest_fields import DRIVER_FIELDS, WINDOWS_KNOB, is_pool
from .subset import C_IDENTIFIER, FILE_NAME, IDENTIFIER, File, Report, line_of

# The versions of the manifest format this tool reads.
MANIFEST_VERSIONS = (1,)
MANIFEST_FIELDS = ("version", "abi", "target", "protection", "pools", "threads", "init", "descriptions", "default",
                   "drivers")
TARGET_FIELDS = ("board", "chip", "arch", "cores", "kernel_cores", "isolated_cores", "amp")
PROTECTION_FIELDS = ("enforced", "window_rule", "smallest_window", "thread_windows", "fault_isolation")
ABI_FIELDS = ("table", "cap_reserved", "symbol_prefix")
# What the C ABI puts before a C name in the link: none, or rx-elf's one underscore.
SYMBOL_PREFIX = re.compile(r"_*")
THREAD_FIELDS = ("priority", "min_stack", "user_stack", "idle_stack", "root_stack", "stack_align", "stack_stride")
INIT_FIELDS = ("status_record_size", "private_record_size", "free_regions")
THREAD_ENTRY_FIELDS = ("name", "priority", "stack", "caps", "badged")
CORES = 32
# The layouts of the emitted table emit.py writes.
TABLE_LAYOUTS = (3,)


class Manifest:
    """What admission reads of a manifest."""

    def __init__(self):
        self.board = None
        self.chip = None
        self.cores = None
        self.kernel_cores = None
        self.isolated_cores = 0
        self.amp_ports = 0
        # Bytes of the partition's user share, the top of the region every node writes.
        self.amp_share = 0
        # Its one memory type, in a region's `cache` vocabulary.
        self.amp_share_cache = "cached"
        self.cap_reserved = None
        # What the link spells before a C name.
        self.symbol_prefix = None
        self.table = None
        self.enforced = None
        # Whether a fault ends its task rather than the system.
        self.fault_isolation = None
        self.window_rule = None
        self.smallest_window = None
        self.thread_windows = None
        self.pools = {}
        self.priority = None
        self.min_stack = None
        self.user_stack = None
        self.idle_stack = None
        self.root_stack = None
        self.stack_align = None
        # The block every thread stack is where the thread pointer is masked from SP, or None.
        self.stack_stride = None
        # The bytes of one record in the init's public status block, one per task, and in its
        # private block, one per task and per shared region, and the regions root's set holds past
        # its statics and stack, which a region board's init self-grants into.
        self.status_record_size = None
        self.private_record_size = None
        self.free_regions = None
        # The (chip file, board file) its `descriptions` names, by absolute path, or None.
        self.descriptions = None
        self.drivers = {}


class Driver:
    def __init__(self):
        self.windows = []
        self.lines = []
        # In thread order: each thread as (name, priority offset, stack or "default"), the
        # capabilities its spawn delegates, and the badged copies of the driver's notification
        # among them.
        self.threads = []
        self.caps = []
        self.badged = []
        # The index of the thread that receives on the endpoint.
        self.receiver = None
        self.endpoints = 0
        self.notifications = 0
        self.block = "none"
        self.posture = None
        self.console = False
        self.start = None
        # The libraries a task using the driver links, by target name.
        self.client = []


def check_manifests(paths):
    report = Report()
    count = 0
    for path in paths:
        if read_manifest(path, report, False) is not False:
            count = count + 1
    return report, count


def read_manifest(path, report, clean=True):
    """The Manifest, or None once refused, or False when the file cannot be read. With `clean`, a
    manifest any refusal was made in reads as None."""
    try:
        with open(path, encoding="utf-8") as stream:
            text = stream.read()
    except (OSError, UnicodeDecodeError) as error:
        report.refuse(path, 1, "form.unreadable", "the file cannot be read: %s" % error)
        return False
    before = len(report.refusals)
    manifest = check_manifest(path, text, report)
    if clean and len(report.refusals) != before:
        return None
    return manifest


def check_manifest(path, text, report):
    f = File(path, report)
    root = f.load(text)
    if root is None:
        return None
    top = f.fields(root, "the manifest", MANIFEST_FIELDS,
                   ("version", "abi", "target", "protection", "pools", "threads", "init", "drivers"))
    if top is None or not f.version(top, "the manifest", MANIFEST_VERSIONS):
        return None
    manifest = Manifest()
    if "abi" in top:
        abi = f.fields(top["abi"], "`abi`", ABI_FIELDS, ABI_FIELDS)
        if abi is not None and "table" in abi:
            manifest.table = f.integer(abi["table"], "`abi` table", 16)
        if manifest.table is not None and manifest.table not in TABLE_LAYOUTS:
            f.refuse(abi["table"], "form.version",
                     "`abi` table %d is a table layout this tool does not emit (%s)"
                     % (manifest.table, ", ".join(str(v) for v in TABLE_LAYOUTS)))
        if abi is not None and "cap_reserved" in abi:
            manifest.cap_reserved = f.integer(abi["cap_reserved"], "`abi` cap_reserved", 8)
        if abi is not None and "symbol_prefix" in abi:
            manifest.symbol_prefix = f.name(abi["symbol_prefix"], "`abi` symbol_prefix", SYMBOL_PREFIX)
    target = None
    if "target" in top:
        target = check_target(f, top["target"], manifest)
    if target is not None:
        manifest.board = target.get("board")
        manifest.chip = target.get("chip")
    if "protection" in top:
        check_protection(f, top["protection"], manifest)
    if "pools" in top:
        pools = f.mapping(top["pools"], "`pools`")
        for name, (key, value) in (pools or {}).items():
            if not is_pool(name):
                f.refuse(key, "form.unknown-field",
                         "`pools` has no field `%s`; it holds every KICKOS_MAX_* but %s, every "
                         "KICKOS_TASK_*_BUDGET, KICKOS_CAP_TABLE_SUPPLY, KICKOS_RAM_OWNER_SLOTS and "
                         "KICKOS_ASPACE_RANGES" % (name, WINDOWS_KNOB))
                continue
            manifest.pools[name] = f.integer(value, "pool `%s`" % name, 32)
    if "threads" in top:
        check_threads(f, top["threads"], manifest)
    if "init" in top:
        check_init(f, top["init"], manifest)
    if "descriptions" in top and target is not None:
        manifest.descriptions = check_descriptions(f, top["descriptions"], target)
    if "default" in top and target is not None:
        check_default(f, top["default"], target, "descriptions" in top)
    if "drivers" in top:
        drivers = f.mapping(top["drivers"], "`drivers`")
        for name, (key, value) in (drivers or {}).items():
            f.name(key, "driver", IDENTIFIER)
            manifest.drivers[name] = check_driver(f, value, "driver `%s`" % name)
    return manifest


def check_target(f, node, manifest):
    """The target's board and chip names by field, None where refused, or None for no target."""
    values = f.fields(node, "`target`", TARGET_FIELDS, ("board", "arch", "cores", "kernel_cores", "isolated_cores"))
    if values is None:
        return None
    names = {}
    for field in ("board", "chip"):
        if field in values:
            names[field] = f.name(values[field], "`target` %s" % field, FILE_NAME)
    if "arch" in values:
        f.name(values["arch"], "`target` arch", IDENTIFIER)
    counts = {}
    for field in ("cores", "kernel_cores"):
        if field in values:
            counts[field] = f.integer(values[field], "`target` %s" % field, 16)
    mask = None
    if "isolated_cores" in values:
        mask = f.integer(values["isolated_cores"], "`target` isolated_cores", CORES)
    cores = counts.get("cores")
    kernel = counts.get("kernel_cores")
    manifest.cores = cores
    manifest.kernel_cores = kernel
    if mask is not None:
        manifest.isolated_cores = mask
    if cores is not None and kernel is not None:
        if cores == 0 or cores > CORES or kernel == 0 or kernel > cores:
            f.refuse(values["kernel_cores"], "manifest.cores",
                     "`target` schedules %d kernel core(s) of %d, which is no count from 1 to the "
                     "image's cores, at most %d" % (kernel, cores, CORES))
        elif mask is not None and (mask & 1 or mask >> kernel):
            f.refuse(values["isolated_cores"], "manifest.cores",
                     "`target` isolated_cores 0x%X names core 0, the boot core, or a core past the "
                     "%d the kernel schedules" % (mask, kernel))
    if "amp" in values:
        if kernel is not None and kernel != 1:
            f.refuse(values["kernel_cores"], "manifest.amp",
                     "`target` is an AMP node, which schedules one kernel core, not %d" % kernel)
        check_amp(f, values["amp"], manifest)
    return names


def check_amp(f, node, manifest):
    values = f.fields(node, "`target` amp", ("node", "nodes", "ports", "share", "share_cache"),
                      ("node", "nodes", "ports", "share", "share_cache"))
    if values is None:
        return
    if "share" in values:
        manifest.amp_share = f.integer(values["share"], "`target` amp share", 32) or 0
    if "share_cache" in values:
        manifest.amp_share_cache = f.enum(values["share_cache"], "`target` amp share_cache",
                                          ("cached", "uncached")) or "cached"
    if "nodes" not in values:
        return
    nodes = f.integer(values["nodes"], "`target` amp nodes", 8)
    if "node" in values:
        own = f.integer(values["node"], "`target` amp node", 8)
        if own is not None and nodes is not None and own >= nodes:
            f.refuse(values["node"], "manifest.amp",
                     "`target` amp node %d is not one of the partition's %d" % (own, nodes))
    if "ports" in values:
        ports = f.sequence(values["ports"], "`target` amp ports") or ()
        manifest.amp_ports = len(ports)
        for item in ports:
            pair = f.pair(item, "`target` amp port", "node", "port", (8, 8))
            if pair is not None and nodes is not None and pair[0] >= nodes:
                f.refuse(item, "manifest.amp",
                         "`target` amp port %d is served by node %d, not one of the partition's %d"
                         % (pair[1], pair[0], nodes))


def check_protection(f, node, manifest):
    values = f.fields(node, "`protection`", PROTECTION_FIELDS,
                      ("enforced", "window_rule", "thread_windows", "fault_isolation"))
    if values is None:
        return
    if "enforced" in values:
        manifest.enforced = f.boolean(values["enforced"], "`protection` enforced")
    if "fault_isolation" in values:
        manifest.fault_isolation = f.boolean(values["fault_isolation"], "`protection` fault_isolation")
    if "thread_windows" in values:
        windows = f.integer(values["thread_windows"], "`protection` thread_windows", 8)
        if windows == 0:
            f.refuse(values["thread_windows"], "manifest.bound", "`protection` thread_windows is 0, so no task holds a window")
        manifest.thread_windows = windows
    rule = None
    if "window_rule" in values:
        rule = f.enum(values["window_rule"], "`protection` window_rule", ("pow2", "granule", "none"))
    manifest.window_rule = rule
    if rule in ("pow2", "granule") and "smallest_window" not in values:
        f.refuse(node, "form.missing", "`protection` encodes windows as `%s`, so it needs `smallest_window`" % rule)
    if "smallest_window" not in values:
        return
    if "window_rule" not in values or rule == "none":
        f.refuse(values["smallest_window"], "form.inapplicable",
                 "`protection` has a `smallest_window`, which only a `window_rule` of pow2 or granule has")
        return
    if rule is None:
        return
    smallest = f.integer(values["smallest_window"], "`protection` smallest_window", 32)
    if smallest is not None and (smallest == 0 or smallest & (smallest - 1)):
        f.refuse(values["smallest_window"], "manifest.window",
                 "`protection` smallest_window 0x%X is not a power of two" % smallest)
    manifest.smallest_window = smallest


def check_threads(f, node, manifest):
    values = f.fields(node, "`threads`", THREAD_FIELDS, THREAD_FIELDS)
    if values is None:
        return
    if "priority" in values:
        pair = f.pair(values["priority"], "`threads` priority", "lo", "hi", (8, 8))
        if pair is not None and (pair[0] == 0 or pair[1] < pair[0]):
            f.refuse(values["priority"], "manifest.priority",
                     "`threads` priority `[%d, %d]` is no range above the idle priority 0" % pair)
        else:
            manifest.priority = pair
    for field in ("min_stack", "user_stack", "idle_stack", "root_stack"):
        if field not in values:
            continue
        size = f.integer(values[field], "`threads` %s" % field, 32)
        if size == 0:
            f.refuse(values[field], "manifest.bound", "`threads` %s is 0, which seats no thread" % field)
        setattr(manifest, field, size)
    if "stack_align" in values:
        align = f.integer(values["stack_align"], "`threads` stack_align", 16)
        if align is not None and (align == 0 or align & (align - 1)):
            f.refuse(values["stack_align"], "manifest.bound",
                     "`threads` stack_align %d is not a power of two" % align)
        manifest.stack_align = align
    if "stack_stride" in values:
        stride = word_or_integer(f, values["stack_stride"], "`threads` stack_stride", "none", 32)
        if isinstance(stride, int) and (stride == 0 or stride & (stride - 1)):
            f.refuse(values["stack_stride"], "manifest.bound",
                     "`threads` stack_stride %d is not a power of two" % stride)
        elif isinstance(stride, int):
            manifest.stack_stride = stride


def check_init(f, node, manifest):
    values = f.fields(node, "`init`", INIT_FIELDS, INIT_FIELDS)
    if values is None:
        return
    for field in ("status_record_size", "private_record_size"):
        if field not in values:
            continue
        size = f.integer(values[field], "`init` %s" % field, 16)
        if size == 0:
            f.refuse(values[field], "manifest.bound", "`init` %s is 0, which holds no task's record" % field)
        setattr(manifest, field, size)
    if "free_regions" in values:
        manifest.free_regions = f.integer(values["free_regions"], "`init` free_regions", 8)


def check_descriptions(f, node, target):
    """The (chip file, board file) `descriptions` names, by absolute path, or None."""
    values = f.fields(node, "`descriptions`", ("chip", "board"), ("chip", "board"))
    if values is None:
        return None
    if "chip" not in target:
        f.refuse(node, "manifest.description-path", "`descriptions` belong to a `target` that names its chip")
        return None
    chip = target["chip"]
    board = target.get("board")
    if chip is None or board is None:
        return None
    expected = {"chip": "platform/%s/chip.yaml" % chip, "board": "platform/%s/%s.yaml" % (chip, board)}
    here = os.path.dirname(os.path.abspath(f.filename))
    files = []
    for field in ("chip", "board"):
        if field not in values:
            continue
        text = f.string(values[field], "`descriptions` %s" % field)
        if text is None:
            continue
        if text != expected[field]:
            f.refuse(values[field], "manifest.description-path",
                     "`descriptions` %s is `%s`; the %s file of this target is `%s`"
                     % (field, text, field, expected[field]))
            continue
        path = os.path.join(here, text)
        if not os.path.isfile(path):
            f.refuse(values[field], "manifest.description-unknown",
                     "`descriptions` %s names %s, which does not exist" % (field, path))
            continue
        files.append(path)
    if len(files) != 2:
        return None
    report, count = check_platform(files)
    f.report.refusals.extend(report.refusals)
    return tuple(files)


def check_default(f, node, target, described):
    values = f.fields(node, "`default`", ("composition",), ("composition",))
    if values is None or "composition" not in values:
        return
    board = target.get("board")
    text = f.string(values["composition"], "`default` composition")
    if board is None or text is None:
        return
    if not described:
        f.refuse(node, "manifest.default-path",
                 "`default` names a composition for a target with no `descriptions` to admit it against")
        return
    expected = "boards/%s/composition.yaml" % board
    if text != expected:
        f.refuse(values["composition"], "manifest.default-path",
                 "`default` composition is `%s`; the default composition of this target is `%s`" % (text, expected))
        return
    path = os.path.join(os.path.dirname(os.path.abspath(f.filename)), text)
    if not os.path.isfile(path):
        f.refuse(values["composition"], "manifest.default-unknown",
                 "`default` composition names %s, which does not exist" % path)


def word_or_integer(f, node, what, word, bits):
    if isinstance(node, ScalarNode) and node.style is None and node.value == word:
        return word
    return f.integer(node, "%s, or `%s`," % (what, word), bits)


def roles(f, node, what, pattern=IDENTIFIER, kind="a role"):
    items = f.sequence(node, what)
    seen = {}
    for item in items or ():
        name = f.name(item, "%s in %s" % (kind, what), pattern)
        if name is None:
            continue
        if name in seen:
            f.refuse(item, "form.duplicate-entry", "%s lists `%s` twice, first on line %d" % (what, name, seen[name]))
        else:
            seen[name] = line_of(item)
    return list(seen)


def check_driver(f, node, what):
    driver = Driver()
    values = f.fields(node, what, DRIVER_FIELDS, DRIVER_FIELDS)
    if values is None:
        return driver
    for field in ("windows", "lines"):
        if field in values:
            setattr(driver, field, roles(f, values[field], "%s %s" % (what, field)))
    threads = None
    if "threads" in values:
        threads = f.sequence(values["threads"], "%s threads" % what)
        seen = {}
        for n, item in enumerate(threads or ()):
            twhat = "%s thread %d" % (what, n + 1)
            thread = f.fields(item, twhat, THREAD_ENTRY_FIELDS, THREAD_ENTRY_FIELDS)
            if thread is None:
                continue
            name = None
            offset = None
            stack = None
            if "name" in thread:
                name = f.name(thread["name"], "%s name" % twhat, IDENTIFIER)
                if name in seen:
                    f.refuse(thread["name"], "form.duplicate-entry",
                             "%s threads name `%s` twice, first on line %d" % (what, name, seen[name]))
                elif name is not None:
                    seen[name] = line_of(thread["name"])
            if "priority" in thread:
                offset = f.integer(thread["priority"], "%s priority offset" % twhat, 8)
            if "stack" in thread:
                stack = word_or_integer(f, thread["stack"], "%s stack" % twhat, "default", 32)
            driver.threads.append((name, offset, stack))
            caps = None
            if "caps" in thread:
                caps = f.integer(thread["caps"], "%s caps" % twhat, 8)
            driver.caps.append(caps)
            badged = None
            if "badged" in thread:
                badged = f.integer(thread["badged"], "%s badged" % twhat, 8)
            if badged is not None and caps is not None and badged > caps:
                f.refuse(thread["badged"], "manifest.bound",
                         "%s mints %d badged copies for a spawn that delegates %d capabilities" % (twhat, badged, caps))
            driver.badged.append(badged)
    for field in ("endpoints", "notifications"):
        if field in values:
            setattr(driver, field, f.integer(values[field], "%s %s" % (what, field), 8))
    block = None
    if "block" in values:
        block = word_or_integer(f, values["block"], "%s block" % what, "none", 32)
        if isinstance(block, int) and (block == 0 or block & (block - 1)):
            f.refuse(values["block"], "manifest.bound",
                     "%s has a %d-byte ring block, and the kernel grants a ring block only as one power "
                     "of two" % (what, block))
        driver.block = block
    posture = None
    if "posture" in values:
        posture = f.enum(values["posture"], "%s posture" % what, ("handover", "retain"))
        driver.posture = posture
    if "console" in values:
        driver.console = f.boolean(values["console"], "%s console" % what) is True
    if "start" in values:
        driver.start = f.name(values["start"], "%s start" % what, C_IDENTIFIER)
    if "client" in values:
        driver.client = roles(f, values["client"], "%s client" % what, C_IDENTIFIER, "a library")
    if "receiver" in values:
        receiver = f.name(values["receiver"], "%s receiver" % what, IDENTIFIER)
        names = [name for name, offset, stack in driver.threads]
        if receiver is not None and receiver not in names:
            f.refuse(values["receiver"], "manifest.receiver",
                     "%s receives on its endpoint in thread `%s`, which is none of its threads" % (what, receiver))
        elif receiver is not None:
            driver.receiver = names.index(receiver)
    if driver.console and posture == "retain":
        f.refuse(values["console"], "manifest.console",
                 "%s takes the console and retains its endpoint; a console driver hands it over" % what)
    if "barrier" in values:
        barrier = word_or_integer(f, values["barrier"], "%s barrier" % what, "none", 8)
        if isinstance(barrier, int) and block == "none":
            f.refuse(values["barrier"], "manifest.barrier",
                     "%s polls a readiness latch after %d thread(s), and the latch lives in the ring "
                     "block it has none of" % (what, barrier))
        elif isinstance(barrier, int) and threads is not None and (barrier == 0 or barrier > len(threads)):
            f.refuse(values["barrier"], "manifest.barrier",
                     "%s polls its readiness after %d of its %d threads, which is no point between "
                     "two spawns or after the last" % (what, barrier, len(threads)))
    return driver
