# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

import os

from ruamel.yaml.nodes import ScalarNode

from .descriptions import PART, check_board
from .manifest import read_manifest
from .supply import check_drivers, check_priorities, check_scheduling, check_supply
from .subset import C_IDENTIFIER, FILE_NAME, IDENTIFIER, File, Report, index_below, line_of

# The versions of the composition format this tool reads.
COMPOSITION_VERSIONS = (1,)
COMPOSITION_FIELDS = ("version", "board", "cluster", "stdout", "ends", "accepts", "heap", "init", "shared", "tasks")
INIT_FIELDS = ("priority",)
TASK_FIELDS = (
    "name", "entry", "driver", "stack", "priority", "ceiling", "core", "devices", "lines", "serves", "uses",
    "maps", "watches", "authority", "accepts", "restart",
)
SHARED_FIELDS = ("name", "size", "cache", "partition")
SHARED_REQUIRED = ("name", "size", "cache")
AUTHORITIES = ("memory", "pinmux", "pstate", "irq", "system", "console", "tasks", "bus_master")
NOT_NEEDED = {
    "no_protection": ("a build that enforces nothing or a unit `none`, and every unit here translates, "
                      "which no build turns off"),
    "no_privilege_split": "a unit with `privilege: false`",
    "cached_incoherent": "a `cached` region over a data cache while a task holds a bus master",
    "device_not_isolated": "a device window granted where the unit has `covers_devices: false`",
    "coarse_gate": "a device window a `device_gate`, a translating page or a pin's port bank opens with its neighbours",
    "bus_master": "a grant of a `bus_master` device",
}
NOCACHE_REFUSED = "refused"
NOCACHE_PROGRAMMED = "programmed"
NOCACHE_ALREADY = "already"
LIMITATIONS = (
    "no_protection", "no_privilege_split", "device_not_isolated", "coarse_gate", "bus_master",
    "cached_incoherent",
)
COMPOSITION_LIMITATIONS = ("no_protection", "no_privilege_split", "cached_incoherent")
TASK_LIMITATIONS = ("device_not_isolated", "coarse_gate", "bus_master")
CORES = 32
# The bits of a watcher's notification, the i-th task it watches raising bit i.
WATCH_BITS = 32
# The ports an /amp crossing names: the window layer carries one in a byte.
AMP_PORTS = 256
# The port the window layer keeps for replies, which no crossing names.
AMP_PORT_REPLY = 1
# KOS_TABLE_NONE, which no line of the table may be.
LINE_NONE = 0xFFFF
# What the emitted table's C source defines, and the prefixes its headers declare and define in.
TABLE_NAMES = ("kickos_table", "kickos_table_image")
ABI_PREFIXES = ("kos_", "KOS_", "KICKOS_")
# C11 and C++20 keywords, and the C++ alternative tokens, none of which names a function.
KEYWORDS = frozenset("""
    alignas alignof and and_eq asm auto bitand bitor bool break case catch char char8_t char16_t
    char32_t class compl concept const consteval constexpr constinit const_cast continue co_await
    co_return co_yield decltype default delete do double dynamic_cast else enum explicit export
    extern false float for friend goto if inline int long mutable namespace new noexcept not not_eq
    nullptr operator or or_eq private protected public register reinterpret_cast requires restrict
    return short signed sizeof static static_assert static_cast struct switch template this
    thread_local throw true try typedef typeid typename union unsigned using virtual void volatile
    wchar_t while xor xor_eq _Alignas _Alignof _Atomic _Bool _Complex _Generic _Imaginary
    _Noreturn _Static_assert _Thread_local
""".split())


class Grant:
    def __init__(self, task, device, space, base, size, path, node):
        self.task = task
        self.device = device
        self.space = space
        self.base = base
        self.size = size
        self.path = path
        self.node = node

    def end(self):
        return self.base + self.size


class Line:
    def __init__(self, task, source, index, path, node):
        self.task = task
        self.source = source
        # The line's position among its device's lines, and their names in that order.
        self.index = index
        self.device_lines = []
        self.path = path
        self.node = node
        self.name = None


class Task:
    def __init__(self, index, node):
        self.index = index
        self.node = node
        self.name = None
        self.name_node = None
        self.serves = None
        self.uses = []
        self.watches = []
        self.maps = []
        self.grants = []
        self.lines = []
        self.accepts = []
        self.unresolved = False
        self.entry = False
        self.entry_name = None
        self.driver = None
        self.nodes = {}
        self.stack = None
        self.priority = None
        self.ceiling = None
        self.core = None
        self.device_count = 0
        self.line_names = []
        self.modes = {}
        self.authority = []
        self.restart_max = 0

    def label(self):
        if self.name is None:
            return "task %d" % (self.index + 1)
        return "task `%s`" % self.name


class Region:
    def __init__(self, path, node):
        self.path = path
        self.node = node
        self.size = None
        self.size_node = None
        self.cache = None
        self.cache_node = None
        # Whether it lives in the partition's user share.
        self.partition = False


class Cache:
    def __init__(self):
        self.chips = {}
        self.boards = {}


class Admitted:
    """What a composition checked against a manifest leaves for the emitter."""

    def __init__(self, root, tasks, shared, stdout, ends, heap, init_priority, chip, cluster, manifest, translating,
                 accepts):
        self.root = root
        self.tasks = tasks
        self.shared = shared
        # `stdout`: "kernel", or the task serving it.
        self.stdout = stdout
        # The node of `ends`, read only once the composition is admitted.
        self.ends = ends
        # The bytes of libc heap the image carves, its `heap`.
        self.heap = heap
        # The priority the init lowers itself to.
        self.init_priority = init_priority
        self.chip = chip
        self.cluster = cluster
        self.manifest = manifest
        # Whether each task runs in an address space of its own.
        self.translating = translating
        # The composition's `accepts`, as (name, node).
        self.accepts = accepts
        # Each partition region's offset in the user share, which the partition build places.
        self.offsets = {}


def admit(paths, platform, manifest_path=None):
    """Admits each composition: against the chip and board files the manifest at `manifest_path`
    names, with the rules a kernel build decides, or without one against the files under the
    `platform` directory."""
    report = Report()
    cache = Cache()
    count = 0
    manifest = None
    if manifest_path is not None:
        manifest = read_manifest(manifest_path, report)
        if not manifest:
            return report, count
    for path in paths:
        text = read_composition(path, report)
        if text is None:
            continue
        count = count + 1
        admit_composition(path, text, platform, report, cache, manifest)
    return report, count


def read_composition(path, report):
    """The file's text, or None once refused."""
    if not os.path.isfile(path):
        report.refuse(path, 1, "form.layout", "no such file")
        return None
    try:
        with open(path, encoding="utf-8") as stream:
            return stream.read()
    except (OSError, UnicodeDecodeError) as error:
        report.refuse(path, 1, "form.unreadable", "the file cannot be read as UTF-8: %s" % error)
        return None


