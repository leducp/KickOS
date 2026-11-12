# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# docs/design-m10-composition.md, "What one task costs", counted against the manifest.

from .region import ArenaModel, ram_align, ram_size

U32 = 0xFFFFFFFF
# The ranges every address space opens with, its image's text and data
# (kernel/include/kickos/vrange.h, VR_IMAGE_SLOTS).
IMAGE_RANGES = 2


def check_drivers(f, tasks, manifest):
    for task in tasks:
        task.catalogue = None
        if task.driver is None:
            continue
        what = task.label()
        driver = manifest.drivers.get(task.driver)
        if driver is None:
            listed = "lists none"
            if manifest.drivers:
                listed = "lists %s" % ", ".join(sorted(manifest.drivers))
            f.refuse(task.nodes["driver"], "name.driver-unknown",
                     "%s runs driver `%s`, which the kernel build's catalogue does not list; it %s"
                     % (what, task.driver, listed))
            continue
        task.catalogue = driver
        bound = [name for name, key in task.line_names]
        for name, key in task.line_names:
            if name not in driver.lines:
                f.refuse(key, "driver.line-role",
                         "%s binds line `%s`, which is no line role of driver `%s`; its roles are %s"
                         % (what, name, task.driver, roles_prose(driver.lines)))
        for held in task.lines:
            named = held.path.split("/")[-1]
            if held.name in held.device_lines and held.name != named:
                f.refuse(held.node, "driver.line-name",
                         "%s binds line role `%s` of driver `%s` to `%s`, and its device names a line `%s`, the "
                         "line that role is named for" % (what, held.name, task.driver, held.path, held.name))
        for role in driver.lines:
            if role not in bound:
                f.refuse(task.nodes.get("lines", task.node), "driver.line-role",
                         "%s leaves line role `%s` of driver `%s` unbound" % (what, role, task.driver))
        if task.device_count != len(driver.windows):
            f.refuse(task.nodes.get("devices", task.node), "driver.window-role",
                     "%s grants %d device(s), and driver `%s` binds its devices in order to its window "
                     "roles, %s" % (what, task.device_count, task.driver, roles_prose(driver.windows)))
        for held in task.grants:
            if held.space == "port":
                f.refuse(held.node, "driver.port-window",
                         "%s binds port range `%s` to a window role of driver `%s`, and a driver's "
                         "descriptor takes every window role as a device window, no descriptor stating a "
                         "port kind" % (what, held.path, task.driver))
        if "authority" in task.nodes:
            f.refuse(task.nodes["authority"], "driver.authority",
                     "%s runs driver `%s`, whose threads take the authority its descriptor states, so it "
                     "declares no `authority`" % (what, task.driver))
        if task.restart_max and manifest.fault_isolation is False:
            f.refuse(task.nodes["restart"], "restart.no-isolation",
                     "%s runs driver `%s` with a `restart`, and on a kernel build without fault isolation a "
                     "failing driver thread's trap ends the system, so it never dies alone" % (what, task.driver))


def roles_prose(roles):
    if not roles:
        return "none"
    return ", ".join("`%s`" % role for role in roles)


def threads_of(task, manifest):
    if task.driver is None:
        held = held_by(task)
        return [(task.name, task.priority, task.stack, held.endpoints + held.notifications + held.lines)]
    if task.catalogue is None:
        return []
    threads = []
    for (name, offset, stack), caps in zip(task.catalogue.threads, task.catalogue.caps):
        priority = None
        if task.priority is not None:
            priority = task.priority + offset
        if stack == "default":
            stack = manifest.user_stack
        threads.append((name, priority, stack, caps))
    return threads


def check_priorities(f, tasks, manifest):
    if manifest.priority is None:
        return
    lo, hi = manifest.priority
    for task in tasks:
        for name, priority, stack, caps in threads_of(task, manifest):
            if priority is None:
                continue
            thread = ""
            if task.driver is not None:
                thread = ", its thread `%s` at %d," % (name, priority)
            if not lo <= priority <= hi:
                f.refuse(task.nodes["priority"], "scheduling.priority",
                         "%s runs at priority %d%s outside the kernel build's range [%d, %d]"
                         % (task.label(), task.priority, thread, lo, hi))
                break
        if task.hosted is not None:
            host = task.hosted
            ceiling = host.ceiling
            if ceiling is None:
                ceiling = host.priority
            run = [priority for name, priority, stack, caps in threads_of(task, manifest) if priority is not None]
            if run and ceiling is not None and max(run) > ceiling:
                f.refuse(task.nodes["priority"], "scheduling.hosted",
                         "%s runs a thread at priority %d, and a driver inside a task runs within that task's "
                         "ceiling %d" % (task.label(), max(run), ceiling))
        if task.ceiling is not None and not lo <= task.ceiling <= hi:
            f.refuse(task.nodes["ceiling"], "scheduling.ceiling",
                     "%s ceiling %d, the highest priority its threads may take, is outside the kernel build's "
                     "range [%d, %d]; the usual value is its top, %d" % (task.label(), task.ceiling, lo, hi, hi))


