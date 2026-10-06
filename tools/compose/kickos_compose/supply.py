# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# docs/design-m10-composition.md, "What one task costs", counted against the manifest.

U32 = 0xFFFFFFFF
# The ranges every address space opens with, its image's text and data
# (kernel/include/kickos/vrange.h, VR_IMAGE_SLOTS).
IMAGE_RANGES = 2


def check_drivers(f, tasks, manifest):
    starts = {}
    for name, driver in manifest.drivers.items():
        starts[driver.start] = name
    for task in tasks:
        task.catalogue = None
        if task.entry_name in starts:
            f.refuse(task.nodes["entry"], "name.entry",
                     "%s entry `%s` is the start of packaged driver `%s`, which the init calls itself"
                     % (task.label(), task.entry_name, starts[task.entry_name]))
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
        if task.ceiling is not None and not lo <= task.ceiling <= hi:
            f.refuse(task.nodes["ceiling"], "scheduling.ceiling",
                     "%s ceiling %d, the highest priority its threads may take, is outside the kernel build's "
                     "range [%d, %d]; the usual value is its top, %d" % (task.label(), task.ceiling, lo, hi, hi))


class Held:
    def __init__(self):
        self.endpoints = 0
        self.notifications = 0
        self.lines = 0
        # Capabilities past one per object: a notification's badged copies.
        self.copies = 0

    def add(self, other):
        self.endpoints = self.endpoints + other.endpoints
        self.notifications = self.notifications + other.notifications
        self.lines = self.lines + other.lines
        self.copies = self.copies + other.copies

    def caps(self):
        return self.endpoints + self.notifications + self.lines + self.copies


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
    held.endpoints = held.endpoints + len(task.uses)
    if task.watches:
        held.notifications = 1
    return held


def kept_by_init(tasks, manifest):
    """What the init holds for life: its notification, each AMP port, every endpoint it creates for a
    served /svc path, a console driver's included, and each watcher's notification."""
    kept = Held()
    kept.notifications = 1
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
    """What the init holds past what it keeps while it starts or restarts `task`: the task's lines and
    the badged copy of the init's notification; for a driver with a notification, that notification
    and the most badged copies of it one step of the bring-up holds, one while it binds a line or
    those one thread's spawn mints."""
    held = Held()
    held.lines = len(task.lines)
    held.copies = 1
    if task.catalogue is not None and task.catalogue.notifications:
        held.notifications = task.catalogue.notifications
        minted = [badged for badged in task.catalogue.badged if badged is not None]
        if task.lines:
            minted.append(1)
        held.copies = held.copies + max(minted + [0])
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
            if task.entry and task.stack is not None:
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
        return 2 + len(tasks) + 1
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
        ("tasks", len(tasks)),
        ("irq_handles", line_count(tasks)),
        ("domains", domain_count(tasks, translating)),
        ("reservations", len(init_reservations(tasks, shared, manifest))),
        ("self_grants", init_self_grants(tasks)),
    ]


def check_supply(f, root, tasks, shared, chip, cluster, manifest, translating, region_size):
    threads = thread_count(tasks, manifest)
    kept = kept_by_init(tasks, manifest)
    notifications = notification_count(tasks, manifest)
    kernel = manifest.kernel_cores or 1
    domains = domain_count(tasks, translating)
    domain_prose = "the two the kernel keeps, the init's and one per task"
    if not translating:
        domain_prose = "the two the kernel keeps and one per ring block"
    pools = [
        ("KICKOS_MAX_TASKS", kernel + 1 + len(tasks), "each kernel core's idle, the init's, and one per task"),
        ("KICKOS_MAX_THREADS", threads, "the threads of every task, the init's being root's own slot"),
        ("KICKOS_MAX_DOMAINS", domains, domain_prose),
        ("KICKOS_MAX_ENDPOINTS", kept.endpoints, "every served endpoint and each AMP port"),
        ("KICKOS_MAX_NOTIFY", notifications, "the init's, one per watching task and each driver's"),
        ("KICKOS_MAX_IRQ_HANDLES", line_count(tasks), "one per line a task takes"),
    ]
    for knob, used, prose in pools:
        if knob in manifest.pools and used > manifest.pools[knob]:
            f.refuse(root, "supply.pool",
                     "the composition takes %d of %s's %d, %s, so %s would have to grow"
                     % (used, knob, manifest.pools[knob], prose, knob))

    budgets = (("endpoints", "KICKOS_TASK_ENDPOINT_BUDGET"), ("notifications", "KICKOS_TASK_NOTIFY_BUDGET"),
               ("lines", "KICKOS_TASK_IRQ_HANDLE_BUDGET"))
    for kind, knob in budgets:
        task, peak = init_peak(tasks, manifest, lambda held, kind=kind: getattr(held, kind))
        check_budget(f, root, "the init%s" % at_step(task), getattr(peak, kind), kind, knob, manifest)
    bound = manifest.pools.get("KICKOS_MAX_SPAWN_GRANTS")
    for task in tasks:
        held = held_by(task)
        for kind, knob in budgets:
            check_budget(f, task.node, task.label(), getattr(held, kind), kind, knob, manifest)
        for name, priority, stack, caps in threads_of(task, manifest):
            if bound is not None and caps is not None and caps > bound:
                thread = ""
                if task.driver is not None:
                    thread = " thread `%s`" % name
                f.refuse(task.node, "supply.spawn-grants",
                         "%s%s is spawned with %d capabilities, and one spawn delegates at most %d, so "
                         "KICKOS_MAX_SPAWN_GRANTS would have to grow" % (task.label(), thread, caps, bound))
        check_stack(f, task, manifest)

    supply = manifest.pools.get("KICKOS_CAP_TABLE_SUPPLY")
    if supply is not None and manifest.cap_reserved is not None:
        task, peak = init_peak(tasks, manifest, Held.caps)
        slots = manifest.cap_reserved + peak.caps()
        if slots > supply:
            f.refuse(root, "supply.cap-table",
                     "the init's capability table needs %d slots%s: %d reserved, %d endpoints, %d "
                     "notifications and their copies, and %d lines, and the board backs %d, so "
                     "KICKOS_CAP_TABLE_SUPPLY would have to grow"
                     % (slots, at_step(task), manifest.cap_reserved, peak.endpoints,
                        peak.notifications + peak.copies, peak.lines, supply))

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
        return
    windows = init_self_grants(tasks)
    if manifest.free_regions is not None and windows > manifest.free_regions:
        f.refuse(root, "supply.init-windows",
                 "the init self-grants %d regions, its private block, each watcher's status block and each "
                 "ring block, and root's region set, KICKOS_MPU_MAX_REGIONS less its static regions and its "
                 "stack, holds %d more" % (windows, manifest.free_regions))
    slots = manifest.pools.get("KICKOS_RAM_OWNER_SLOTS")
    seat = share_seat(manifest)
    if manifest.enforced and slots is not None and len(reserved) + seat > slots:
        f.refuse(root, "supply.reservations",
                 "the init reserves %d arena blocks, its private block, each watcher's status block, each "
                 "shared region, each ring block and each stack, beside the %d the kernel seats in root for "
                 "the partition's user share, and the kernel records the owner of %d, so "
                 "KICKOS_RAM_OWNER_SLOTS would have to grow" % (len(reserved), seat, slots))
    check_arena(f, root, shared, tasks, chip, cluster, manifest)


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
    for task in tasks:
        held = space_of(task)
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
    held = IMAGE_RANGES + 1 + len(task.grants) + len(task.maps)
    if task.watches:
        held = held + 1
    return held


