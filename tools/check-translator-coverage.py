#!/usr/bin/env python3
"""Source check for the P19 owned-structure registry (T1c).

Fails when a dimension the agent runtime creates, or a profile/observation
field constant its ABI headers declare, has no entry in the registry
(apps/common/xanadu/vagent/runtime/epistemic_translate_registry.cpp). It reads
sources only, so it needs no test output and a filtered gtest run cannot make
it pass or fail. The parity tests cover what a test can see; this covers what
only the source shows.

  tools/check-translator-coverage.py
  tools/check-translator-coverage.py --registry R.cpp --abi A.hpp... --sources S.cpp...
"""
import argparse
import pathlib
import re
import sys

RUNTIME = pathlib.Path("apps/common/xanadu/vagent/runtime")

# Constants whose prefix says "field" but whose value is a field's content.
VALUE_CONSTANTS = re.compile(
    r"^k(ObsStatus(Current|Retired)|ObsVerdictUntested"
    r"|ObsHealth(Nominal|Faulted|Suspect|Degraded)"
    r"|Reason(Kind|Origin)[A-Z]\w*|RetiredBy[A-Z]\w*)$"
)
FIELD_PREFIXES = ("kProfile", "kObs", "kReason", "kAction")
CONSTANT = re.compile(r"inline\s+constexpr\s+std::string_view\s+(k\w+)\s*=")
DIMENSION = re.compile(r'ensureDimension\("(d\.[\w.]+)"\)')
LITERAL = re.compile(r'"(d\.[\w.]+)"')


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--registry", type=pathlib.Path,
                        default=RUNTIME / "epistemic_translate_registry.cpp")
    parser.add_argument("--abi", type=pathlib.Path, nargs="*")
    parser.add_argument("--sources", type=pathlib.Path, nargs="*")
    args = parser.parse_args()
    abi = args.abi or sorted(RUNTIME.glob("epistemic_abi_*.hpp")) + [RUNTIME / "runtime_abi.hpp"]
    sources = args.sources or sorted(RUNTIME.glob("*.[ch]pp"))

    registry = args.registry.read_text()
    registered = set(LITERAL.findall(registry))
    failures = []

    for path in sources:
        for dim in sorted(set(DIMENSION.findall(path.read_text()))):
            if dim not in registered:
                failures.append(f"{path}: dimension {dim} has no registry entry")

    for path in abi:
        for name in sorted(set(CONSTANT.findall(path.read_text()))):
            if not name.startswith(FIELD_PREFIXES) or VALUE_CONSTANTS.match(name):
                continue
            if not re.search(rf"\b{name}\b", registry):
                failures.append(f"{path}: field constant {name} is in no registry entry")

    for failure in failures:
        print(failure, file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