class Held:
    def __init__(self):
        self.endpoints = 0
        self.notifications = 0
        self.lines = 0
        # Capabilities past one per object: a notification's or an endpoint's badged copies.
        self.copies = 0
        self.programs = 0

    def add(self, other):
        self.endpoints = self.endpoints + other.endpoints
        self.notifications = self.notifications + other.notifications
        self.lines = self.lines + other.lines
        self.copies = self.copies + other.copies
        self.programs = self.programs + other.programs

    def caps(self):
        return self.endpoints + self.notifications + self.lines + self.copies + self.programs


def own_tasks(tasks):
    """The tasks of their own, a driver listed under a task's `drivers` being its host's threads."""
    return [task for task in tasks if task.hosted is None]


def hosted_by(task, tasks):
    """The drivers listed under `task`'s `drivers`."""
    return [other for other in tasks if other.hosted is task]


def copy(held):
    made = Held()
    made.add(held)
    return made


def held_by(task):
    held = Held()
    held.lines = len(task.lines)
    if task.catalogue is not None:
        held.endpoints = task.catalogue.endpoints
        held.notifications = task.catalogue.notifications
        return held
    if task.driver is not None:
        return held
    if task.serves is not None:
        held.endpoints = 1
    # Its spawn passes it the endpoint of each driver it runs.
    held.endpoints = held.endpoints + len(task.uses) + len(task.hosts)
    if task.watches:
        held.notifications = 1
    return held


def kept_by_init(tasks, manifest):
    """What the init holds for life: its notification, the program its tasks are instances of, each AMP
    port, every endpoint it creates for a served /svc path, a console driver's included, and each
    watcher's notification."""
    kept = Held()
    kept.notifications = 1
    kept.programs = 1
    kept.endpoints = manifest.amp_ports
    for task in tasks:
        if task.catalogue is not None:
            kept.endpoints = kept.endpoints + task.catalogue.endpoints
        elif task.driver is None:
            if task.serves is not None and not task.serves[0].startswith("/amp/"):
                kept.endpoints = kept.endpoints + 1
            if task.watches:
                kept.notifications = kept.notifications + 1
    return kept


def start_of(task):
    """What the init holds past what it keeps while it starts or restarts `task`: the badged copy of
    the init's notification, and for a user task its lines, the badged copy of each endpoint whose
    use names slots and the program it runs where that is not the executable's; for a driver with a
    notification, that notification and its doorbell copy, which its create passes. A driver's lines
    are claimed by its create, never in the init's table. The init never starts a driver inside a
    task."""
    held = Held()
    if task.hosted is not None:
        return held
    held.copies = 1
    if task.catalogue is None:
        held.lines = len(task.lines)
        held.copies = held.copies + len(task.use_slots)
        # A program of another name than the executable's, found for the create alone.
        if task.program is not None and task.program != "main":
            held.programs = 1
        return held
    if task.catalogue.notifications:
        held.notifications = task.catalogue.notifications
        if "doorbell" in task.catalogue.entry:
            held.copies = held.copies + 1
    return held


def init_peak(tasks, manifest, measure):
    """(the task whose start makes `measure` of the init's holding peak, or None, that Held): what
    the init keeps for life plus the most one start holds."""
    kept = kept_by_init(tasks, manifest)
    largest = None
    peak = Held()
    for task in tasks:
        start = start_of(task)
        if measure(start) > measure(peak):
            largest = task
            peak = start
    held = copy(kept)
    held.add(peak)
    return largest, held


def status_block(task, manifest):
    """The bytes of watcher `task`'s status block, one record per task it watches, in `watches`
    order."""
    return len(task.watches) * (manifest.status_record_size or 0)


def watchers(tasks):
    """The tasks that watch, each holding a status block of its own, in table order."""
    return [task for task in tasks if task.watches]


def reserved_regions(shared):
    """The shared regions the init reserves, each one a size was admitted for."""
    return [region for region in shared.values() if region.size is not None]