def admit_composition(path, text, platform, report, cache, manifest, partition=False):
    """The Admitted composition once every rule was checked against `manifest`, or None. `partition`
    is whether it is admitted as one node of a partition."""
    f = File(path, report)
    root = f.load(text)
    if root is None:
        return
    top = f.fields(root, "the composition", COMPOSITION_FIELDS, ("version", "stdout", "ends", "tasks"))
    if top is None or not f.version(top, "the composition", COMPOSITION_VERSIONS):
        return
    board = None
    base = None
    if "board" in top:
        board, clean = find_board(f, top["board"], platform, cache, manifest)
        if not clean:
            return
    elif manifest is not None:
        board, clean = built_board(f, root, cache, manifest)
        if not clean:
            return
        base = board_default(f.report, manifest)
    # Whether the figures it leaves out are its build board default's, or unknown to an admission
    # without a manifest.
    board_less = "board" not in top and (manifest is None or base is not None)
    chip = None
    if board is not None:
        chip = board.chip
    cluster = check_cluster(f, top, root, chip, base)
    accepts = []
    if "accepts" in top:
        accepts = check_accepts(f, top["accepts"], "the composition's `accepts`", COMPOSITION_LIMITATIONS)
    elif base is not None:
        accepts = [(name, root) for name in base.accepts]
    heap = None
    if "heap" in top:
        heap = f.integer(top["heap"], "`heap`", 32)
    elif base is not None:
        heap = base.heap
    elif not board_less:
        f.refuse(root, "form.missing", "the composition needs `heap`, the bytes of libc heap its image carves "
                 "(0 for none); a composition naming no `board` inherits its build board's")
    stack = None
    if base is not None:
        stack = base.stack
    init_priority, init_node = read_init(f, top)
    shared = check_shared(f, top)
    tasks = []
    if "tasks" in top:
        items = f.sequence(top["tasks"], "`tasks`")
        for index, item in enumerate(items or ()):
            tasks.append(check_task(f, item, index, chip, cluster, board_less, stack))
    named = name_tasks(f, tasks)
    served = serve_endpoints(f, tasks)
    stdout = check_order(f, top, tasks, named, served, shared)
    check_restart(f, top, named)
    if chip is not None:
        check_ownership(f, tasks, chip, cluster, board, stdout)
    if chip is not None and manifest is not None:
        check_window_rule(f, top.get("board", root), chip, cluster, manifest)
        check_encoding(f, tasks, chip, cluster, manifest)
        needed = check_enforcement(f, root, tasks, chip, cluster, board, manifest, accepts)
        check_memory_type(f, tasks, shared, chip, cluster, manifest, accepts, needed)
        for name, node in accepts:
            if name not in needed:
                unneeded(f, node, "the composition's", name)
        check_drivers(f, tasks, manifest)
        check_priorities(f, tasks, manifest)
        check_scheduling(f, top, tasks, stdout, manifest)
        init_priority = check_init_priority(f, root, init_priority, init_node, manifest)
        translating = any(unit.page is not None for view, unit in unit_views(chip, cluster))
        check_supply(f, root, tasks, shared, chip, cluster, manifest, translating, region_size)
        check_ports(f, tasks, manifest)
        check_partitioned(f, tasks, shared, manifest, partition)
        return Admitted(root, tasks, shared, stdout, top.get("ends"), heap, init_priority, chip, cluster, manifest,
                        translating, accepts)
    return None


def read_init(f, top):
    """(the priority `init` states or None, the node stating it or None)."""
    if "init" not in top:
        return None, None
    values = f.fields(top["init"], "`init`", INIT_FIELDS, ())
    if values is None or "priority" not in values:
        return None, None
    return f.integer(values["priority"], "`init` priority", 8), values["priority"]


def check_init_priority(f, root, priority, node, manifest):
    """The priority the init lowers itself to: the one `init` states, else one above the kernel
    build's lowest. Either is refused outside the build's range."""
    if manifest.priority is None:
        return priority
    lo, hi = manifest.priority
    if node is None:
        priority = lo + 1
        node = root
        what = "the init's default priority %d, one above the lowest," % priority
    elif priority is None:
        return None
    else:
        what = "`init` priority %d" % priority
    if not lo <= priority <= hi:
        f.refuse(node, "scheduling.init-priority-range",
                 "%s is outside the kernel build's range [%d, %d]" % (what, lo, hi))
    return priority


def find_board(f, node, platform, cache, manifest):
    """(the Board `board:` names, or None; whether its board and chip files were admitted). Against
    a manifest the board is the one its `descriptions` names, and otherwise the one under `platform`.
    A composition over a refused description is not checked, its refusals following from that one."""
    name = f.name(node, "`board`", FILE_NAME)
    if name is None:
        return None, True
    if manifest is not None:
        path = described_board(f, node, name, manifest)
        if path is None:
            return None, True
    else:
        path = platform_board(f, node, name, platform)
        if path is None:
            return None, True
    return load_board(f, path, cache)


def built_board(f, node, cache, manifest):
    """(the Board of the build a composition naming no board is admitted against, or None; whether
    its board and chip files were admitted)."""
    if manifest.descriptions is None:
        f.refuse(node, "manifest.target",
                 "the composition names no board and is admitted against a manifest of board `%s`, which "
                 "names no chip and board files to admit it against" % manifest.board)
        return None, True
    return load_board(f, manifest.descriptions[1], cache)


class BoardDefault:
    """What a composition naming no board takes from its build board's default composition when it
    states none of it: the `heap`, the `accepts`, the `cluster`, and the stack of its entry task."""

    def __init__(self):
        self.heap = None
        self.accepts = []
        self.cluster = None
        self.stack = None


def board_default(report, manifest):
    """The BoardDefault of the default composition `manifest` names, or None where it names none or
    the file is refused."""
    if manifest.default is None:
        return None
    text = read_composition(manifest.default, report)
    if text is None:
        return None
    g = File(manifest.default, report)
    root = g.load(text)
    if root is None:
        return None
    top = g.fields(root, "the composition", COMPOSITION_FIELDS, ("heap", "tasks"))
    if top is None or "tasks" not in top:
        return None
    base = BoardDefault()
    if "heap" in top:
        base.heap = g.integer(top["heap"], "`heap`", 32)
    if "accepts" in top:
        base.accepts = [name for name, _ in check_accepts(g, top["accepts"], "the composition's `accepts`",
                                                          COMPOSITION_LIMITATIONS)]
    if "cluster" in top:
        base.cluster = g.name(top["cluster"], "`cluster`", IDENTIFIER)
    for item in g.sequence(top["tasks"], "`tasks`") or ():
        values = g.mapping(item, "a task")
        if values is not None and "entry" in values and "stack" in values:
            base.stack = g.integer(values["stack"][1], "the entry task's stack", 32)
            break
    return base


