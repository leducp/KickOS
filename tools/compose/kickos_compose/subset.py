# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The YAML subset every description file is written in. A directive, an anchor, an alias, a tag,
# a merge key and a duplicate key are refused from the token and event streams, so a second
# document or an alias, which leave no tree to walk, hide none of them.

import re

from ruamel.yaml import YAML
from ruamel.yaml.error import YAMLError
from ruamel.yaml.events import (
    AliasEvent,
    DocumentStartEvent,
    MappingEndEvent,
    MappingStartEvent,
    ScalarEvent,
    SequenceEndEvent,
    SequenceStartEvent,
)
from ruamel.yaml.nodes import MappingNode, ScalarNode, SequenceNode
from ruamel.yaml.tokens import AliasToken, AnchorToken, DirectiveToken, TagToken

RULES = (
    "form.syntax", "form.documents", "form.directive", "form.tag", "form.anchor", "form.alias",
    "form.merge-key", "form.duplicate-key", "form.unknown-field", "form.missing", "form.version",
    "form.type", "form.boolean", "form.integer", "form.path", "form.name", "form.enum",
    "form.exclusive", "form.inapplicable", "form.range", "form.layout", "form.unreadable",
    "chip.name-mismatch", "chip.name-collision", "chip.overlap", "chip.cluster-unknown",
    "chip.kernel-window", "chip.memory-required", "chip.arena", "chip.page-unit", "chip.page-size",
    "chip.core-count", "chip.zero-size", "chip.device-unknown", "chip.register-outside", "chip.gate-straddle",
    "chip.block-outside", "chip.line-range", "chip.link-duplicate", "chip.symbol-collision", "chip.reserved",
    "chip.address-width", "form.text",
    "board.name-mismatch", "board.name-collision", "board.chip-unknown", "board.chip-unreadable",
    "board.chip-folder", "board.device-unknown",
    "board.pin-unknown", "board.pin-function", "board.pin-not-gpio", "board.reserved-pin-used",
    "board.memory-overlap", "board.link-duplicate", "board.symbol-collision", "board.pin-reused",
    "board.pin-signal",
    "form.duplicate-entry", "form.scope",
    "name.board-unknown", "name.board-ambiguous", "name.cluster-unknown", "name.device-unknown",
    "name.line-unknown", "name.namespace", "name.reserved", "name.duplicate",
    "order.undeclared", "order.forward",
    "ownership.device", "ownership.line", "ownership.gate", "ownership.kernel", "ownership.console",
    "encoding.window", "encoding.budget", "encoding.page", "encoding.page-shared", "encoding.table",
    "encoding.watches", "restart.ends", "restart.no-isolation",
    "enforcement.no-protection", "enforcement.no-privilege-split", "enforcement.device-not-isolated",
    "enforcement.coarse-gate", "enforcement.port-bank", "enforcement.bus-master", "enforcement.unneeded",
    "memory.uncached", "memory.cached-incoherent",
    "name.driver-unknown", "name.entry", "driver.line-role", "driver.window-role", "driver.port-window",
    "driver.authority",
    "scheduling.priority", "scheduling.core", "scheduling.line-core", "scheduling.stdout-priority",
    "scheduling.stdout-order", "scheduling.console-driver", "scheduling.init-priority-range",
    "supply.pool", "supply.budget", "supply.spawn-grants", "supply.cap-table", "supply.stack", "supply.arena",
    "supply.size", "supply.init-windows", "supply.reservations", "supply.ranges",
    "manifest.cores", "manifest.amp", "manifest.window", "manifest.priority", "manifest.barrier",
    "manifest.description-path", "manifest.description-unknown", "manifest.default-path", "manifest.default-unknown",
    "manifest.bound", "manifest.console",
    "manifest.target", "manifest.receiver",
)

HEX = re.compile(r"0x[0-9A-Fa-f]+")
DECIMAL = re.compile(r"0|[1-9][0-9]*")
NUMERIC_LOOKING = re.compile(r"[-+.]?[0-9]")
YAML11_BOOLEANS = ("y", "yes", "n", "no", "on", "off", "true", "false")
NULLS = ("", "~", "null", "Null", "NULL")
# Python refuses to convert a decimal string past 4300 digits, and 2**64 has 20.
DECIMAL_DIGITS = 20

IDENTIFIER = re.compile(r"[a-z][a-z0-9_]*")
C_IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
NAMESPACE = re.compile(r"kickos(::[a-z][a-z0-9_]*)+")
LINE_ENUM = re.compile(r"[a-z_][a-z0-9_]*( : [a-z_][a-z0-9_]*( [a-z_][a-z0-9_]*)*)?")
REGION = re.compile(r"[A-Z][A-Z0-9_]*")
ACCESS = re.compile(r"(?=.)r?w?x?")
FILE_NAME = re.compile(r"[a-z0-9][a-z0-9_-]*")
PIN = re.compile(r"[A-Z][A-Z0-9_]*(\.[0-9]+)?")
SELECTOR = re.compile(r"[a-z][a-z0-9]*")
FUNCTION = re.compile(r"[a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*)+")
GPIO_FUNCTION = re.compile(r"[a-z][a-z0-9_]*(\.(0|[1-9][0-9]*)){1,2}")