def ring_block(task):
    """The bytes of `task`'s ring block, or None."""
    if task.catalogue is not None and isinstance(task.catalogue.block, int):
        return task.catalogue.block
    return None


def at_step(task):
    if task is None:
        return ""
    return " while it starts %s" % task.label()


def check_budget(f, node, holder, used, kind, knob, manifest):
    if knob in manifest.pools and used > manifest.pools[knob]:
        f.refuse(node, "supply.budget",
                 "%s holds %d %s, and %s lets one task hold %d, so it would have to grow"
                 % (holder, used, kind, knob, manifest.pools[knob]))


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
        if task.entry and manifest.stack_stride is None:
            continue
        # arch_ram_alloc(KICKOS_USER_STACK_SIZE), which ram_align strides where SP is masked.
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
        cursor = base
        for want in blocks:
            if not want:
                continue
            align = ram_align(want, manifest)
            cursor = -(-cursor // align) * align + ram_size(want, manifest)
        if cursor > base + size:
            f.refuse(root, "supply.arena",
                     "the composition carves 0x%X bytes from %s, its boot stacks, the init's "
                     "private block, the watchers' status blocks, shared regions, ring blocks and stacks, and the "
                     "kernel's default stacks, each rounded and aligned as the allocator places them, and the "
                     "arena holds 0x%X"
                     % (cursor - base, where, size))
            break


def ram_size(want, manifest):
    """arch_ram_region_size()."""
    smallest = manifest.smallest_window
    if manifest.window_rule not in ("pow2", "granule"):
        return -(-want // 16) * 16
    want = max(want, smallest)
    if manifest.window_rule == "granule":
        return -(-want // smallest) * smallest
    return pow2_ceil(want)


def ram_align(want, manifest):
    """arch_ram_region_align()."""
    geometry = 16
    if manifest.window_rule == "granule":
        geometry = manifest.smallest_window
    if manifest.window_rule == "pow2":
        geometry = ram_size(want, manifest)
    if manifest.stack_stride is not None:
        geometry = max(geometry, pow2_ceil(want))
    return geometry


def pow2_ceil(want):
    power = 1
    while power < want:
        power = power * 2
    return power


def check_scheduling(f, top, tasks, stdout, manifest):
    kernel = manifest.kernel_cores
    for task in tasks:
        if task.core is not None and kernel is not None and task.core >= kernel:
            f.refuse(task.nodes["core"], "scheduling.core",
                     "%s declares core %d, and the kernel build schedules %d core(s), 0 to %d"
                     % (task.label(), task.core, kernel, kernel - 1))
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
        if task.catalogue is not None and task.catalogue.console and stdout is not None and task is not stdout:
            f.refuse(task.nodes["driver"], "scheduling.console-driver",
                     "%s runs driver `%s`, which takes the console, and `stdout` does not name its endpoint"
                     % (task.label(), task.driver))
    if console is None:
        return
    for task in tasks:
        if task.entry and task.index < console.index:
            f.refuse(task.node, "scheduling.stdout-order",
                     "%s writes standard output and is declared before %s, which serves `stdout`"
                     % (task.label(), console.label()))