def load_board(f, path, cache):
    """(the Board the file at `path` describes, or None; whether it and its chip file were admitted)."""
    if path not in cache.boards:
        try:
            with open(path, encoding="utf-8") as stream:
                text = stream.read()
        except (OSError, UnicodeDecodeError) as error:
            f.report.refuse(path, 1, "form.unreadable", "the file cannot be read as UTF-8: %s" % error)
            cache.boards[path] = None
            return None, False
        cache.boards[path] = check_board(path, text, f.report, cache.chips, {})
    board = cache.boards[path]
    refused = set(r.path for r in f.report.refusals)
    if board is None or path in refused or board.chip is None or board.chip.path in refused:
        return None, False
    return board, True


def described_board(f, node, name, manifest):
    """The board file the manifest's `descriptions` names for `board: name`, or None once refused."""
    built = "board `%s`" % manifest.board
    if manifest.chip is not None:
        built = "board `%s` on chip `%s`" % (manifest.board, manifest.chip)
    if manifest.board != name:
        f.refuse(node, "manifest.target",
                 "`board: %s` is admitted against a manifest of %s, another kernel build's" % (name, built))
        return None
    if manifest.descriptions is None:
        f.refuse(node, "manifest.target",
                 "`board: %s` is admitted against a manifest of %s, which names no chip and board files "
                 "to admit it against" % (name, built))
        return None
    return manifest.descriptions[1]


def platform_board(f, node, name, platform):
    """The file platform/<chip>/<name>.yaml, or None once refused."""
    found = []
    for folder in sorted(os.listdir(platform)):
        candidate = os.path.join(platform, folder, name + ".yaml")
        if name != "chip" and os.path.isfile(candidate):
            found.append(candidate)
    if not found:
        f.refuse(node, "name.board-unknown",
                 "`board: %s` names no file %s/<chip>/%s.yaml" % (name, platform, name))
        return None
    if len(found) > 1:
        f.refuse(node, "name.board-ambiguous", "`board: %s` names both %s" % (name, " and ".join(found)))
        return None
    return found[0]


def check_cluster(f, top, root, chip, base):
    """The cluster this image runs on, or None on a part that has no choice of one. A composition
    naming no board and no cluster runs on its build board default's."""
    if chip is None:
        if "cluster" in top:
            f.name(top["cluster"], "`cluster`", IDENTIFIER)
        return None
    if not chip.multi_arch:
        if "cluster" in top:
            f.refuse(top["cluster"], "form.inapplicable",
                     "`cluster` is for a part whose clusters each carry their own `arch`, which chip "
                     "`%s` is not" % chip.name)
        return None
    if "cluster" not in top and base is not None and base.cluster in chip.clusters:
        return base.cluster
    if "cluster" not in top:
        f.refuse(root, "form.missing",
                 "the composition needs `cluster`, one of chip `%s`'s %s" % (chip.name, ", ".join(chip.clusters)))
        return None
    name = f.name(top["cluster"], "`cluster`", IDENTIFIER)
    if name is not None and name not in chip.clusters:
        f.refuse(top["cluster"], "name.cluster-unknown",
                 "`cluster: %s` names no cluster of chip `%s`, whose clusters are %s"
                 % (name, chip.name, ", ".join(chip.clusters)))
        return None
    return name


def views(chip, cluster):
    if cluster is not None:
        return (cluster,)
    if chip.clusters:
        return tuple(chip.clusters)
    return (PART,)


def unique(f, values, what):
    """Refuses a value `values`, a list of (value, node), holds twice."""
    seen = {}
    for value, node in values:
        if value in seen:
            f.refuse(node, "form.duplicate-entry", "%s lists `%s` twice, first on line %d" % (what, value, seen[value]))
        else:
            seen[value] = line_of(node)


def check_accepts(f, node, what, scope):
    """The (name, node) of each limitation listed at its scope."""
    items = f.sequence(node, what)
    values = []
    for item in items or ():
        name = f.enum(item, "a name in %s" % what, LIMITATIONS)
        if name is None:
            continue
        if name not in scope:
            where = "is platform-wide, so it is accepted once, at the top of the composition"
            if name in TASK_LIMITATIONS:
                where = "belongs to a grant, so it is accepted on the task holding it"
            f.refuse(item, "form.scope", "%s lists `%s`, which %s" % (what, name, where))
            continue
        values.append((name, item))
    unique(f, values, what)
    return values


def declared_path(f, node, what, space, crossing=False):
    """The path `/<space>/<name>`, or with `crossing` an /amp/<port> path too, or None once refused."""
    path = f.path(node, what)
    if path is None:
        return None
    if path == "/init" or path.startswith("/init/"):
        f.refuse(node, "name.reserved", "%s `%s` is under /init, which the init alone defines" % (what, path))
        return None
    parts = path.split("/")
    named = len(parts) == 3 and parts[1] == space and IDENTIFIER.fullmatch(parts[2])
    if crossing and len(parts) == 3 and parts[1] == "amp":
        named = amp_port(path) is not None
    if not named:
        also = ""
        if crossing:
            also = ", or a crossing /amp/<port>, its port a decimal below %d" % AMP_PORTS
        f.refuse(node, "name.namespace",
                 "%s `%s` is not a path /%s/<name>, its name of the form %s%s"
                 % (what, path, space, IDENTIFIER.pattern, also))
        return None
    return path


def amp_port(path):
    """The port of a crossing path /amp/<port>, or None for any other path."""
    parts = path.split("/")
    if len(parts) != 3 or parts[0] != "" or parts[1] != "amp":
        return None
    return index_below(parts[2], AMP_PORTS)


def check_shared(f, top):
    """Each declared region's path, mapped to its Region."""
    shared = {}
    if "shared" not in top:
        return shared
    items = f.sequence(top["shared"], "`shared`")
    for n, item in enumerate(items or ()):
        what = "shared region %d" % (n + 1)
        values = f.fields(item, what, SHARED_FIELDS, SHARED_REQUIRED)
        if values is None:
            continue
        name = None
        if "name" in values:
            name = declared_path(f, values["name"], "%s name" % what, "shm")
        if name is not None:
            what = "shared region `%s`" % name
        size = None
        if "size" in values:
            size = f.integer(values["size"], "%s size" % what, 32)
        if size == 0:
            f.refuse(values["size"], "form.range", "%s has size 0, and a region holds at least one byte" % what)
            size = None
        cache = None
        if "cache" in values:
            cache = f.enum(values["cache"], "%s cache" % what, ("cached", "uncached"))
        partition = False
        if "partition" in values:
            partition = f.boolean(values["partition"], "%s partition" % what) is True
        if name is None:
            continue
        region = Region(name, values["name"])
        region.size = size
        region.cache = cache
        region.cache_node = values.get("cache")
        region.size_node = values.get("size")
        region.partition = partition
        if name in shared:
            f.refuse(values["name"], "name.duplicate",
                     "shared region `%s` is declared twice, first on line %d" % (name, line_of(shared[name].node)))
        else:
            shared[name] = region
    return shared


