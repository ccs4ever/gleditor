#!/usr/bin/env python3
"""Check generated Doxygen XML against authored first-party source files.

This checks indexed files and representative symbols, not prose completeness or
an exhaustive compiler AST. Doxygen still selects preprocessor branches.
"""

import argparse
from collections import Counter
import fnmatch
import json
from pathlib import Path
import sys
import xml.etree.ElementTree as ET


SOURCE_ROOTS = ("include", "src", "apps", "tools", "tests", "assets/shaders")
SOURCE_PATTERNS = (
    "*.c", "*.cc", "*.cxx", "*.cpp", "*.cxxm", "*.cppm", "*.h", "*.hh",
    "*.hxx", "*.hpp", "*.ipp", "*.inl", "*.inc", "*.h.in", "*.py", "*.pyw",
    "*.glsl",
)
IGNORED_PARTS = {".git", ".venv", "__pycache__"}
REQUIRED_COMPOUNDS = (
    "Doc", "gleditor::Application", "gleditor::text::TextFit",
    "gleditor::text::FittedText", "xanadu::Store", "zigzag::ArenaManifold",
    "zigzag::vortex::VortexVM", "xanadu::vagent::Parser",
    "gleditor::anonymous_namespace{decode_index.cpp}::MemoryFlacDecoder",
    "anonymous_namespace{text_source.cpp}::PdfImageExtractor",
)
REQUIRED_MEMBERS = (
    ("gleditor::logging", "category"),
    ("gleditor::cpp26::views", "concat"),

)
REQUIRED_FUNCTIONS = (
    ("gleditor::anonymous_namespace{decode_index.cpp}", "buildJpegIndex"),
    ("gleditor::anonymous_namespace{decode_index.cpp}", "buildAvIndex"),
    ("gleditor::anonymous_namespace{decode_index.cpp}", "buildZstdIndex"),
    ("gleditor::anonymous_namespace{decode_index.cpp}", "buildFlacIndex"),
    ("gleditor::anonymous_namespace{decode_index.cpp}", "buildTiffIndex"),
    ("gleditor::anonymous_namespace{decode_index.cpp}", "buildGifIndex"),
)


def expected_files(root):
    files = set()
    for directory in SOURCE_ROOTS:
        for path in (root / directory).rglob("*"):
            relative = path.relative_to(root)
            if (path.is_file() and not path.is_symlink()
                    and not IGNORED_PARTS.intersection(relative.parts)
                    and any(fnmatch.fnmatchcase(path.name, p) for p in SOURCE_PATTERNS)):
                files.add(relative.as_posix())
    return files


def relative_source(name, root):
    path = Path(name)
    path = path.resolve() if path.is_absolute() else (root / path).resolve()
    try:
        return path.relative_to(root).as_posix()
    except ValueError:
        return None


def inspect_xml(root, directory):
    index = ET.parse(directory / "index.xml").getroot()
    compounds = index.findall("compound")
    names = {c.findtext("name", "") for c in compounds}
    member_pairs = {
        (c.findtext("name", ""), m.findtext("name", ""))
        for c in compounds for m in c.findall("member")
    }
    function_pairs = {
        (c.findtext("name", ""), m.findtext("name", ""))
        for c in compounds for m in c.findall("member")
        if m.attrib["kind"] == "function"
    }
    members = {
        m.attrib["refid"]: m.attrib["kind"]
        for c in compounds for m in c.findall("member")
    }
    indexed_files = set()
    file_members = {}
    shader_mains = set()
    for compound in compounds:
        if compound.attrib["kind"] != "file":
            continue
        xml_file = (directory / (compound.attrib["refid"] + ".xml")).resolve()
        if not xml_file.is_relative_to(directory):
            raise ValueError("file XML reference escapes the generated directory")
        location = ET.parse(xml_file).find("compounddef/location")
        if location is None or "file" not in location.attrib:
            raise ValueError(f"missing source location in {xml_file}")
        source = relative_source(location.attrib["file"], root)
        if source is not None:
            indexed_files.add(source)
            file_members[source] = {
                (m.attrib["kind"], m.findtext("name", ""))
                for m in compound.findall("member")
            }
            if source.endswith(".glsl") and any(
                m.attrib["kind"] == "function" and m.findtext("name") == "main"
                for m in compound.findall("member")
            ):
                shader_mains.add(source)
    missing_symbols = [name for name in REQUIRED_COMPOUNDS if name not in names]
    missing_symbols += [
        scope + "::" + name for scope, name in REQUIRED_MEMBERS
        if (scope, name) not in member_pairs
    ]
    missing_symbols += [
        scope + "::" + name + " (function)" for scope, name in REQUIRED_FUNCTIONS
        if (scope, name) not in function_pairs
    ]
    checker_names = {
        name for name in names if "anonymous_namespace" in name
        and "vagent" in name and name.endswith("::Checker")
    }
    if not checker_names:
        missing_symbols.append("vagent anonymous-namespace Checker")
    if not any((name, "preflight") in member_pairs for name in checker_names):
        missing_symbols.append("vagent Checker private preflight member")
    for source, kind, name in [
        ("src/config.h.in", "define", "GLEDITOR_VERSION"),
        ("assets/shaders/glyph.vert.glsl", "variable", "position"),
    ]:
        if (kind, name) not in file_members.get(source, set()):
            missing_symbols.append(source + ": " + name)
    return {
        "doxygen_version": index.attrib.get("version"),
        "indexed_files": sorted(indexed_files),
        "compound_counts": dict(sorted(Counter(c.attrib["kind"] for c in compounds).items())),
        "unique_member_counts": dict(sorted(Counter(members.values()).items())),
        "unique_members": len(members),
        "missing_symbol_probes": missing_symbols,
        "shader_mains": sorted(shader_mains),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--xml", type=Path, default=Path("docs/xml"))
    parser.add_argument("--report", type=Path, default=Path("docs/coverage.json"))
    args = parser.parse_args()
    root = args.root.resolve()
    directory = (root / args.xml).resolve()
    report_path = (root / args.report).resolve()
    expected = expected_files(root)
    try:
        result = inspect_xml(root, directory)
    except (OSError, ET.ParseError, ValueError, KeyError) as error:
        print(f"Doxygen coverage check failed: {error}", file=sys.stderr)
        return 1
    actual = set(result["indexed_files"])
    result["expected_source_files"] = len(expected)
    result["indexed_source_files"] = len(expected & actual)
    result["missing_source_files"] = sorted(expected - actual)
    result["missing_shader_mains"] = sorted(
        p for p in expected if p.endswith(".glsl") and p not in result["shader_mains"]
    )
    result["scope"] = list(SOURCE_ROOTS)
    result["limitations"] = (
        "Source-file coverage and representative symbol probes only; not an exhaustive "
        "compiler AST, all conditional build variants, or prose-documentation coverage."
    )
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(result, indent=2) + "\n")
    print(f"Doxygen indexed {result['indexed_source_files']}/{len(expected)} authored source files; "
          f"{result['unique_members']} unique member entries.")
    failures = result["missing_source_files"] + result["missing_symbol_probes"] + result["missing_shader_mains"]
    for missing in failures:
        print(f"  missing: {missing}", file=sys.stderr)
    print(f"Coverage report: {report_path}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