def private_block(tasks, shared, manifest):
    """The bytes of the init's private block, one record per task and one per shared region, which
    holds where the region was reserved."""
    return (len(tasks) + len(reserved_regions(shared))) * (manifest.private_record_size or 0)


def thread_count(tasks, manifest):
    return sum(len(threads_of(task, manifest)) for task in tasks)


def notification_count(tasks, manifest):
    """The init's notification, each watcher's and each driver's catalogue notifications."""
    notifications = kept_by_init(tasks, manifest).notifications
    for task in tasks:
        if task.catalogue is not None:
            notifications = notifications + task.catalogue.notifications
    return notifications


def init_reserved_blocks(tasks, shared, manifest):
    """Each block the init reserves at boot as (what, bytes, the figure that sizes it), in the
    order it reserves them: its private block, each watcher's status block, each shared region,
    each ring block and, where the thread pointer is not SP masked, each user task's stack. A
    partition region is the user share's, never reserved."""
    reserved = [("the init's private block", private_block(tasks, shared, manifest),
                 "the task and shared region count")]
    for task in watchers(tasks):
        reserved.append(("%s's status block" % task.label(), status_block(task, manifest), "its `watches`"))
    for path, region in shared.items():
        if region.size is not None and not region.partition:
            reserved.append(("shared region `%s`" % path, region.size, "its `size`"))
    for task in tasks:
        if ring_block(task) is not None:
            reserved.append(("%s's ring block" % task.label(), ring_block(task),
                             "driver `%s`'s BLOCK" % task.driver))
    if manifest.stack_stride is None:
        for task in tasks:
            if task.entry and task.stack is not None and not task.on_request:
                reserved.append(("%s's stack" % task.label(), task.stack, "its `stack`"))
    return reserved


def share_seat(manifest):
    """The reservation the kernel seats in root for the partition's user share before the init runs."""
    if manifest.amp_share:
        return 1
    return 0


def init_reservations(tasks, shared, manifest):
    """The bytes of each block the init reserves at boot, in the order it reserves them."""
    return [size for what, size, figure in init_reserved_blocks(tasks, shared, manifest)]


def init_self_grants(tasks):
    """The init's self-grants: its private block, each watcher's status block and each ring block."""
    return 1 + len(watchers(tasks)) + len([task for task in tasks if ring_block(task) is not None])


def line_count(tasks):
    """The IRQ bindings of the composition: one per line a task takes."""
    return sum(len(task.lines) for task in tasks)


def domain_count(tasks, translating):
    """The memory domains of the composition: the two the kernel keeps, and on a translating board
    the init's and one per task, on a region board one per ring block."""
    if translating:
        return 2 + len(own_tasks(tasks)) + 1
    return 2 + len([task for task in tasks if ring_block(task) is not None])


def init_figures(tasks, shared, manifest, translating):
    """What the init spends, as (name, count): the most slots its capability table holds, the
    composition's endpoints, notifications, threads, tasks, IRQ bindings and domains in the kernel's
    pools, and the reservations and self-grants the init makes."""
    largest, peak = init_peak(tasks, manifest, Held.caps)
    return [
        ("cap_slots", (manifest.cap_reserved or 0) + peak.caps()),
        ("endpoints", kept_by_init(tasks, manifest).endpoints),
        ("notifications", notification_count(tasks, manifest)),
        ("threads", thread_count(tasks, manifest)),
        ("tasks", len(own_tasks(tasks))),
        ("irq_handles", line_count(tasks)),
        ("domains", domain_count(tasks, translating)),
        ("reservations", len(init_reservations(tasks, shared, manifest))),
        ("self_grants", init_self_grants(tasks)),
    ]


# Where a declaration's figures size nothing: what the kernel's pools TU takes, in
# KICKOS_SYSTEM_POOLS order (<kickos/pool_storage.h>). `tasks` and `domains` follow the declared
# tasks; every other pool is the sum of the budgets of the kind that names it.
POOL_ORDER = ("threads", "tasks", "domains", "endpoints", "notifies", "irq_bindings", "frame_runs", "ram_owners",
              "cap_slab")
# The kinds whose pool is no pool of the system's TU.
UNSIZED_POOLS = ("none",)
# What one ceiling bounds, for a refusal that names it.
CEILING_PROSE = {
    "threads": "thread pool, KICKOS_THREAD_SLOT_CEILING",
    "tasks": "task pool, KICKOS_TASK_SLOT_CEILING",
    "domains": "domain pool, KICKOS_SLOT_POOL_CEILING",
    "endpoints": "endpoint pool, KICKOS_SLOT_POOL_CEILING",
    "notifies": "notification pool, KICKOS_SLOT_POOL_CEILING",
    "irq_bindings": "IRQ binding pool, KICKOS_IRQ_BINDING_CEILING",
    "frame_runs": "frame-run pool, KICKOS_SLOT_POOL_CEILING",
    "ram_owners": "RAM owner table, KICKOS_SLOT_POOL_CEILING",
}


