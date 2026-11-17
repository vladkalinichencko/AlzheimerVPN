#!/usr/bin/env python3
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

FAIL_PATTERNS = (
    re.compile(r"is not a type"),
    re.compile(r"is not a component"),
    re.compile(r"is not a singleton"),
    re.compile(r"cannot resolve", re.IGNORECASE),
    re.compile(r"unknown type", re.IGNORECASE),
    re.compile(r"^Error:", re.MULTILINE),
)

STYLE_COLOR_RE = re.compile(r"AmneziaStyle\.color\.([A-Za-z_][A-Za-z0-9_]*)")
STYLE_DEF_RE = re.compile(r"readonly property color\s+([A-Za-z_][A-Za-z0-9_]*)")
NOT_FOUND_RE = re.compile(r":\d+:\d+: ([A-Za-z_][A-Za-z0-9_]*) was not found")

# Types provided by C++-registered QML modules or qmldir modules outside
# client/ui/qml/Modules, which qmllint cannot resolve.
KNOWN_UNRESOLVABLE_TYPES = {
    "AnyOf",
    "Gamepad",
    "GamepadKeyNavigation",
    "InstalledAppsModel",
    "PublicHostInputValidator",
    "QRCodeReader",
    "RegExpFilter",
    "ValueFilter",
}


def check_style_colors(qml_root: Path) -> list[str]:
    style_file = qml_root / "Modules" / "Style" / "AmneziaStyle.qml"
    defined = set(STYLE_DEF_RE.findall(style_file.read_text()))
    undefined = set()
    for path in qml_root.rglob("*.qml"):
        undefined.update(STYLE_COLOR_RE.findall(path.read_text(errors="ignore")))
    return sorted(undefined - defined)


def main() -> int:
    repo = Path(sys.argv[1]).resolve()
    qmllint = os.environ.get("QMLLINT") or shutil.which("qmllint") or shutil.which("qmllint6")
    if not qmllint:
        print("qmllint not found; skipping")
        return 0

    qml_root = repo / "client" / "ui" / "qml"
    modules = qml_root / "Modules"
    files = sorted(str(path) for path in qml_root.rglob("*.qml"))
    if not files:
        print("no QML files found", file=sys.stderr)
        return 1

    proc = subprocess.run(
        [qmllint, "-I", str(modules), *files],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    output = proc.stdout

    failures = [line for line in output.splitlines() if any(pattern.search(line) for pattern in FAIL_PATTERNS)]
    if failures:
        print("QML type resolution failures:")
        print("\n".join(failures[:200]))
        return 1

    missing_types = sorted(
        {match.group(1) for match in (NOT_FOUND_RE.search(line) for line in output.splitlines()) if match}
        - KNOWN_UNRESOLVABLE_TYPES
    )
    if missing_types:
        print("QML types not found by qmllint:")
        print("\n".join(missing_types))
        return 1

    missing_colors = check_style_colors(qml_root)
    if missing_colors:
        print("Undefined AmneziaStyle.color properties:")
        print("\n".join(missing_colors))
        return 1

    if proc.returncode != 0:
        print(output[-4000:])
        return proc.returncode

    print(f"qml types ok ({len(files)} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