def check_task(f, item, index, chip, cluster, board_less, base_stack):
    """The Task `item` declares; where `board_less`, an entry task stating no `stack` takes
    `base_stack`."""
    task = Task(index, item)
    values = f.fields(item, "task %d" % (index + 1), TASK_FIELDS, ("name", "priority"))
    if values is None:
        return task
    if "name" in values:
        task.name = f.name(values["name"], "task %d name" % (index + 1), IDENTIFIER)
        task.name_node = values["name"]
    what = task.label()
    task.nodes = values

    if "entry" in values and "driver" in values:
        f.refuse(values["driver"], "form.exclusive", "%s has both `entry` and `driver`; it runs one of the two" % what)
    if "entry" not in values and "driver" not in values:
        f.refuse(item, "form.missing",
                 "%s needs `entry`, a symbol in the user's sources, or `driver`, one the package ships" % what)
    if "entry" in values:
        task.entry_name = f.name(values["entry"], "%s entry" % what, C_IDENTIFIER)
        if task.entry_name in KEYWORDS:
            f.refuse(values["entry"], "name.entry",
                     "%s entry `%s` is a C or C++ keyword, which names no function" % (what, task.entry_name))
            task.entry_name = None
        elif task.entry_name is not None and table_name(task.entry_name):
            f.refuse(values["entry"], "name.entry",
                     "%s entry `%s` is a name the emitted table's C source defines, or of the kos_, KOS_ "
                     "and KICKOS_ names its headers declare" % (what, task.entry_name))
            task.entry_name = None
        task.entry = task.entry_name is not None
        if "stack" not in values and "driver" not in values and not board_less:
            f.refuse(item, "form.missing", "%s runs an `entry`, so it needs `stack`" % what)
        elif "stack" not in values and "driver" not in values:
            task.stack = base_stack
    if "driver" in values and "entry" not in values:
        task.driver = f.name(values["driver"], "%s driver" % what, IDENTIFIER)
    elif "driver" in values:
        f.name(values["driver"], "%s driver" % what, IDENTIFIER)
    if "driver" in values and "entry" not in values:
        for field in ("uses", "watches"):
            if field in values:
                f.refuse(values[field], "form.inapplicable",
                         "%s runs a packaged driver, whose descriptor grants it only its own "
                         "capabilities, so it has no `%s`" % (what, field))
    if "driver" in values:
        if "stack" in values and "entry" not in values:
            f.refuse(values["stack"], "form.inapplicable",
                     "%s runs a packaged driver, whose stack its metadata states, so it has no `stack`" % what)
    if "stack" in values:
        task.stack = f.integer(values["stack"], "%s stack" % what, 32)
    if "priority" in values:
        task.priority = f.integer(values["priority"], "%s priority" % what, 8)
    if "ceiling" in values and "driver" in values and "entry" not in values:
        f.refuse(values["ceiling"], "form.inapplicable",
                 "%s runs a packaged driver, whose ceiling is its priority plus its threads' highest "
                 "offset, so it has no `ceiling`" % what)
    elif "ceiling" in values:
        task.ceiling = f.integer(values["ceiling"], "%s ceiling" % what, 8)
        if task.ceiling is not None and task.priority is not None and task.ceiling < task.priority:
            f.refuse(values["ceiling"], "scheduling.ceiling",
                     "%s ceiling %d, the highest priority its threads may take, is below its priority %d, which "
                     "its entry thread runs at; the usual value is the kernel build's top priority"
                     % (what, task.ceiling, task.priority))
    elif "entry" in values and "driver" not in values:
        f.refuse(item, "form.missing", "%s runs an `entry`, so it needs `ceiling`, the highest priority its threads "
                 "may take; the usual value is the kernel build's top priority" % what)
    if "core" in values:
        core = f.integer(values["core"], "%s core" % what, 16)
        if core is not None and core >= CORES:
            f.refuse(values["core"], "form.range",
                     "%s core %d is past the %d cores a task's core mask carries" % (what, core, CORES))
        elif core is not None:
            task.core = core

    if "devices" in values:
        items = f.sequence(values["devices"], "%s devices" % what)
        task.device_count = len(items or ())
        for node in items or ():
            grant = resolve_grant(f, chip, cluster, task, node, "%s device" % what)
            if grant is None:
                task.unresolved = True
            if grant is not None:
                task.grants.append(grant)
        unique(f, [(g.path, g.node) for g in task.grants], "%s devices" % what)
    if "lines" in values:
        lines = f.mapping(values["lines"], "%s lines" % what)
        for name, (key, value) in (lines or {}).items():
            if f.name(key, "%s line name" % what, IDENTIFIER) is not None:
                task.line_names.append((name, key))
            line = resolve_line(f, chip, cluster, task, value, "%s line `%s`" % (what, name))
            if line is not None:
                line.name = name
                task.lines.append(line)
    if "serves" in values:
        path = declared_path(f, values["serves"], "%s served endpoint" % what, "svc", True)
        if path is not None and amp_port(path) is not None and "driver" in values:
            f.refuse(values["serves"], "partition.port",
                     "%s runs a packaged driver, whose descriptor creates the endpoint it serves, so it "
                     "serves no crossing `%s`" % (what, path))
            path = None
        if path is not None:
            task.serves = (path, values["serves"])
    if "uses" in values:
        items = f.sequence(values["uses"], "%s uses" % what)
        for node in items or ():
            path = declared_path(f, node, "%s used endpoint" % what, "svc", True)
            if path is not None:
                task.uses.append((path, node))
        unique(f, task.uses, "%s uses" % what)
    if "maps" in values:
        maps = f.mapping(values["maps"], "%s maps" % what)
        for name, (key, value) in (maps or {}).items():
            path = declared_path(f, key, "%s mapped region" % what, "shm")
            mode = f.enum(value, "%s mapping of `%s`" % (what, name), ("ro", "rw"))
            if path is not None:
                task.maps.append((path, key))
                task.modes[path] = mode
    if "watches" in values:
        items = f.sequence(values["watches"], "%s watches" % what)
        for node in items or ():
            name = f.name(node, "%s watches" % what, IDENTIFIER)
            if name is not None:
                task.watches.append((name, node))
        unique(f, task.watches, "%s watches" % what)
        if len(items or ()) > WATCH_BITS:
            f.refuse(values["watches"], "encoding.watches",
                     "%s watches %d tasks, and the i-th it watches raises bit i of its %d-bit notification"
                     % (what, len(items), WATCH_BITS))
    if "authority" in values:
        items = f.sequence(values["authority"], "%s authority" % what)
        held = []
        for node in items or ():
            name = f.enum(node, "%s authority" % what, AUTHORITIES)
            if name is not None:
                held.append((name, node))
        unique(f, held, "%s authority" % what)
        task.authority = [name for name, node in held]
    if "accepts" in values:
        task.accepts = check_accepts(f, values["accepts"], "%s accepts" % what, TASK_LIMITATIONS)
    if "restart" in values:
        restart = f.fields(values["restart"], "%s restart" % what, ("max",), ("max",))
        if restart is not None and "max" in restart:
            task.restart_max = f.integer(restart["max"], "%s restart max" % what, 8)
    return task


