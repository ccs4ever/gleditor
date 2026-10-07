#!/usr/bin/env python3
"""Reject byte ellipsizing and fixed Sans font descriptions in production UI."""

import re
import sys
from pathlib import Path

# Tokenize instead of scanning lines: comments and multiline expressions must
# not hide a violation, and examples inside comments are not executable code.
TOKENS = re.compile(
    r'//[^\n]*|/\*.*?\*/|R"(?P<delimiter>[^ ()\\\t\r\n]{0,16})\(.*?\)(?P=delimiter)"'
    r'|(?:u8|u|U|L)?"(?:\\.|[^"\\])*"|\w+|[^\s]',
    re.DOTALL,
)
FONT = re.compile(r'(?:u8|u|U|L)?"Sans(?:\s+\w+)*\s+\d+(?:\.\d+)?"$')
ELLIPSIS = {'"..."', '"…"', 'u8"…"', '"\\u2026"'}
UI_NAMES = {
    "form", "radial_menu", "grounding_modal", "media_widget", "toast",
    "doc_switcher", "images", "views", "xuzz_app", "zigzag_visualizer",
    "satelloid", "clasp_link_forge", "wireframe_hull", "pouch_drawer",
    "hypertime_graph", "store_object_manager",
}


def is_ui(path):
    return ("/ui/" in f"/{path.as_posix()}" or "overlay" in path.stem
            or path.stem in UI_NAMES)


def violations(source, fonts=True):
    tokens = [(m.group(), m.start()) for m in TOKENS.finditer(source)
              if not m.group().startswith(("//", "/*"))]
    findings = []
    for i, (token, offset) in enumerate(tokens):
        if fonts and FONT.fullmatch(token):
            findings.append((offset, "use a typed typography role instead of a Sans size literal"))
        if token != "substr" or [v[0] for v in tokens[i + 1:i + 4]] != ["(", "0", ","]:
            continue
        depth = 1
        j = i + 4
        while j < len(tokens) and depth:
            depth += (tokens[j][0] == "(") - (tokens[j][0] == ")")
            j += 1
        # Permit redundant parentheses around the substr expression.
        while j < len(tokens) and tokens[j][0] == ")":
            j += 1
        if j + 1 < len(tokens) and tokens[j][0] == "+" and tokens[j + 1][0] in ELLIPSIS:
            findings.append((offset, "use TextFit instead of byte truncation with an ellipsis"))
    return [(source.count("\n", 0, offset) + 1, message)
            for offset, message in findings]


def main():
    root = Path(__file__).resolve().parent.parent
    paths = [Path(arg) for arg in sys.argv[1:]] if len(sys.argv) > 1 else [
        path for base in ("include", "src", "apps")
        for path in (root / base).rglob("*")
        if path.suffix in (".cpp", ".hpp", ".h")
    ]
    failed = False
    for path in sorted(paths):
        relative = path.relative_to(root) if path.is_relative_to(root) else path
        for line, message in violations(path.read_text(), is_ui(relative)):
            print(f"{relative}:{line}: {message}")
            failed = True
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