class Sizing:
    """The init's budgets and the size of every pool, by kind and by pool name."""

    def __init__(self):
        self.init = {}
        self.reserve = {}
        self.pools = {}
        # The chunks of a capability directory row: those of the widest table the system seats.
        self.cap_dir = 0

    def pool_sizes(self):
        """The pools in KICKOS_SYSTEM_POOLS order."""
        return [self.pools.get(name, 0) for name in POOL_ORDER]


def consoles_of(tasks, stdout):
    """The tasks that may serve the console: the one `stdout` names, and each console driver started on
    request."""
    consoles = [task for task in tasks if task.on_request and task.catalogue is not None
                and task.catalogue.console]
    if stdout is not None and stdout != "kernel":
        consoles.append(stdout)
    return consoles


def task_needs(task, tasks, manifest, consoles):
    """What `task` holds by kind as far as the composition states it, the drivers it runs as its
    threads included, and the console endpoint the kernel seats as its standard output wherever a task
    other than it may serve the console: the least a budget of each kind may be."""
    held = held_by(task)
    if consoles and task not in consoles:
        held.endpoints = held.endpoints + 1
    threads = len(threads_of(task, manifest))
    for other in hosted_by(task, tasks):
        driver = held_by(other)
        driver.endpoints = 0
        held.add(driver)
        threads = threads + len(threads_of(other, manifest))
    return {"threads": threads, "endpoints": held.endpoints, "notifications": held.notifications,
            "irq_bindings": held.lines}


def whole_chunks(slots, manifest):
    """`slots` rounded up to the capability slab's chunk."""
    chunk = manifest.cap_chunk
    return (slots + chunk - 1) // chunk * chunk


def init_width(tasks, manifest):
    """The init's capability-table width: the reserved indices and the most it holds at once, in whole
    chunks."""
    task, peak = init_peak(tasks, manifest, Held.caps)
    return whole_chunks((manifest.cap_reserved or 0) + peak.caps(), manifest)


def default_width(manifest):
    """The width a declaration's threads take where no `caps` states one: a full grant list above the
    reserved indices, in whole chunks."""
    grants = manifest.pools.get("KICKOS_MAX_SPAWN_GRANTS", 0)
    return whole_chunks((manifest.cap_reserved or 0) + grants, manifest)


def check_caps(f, task, tasks, manifest, cap_defaults):
    """`task`'s capability-table width and its CAP_SLOTS budget, the sum of the widths of its live
    threads: each as stated, else the composition's default, else default_width and a table of that
    width for each thread of its THREADS budget, both in whole chunks."""
    width, node = default_width(manifest), task.node
    if "width" in task.caps:
        width, node = task.caps["width"]
    elif "width" in cap_defaults:
        width, node = cap_defaults["width"]
    width = whole_chunks(max(width, 1), manifest)
    if manifest.cap_width_max is not None and width > manifest.cap_width_max:
        f.refuse(node, "supply.cap-width",
                 "%s takes capability tables %d slots wide, past the widest one thread seats, %d"
                 % (task.label(), width, manifest.cap_width_max))
    runs = threads_of(task, manifest)
    for other in hosted_by(task, tasks):
        runs = runs + threads_of(other, manifest)
    for name, priority, stack, caps in runs:
        need = (manifest.cap_reserved or 0) + (caps or 0)
        if need > width:
            f.refuse(node, "supply.cap-width",
                     "%s takes capability tables %d slots wide, and its thread `%s` is passed %d capabilities "
                     "above the %d reserved indices" % (task.label(), width, name, caps, manifest.cap_reserved or 0))
    task.cap_width = width
    least = width * max(1, len(runs))
    if "slots" in task.caps:
        slots, node = task.caps["slots"]
        slots = whole_chunks(slots, manifest)
        if slots < least:
            f.refuse(node, "supply.budget",
                     "%s states %d capability slots, and its %d threads at its width %d take %d"
                     % (task.label(), slots, max(1, len(runs)), width, least))
        return slots
    if "slots" in cap_defaults:
        return max(whole_chunks(cap_defaults["slots"][0], manifest), least)
    return max(width * task.budgets.get("threads", 1), least)


