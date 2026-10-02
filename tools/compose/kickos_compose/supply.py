# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# docs/design-m10-composition.md, "What one task costs", counted against the manifest.

U32 = 0xFFFFFFFF
# The badged copies of a driver's notification its bring-up holds at once
# (user/src/driver_service.cc, spawn_one).
BADGED_COPIES = 2


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
        for role in driver.lines:
            if role not in bound:
                f.refuse(task.nodes.get("lines", task.node), "driver.line-role",
                         "%s leaves line role `%s` of driver `%s` unbound" % (what, role, task.driver))
        if task.device_count != len(driver.windows):
            f.refuse(task.nodes.get("devices", task.node), "driver.window-role",
                     "%s grants %d device(s), and driver `%s` binds its devices in order to its window "
                     "roles, %s" % (what, task.device_count, task.driver, roles_prose(driver.windows)))
        if "authority" in task.nodes:
            f.refuse(task.nodes["authority"], "driver.authority",
                     "%s runs driver `%s`, whose threads take the authority its descriptor states, so it "
                     "declares no `authority`" % (what, task.driver))


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
            if priority is not None and not lo <= priority <= hi:
                thread = ""
                if task.driver is not None:
                    thread = ", its thread `%s` at %d," % (name, priority)
                f.refuse(task.nodes["priority"], "scheduling.priority",
                         "%s runs at priority %d%s outside the kernel build's range [%d, %d]"
                         % (task.label(), task.priority, thread, lo, hi))
                break


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


def init_steps(tasks, manifest):
    """What the init holds at each step of its walk, as (task it starts or restarts, Held), the
    first entry being the init before any task."""
    kept = Held()
    kept.notifications = 1
    kept.endpoints = manifest.amp_ports
    keeps = []
    passes = []
    for task in tasks:
        keep = Held()
        passing = Held()
        passing.lines = len(task.lines)
        if task.catalogue is not None:
            if task.catalogue.posture == "retain":
                keep.endpoints = task.catalogue.endpoints
            else:
                passing.endpoints = task.catalogue.endpoints
            passing.notifications = task.catalogue.notifications
            if task.catalogue.notifications:
                passing.copies = BADGED_COPIES
        elif task.driver is None:
            if task.serves is not None:
                keep.endpoints = 1
            if task.watches:
                keep.notifications = 1
        keeps.append(keep)
        passes.append(passing)
    steps = [(None, copy(kept))]
    for task, keep, passing in zip(tasks, keeps, passes):
        kept.add(keep)
        step = copy(kept)
        step.add(passing)
        steps.append((task, step))
    for task, passing in zip(tasks, passes):
        if task.restart_max:
            step = copy(kept)
            step.add(passing)
            steps.append((task, step))
    return steps


def check_supply(f, root, heap, tasks, shared, chip, cluster, manifest, translating, region_size):
    threads = sum(len(threads_of(task, manifest)) for task in tasks)
    endpoints = manifest.amp_ports
    notifications = 1
    for task in tasks:
        if task.catalogue is not None:
            endpoints = endpoints + task.catalogue.endpoints
            notifications = notifications + task.catalogue.notifications
        elif task.driver is None:
            if task.serves is not None:
                endpoints = endpoints + 1
            if task.watches:
                notifications = notifications + 1
    kernel = manifest.kernel_cores or 1
    domains = 2 + len(tasks) + 1
    domain_prose = "the two the kernel keeps, the init's and one per task"
    if not translating:
        domains = 2 + sum(1 for task in tasks if brings_grant(task))
        domain_prose = "the two the kernel keeps and one per task bringing a ring block or a shared map"
    pools = [
        ("KICKOS_MAX_TASKS", kernel + 1 + len(tasks), "each kernel core's idle, the init's, and one per task"),
        ("KICKOS_MAX_THREADS", threads, "the threads of every task, the init's being root's own slot"),
        ("KICKOS_MAX_DOMAINS", domains, domain_prose),
        ("KICKOS_MAX_ENDPOINTS", endpoints, "every served endpoint and each AMP port"),
        ("KICKOS_MAX_NOTIFY", notifications, "the init's, one per watching task and each driver's"),
        ("KICKOS_MAX_IRQ_HANDLES", sum(len(task.lines) for task in tasks), "one per line a task takes"),
    ]
    for knob, used, prose in pools:
        if knob in manifest.pools and used > manifest.pools[knob]:
            f.refuse(root, "supply.pool",
                     "the composition takes %d of %s's %d, %s, so %s would have to grow"
                     % (used, knob, manifest.pools[knob], prose, knob))

    budgets = (("endpoints", "KICKOS_TASK_ENDPOINT_BUDGET"), ("notifications", "KICKOS_TASK_NOTIFY_BUDGET"),
               ("lines", "KICKOS_TASK_IRQ_HANDLE_BUDGET"))
    steps = init_steps(tasks, manifest)
    for kind, knob in budgets:
        task, step = max(steps, key=lambda entry: getattr(entry[1], kind))
        check_budget(f, root, "the init%s" % at_step(task), getattr(step, kind), kind, knob, manifest)
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
        task, step = max(steps, key=lambda entry: entry[1].caps())
        slots = manifest.cap_reserved + step.caps()
        if slots > supply:
            f.refuse(root, "supply.cap-table",
                     "the init's capability table needs %d slots%s: %d reserved, %d endpoints, %d "
                     "notifications and their copies, and %d lines, and the board backs %d, so "
                     "KICKOS_CAP_TABLE_SUPPLY would have to grow"
                     % (slots, at_step(task), manifest.cap_reserved, step.endpoints,
                        step.notifications + step.copies, step.lines, supply))

    regions = []
    for region in shared.values():
        if region.size is None:
            continue
        rounded = region_size(region.size, chip, cluster, manifest)
        if rounded > U32:
            f.refuse(region.size_node, "supply.size",
                     "shared region `%s` rounds up to 0x%X bytes, past the 32 bits the table carries a "
                     "region's size in" % (region.path, rounded))
        regions.append(region.size)
    if not translating:
        check_arena(f, root, heap, regions, tasks, chip, cluster, manifest)


