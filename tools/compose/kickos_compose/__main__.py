# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# python -m kickos_compose platform <file or directory>...
# python -m kickos_compose admit <composition>... --manifest <manifest> | --platform <platform directory>
# python -m kickos_compose manifest <manifest>...
# python -m kickos_compose emit <composition> --manifest <manifest> -o <file.c>
#
# Against a manifest, the chip and board files are the ones its `descriptions` names.

import argparse
import os
import sys

from .composition import admit
from .descriptions import check_platform
from .emit import emit
from .manifest import check_manifests


def finish(report, count, what, scope=""):
    refusals = sorted(report.refusals, key=lambda r: (r.path, r.line, r.rule))
    for refusal in refusals:
        print(refusal)
    if refusals:
        print("kickos_compose: %d refusal(s) in %d %s(s) read%s" % (len(refusals), count, what, scope))
        return 1
    print("kickos_compose: %d %s(s) read, none refused%s" % (count, what, scope))
    return 0


def main(argv):
    parser = argparse.ArgumentParser(prog="kickos_compose")
    commands = parser.add_subparsers(dest="command", required=True)
    platform = commands.add_parser("platform", help="check chip and board description files")
    platform.add_argument("paths", nargs="+", help="description files, or directories holding them")
    composition = commands.add_parser("admit", help="admit compositions against the platform descriptions")
    composition.add_argument("compositions", nargs="+", help="composition files")
    against = composition.add_mutually_exclusive_group(required=True)
    against.add_argument("--manifest", help="the export manifest of the kernel build they run on")
    against.add_argument("--platform", help="without a manifest, the directory holding platform/<chip>/")
    manifest = commands.add_parser("manifest", help="check export manifests and the descriptions they name")
    manifest.add_argument("manifests", nargs="+", help="manifest files")
    table = commands.add_parser("emit", help="admit a composition and emit its table as C")
    table.add_argument("composition", help="the composition file")
    table.add_argument("--manifest", required=True, help="the export manifest of the kernel build it runs on")
    table.add_argument("-o", dest="output", required=True, help="the C source to write")
    arguments = parser.parse_args(argv)

    if arguments.command in ("admit", "emit"):
        platform = getattr(arguments, "platform", None)
        if platform is not None and not os.path.isdir(platform):
            print("kickos_compose: --platform %s is no directory" % platform, file=sys.stderr)
            return 1
        if arguments.manifest is not None and not os.path.isfile(arguments.manifest):
            print("kickos_compose: --manifest %s is no file" % arguments.manifest, file=sys.stderr)
            return 1

    if arguments.command == "admit":
        report, count = admit(arguments.compositions, arguments.platform, arguments.manifest)
        scope = ""
        if arguments.manifest is None:
            scope = "; only the description rules ran, and a kernel build's need --manifest"
        return finish(report, count, "composition", scope)

    if arguments.command == "emit":
        # A refused composition leaves no table behind, so a build cannot link the last one.
        if os.path.exists(arguments.output):
            os.remove(arguments.output)
        report, text = emit(arguments.composition, arguments.manifest)
        if text is None and not report.refusals:
            print("kickos_compose: %s was not admitted" % arguments.composition, file=sys.stderr)
            return 1
        if text is None:
            return finish(report, 1, "composition")
        with open(arguments.output, "w", encoding="ascii", newline="\n") as stream:
            stream.write(text)
        return 0

    if arguments.command == "manifest":
        report, count = check_manifests(arguments.manifests)
        return finish(report, count, "manifest")

    report, count = check_platform(arguments.paths)
    if count == 0 and not report.refusals:
        print("kickos_compose: no description file under %s" % " ".join(arguments.paths), file=sys.stderr)
        return 1
    return finish(report, count, "description file")


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