def init_budgets(tasks, shared, manifest, translating, rounded):
    """The init's figure of each charged kind: what it holds at its peak, its one thread, every block
    it reserves and the partition's share where the kernel seats it in root's own table; the build's
    default for a kind the composition cannot price."""
    figures = {}
    for kind in manifest.budgets:
        if kind.charged():
            figures[kind.key] = 0
    figures["threads"] = 1
    # A line the init still holds also holds the notification its task binds to it before the
    # init lets the line go.
    for key, measure in (("endpoints", lambda held: held.endpoints),
                         ("notifications", lambda held: held.notifications + held.lines),
                         ("irq_bindings", lambda held: held.lines)):
        task, peak = init_peak(tasks, manifest, measure)
        figures[key] = measure(peak)
    if manifest.cap_chunk is not None:
        figures["cap_slots"] = init_width(tasks, manifest)
    reserved = init_reservations(tasks, shared, manifest)
    blocks = len(reserved)
    sizes = sum(rounded(want) for want in reserved if want)
    if not translating and manifest.amp_share:
        blocks = blocks + share_seat(manifest)
        sizes = sizes + manifest.amp_share
    figures["ram_blocks"] = blocks
    figures["ram_bytes"] = sizes
    for kind in manifest.budgets:
        if kind.key == "pages" and kind.charged():
            figures["pages"] = kind.default
    return {key: value for key, value in figures.items() if key in charged_keys(manifest)}


def charged_keys(manifest):
    return [kind.key for kind in manifest.budgets if kind.charged()]


def check_budget_kinds(f, stated, what, manifest):
    """Each kind `stated` names is one of the kernel build's."""
    known = [kind.key for kind in manifest.budgets]
    for name, (figure, node) in stated.items():
        if name not in known:
            f.refuse(node, "supply.budget-kind",
                     "%s names budget `%s`, and the kernel build's kinds are %s" % (what, name, ", ".join(known)))


def check_budgets(f, root, tasks, shared, manifest, translating, defaults, cap_defaults, reserve, rounded, stdout):
    """Each task's budgets, the init's and the pools they size, every pool past its type ceiling
    refused. `rounded` is the bytes the kernel records a reservation of a want at."""
    sizing = Sizing()
    check_budget_kinds(f, defaults, "the composition's `budgets`", manifest)
    check_budget_kinds(f, reserve, "the composition's `reserve`", manifest)
    own = own_tasks(tasks)
    consoles = consoles_of(tasks, stdout)
    for stated, what in [(defaults, "the composition's `budgets`")] + [(task.stated, task.label()) for task in own]:
        if "cap_slots" in stated:
            f.refuse(stated["cap_slots"][1], "supply.budget-kind",
                     "%s names budget `cap_slots`, which a task states as `caps: { slots: N }`" % what)
    for task in own:
        check_budget_kinds(f, task.stated, task.label(), manifest)
        needs = task_needs(task, tasks, manifest, consoles)
        for kind in manifest.budgets:
            if not kind.charged():
                continue
            if kind.key == "cap_slots":
                if manifest.cap_chunk is not None:
                    task.budgets[kind.key] = check_caps(f, task, tasks, manifest, cap_defaults)
                continue
            need = needs.get(kind.key, 0)
            if kind.key in task.stated:
                figure, node = task.stated[kind.key]
                if figure < need:
                    f.refuse(node, "supply.budget",
                             "%s states a %s budget of %d, and it holds %d of them by its composition"
                             % (task.label(), kind.key, figure, need))
                task.budgets[kind.key] = figure
                continue
            figure = kind.default
            if kind.key == "threads" and task.driver is not None:
                figure = need
            elif kind.key in defaults:
                figure = defaults[kind.key][0]
            task.budgets[kind.key] = max(figure, need)
    sizing.init = init_budgets(tasks, shared, manifest, translating, rounded)
    sizing.reserve = {name: figure for name, (figure, node) in reserve.items() if name in charged_keys(manifest)}
    if "cap_slots" in sizing.reserve and manifest.cap_chunk is not None:
        sizing.reserve["cap_slots"] = whole_chunks(sizing.reserve["cap_slots"], manifest)
    for kind in manifest.budgets:
        if not kind.charged() or kind.pool in UNSIZED_POOLS:
            continue
        total = sizing.init.get(kind.key, 0) + sizing.reserve.get(kind.key, 0)
        total = total + sum(task.budgets.get(kind.key, 0) for task in own)
        if kind.pool not in POOL_ORDER:
            f.refuse(root, "supply.pool",
                     "budget `%s` sizes pool `%s`, which no system's pools TU holds" % (kind.key, kind.pool))
            continue
        sizing.pools[kind.pool] = sizing.pools.get(kind.pool, 0) + total
    kernel = manifest.kernel_cores or 1
    sizing.pools["tasks"] = kernel + 1 + len(own)
    sizing.pools["domains"] = 2 + len(own)
    if translating:
        # Root's, and for each task two spaces of tasks already dead that it may still hold: the
        # one it borrows its block from and the one its entry thread's windows map.
        sizing.pools["domains"] = sizing.pools["domains"] + 1 + 2 * len(own)
    if manifest.cap_chunk is not None:
        widest = max([sizing.init.get("cap_slots", 0)] + [task.budgets.get("cap_slots", 0) for task in own])
        if manifest.cap_width_max is not None:
            widest = min(widest, manifest.cap_width_max)
        sizing.cap_dir = widest // manifest.cap_chunk
    for name in POOL_ORDER:
        ceiling = manifest.ceilings.get(name)
        if ceiling is not None and sizing.pools.get(name, 0) > ceiling:
            f.refuse(stating(root, name, own, defaults, reserve, manifest), "supply.ceiling",
                     "the composition's budgets size the %s to %d, past its type ceiling %d (the %s)"
                     % (name.replace("_", " "), sizing.pools[name], ceiling, CEILING_PROSE[name]))
    return sizing