def brings_grant(task):
    if task.catalogue is not None:
        return isinstance(task.catalogue.block, int)
    return bool(task.maps)


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
        f.refuse(task.nodes["stack"], "supply.stack",
                 "%s has a %d-byte stack, below the %d bytes, KICKOS_MIN_STACK_SIZE, a thread's exit "
                 "needs" % (task.label(), task.stack, floor))
    stride = manifest.stack_stride
    if stride is not None and task.stack > stride:
        f.refuse(task.nodes["stack"], "supply.stack",
                 "%s asks for a %d-byte stack, and where the thread pointer is SP masked every stack is "
                 "one %d-byte stride, KICKOS_USER_STACK_SIZE rounded to a power of two"
                 % (task.label(), task.stack, stride))
    align = manifest.stack_align
    if align and task.stack % align:
        f.refuse(task.nodes["stack"], "supply.stack",
                 "%s has a %d-byte stack, which a spawn refuses unless it is a multiple of %d, "
                 "KICKOS_STACK_ALIGN" % (task.label(), task.stack, align))


def check_arena(f, root, heap, regions, tasks, chip, cluster, manifest):
    """Replays arch_ram_alloc over the arena entry in the order the boot and the walk allocate."""
    blocks = [manifest.idle_stack, manifest.root_stack]
    if heap is not None:
        blocks.append(heap)
    blocks.extend(regions)
    for task in tasks:
        if task.catalogue is not None and isinstance(task.catalogue.block, int):
            blocks.append(task.catalogue.block)
        for name, priority, stack, caps in threads_of(task, manifest):
            if manifest.stack_stride is not None and stack is not None:
                stack = manifest.stack_stride
            blocks.append(stack)
    for view, (base, size) in sorted(chip.arena.items(), key=lambda item: str(item[0])):
        if cluster is not None and view != cluster:
            continue
        cursor = base
        for want in blocks:
            if want is None:
                continue
            align = ram_align(want, manifest)
            cursor = -(-cursor // align) * align + ram_size(want, manifest)
        if cursor > base + size:
            f.refuse(root, "supply.arena",
                     "the composition carves 0x%X bytes from the arena at 0x%X, its boot stacks, heap, "
                     "shared regions, ring blocks and stacks each rounded and aligned as the allocator "
                     "places them, and the arena holds 0x%X" % (cursor - base, base, size))
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
    if console is None or console.priority is None or console.catalogue.receiver is None:
        return
    name, offset, stack = console.catalogue.threads[console.catalogue.receiver]
    ceiling = console.priority + offset
    for task in tasks:
        if not task.entry:
            continue
        if task.index < console.index:
            f.refuse(task.node, "scheduling.stdout-order",
                     "%s writes standard output and is declared before %s, which serves `stdout`"
                     % (task.label(), console.label()))
        if task.priority is not None and task.priority > ceiling:
            f.refuse(task.nodes["priority"], "scheduling.stdout-priority",
                     "%s writes standard output at priority %d, above the %d of thread `%s`, which "
                     "receives on the console's endpoint with no priority inheritance"
                     % (task.label(), task.priority, ceiling, name))