def table_name(name):
    return name in TABLE_NAMES or name.startswith(ABI_PREFIXES)


def reaches(device, cluster):
    return device.cluster is None or cluster is None or device.cluster == cluster


def from_cluster(cluster):
    if cluster is None:
        return ""
    return " reached from cluster `%s`" % cluster


def chip_device(chip, parts, lengths):
    if len(parts) in lengths and parts[1] == "dev" and parts[0] == "":
        return chip.devices.get(parts[2])
    return None


def resolve_grant(f, chip, cluster, task, node, what):
    path = f.path(node, what)
    if path is None or chip is None:
        return None
    parts = path.split("/")
    device = chip_device(chip, parts, (3, 4))
    if device is None or not reaches(device, cluster):
        f.refuse(node, "name.device-unknown",
                 "%s `%s` is no device path of chip `%s`%s" % (what, path, chip.name, from_cluster(cluster)))
        return None
    if device.owner == "kernel":
        kernel_owned(f, node, "%s `%s`" % (what, path), device)
        return None
    window = device_window(device, parts)
    if window is None:
        f.refuse(node, "name.device-unknown",
                 "%s `%s` is no window or port range of device `%s`; %s" % (what, path, device.name, grantable(device)))
        return None
    space, base, size = window
    return Grant(task, device, space, base, size, path, node)


def device_window(device, parts):
    """The (space, base, size) a device path grants, or None."""
    if len(parts) == 3:
        if device.ports is not None:
            return ("port", device.ports[0], device.ports[1])
        if device.window is not None and device.count is None:
            return ("mem", device.window[0], device.window[1])
        return None
    leaf = parts[3]
    if leaf in device.channel_windows:
        base, size = device.channel_windows[leaf]
        return ("mem", base, size)
    k = None
    if device.count is not None and device.window is not None:
        k = index_below(leaf, device.count)
    if k is not None:
        return ("mem", device.window[0] + k * device.stride, device.window[1])
    return None


def grantable(device):
    if device.channel_windows:
        return "grant one of its channels, %s" % ", ".join(sorted(device.channel_windows))
    if device.count is not None:
        return "grant one of its instances, /dev/%s/0 to /dev/%s/%d" % (device.name, device.name, device.count - 1)
    if device.window is not None or device.ports is not None:
        return "grant /dev/%s itself" % device.name
    return "it has none to grant"


def resolve_line(f, chip, cluster, task, node, what):
    path = f.path(node, what)
    if path is None or chip is None:
        return None
    parts = path.split("/")
    device = chip_device(chip, parts, (4, 5))
    source = None
    if device is not None and reaches(device, cluster):
        source = line_source(device, parts)
    if source is None:
        f.refuse(node, "name.line-unknown",
                 "%s, `%s`, is no line path of chip `%s`%s%s" % (what, path, chip.name, from_cluster(cluster),
                                                                line_hint(device)))
        return None
    if device.owner == "kernel":
        kernel_owned(f, node, "%s, `%s`," % (what, path), device)
        return None
    raiser = kernel_line(chip, cluster, source)
    if raiser is not None:
        f.refuse(node, "ownership.kernel",
                 "%s, `%s`, is source %d, which kernel-owned device `%s` raises too" % (what, path, source, raiser.name))
        return None
    if source >= LINE_NONE:
        f.refuse(node, "form.range",
                 "%s, `%s`, is source %d, past the %d the table carries a line as, %d meaning none"
                 % (what, path, source, LINE_NONE - 1, LINE_NONE))
        return None
    line = Line(task, source, list(device.sources).index(parts[-1]), path, node)
    line.device_lines = list(device.sources)
    return line


def kernel_line(chip, cluster, source):
    """The kernel-owned device raising line `source`, or None."""
    for device in chip.devices.values():
        if device.owner != "kernel" or not reaches(device, cluster):
            continue
        count = 1
        if device.count is not None:
            count = device.count
        for number in device.sources.values():
            if number <= source < number + count:
                return device
    return None


def kernel_windows(chip, cluster):
    """Each kernel-owned register window, as (device, base, size)."""
    return [(device, base, size) for device, path, base, size in device_windows(chip, cluster)
            if device.owner == "kernel"]


def device_windows(chip, cluster):
    """Each register window of the chip, as (device, path, base, size)."""
    windows = []
    for device in chip.devices.values():
        if not reaches(device, cluster):
            continue
        if device.window is not None and device.count is None:
            windows.append((device, "/dev/%s" % device.name, device.window[0], device.window[1]))
        if device.window is not None and device.count is not None:
            for k in range(device.count):
                windows.append((device, "/dev/%s/%d" % (device.name, k), device.window[0] + k * device.stride,
                                device.window[1]))
        for channel, (base, size) in device.channel_windows.items():
            windows.append((device, "/dev/%s/%s" % (device.name, channel), base, size))
    return windows


def line_source(device, parts):
    """The source number a line path names, an instance's being its device's line plus k."""
    if len(parts) == 4:
        if device.count is not None:
            return None
        return device.sources.get(parts[3])
    if device.count is None:
        return None
    k = index_below(parts[3], device.count)
    number = device.sources.get(parts[4])
    if k is None or number is None:
        return None
    return number + k


def line_hint(device):
    if device is None or not device.sources:
        return ""
    names = ", ".join(device.sources)
    if device.count is not None:
        return "; device `%s` has lines %s, each instance's as /dev/%s/<k>/<line>" % (device.name, names, device.name)
    return "; device `%s` has lines %s" % (device.name, names)


def kernel_owned(f, node, subject, device):
    f.refuse(node, "ownership.kernel", "%s is of device `%s`, which the kernel holds for life" % (subject, device.name))


def name_tasks(f, tasks):
    named = {}
    for task in tasks:
        if task.name is None:
            continue
        if task.name == "never":
            f.refuse(task.name_node, "name.reserved", "no task is named `never`, which `ends` spells for no task")
        if task.name in named:
            f.refuse(task.name_node, "name.duplicate",
                     "task `%s` is declared twice, first on line %d" % (task.name, line_of(named[task.name].node)))
        else:
            named[task.name] = task
    return named