def stating(root, pool, own, defaults, reserve, manifest):
    """The node of the composition that states a budget sizing `pool`: the reserve's, else the
    composition's defaults', else the first task's that states one; `root` where none does."""
    keys = [kind.key for kind in manifest.budgets if kind.pool == pool]
    for stated in [reserve, defaults] + [task.stated for task in own]:
        for key in keys:
            if key in stated:
                return stated[key][1]
    return root


def check_supply(f, root, tasks, shared, chip, cluster, manifest, translating, region_size, defaults, cap_defaults,
                 reserve, stdout):
    def rounded(want):
        if translating:
            return region_size(want, chip, cluster, manifest)
        return ram_size(want, manifest)

    sizing = check_budgets(f, root, tasks, shared, manifest, translating, defaults, cap_defaults, reserve, rounded,
                           stdout)
    bound = manifest.pools.get("KICKOS_MAX_SPAWN_GRANTS")
    for task in tasks:
        for name, priority, stack, caps in threads_of(task, manifest):
            if bound is not None and caps is not None and caps > bound:
                thread = ""
                if task.driver is not None:
                    thread = " thread `%s`" % name
                f.refuse(task.node, "supply.spawn-grants",
                         "%s%s is spawned with %d capabilities, and one spawn delegates at most %d, so "
                         "KICKOS_MAX_SPAWN_GRANTS would have to grow" % (task.label(), thread, caps, bound))
        check_stack(f, task, manifest)
        check_hosted_blocks(f, task, tasks, translating)

    for region in shared.values():
        if region.size is None:
            continue
        rounded = region_size(region.size, chip, cluster, manifest)
        if rounded > U32:
            f.refuse(region.size_node, "supply.size",
                     "shared region `%s` rounds up to 0x%X bytes, past the 32 bits the table carries a "
                     "region's size in" % (region.path, rounded))

    reserved = init_reservations(tasks, shared, manifest)
    if translating:
        check_ranges(f, root, tasks, reserved, manifest)
        return sizing
    windows = init_self_grants(tasks)
    if manifest.free_regions is not None and windows > manifest.free_regions:
        f.refuse(root, "supply.init-windows",
                 "the init self-grants %d regions, its private block, each watcher's status block and each "
                 "ring block, and root's region set, KICKOS_MPU_MAX_REGIONS less its static regions and its "
                 "stack, holds %d more" % (windows, manifest.free_regions))
    check_arena(f, root, shared, tasks, chip, cluster, manifest)
    return sizing


def check_hosted_blocks(f, task, tasks, translating):
    """The ring block of a driver inside `task` is the task's own shared block, which a translating
    build spends on its stack."""
    held = [other for other in hosted_by(task, tasks) if ring_block(other) is not None]
    limit = 1
    if translating:
        limit = 0
    if len(held) > limit:
        f.refuse(held[limit].nodes["driver"], "supply.hosted-block",
                 "%s runs driver `%s`, whose ring block would be the task's shared block, and %s"
                 % (held[limit].label(), held[limit].driver, hosted_block_prose(translating)))


