#!/usr/bin/env python3
"""Pins the sites that emit a counted refusal code (P19 I3-3b1, design 4.3).

The host counts a refusal by its code name, so a counted code may be emitted
only where the decision rests on fields the origin signed. For each counted
code this lists every `refuse(EpistemicRefusal::X)` in
agent_executor*.cpp and every `endObservation(kTurnResultX)` in compiler*.cpp
and epistemic_emit_*.cpp, and fails unless the sites match the table below,
which mirrors the table of design section 4.3. Adding a site for a counted
code, or counting a new code, is a design change: edit the table with it.

  tools/check-counted-sites.py [--root DIR]
"""
import argparse
import pathlib
import re
import sys

RUNTIME = pathlib.Path("apps/common/xanadu/vagent/runtime")

# code -> {file: number of sites}; the first four are the content codes of
# I3-2, the rest are rows 1 to 3 and A1/A2 of design 4.3.
PINNED = {
    "ObservationFrameMismatch": {"agent_executor.cpp": 1},
    "MissingVariance": {"agent_executor.cpp": 1},
    "InvalidVariance": {"agent_executor.cpp": 2},
    "InvalidValue": {"agent_executor.cpp": 2},
    "InvalidAcquisition": {"agent_executor_clock.cpp": 1},
    "ClockEpochRegression": {"compiler.cpp": 2},
    "ObservationTimeMismatch": {"agent_executor_clock.cpp": 1},
    "UncertainAcquisition": {"agent_executor_clock.cpp": 1},
}

COUNTED = re.compile(r'\{\s*"(\w+)"\s*,\s*true\s*\}')
CONSTANT = re.compile(r'string_view\s+(kTurnResult\w+)\s*=\s*"(\w+)"')
COMMENT = re.compile(r"//.*")


def sites(root: pathlib.Path):
    """Yield (code, file name, line number) for every emitting site."""
    runtime = root / RUNTIME
    turn_result = {}
    for header in runtime.glob("*.hpp"):
        turn_result.update(dict(CONSTANT.findall(header.read_text())))
    refuse = re.compile(r"\brefuse\(\s*EpistemicRefusal::(\w+)")
    end = re.compile(r"\bendObservation\(\s*(kTurnResult\w+)")
    for pattern, globs in ((refuse, ["agent_executor*.cpp"]),
                           (end, ["compiler*.cpp", "epistemic_emit_*.cpp"])):
        for glob in globs:
            for path in sorted(runtime.glob(glob)):
                text = COMMENT.sub("", path.read_text())
                for match in pattern.finditer(text):
                    line = text.count("\n", 0, match.start()) + 1
                    yield turn_result.get(match[1], match[1]), path.name, line


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=pathlib.Path,
                        default=pathlib.Path(__file__).resolve().parent.parent)
    root = parser.parse_args().root
    failures = []
    counted = set(COUNTED.findall((root / RUNTIME / "epistemic_health.hpp").read_text()))
    if counted != set(PINNED):
        failures.append(f"counted codes {sorted(counted)} differ from the pinned "
                        f"table {sorted(PINNED)}")
    found = {}
    for code, name, number in sites(root):
        if code in PINNED:
            found.setdefault(code, {}).setdefault(name, []).append(number)
    for code, want in sorted(PINNED.items()):
        have = found.get(code, {})
        for name in sorted(set(want) | set(have)):
            lines = have.get(name, [])
            if len(lines) != want.get(name, 0):
                failures.append(
                    f"{name}: counted code {code} has {len(lines)} site(s) "
                    f"(lines {lines}), the pinned table says {want.get(name, 0)}")
    for failure in failures:
        print(failure, file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