def serve_endpoints(f, tasks):
    """Each endpoint and crossing a task serves, mapped to that task."""
    served = {}
    for task in tasks:
        if task.serves is None:
            continue
        path, node = task.serves
        if path in served and amp_port(path) is not None:
            first = served[path]
            f.refuse(node, "partition.port",
                     "crossing `%s` is served by %s and by %s on line %d, and the init hands its one port "
                     "to one server" % (path, task.label(), first.label(), line_of(first.serves[1])))
        elif path in served:
            first = served[path]
            f.refuse(node, "name.duplicate",
                     "`%s` is served by %s and by %s on line %d"
                     % (path, task.label(), first.label(), line_of(first.serves[1])))
        else:
            served[path] = task
    return served


def check_order(f, top, tasks, named, served, shared):
    """References point backward. Returns `stdout`: "kernel", its server, or None when unknown."""
    for task in tasks:
        for path, node in task.uses:
            if amp_port(path) is not None:
                continue
            server = served.get(path)
            if server is None:
                f.refuse(node, "order.undeclared", "%s uses `%s`, which no task serves" % (task.label(), path))
            elif server.index >= task.index:
                f.refuse(node, "order.forward",
                         "%s uses `%s`, which %s serves on line %d, not above it"
                         % (task.label(), path, server.label(), line_of(server.serves[1])))
        for name, node in task.watches:
            watched = named.get(name)
            if watched is None:
                f.refuse(node, "order.undeclared", "%s watches `%s`, which no task is named" % (task.label(), name))
            elif watched.index >= task.index:
                f.refuse(node, "order.forward",
                         "%s watches %s, declared on line %d, not above it"
                         % (task.label(), watched.label(), line_of(watched.node)))
        for path, node in task.maps:
            if path not in shared:
                f.refuse(node, "order.undeclared", "%s maps `%s`, which `shared` does not declare" % (task.label(), path))

    if "ends" in top and f.string(top["ends"], "`ends`") not in (None, "never"):
        name = f.name(top["ends"], "`ends`", IDENTIFIER)
        if name is not None and name not in named:
            f.refuse(top["ends"], "order.undeclared", "`ends: %s` names no task" % name)

    if "stdout" not in top:
        return None
    text = f.string(top["stdout"], "`stdout`")
    if text is None or text == "kernel":
        return text
    path = declared_path(f, top["stdout"], "`stdout`", "svc")
    if path is None:
        return None
    if path not in served:
        f.refuse(top["stdout"], "order.undeclared", "`stdout: %s` names an endpoint no task serves" % path)
        return None
    return served[path]


def check_ports(f, tasks, manifest):
    """Each crossing a task serves or uses, against the partition's port list and this node's index."""
    servers = {}
    for node, port in manifest.amp_list:
        servers.setdefault(port, []).append(node)
    for task in tasks:
        crossings = [(path, node, "uses") for path, node in task.uses]
        if task.serves is not None:
            crossings.insert(0, (task.serves[0], task.serves[1], "serves"))
            if amp_port(task.serves[0]) is not None:
                for path, node in task.uses:
                    if path == task.serves[0]:
                        f.refuse(node, "partition.port",
                                 "%s uses `%s`, which it serves, so its calls would wait on its own receive"
                                 % (task.label(), path))
        for path, node, verb in crossings:
            port = amp_port(path)
            if port is None:
                continue
            nodes = servers.get(port, [])
            reason = None
            if port == AMP_PORT_REPLY:
                reason = "port %d, which the window layer keeps for replies" % AMP_PORT_REPLY
            elif not nodes:
                reason = "a port the kernel build's partition list does not name; it names %s" % port_ranges(servers)
            elif len(nodes) > 1:
                reason = "a port the partition list names for nodes %s, so no one node serves it" % (
                    ", ".join(str(n) for n in nodes))
            elif verb == "serves" and nodes[0] != manifest.amp_node:
                reason = "which the partition list names for node %d, and this composition is node %s's" % (
                    nodes[0], manifest.amp_node)
            if reason is not None:
                f.refuse(node, "partition.port", "%s %s `%s`, %s" % (task.label(), verb, path, reason))


def port_ranges(servers):
    """The ports of `servers` as `a..b` runs, or `no port`."""
    ports = sorted(servers)
    if not ports:
        return "no port"
    runs = []
    first = ports[0]
    last = ports[0]
    for port in ports[1:] + [None]:
        if port is not None and port == last + 1:
            last = port
            continue
        if first == last:
            runs.append("%d" % first)
        else:
            runs.append("%d..%d" % (first, last))
        if port is not None:
            first = port
            last = port
    return "port " + ", ".join(runs)


def check_partitioned(f, tasks, shared, manifest, partition):
    """A partition region where the kernel build states no shared window, and on an AMP build a
    crossing or a partition region in a composition admitted apart from its partition."""
    for region in shared.values():
        if region.partition and manifest.amp_window is None:
            f.refuse(region.node, "partition.region",
                     "partition region `%s` lives in the partition's user share, and the kernel build states no "
                     "AMP shared window to carve one from" % region.path)
        elif region.partition and manifest.amp_nodes is not None and not partition:
            f.refuse(region.node, "partition.lone",
                     "partition region `%s` is placed by the partition build, and this composition is admitted "
                     "apart from its partition: admit every node's with `kickos_compose partition`, or "
                     "kickos_compose(... PARTITION ...) in CMake" % region.path)
    if manifest.amp_nodes is None or partition or manifest.amp_image == "shared":
        return
    for task in tasks:
        crossings = list(task.uses)
        if task.serves is not None:
            crossings.insert(0, task.serves)
        for path, node in crossings:
            if amp_port(path) is not None:
                f.refuse(node, "partition.lone",
                         "%s names crossing `%s`, which the partition build delegates, and this composition is "
                         "admitted apart from its partition: admit every node's with `kickos_compose partition`, or "
                         "kickos_compose(... PARTITION ...) in CMake" % (task.label(), path))


def check_restart(f, top, named):
    """A task that ends the system starts once."""
    ending = None
    if "ends" in top and isinstance(top["ends"], ScalarNode):
        ending = named.get(top["ends"].value)
    if ending is not None and ending.restart_max:
        f.refuse(ending.nodes["restart"], "restart.ends",
                 "%s ends the system, `ends` naming it, so it is never restarted" % ending.label())