def hosted_block_prose(translating):
    if translating:
        return "on a build that translates that block is the task's stack and data"
    return "the task has one, which another driver it runs takes"


def check_ranges(f, root, tasks, reserved, manifest):
    """Each address space's range records on a translating board: the init's, its image, its stack
    and every reservation, a self-grant taking none of its own, and each task's own."""
    ranges = manifest.pools.get("KICKOS_ASPACE_RANGES")
    if ranges is None:
        return
    seat = share_seat(manifest)
    init = IMAGE_RANGES + 1 + len(reserved) + seat
    if init > ranges:
        f.refuse(root, "supply.ranges",
                 "the init's address space holds %d ranges, its image's %d, its stack's, %d for the "
                 "partition's user share the kernel seats in root, and one per reservation, its private "
                 "block, each watcher's status block, each shared region, each ring block and each stack, "
                 "and a space holds %d, so KICKOS_ASPACE_RANGES would have to grow"
                 % (init, IMAGE_RANGES, seat, ranges))
    for task in own_tasks(tasks):
        held = space_of(task)
        for other in hosted_by(task, tasks):
            if held is not None and space_of(other) is not None:
                held = held + space_of(other) - IMAGE_RANGES
        if held is not None and held > ranges:
            f.refuse(task.node, "supply.ranges",
                     "%s's address space holds %d ranges, its image's %d, its data, each thread's stack and "
                     "one per window, and a space holds %d, so KICKOS_ASPACE_RANGES would have to grow"
                     % (task.label(), held, IMAGE_RANGES, ranges))


def space_of(task):
    """The range records `task`'s own address space holds, or None for an unknown driver. A user
    task's stack is its data mapping, its one thread running on it."""
    if task.catalogue is not None:
        held = IMAGE_RANGES + len(task.catalogue.threads) + len(task.grants)
        if ring_block(task) is not None:
            held = held + 1
        return held
    if task.driver is not None:
        return None
    # The entry's stack is its data mapping; every other thread of its budget takes a stack.
    threads = task.budgets.get("threads", 1)
    held = IMAGE_RANGES + 1 + max(threads - 1, 0) + len(task.grants) + len(task.maps)
    if task.watches:
        held = held + 1
    return held


def ring_block(task):
    """The bytes of `task`'s ring block, or None."""
    if task.catalogue is not None and isinstance(task.catalogue.block, int):
        return task.catalogue.block
    return None


def check_stack(f, task, manifest):
    if task.driver is not None or task.stack is None:
        return
    floor = manifest.min_stack
    if floor is not None and task.stack < floor:
        f.refuse(task.nodes.get("stack", task.node), "supply.stack",
                 "%s has a %d-byte stack, below the %d bytes, KICKOS_MIN_STACK_SIZE, a thread's exit "
                 "needs" % (task.label(), task.stack, floor))
    stride = manifest.stack_stride
    if stride is not None and task.stack > stride:
        f.refuse(task.nodes.get("stack", task.node), "supply.stack",
                 "%s asks for a %d-byte stack, and where the thread pointer is SP masked every stack is "
                 "one %d-byte stride, KICKOS_USER_STACK_SIZE rounded to a power of two"
                 % (task.label(), task.stack, stride))
    align = manifest.stack_align
    if align and task.stack % align:
        f.refuse(task.nodes.get("stack", task.node), "supply.stack",
                 "%s has a %d-byte stack, which a spawn refuses unless it is a multiple of %d, "
                 "KICKOS_STACK_ALIGN" % (task.label(), task.stack, align))


def arena_blocks(tasks, shared, manifest):
    """Each block arch_ram_alloc places in the arena as (what, bytes, the figure that sizes it): the
    boot stacks, the init's reservations in the order it makes them, then the kernel's default
    stacks in start order."""
    blocks = [("idle's boot stack", manifest.idle_stack, "KICKOS_IDLE_STACK_SIZE"),
              ("root's boot stack", manifest.root_stack, "KICKOS_ROOT_STACK_SIZE")]
    blocks.extend(init_reserved_blocks(tasks, shared, manifest))
    for task in tasks:
        if task.on_request or (task.entry and manifest.stack_stride is None):
            continue
        # arch_ram_alloc(KICKOS_USER_STACK_SIZE).
        for name, priority, stack, caps in threads_of(task, manifest):
            what = "%s's default stack" % task.label()
            if task.driver is not None:
                what = "%s's thread `%s`'s default stack" % (task.label(), name)
            blocks.append((what, manifest.user_stack, "KICKOS_USER_STACK_SIZE"))
    return blocks


