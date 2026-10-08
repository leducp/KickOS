# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The partition build (docs/design-m10-fleet.md, section 9): every node's composition admitted
# together against one kernel build's manifest, the node index set to each composition's own, the
# partition's rules over all of them, the partition regions placed in the user share, and the gate
# assignment derived from the grants.

import copy

from .composition import (
    Cache, admit_composition, amp_port, gate_unit, read_composition, region_size,
)
from .descriptions import cluster_views
from .manifest import read_manifest
from .subset import File, Report, line_of

# The port the window layer answers itself, which needs no serving task.
AMP_PORT_ECHO = 0


class Row:
    """One row of the gate assignment: a region of a region gate, or a register of a per-peripheral
    gate, given to a node."""

    def __init__(self, gate, node, base, size, access, register=0, grant=None):
        self.gate = gate
        self.node = node
        self.base = base
        self.size = size
        self.access = access
        self.register = register
        # The grant that made the row, or None for the kernel's own.
        self.grant = grant

    def end(self):
        return self.base + self.size


class Partition:
    """The node compositions of one partition, each Admitted or None."""

    def __init__(self, paths, admitted, manifest):
        self.paths = paths
        self.admitted = admitted
        self.manifest = manifest
        self.rows = []


def node_manifest(manifest, node):
    """The manifest as node `node`'s build reads it: this build's, its node index set."""
    made = copy.copy(manifest)
    made.amp_node = node
    return made


def admit_partition(paths, manifest_path, node):
    """(the Report, the Partition or None once refused). `node` is this build's index, which its
    manifest states."""
    report = Report()
    manifest = read_manifest(manifest_path, report)
    if not manifest:
        return report, None
    if manifest.amp_nodes != len(paths) or manifest.amp_node != node:
        report.refuse(paths[0], 1, "manifest.amp",
                      "the partition names %d node composition(s) and this build is node %d, where the kernel "
                      "build's manifest states %s node(s) and node %s"
                      % (len(paths), node, manifest.amp_nodes, manifest.amp_node))
        return report, None
    cache = Cache()
    admitted = []
    for k, path in enumerate(paths):
        text = read_composition(path, report)
        made = None
        if text is not None:
            made = admit_composition(path, text, None, report, cache, node_manifest(manifest, k), partition=True)
        admitted.append(made)
    files = [File(path, report) for path in paths]
    if report.refusals or None in admitted:
        return report, None
    partition = Partition(paths, admitted, manifest)
    check_devices(files, admitted)
    check_unserved(files, admitted, manifest)
    place_regions(files, admitted, manifest)
    check_cached(files, admitted)
    partition.rows = partition_gate(files, admitted, manifest)
    if report.refusals:
        return report, None
    return report, partition


def mem_grants(admitted):
    return [grant for task in admitted.tasks for grant in task.grants]


def check_devices(files, admitted):
    """partition.device: a device, or a device gate's unit around it, two nodes grant."""
    for k, later in enumerate(admitted):
        for j in range(k):
            earlier = admitted[j]
            task_views = cluster_views(later.chip, later.cluster) + cluster_views(earlier.chip, earlier.cluster)
            for grant in mem_grants(later):
                for held in mem_grants(earlier):
                    if grant.space != held.space:
                        continue
                    shared = None
                    if grant.base < held.end() and held.base < grant.end():
                        shared = "`%s`" % held.path
                    elif grant.space == "mem":
                        unit = gate_unit(later.chip, task_views, (held.base, held.end()), (grant.base, grant.end()))
                        if unit is not None:
                            shared = "the 0x%X-byte %s at 0x%X that opens `%s`" % (unit[1], unit[0], unit[2], held.path)
                    if shared is None:
                        continue
                    files[k].refuse(grant.node, "partition.device",
                                    "%s holds `%s` on node %d, and %s is held by %s on node %d (%s:%d)"
                                    % (grant.task.label(), grant.path, k, shared, held.task.label(), j,
                                       files[j].filename, line_of(held.node)))
                    break


def check_unserved(files, admitted, manifest):
    """partition.unserved: a crossing a node uses that no task of its server serves."""
    servers = {}
    for node, port in manifest.amp_list:
        servers.setdefault(port, []).append(node)
    served = []
    for made in admitted:
        served.append(set(task.serves[0] for task in made.tasks if task.serves is not None))
    for k, made in enumerate(admitted):
        for task in made.tasks:
            for path, node in task.uses:
                port = amp_port(path)
                if port is None or port == AMP_PORT_ECHO or len(servers.get(port, [])) != 1:
                    continue
                server = servers[port][0]
                if path not in served[server]:
                    files[k].refuse(node, "partition.unserved",
                                    "%s uses `%s`, which node %d serves in the partition list, and no task of "
                                    "node %d's composition, %s, serves it"
                                    % (task.label(), path, server, server, files[server].filename))