def check_ownership(f, tasks, chip, cluster, board, stdout):
    task_views = views(chip, cluster)
    grants = [grant for task in tasks for grant in task.grants]
    for i, later in enumerate(grants):
        for earlier in grants[:i]:
            if earlier.task is later.task or earlier.space != later.space:
                continue
            if earlier.base < later.end() and later.base < earlier.end():
                held = "which %s holds" % earlier.task.label()
                if earlier.path != later.path:
                    held = "which overlaps `%s`, held by %s" % (earlier.path, earlier.task.label())
                f.refuse(later.node, "ownership.device",
                         "%s holds `%s`, %s on line %d" % (later.task.label(), later.path, held, line_of(earlier.node)))
            elif later.space == "mem":
                unit = gate_unit(chip, task_views, (earlier.base, earlier.end()), (later.base, later.end()))
                if unit is not None:
                    f.refuse(later.node, "ownership.gate",
                             "%s holds `%s`, in the 0x%X-byte %s at 0x%X that %s opens for `%s` on line %d"
                             % (later.task.label(), later.path, unit[1], unit[0], unit[2], earlier.task.label(),
                                earlier.path, line_of(earlier.node)))

    kernel = kernel_windows(chip, cluster)
    for grant in grants:
        if grant.space != "mem":
            continue
        for device, base, size in kernel:
            unit = gate_unit(chip, task_views, (base, base + size), (grant.base, grant.end()))
            if unit is not None:
                f.refuse(grant.node, "ownership.gate",
                         "%s holds `%s`, in the 0x%X-byte %s at 0x%X that also opens kernel-owned device `%s`"
                         % (grant.task.label(), grant.path, unit[1], unit[0], unit[2], device.name))
                break

    lines = [line for task in tasks for line in task.lines]
    for i, later in enumerate(lines):
        for earlier in lines[:i]:
            if earlier.source == later.source:
                f.refuse(later.node, "ownership.line",
                         "`%s` of %s is source %d, which %s takes as `%s` on line %d"
                         % (later.path, later.task.label(), later.source, earlier.task.label(), earlier.path,
                            line_of(earlier.node)))
                break

    if board is None or board.console is None or stdout is None:
        return
    for grant in grants:
        if grant.path != board.console or grant.task is stdout:
            continue
        holder = "`stdout: kernel` leaves it to the kernel"
        if stdout != "kernel":
            holder = "it belongs to %s, which serves `stdout`" % stdout.label()
        f.refuse(grant.node, "ownership.console",
                 "%s holds `%s`, the board's console device, and %s" % (grant.task.label(), grant.path, holder))