def check_arena(f, root, shared, tasks, chip, cluster, manifest):
    """Replays arch_ram_alloc over the arena entry."""
    blocks = [size for what, size, figure in arena_blocks(tasks, shared, manifest)]
    for view, (base, size) in sorted(chip.arena.items(), key=lambda item: str(item[0])):
        if cluster is not None and view != cluster:
            continue
        where = "the host-placed arena"
        if base is None:
            base = 0
        else:
            where = "the arena at 0x%X" % base
        arena = ArenaModel(base, chip.arena_line.get(view))
        for want in blocks:
            if not want:
                continue
            arena.place(ram_size(want, manifest), ram_align(want, manifest))
        cursor = arena.cursor
        if cursor > base + size:
            f.refuse(root, "supply.arena",
                     "the composition carves 0x%X bytes from %s, its boot stacks, the init's "
                     "private block, the watchers' status blocks, shared regions, ring blocks and stacks, and the "
                     "kernel's default stacks, each rounded and aligned as the allocator places them, and the "
                     "arena holds 0x%X"
                     % (cursor - base, where, size))
            break


def check_scheduling(f, top, tasks, stdout, manifest):
    kernel = manifest.kernel_cores
    for task in tasks:
        if task.core is not None and kernel is not None and task.core >= kernel:
            f.refuse(task.nodes["core"], "scheduling.core",
                     "%s declares core %d, and the kernel build schedules %d core(s), 0 to %d"
                     % (task.label(), task.core, kernel, kernel - 1))
        if task.hosted is None and task.core is None and kernel is not None and kernel > 1 and any(
                other.hosted is task for other in tasks):
            f.refuse(task.nodes["drivers"], "scheduling.hosted",
                     "%s runs drivers as its threads on a build of %d kernel cores, so it declares the one "
                     "`core` they and its entry run on" % (task.label(), kernel))
        if task.lines and task.core is None and kernel is not None and kernel > 1:
            f.refuse(task.nodes["lines"], "scheduling.line-core",
                     "%s takes a line on a build of %d kernel cores, so it declares the one `core` its "
                     "line is claimed and waited on" % (task.label(), kernel))
    console = None
    if stdout is not None and stdout != "kernel":
        console = stdout
        if console.catalogue is None or not console.catalogue.console:
            f.refuse(top["stdout"], "scheduling.console-driver",
                     "`stdout` names `%s`, served by %s, which is no packaged driver that takes the console"
                     % (console.serves[0], console.label()))
            console = None
    for task in tasks:
        if task.catalogue is not None and task.catalogue.console and task.hosted is not None:
            f.refuse(task.nodes["driver"], "scheduling.console-driver",
                     "%s runs driver `%s`, which takes the console, and the kernel closes the standard "
                     "output of every thread of the task serving the console: it runs as a task of its own"
                     % (task.label(), task.driver))
            continue
        if task.catalogue is not None and task.on_request:
            if not task.catalogue.console or task.catalogue.windows or task.catalogue.lines:
                f.refuse(task.nodes["driver"], "scheduling.console-driver",
                         "%s runs driver `%s` on request, and a driver started on request takes the console "
                         "and holds no window and no line" % (task.label(), task.driver))
            continue
        if task.catalogue is not None and task.catalogue.console and stdout is not None and task is not stdout:
            f.refuse(task.nodes["driver"], "scheduling.console-driver",
                     "%s runs driver `%s`, which takes the console, and `stdout` does not name its endpoint"
                     % (task.label(), task.driver))
    requested = [task for task in tasks if task.on_request and task.catalogue is not None
                 and task.catalogue.console]
    if requested and stdout is not None and stdout != "kernel":
        f.refuse(requested[0].nodes["driver"], "scheduling.console-driver",
                 "%s runs driver `%s` on request, and the console is %s's from the start: one task holds the "
                 "console at a time" % (requested[0].label(), requested[0].driver, stdout.label()))
    for extra in requested[1:]:
        f.refuse(extra.nodes["driver"], "scheduling.console-driver",
                 "%s runs driver `%s` on request, and %s could hold the console beside it: one task holds the "
                 "console at a time" % (extra.label(), extra.driver, requested[0].label()))
    if console is None:
        return
    first = console
    if console.hosted is not None:
        first = console.hosted
    for task in tasks:
        if task.entry and task.index < first.index:
            f.refuse(task.node, "scheduling.stdout-order",
                     "%s writes standard output and is declared before %s, which serves `stdout`"
                     % (task.label(), console.label()))