def index_below(text, count):
    """The decimal `text` as an index below `count`, or None."""
    if not DECIMAL.fullmatch(text) or len(text) > DECIMAL_DIGITS:
        return None
    value = int(text, 10)
    if value >= count:
        return None
    return value


class Refusal:
    def __init__(self, path, line, rule, message):
        self.path = path
        self.line = line
        self.rule = rule
        self.message = message

    def __str__(self):
        return "%s:%d: %s: %s" % (self.path, self.line, self.rule, self.message)


class Report:
    def __init__(self):
        self.refusals = []

    def refuse(self, path, line, rule, message):
        if rule not in RULES:
            raise ValueError("refusal under the unlisted rule %s" % rule)
        self.refusals.append(Refusal(path, line, rule, message))


def line_of(node):
    return node.start_mark.line + 1


def kind_name(node):
    if isinstance(node, MappingNode):
        return "a mapping"
    if isinstance(node, SequenceNode):
        return "a list"
    if node.style is not None:
        return "the quoted string `%s`" % node.value
    if node.value in NULLS:
        return "an empty value"
    return "`%s`" % node.value


class File:
    def __init__(self, filename, report):
        self.filename = filename
        self.report = report

    def refuse(self, node_or_line, rule, message):
        line = node_or_line
        if not isinstance(node_or_line, int):
            line = line_of(node_or_line)
        self.report.refuse(self.filename, line, rule, message)

    def load(self, text):
        """The root node of the one document, or None once refused."""
        yaml = YAML(typ="rt", pure=True)
        try:
            tokens = list(yaml.scan(text))
            events = list(yaml.parse(text))
        except YAMLError as error:
            self.refuse(error_line(error), "form.syntax", error_problem(error))
            return None
        has_alias = False
        for token in tokens:
            line = token.start_mark.line + 1
            if isinstance(token, DirectiveToken):
                self.refuse(line, "form.directive", "the directive `%%%s`; the subset has none" % token.name)
            if isinstance(token, AnchorToken):
                self.refuse(line, "form.anchor", "the anchor `&%s`; the subset has none" % token.value)
            if isinstance(token, AliasToken):
                has_alias = True
                self.refuse(line, "form.alias", "the alias `*%s`; write the value out" % token.value)
            if isinstance(token, TagToken):
                self.refuse(line, "form.tag", "an explicit tag; the schema types every scalar")
        documents = 0
        for event in events:
            if isinstance(event, DocumentStartEvent):
                documents = documents + 1
                if documents == 2:
                    self.refuse(event.start_mark.line + 1, "form.documents", "a second document; a file holds one")
        if documents == 0:
            self.refuse(1, "form.documents", "no document")
        self.refuse_keys(events)
        if documents != 1 or has_alias:
            return None
        return yaml.compose(text)

    def refuse_keys(self, events):
        """Merge keys and duplicate keys in every document. A schema walk skips both silently."""
        frames = []
        for event in events:
            if isinstance(event, (MappingEndEvent, SequenceEndEvent)):
                frames.pop()
                continue
            if not isinstance(event, (ScalarEvent, AliasEvent, MappingStartEvent, SequenceStartEvent)):
                continue
            frame = None
            if frames:
                frame = frames[-1]
            if frame is not None:
                if frame["at_key"] and isinstance(event, ScalarEvent):
                    self.refuse_key(event, frame["keys"])
                frame["at_key"] = not frame["at_key"]
            if isinstance(event, MappingStartEvent):
                frames.append({"at_key": True, "keys": {}})
            if isinstance(event, SequenceStartEvent):
                frames.append(None)

    def refuse_key(self, event, keys):
        line = event.start_mark.line + 1
        if event.style is None and event.value == "<<":
            self.refuse(line, "form.merge-key", "a merge key; write the fields out")
        elif event.value in keys:
            self.refuse(line, "form.duplicate-key",
                        "`%s` appears twice in its mapping, first on line %d" % (event.value, keys[event.value]))
        else:
            keys[event.value] = line

    def mapping(self, node, what):
        """The mapping's pairs as {key: (key node, value node)}, in file order, or None."""
        if not isinstance(node, MappingNode):
            self.refuse(node, "form.type", "%s must be a mapping, not %s" % (what, kind_name(node)))
            return None
        pairs = {}
        for key, value in node.value:
            if not isinstance(key, ScalarNode):
                self.refuse(key, "form.type", "a key in %s must be a plain name" % what)
                continue
            if key.style is None and key.value == "<<" or key.value in pairs:
                continue
            pairs[key.value] = (key, value)
        return pairs

    def fields(self, node, what, allowed, required):
        pairs = self.mapping(node, what)
        if pairs is None:
            return None
        return self.select(pairs, node, what, allowed, required)

    def select(self, pairs, node, what, allowed, required):
        """The values of `pairs` by field name, refusing a field outside `allowed`."""
        values = {}
        for name, (key, value) in pairs.items():
            if name not in allowed:
                self.refuse(key, "form.unknown-field", "%s has no field `%s`" % (what, name))
                continue
            values[name] = value
        for name in required:
            if name not in values:
                self.refuse(node, "form.missing", "%s needs `%s`" % (what, name))
        return values

    def sequence(self, node, what):
        if not isinstance(node, SequenceNode):
            self.refuse(node, "form.type", "%s must be a list, not %s" % (what, kind_name(node)))
            return None
        return node.value

    def scalar(self, node, what, expected):
        if not isinstance(node, ScalarNode):
            self.refuse(node, "form.type", "%s must be %s, not %s" % (what, expected, kind_name(node)))
            return None
        return node

    def integer(self, node, what, bits):
        """The integer, refused above the `bits`-wide field the emitted table carries it in."""
        node = self.scalar(node, what, "an integer")
        if node is None:
            return None
        text = node.value
        if node.style is not None:
            self.refuse(node, "form.type", "%s must be an integer, not %s" % (what, kind_name(node)))
            return None
        value = None
        wide = False
        if HEX.fullmatch(text):
            value = int(text, 16)
        elif DECIMAL.fullmatch(text) and len(text) > DECIMAL_DIGITS:
            wide = True
        elif DECIMAL.fullmatch(text):
            value = int(text, 10)
        if wide or value is not None and value >> bits:
            shown = text
            if len(text) > 2 * DECIMAL_DIGITS:
                shown = "%s... (%d digits)" % (text[:DECIMAL_DIGITS], len(text))
            self.refuse(node, "form.range", "%s is %s, wider than the %d bits it is carried in" % (what, shown, bits))
            return None
        if value is not None:
            return value
        if NUMERIC_LOOKING.match(text):
            self.refuse(node, "form.integer",
                        "%s is `%s`; an integer is written in hex (0x...) or decimal" % (what, text))
            return None
        self.refuse(node, "form.type", "%s must be an integer, not %s" % (what, kind_name(node)))
        return None

    def boolean(self, node, what):
        node = self.scalar(node, what, "true or false")
        if node is None:
            return None
        if node.style is None and node.value == "true":
            return True
        if node.style is None and node.value == "false":
            return False
        if node.style is None and node.value.lower() in YAML11_BOOLEANS:
            self.refuse(node, "form.boolean",
                        "%s is `%s`; a boolean is written `true` or `false`" % (what, node.value))
            return None
        self.refuse(node, "form.type", "%s must be true or false, not %s" % (what, kind_name(node)))
        return None

    def string(self, node, what):
        node = self.scalar(node, what, "a string")
        if node is None:
            return None
        if node.style is None and node.value in NULLS:
            self.refuse(node, "form.type", "%s must be a string, not %s" % (what, kind_name(node)))
            return None
        return node.value

    def name(self, node, what, pattern):
        text = self.string(node, what)
        if text is None:
            return None
        if not pattern.fullmatch(text):
            self.refuse(node, "form.name", "%s `%s` is not a name of the form %s" % (what, text, pattern.pattern))
            return None
        return text

    def path(self, node, what):
        text = self.string(node, what)
        if text is None:
            return None
        if not text.startswith("/"):
            self.refuse(node, "form.path", "%s `%s` is a path, which begins with `/`" % (what, text))
            return None
        return text

    def enum(self, node, what, values):
        text = self.string(node, what)
        if text is None:
            return None
        if text not in values:
            self.refuse(node, "form.enum", "%s `%s` is not one of %s" % (what, text, ", ".join(values)))
            return None
        return text

    def pair(self, node, what, first, second, bits):
        """Two integers, `bits` giving each one's width."""
        items = self.sequence(node, what)
        if items is None:
            return None
        if len(items) != 2:
            self.refuse(node, "form.type", "%s must be `[%s, %s]`" % (what, first, second))
            return None
        a = self.integer(items[0], "%s %s" % (what, first), bits[0])
        b = self.integer(items[1], "%s %s" % (what, second), bits[1])
        if a is None or b is None:
            return None
        return (a, b)

    def version(self, values, what, known):
        """Whether `values` states a `version` among `known`, the versions of its format this tool reads."""
        if "version" not in values:
            return False
        version = self.integer(values["version"], "`version`", 16)
        if version is None:
            return False
        if version not in known:
            self.refuse(values["version"], "form.version", "%s version %d is not one this tool reads (%s)"
                        % (what, version, ", ".join(str(v) for v in known)))
            return False
        return True


def error_line(error):
    mark = getattr(error, "problem_mark", None)
    if mark is None:
        mark = getattr(error, "context_mark", None)
    if mark is None:
        return 1
    return mark.line + 1


def error_problem(error):
    problem = getattr(error, "problem", None)
    if problem:
        return problem
    return str(error).splitlines()[0]