def gate_unit(chip, task_views, a, b):
    """The `device_gate` unit opening both (base, end) spans, as (kind, size, base), or None."""
    for view in task_views:
        if view in chip.gates:
            kind, size, ranges = chip.gates[view]
        elif view in chip.protection and chip.protection[view].page is not None:
            kind, size, ranges = ("page", chip.protection[view].page, None)
        else:
            continue
        if ranges is not None and not (covered(ranges, a) and covered(ranges, b)):
            continue
        first = max(a[0] // size, b[0] // size)
        last = min((a[1] - 1) // size, (b[1] - 1) // size)
        if first <= last:
            return (kind, size, first * size)
    return None


def covered(ranges, span):
    for base, size in ranges:
        if base <= span[0] and span[1] <= base + size:
            return True
    return False

def unit_views(chip, cluster):
    """Each view this image runs under, as (view, Protection)."""
    return [(view, chip.protection[view]) for view in views(chip, cluster) if view in chip.protection]


def view_name(chip, view):
    if view is PART:
        return "chip `%s`" % chip.name
    return "cluster `%s` of chip `%s`" % (view, chip.name)


def encodable(manifest, base, size):
    smallest = manifest.smallest_window
    if manifest.window_rule == "pow2":
        return size >= smallest and size & (size - 1) == 0 and base % size == 0
    if manifest.window_rule == "granule":
        return size % smallest == 0 and base % smallest == 0
    return True


def window_prose(manifest):
    if manifest.window_rule == "pow2":
        return "a power of two of at least 0x%X, aligned to its size" % manifest.smallest_window
    return "a multiple of 0x%X in size and base" % manifest.smallest_window


def region_size(size, chip, cluster, manifest):
    """The bytes a shared region of `size` occupies once encodable: up to the window rule's granule
    or power of two, then to the page of each translating view."""
    smallest = manifest.smallest_window
    if manifest.window_rule == "pow2":
        rounded = smallest
        while rounded < size:
            rounded = rounded * 2
        size = rounded
    elif manifest.window_rule == "granule":
        size = -(-size // smallest) * smallest
    for view, unit in unit_views(chip, cluster):
        if unit.page is not None:
            size = -(-size // unit.page) * unit.page
    return size


def page_neighbour(page, windows, grant):
    """The path of another device window in a page `grant` spans, or None."""
    first = grant.base // page * page
    last = -(-grant.end() // page) * page
    for device, path, base, size in windows:
        if path != grant.path and base < last and first < base + size:
            return path
    return None


def gpio_path(function):
    """The device path of a `gpio` function's port, or None."""
    if function is None:
        return None
    return "/dev/" + "/".join(function.split(".")[:-1])


def whole_pages(page, grant):
    return grant.base % page == 0 and grant.size % page == 0


def check_window_rule(f, node, chip, cluster, manifest):
    if manifest.window_rule != "none":
        return
    for view, unit in unit_views(chip, cluster):
        if unit.page is None:
            f.refuse(node, "manifest.window",
                     "the manifest's window rule is `none`, which only a translating build has, and %s "
                     "does not translate" % view_name(chip, view))
            return


def check_encoding(f, tasks, chip, cluster, manifest):
    # The spawn checks a window's encoding and count whether or not the build enforces.
    windows = device_windows(chip, cluster)
    for task in tasks:
        for grant in task.grants:
            if grant.space != "mem":
                continue
            if not encodable(manifest, grant.base, grant.size):
                f.refuse(grant.node, "encoding.window",
                         "%s holds `%s`, the 0x%X-byte window at 0x%X, and the protection unit encodes a "
                         "window only as %s" % (task.label(), grant.path, grant.size, grant.base, window_prose(manifest)))
            for view, unit in unit_views(chip, cluster):
                if unit.page is None or whole_pages(unit.page, grant) or page_neighbour(unit.page, windows, grant):
                    continue
                f.refuse(grant.node, "encoding.page",
                         "%s holds `%s`, the 0x%X-byte window at 0x%X, which is not whole 0x%X-byte pages of "
                         "%s; a device window is mapped as whole pages, never rounded, and a device alone in its page "
                         "is described at page size in the chip file"
                         % (task.label(), grant.path, grant.size, grant.base, unit.page, view_name(chip, view)))
                break
        count = len(task.grants) + len(task.maps)
        if task.watches:
            count = count + 1
        if count > manifest.thread_windows:
            f.refuse(task.node, "encoding.budget",
                     "%s holds %d windows, its devices, the regions it maps and a watcher's /init/status, and "
                     "a thread holds at most %d, KICKOS_MAX_THREAD_WINDOWS" % (task.label(), count,
                                                                                manifest.thread_windows))


def grant_limits(chip, cluster, board, windows, grant):
    """Each limitation that makes `grant` unenforceable, as {name: (rule, reason)}."""
    limits = {}
    if grant.device.bus_master:
        limits["bus_master"] = ("enforcement.bus-master",
                                "a bus master that reaches memory by physical address past every unit")
    if grant.space != "mem":
        return limits
    for pin in sorted(chip.pins or {}):
        if gpio_path(chip.pins[pin].get("gpio")) == grant.path and pin in board.wired:
            limits["coarse_gate"] = ("enforcement.port-bank",
                                     "whose registers drive pin `%s` too, the board's %s"
                                     % (pin, board.wired[pin]))
            break
    for view, unit in unit_views(chip, cluster):
        if unit.covers_devices is False and "device_not_isolated" not in limits:
            limits["device_not_isolated"] = ("enforcement.device-not-isolated",
                                             "a device window the unit of %s does not see" % view_name(chip, view))
        if "coarse_gate" in limits:
            continue
        gate = chip.gates.get(view)
        if gate is not None and (gate[2] is None or covered(gate[2], (grant.base, grant.end()))):
            kind, size, ranges = gate
            if grant.base % size or grant.end() % size:
                limits["coarse_gate"] = ("enforcement.coarse-gate",
                                         "which the 0x%X-byte %s of %s opens with what sits beside it"
                                         % (size, kind, view_name(chip, view)))
                continue
        if unit.page is not None and not whole_pages(unit.page, grant):
            neighbour = page_neighbour(unit.page, windows, grant)
            if neighbour is not None:
                limits["coarse_gate"] = ("encoding.page-shared",
                                         "whose 0x%X-byte page of %s also holds `%s`"
                                         % (unit.page, view_name(chip, view), neighbour))
    return limits


def check_enforcement(f, root, tasks, chip, cluster, board, manifest, accepts):
    """Refuses each grant the platform cannot enforce unless its limitation is accepted at its scope.
    Returns the names of the composition-wide limitations some build of the part could need."""
    units = unit_views(chip, cluster)
    union = ""
    if cluster is None and len(units) > 1:
        union = ", and a composition naming no cluster takes every cluster's limitations"
    needed = {}
    if not manifest.enforced:
        needed["no_protection"] = ("enforcement.no-protection", "the kernel build enforces no protection unit")
    no_build_protects = False
    could_need = set()
    for view, unit in units:
        if unit.page is None:
            could_need.add("no_protection")
        if unit.unit == "none":
            no_build_protects = True
        if unit.unit == "none" and "no_protection" not in needed:
            needed["no_protection"] = ("enforcement.no-protection",
                                       "%s has no protection unit%s" % (view_name(chip, view), union))
        if unit.privilege is False and "no_privilege_split" not in needed:
            needed["no_privilege_split"] = ("enforcement.no-privilege-split",
                                            "%s has no privilege split%s" % (view_name(chip, view), union))
    accepted = [name for name, node in accepts]
    for name, (rule, reason) in needed.items():
        if name not in accepted:
            f.refuse(root, rule, "no task here is held as it is declared: %s, so the composition needs `%s` "
                     "in its `accepts`" % (reason, name))

    windows = device_windows(chip, cluster)
    for task in tasks:
        accepted = [name for name, node in task.accepts]
        needs = set()
        for grant in task.grants:
            if no_build_protects:
                break
            for name, (rule, reason) in grant_limits(chip, cluster, board, windows, grant).items():
                needs.add(name)
                if name not in accepted and "no_protection" not in needed:
                    f.refuse(grant.node, rule, "%s holds `%s`, %s, so it needs `%s` in its `accepts`"
                             % (task.label(), grant.path, reason, name))
        if task.unresolved:
            continue
        for name, node in task.accepts:
            if name in needs:
                continue
            if no_build_protects:
                f.refuse(node, "enforcement.unneeded",
                         "%s's `accepts` lists `%s`, which `no_protection` subsumes: with no grant enforced "
                         "by any build of the part, none is unenforceable" % (task.label(), name))
            else:
                unneeded(f, node, "%s's" % task.label(), name)
    return could_need | set(needed)


def unneeded(f, node, owner, name):
    f.refuse(node, "enforcement.unneeded",
             "%s `accepts` lists `%s`, which nothing here needs: it is for %s" % (owner, name, NOT_NEEDED[name]))


def nocache_support(chip, unit, manifest):
    """arch_mpu_nocache_support() as the chip file and the kernel build state it."""
    if not manifest.enforced or unit.unit == "none":
        return NOCACHE_ALREADY
    if unit.memory_type:
        return NOCACHE_PROGRAMMED
    if not chip.data_cache:
        return NOCACHE_ALREADY
    return NOCACHE_REFUSED


def incoherent_part(chip):
    """Whether some two nodes of a partition of this chip can map one memory incoherently."""
    if not chip.data_cache:
        return False
    every = views(chip, None)
    return len(every) != 1 or not chip.smp.get(every[0], False)


def check_memory_type(f, tasks, shared, chip, cluster, manifest, accepts, needed):
    master = None
    for task in tasks:
        for grant in task.grants:
            if grant.device.bus_master and master is None:
                master = grant
    accepted = [name for name, node in accepts]
    for task in tasks:
        driver = manifest.drivers.get(task.driver)
        if driver is None or driver.block_cache != "uncached":
            continue
        for view, unit in unit_views(chip, cluster):
            if nocache_support(chip, unit, manifest) == NOCACHE_REFUSED:
                f.refuse(task.nodes["driver"], "memory.uncached",
                         "%s runs driver `%s`, whose ring block is `uncached`, which the unit of %s cannot "
                         "program over the part's data cache, its descriptor carrying no memory type"
                         % (task.label(), task.driver, view_name(chip, view)))
                break
    for region in shared.values():
        if (region.partition and manifest.amp_window is not None and region.cache is not None
                and region.cache != manifest.amp_share_cache):
            f.refuse(region.cache_node, "partition.region",
                     "partition region `%s` is `%s`, and the partition's user share it lives in is `%s`, the one "
                     "memory type every mapping of it carries" % (region.path, region.cache, manifest.amp_share_cache))
        if region.partition and region.cache == "cached" and incoherent_part(chip):
            needed.add("cached_incoherent")
        if region.cache == "uncached":
            for view, unit in unit_views(chip, cluster):
                if nocache_support(chip, unit, manifest) == NOCACHE_REFUSED:
                    f.refuse(region.cache_node, "memory.uncached",
                             "shared region `%s` is `uncached`, which the unit of %s cannot program over the "
                             "part's data cache, its descriptor carrying no memory type"
                             % (region.path, view_name(chip, view)))
                    break
        if region.cache != "cached" or not chip.data_cache or master is None:
            continue
        needed.add("cached_incoherent")
        if "cached_incoherent" not in accepted:
            f.refuse(region.cache_node, "memory.cached-incoherent",
                     "shared region `%s` is `cached` over the part's data cache, and %s holds `%s`, a bus master "
                     "that reaches memory past it, so the composition needs `cached_incoherent` in its `accepts` "
                     "and the region's users do their own cache maintenance"
                     % (region.path, master.task.label(), master.path))