def is_partition_region(made, path):
    return path in made.shared and made.shared[path].partition


def partition_regions(made):
    return [(path, region) for path, region in made.shared.items() if is_partition_region(made, path)]


def region_align(size, made, manifest):
    """The alignment a partition region's base needs on node `made`: its rounded size under a
    pow2 rule, the granule otherwise, and each translating view's page."""
    align = 1
    if manifest.window_rule == "pow2":
        align = size
    elif manifest.window_rule == "granule" and manifest.smallest_window:
        align = manifest.smallest_window
    for view in cluster_views(made.chip, made.cluster):
        unit = made.chip.protection.get(view)
        if unit is not None and unit.page is not None:
            align = max(align, unit.page)
    return align


def share_base(manifest):
    if manifest.amp_window is None:
        return 0
    base, size = manifest.amp_window
    return base + size - manifest.amp_share


def place_regions(files, admitted, manifest):
    """partition.region: a partition region two nodes declare with different sizes, one another node
    declares without `partition: true`, or one placed past the user share; a `cache` other than the
    share's is each node's own refusal. Each is placed in node 0's declaration order, then in node
    order, at the alignment every node mapping it needs."""
    declared = {}
    order = []
    for k, made in enumerate(admitted):
        for path, region in partition_regions(made):
            if path not in declared:
                declared[path] = (k, region)
                order.append(path)
                continue
            j, first = declared[path]
            if region.size != first.size:
                files[k].refuse(region.size_node, "partition.region",
                                "partition region `%s` has size %s on node %d, and %s on node %d (%s:%d)"
                                % (path, region.size, k, first.size, j, files[j].filename,
                                   line_of(first.size_node)))
    for k, made in enumerate(admitted):
        for path, region in made.shared.items():
            if region.partition or path not in declared:
                continue
            j, first = declared[path]
            files[k].refuse(region.node, "partition.region",
                            "shared region `%s` is not a partition region on node %d, and is one on node %d (%s:%d)"
                            % (path, k, j, files[j].filename, line_of(first.node)))
    base = share_base(manifest)
    cursor = 0
    for path in order:
        k, region = declared[path]
        size = 0
        align = 1
        for made in admitted:
            if is_partition_region(made, path):
                size = max(size, region_size(region.size, made.chip, made.cluster, made.manifest))
                align = max(align, region_align(size, made, made.manifest))
        offset = -(-(base + cursor) // align) * align - base
        cursor = offset + size
        for n, made in enumerate(admitted):
            if not is_partition_region(made, path):
                continue
            made.offsets[path] = offset
            if cursor > manifest.amp_share:
                files[n].refuse(made.shared[path].node, "partition.region",
                                "partition region `%s` is placed at 0x%X in the partition's 0x%X-byte user share, "
                                "0x%X bytes at 0x%X aligned to 0x%X, after every region node 0 declares before it, "
                                "and ends past it" % (path, offset, manifest.amp_share, size, offset, align))


def coherent(chips_views):
    """Whether nodes on these (chip, views) see one memory coherently: no data cache over it, or one
    cluster the chip states `smp`."""
    chip = chips_views[0][0]
    if not chip.data_cache:
        return True
    seen = set()
    for _, node_views in chips_views:
        seen.update(node_views)
    if len(seen) != 1:
        return False
    return chip.smp.get(seen.pop(), False)


def check_cached(files, admitted):
    """partition.cached-incoherent: a `cached` partition region nodes that are not coherent map,
    unless every node mapping it accepts `cached_incoherent`."""
    regions = {}
    for k, made in enumerate(admitted):
        for path, region in partition_regions(made):
            if region.cache != "cached":
                continue
            mapping = [task for task in made.tasks if any(mapped == path for mapped, node in task.maps)]
            if mapping:
                regions.setdefault(path, []).append(k)
    for path, nodes in regions.items():
        if len(nodes) < 2:
            continue
        if coherent([(admitted[k].chip, cluster_views(admitted[k].chip, admitted[k].cluster)) for k in nodes]):
            continue
        for k in nodes:
            made = admitted[k]
            if "cached_incoherent" in [name for name, node in made.accepts]:
                continue
            files[k].refuse(made.shared[path].cache_node, "partition.cached-incoherent",
                            "partition region `%s` is `cached`, and nodes %s map it from caches the chip file "
                            "does not declare coherent, so every one of them needs `cached_incoherent` in its "
                            "`accepts` and does its own cache maintenance"
                            % (path, ", ".join(str(n) for n in nodes)))


def fronted(gate, grant):
    return any(base <= grant.base and grant.end() <= base + size for base, size in gate.ranges)


def coalesce(rows):
    """Rows of one gate, each pair of one node and one access that touch merged."""
    merged = []
    for row in sorted(rows, key=lambda r: (r.node, r.access, r.base)):
        if merged and touches(merged[-1], row):
            last = merged[-1]
            last.size = row.end() - last.base
            if last.grant is None:
                last.grant = row.grant
            continue
        merged.append(Row(row.gate, row.node, row.base, row.size, row.access, row.register, row.grant))
    return merged


def touches(row, after):
    return row.node == after.node and row.access == after.access and row.end() == after.base


def derive_gate(chip, manifest, grants, nodes, refuse):
    """The gate assignment, each row a Row. `grants` holds each memory-window grant as (node, Grant),
    `nodes` is the partition's node count, and `refuse(node, Grant or None, message)` reports
    partition.gate-budget: a gate whose rows outnumber its regions less its catch-all, a grant no
    register or region gate fronts, or a register two nodes' grants would each be assigned."""
    gate = chip.partition_gate
    if gate is None:
        return []
    rows = []
    if gate.kind == "accessctrl":
        for k, grant in grants:
            register = grant.device.gate_register
            if register is None:
                refuse(k, grant, "%s holds `%s` on node %d, which no register of partition gate `%s` assigns, so "
                                 "no node's kernel can keep it from the others"
                       % (grant.task.label(), grant.path, k, gate.device))
                continue
            owner = [row for row in rows if row.register == register]
            if owner and owner[0].node != k:
                refuse(k, grant, "%s holds `%s` on node %d, and register 0x%X of partition gate `%s` already "
                                 "assigns `%s` to node %d, so one register would be both nodes'"
                       % (grant.task.label(), grant.path, k, register, gate.device, owner[0].grant.path,
                          owner[0].node))
                continue
            if not owner:
                rows.append(Row(0, k, grant.base, grant.size, "rw", register, grant))
        return rows
    for k, grant in grants:
        if not any(fronted(unit, grant) for unit in gate.gates.values()):
            refuse(k, grant, "%s holds `%s` on node %d, which the ranges of no gate of the partition front, so no "
                             "node's kernel can keep it from the others" % (grant.task.label(), grant.path, k))
    for index, (name, unit) in enumerate(gate.gates.items()):
        gate_rows = []
        mine = [(k, grant) for k, grant in grants if fronted(unit, grant)]
        for k in range(1, nodes):
            if manifest.amp_slices is not None and manifest.amp_window is not None:
                slice_base = manifest.amp_slices[0] + k * manifest.amp_slices[1]
                for base, size, access in ((slice_base, manifest.amp_slices[1], "rwx"),
                                           (manifest.amp_window[0], manifest.amp_window[1], "rw")):
                    if any(m_base <= base and base + size <= m_base + m_size for m_base, m_size in unit.memory):
                        gate_rows.append(Row(index, k, base, size, access))
            for base, size, access in unit.kernel:
                gate_rows.append(Row(index, k, base, size, access))
        for k, grant in mine:
            gate_rows.append(Row(index, k, grant.base, grant.size, "rw", 0, grant))
        gate_rows = coalesce(gate_rows)
        budget = (unit.regions or 1) - 1
        if len(gate_rows) > budget:
            over = [row for row in gate_rows if row.grant is not None]
            for row in over:
                k = [n for n, grant in grants if grant is row.grant][0]
                refuse(k, row.grant, "%s holds `%s` on node %d, which coalesces with none of partition gate `%s`'s "
                                     "other rows, and the gate needs %d where its regions past its catch-all hold %d"
                       % (row.grant.task.label(), row.grant.path, k, name, len(gate_rows), budget))
            if not over:
                refuse(0, None, "partition gate `%s` needs %d region(s) for the kernel's own ranges, "
                                "and holds %d past its catch-all" % (name, len(gate_rows), budget))
        rows.extend(gate_rows)
    return rows


def partition_gate(files, admitted, manifest):
    """The partition's gate rows, each grant attributed to its node's composition."""
    grants = [(k, grant) for k, made in enumerate(admitted) for grant in mem_grants(made) if grant.space == "mem"]

    def refuse(k, grant, message):
        node = admitted[k].root
        if grant is not None:
            node = grant.node
        files[k].refuse(node, "partition.gate-budget", message)

    return derive_gate(admitted[0].chip, manifest, grants, manifest.amp_nodes or len(admitted), refuse)
