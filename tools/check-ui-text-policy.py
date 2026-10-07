#!/usr/bin/env python3
"""Reject byte ellipsizing and fixed Sans font descriptions in production UI."""

import re
import sys
from pathlib import Path

# Tokenize instead of scanning lines: comments and multiline expressions must
# not hide a violation, and examples inside comments are not executable code.
TOKENS = re.compile(
    r'//[^\n]*|/\*.*?\*/|(?:u8|u|U|L)?R"(?P<delimiter>[^ ()\\\t\r\n]{0,16})\(.*?\)(?P=delimiter)"'
    r'|(?:u8|u|U|L)?"(?:\\.|[^"\\])*"'
    r"|(?:u8|u|U|L)?'(?:\\.|[^'\\])*'|\w+|[^\s]",
    re.DOTALL,
)
FONT = re.compile(r"Sans(?:\s+\w+)*\s+\d+(?:\.\d+)?$")
ZERO = re.compile(r"(?:0+|0[xX]0+|0[bB]0+)(?:[uU](?:[lL]{1,2}|[zZ])?|(?:[lL]{1,2}|[zZ])[uU]?)?$")


def string_value(token):
    raw = re.fullmatch(r'(?:u8|u|U|L)?R"([^ ()\\\t\r\n]{0,16})\((.*?)\)\1"', token, re.DOTALL)
    if raw:
        return raw.group(2)
    literal = re.fullmatch(r'(?:u8|u|U|L)?"((?:\\.|[^"\\])*)"', token, re.DOTALL)
    if not literal:
        return None
    def escape(match):
        text = match.group()[1:]
        if text.startswith(("u", "U", "x")):
            try:
                return chr(int(text[1:], 16))
            except (ValueError, OverflowError):
                return match.group()
        if text[0] in "01234567":
            return chr(int(text, 8))
        return {"n": "\n", "t": "\t", "r": "\r", "\n": ""}.get(text, text)
    return re.sub(r"\\(?:u[0-9a-fA-F]{4}|U[0-9a-fA-F]{8}|x[0-9a-fA-F]+|[0-7]{1,3}|.)",
                  escape, literal.group(1), flags=re.DOTALL)


def tokenize(source):
    tokens = []
    for match in TOKENS.finditer(source):
        token = match.group()
        if token.startswith(("//", "/*")):
            continue
        value = string_value(token)
        # C++ concatenates adjacent string literals, even across comments.
        if value is not None and tokens and tokens[-1][2] is not None:
            previous, offset, decoded = tokens[-1]
            tokens[-1] = (previous + token, offset, decoded + value)
        else:
            tokens.append((token, match.start(), value))
    return tokens


UI_NAMES = {
    "form", "radial_menu", "grounding_modal", "media_widget", "toast",
    "doc_switcher", "images", "views", "xuzz_app", "zigzag_visualizer",
    "satelloid", "clasp_link_forge", "wireframe_hull", "pouch_drawer",
    "hypertime_graph", "store_object_manager", "floating_toolbar_3d",
    "world_card_presentation", "beams", "framing", "tenuous_tether",
}


def is_ui(path):
    return ("/ui/" in f"/{path.as_posix()}" or "overlay" in path.stem
            or path.stem.endswith(("_presentation", "_widget", "_panel", "_card"))
            or path.stem in UI_NAMES)


def violations(source, fonts=True):
    tokens = tokenize(source)
    findings = []
    for i, (token, offset, value) in enumerate(tokens):
        if fonts and value is not None and FONT.fullmatch(value):
            findings.append((offset, "use a typed typography role instead of a Sans size literal"))
        if (token != "substr" or i + 3 >= len(tokens)
                or tokens[i + 1][0] != "(" or not ZERO.fullmatch(tokens[i + 2][0])
                or tokens[i + 3][0] != ","):
            continue
        depth = 1
        j = i + 4
        while j < len(tokens) and depth:
            depth += (tokens[j][0] == "(") - (tokens[j][0] == ")")
            j += 1
        # Permit redundant parentheses around the substr expression.
        while j < len(tokens) and tokens[j][0] == ")":
            j += 1
        if j + 1 < len(tokens) and tokens[j][0] == "+" and tokens[j + 1][2] in {"...", "…"}:
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
