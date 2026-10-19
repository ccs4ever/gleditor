#!/usr/bin/env python3
"""Replay one curated UI/native/PTY stage with isolated, retained evidence.

A stage replay is diagnostic evidence, not journey acceptance.
"""

from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time
from typing import Any

REPO = Path(__file__).resolve().parent.parent
JOURNEYS = REPO / "tools" / "ux-journeys"
BUILTINS = {"repo", "evidence", "profile"}
TEMPLATE = re.compile(r"\$\{([^}]+)\}")
HEADLESS = {
    "SDL_VIDEODRIVER": "offscreen",
    "SDL_AUDIODRIVER": "dummy",
    "LIBGL_ALWAYS_SOFTWARE": "1",
}


class PlanError(ValueError):
    pass


def journey_entries(path: Path) -> tuple[str, list[dict[str, Any]]]:
    data = json.loads(path.read_text(encoding="utf-8"))
    # Catalogues currently use arrays, single stage envelopes, and grouped
    # journey objects whose stages are envelopes.
    if isinstance(data, list):
        entries = data
        name = entries[0].get("journey", path.stem) if entries else path.stem
    elif isinstance(data, dict) and isinstance(data.get("stages"), list):
        entries, name = data["stages"], data.get("journey", path.stem)
    elif isinstance(data, dict) and "stage" in data:
        entries, name = [data], data.get("journey", path.parent.name if path.name == "stage.json" else path.stem)
    else:
        raise PlanError("top-level JSON must be a stage envelope, stage array, or grouped stage array")
    if not entries or any(not isinstance(item, dict) for item in entries):
        raise PlanError("journey must contain at least one stage envelope")
    return str(name), entries


def walk_strings(value: Any, location: str = "plan"):
    if isinstance(value, str):
        yield location, value
    elif isinstance(value, list):
        for index, item in enumerate(value):
            yield from walk_strings(item, f"{location}[{index}]")
    elif isinstance(value, dict):
        for key, item in value.items():
            yield from walk_strings(item, f"{location}.{key}")


def validate_stage(stage: dict[str, Any], path: Path, journey_variables: set[str] | None = None) -> list[str]:
    errors: list[str] = []
    required = {"journey", "stage", "driver", "program", "profile", "source_evidence", "notes", "plan", "variables"}
    missing = required - stage.keys()
    if missing:
        return [f"missing fields: {', '.join(sorted(missing))}"]
    if stage["program"] not in {"xuzz", "vquery", "vpl", "vprolog"}:
        errors.append("program must be xuzz, vquery, vpl, or vprolog")
    for key in ("journey", "stage", "profile"):
        if not isinstance(stage[key], str) or not re.fullmatch(r"[A-Za-z0-9_-]+", stage[key]):
            errors.append(f"{key} must be a simple directory key")
    if stage["driver"] not in {"ui", "native", "pty"}:
        errors.append("driver must be ui, native, or pty")
    if not isinstance(stage["profile"], str) or not stage["profile"]:
        errors.append("profile must be a non-empty group key")
    if not isinstance(stage["source_evidence"], str) or Path(stage["source_evidence"]).is_absolute():
        errors.append("source_evidence must be a repository-relative path")
    if not isinstance(stage["notes"], list) or any(not isinstance(note, str) for note in stage["notes"]):
        errors.append("notes must be an array of strings")
    variables = stage["variables"]
    if not isinstance(variables, dict):
        errors.append("variables must be an object")
        variables = {}
    for name, spec in variables.items():
        if not isinstance(spec, dict) or not isinstance(spec.get("description"), str) or not isinstance(spec.get("example"), str):
            errors.append(f"variable {name!r} needs description and example")
        elif "default" in spec:
            errors.append(f"variable {name!r} must not define a default")
    plan = stage["plan"]
    if not isinstance(plan, dict):
        errors.append("plan must be an object")
        return errors
    if stage["driver"] == "ui":
        for key in ("steps", "launch_args"):
            if key in plan and (not isinstance(plan[key], list) or any(not isinstance(token, str) for token in plan[key])):
                errors.append(f"UI plan {key} must be an array of whole strings")
        if not isinstance(plan.get("steps"), list):
            errors.append("UI plan needs a flat argv steps array")
    elif stage["driver"] == "native":
        if not isinstance(plan.get("launch_args", []), list) or any(not isinstance(token, str) for token in plan.get("launch_args", [])):
            errors.append("native launch_args must be an array of whole strings")
        if not isinstance(plan.get("steps"), list) or any(not isinstance(step, dict) for step in plan.get("steps", [])):
            errors.append("native steps must be an array of operation objects")
    else:
        if not isinstance(plan.get("launch_args", []), list) or any(not isinstance(token, str) for token in plan.get("launch_args", [])):
            errors.append("PTY launch_args must be an array of whole strings")
        if not isinstance(plan.get("steps"), list):
            errors.append("PTY steps must be an array")
        else:
            for i, step in enumerate(plan["steps"]):
                if not isinstance(step, dict) or not isinstance(step.get("line"), str):
                    errors.append(f"PTY step {i} needs one whole-string line")
                elif "contains" in step and (not isinstance(step["contains"], list) or any(not isinstance(s, str) for s in step["contains"])):
                    errors.append(f"PTY step {i} contains must be an array of whole strings")
    if stage["driver"] in {"ui", "native"}:
        argv = list(plan.get("launch_args", []))
        if stage["driver"] == "ui":
            argv += plan.get("steps", [])
        for index, token in enumerate(argv[:-1]):
            if token == "--capture":
                target = argv[index + 1]
                if not (target.startswith("${evidence}/") or target.startswith("${profile}/")
                        or target.startswith("{evidence}/") or target.startswith("{profile}/")):
                    errors.append(f"capture path must be rooted at ${{evidence}} or ${{profile}}: {target}")
    declared = set(variables) | (journey_variables or set()) | BUILTINS
    for where, value in walk_strings(plan):
        if re.search(r"/(?:[^\s\"']*/)?build/ux-green-20261011/", value):
            errors.append(f"old absolute build evidence path at {where}")
        for match in TEMPLATE.finditer(value):
            name = match.group(1)
            if name not in declared:
                errors.append(f"undeclared template ${{{name}}} at {where}")
    return errors


def load_catalog() -> dict[str, list[dict[str, Any]]]:
    catalog: dict[str, list[dict[str, Any]]] = {}
    sources: dict[str, list[Path]] = {}
    for path in sorted(JOURNEYS.rglob("*.json")):
        journey, entries = journey_entries(path)
        stages = catalog.setdefault(journey, [])
        names = {stage.get("stage") for stage in stages}
        for stage in entries:
            name = stage.get("stage")
            if not isinstance(name, str) or not name:
                raise PlanError(f"{path.name}: stage name missing")
            if name in names:
                raise PlanError(f"duplicate stage {journey}/{name} in {path}")
            names.add(name)
            stages.append(stage)
            sources.setdefault(journey, []).append(path)
    for journey, stages in catalog.items():
        journey_variables = set().union(*(set(stage.get("variables", {})) for stage in stages))
        for stage, path in zip(stages, sources[journey]):
            errors = validate_stage(stage, path, journey_variables)
            if errors:
                raise PlanError(f"{path.name}/{stage['stage']}: " + "; ".join(errors))
    return catalog


def replace_templates(value: Any, mapping: dict[str, str]) -> Any:
    if isinstance(value, str):
        def substitute(match: re.Match[str]) -> str:
            name = match.group(1)
            if name not in mapping:
                raise PlanError(f"missing required variable: {name}")
            return mapping[name]
        expanded = TEMPLATE.sub(substitute, value)
        return expanded.replace("{evidence}", mapping["evidence"]).replace(
            "{profile}", mapping.get("profile", "{profile}")
        )
    if isinstance(value, list):
        return [replace_templates(item, mapping) for item in value]
    if isinstance(value, dict):
        return {key: replace_templates(item, mapping) for key, item in value.items()}
    return value


def validate_capture_paths(driver: str, plan: dict[str, Any], evidence: Path, profile: Path) -> None:
    argv = list(plan.get("launch_args", []))
    if driver == "ui":
        argv += plan.get("steps", [])
    for index, token in enumerate(argv[:-1]):
        if token not in {"--capture", "--capture-live"}:
            continue
        value = argv[index + 1]
        if token == "--capture-live":
            if "," not in value:
                raise PlanError("--capture-live requires MS,PATH")
            value = value.split(",", 1)[1]
        target = Path(value).resolve()
        if not target.is_relative_to(evidence) and not target.is_relative_to(profile):
            raise PlanError(f"capture output must be under fresh evidence or profile: {target}")


def write_json(path: Path, data: Any) -> None:
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument("--list", action="store_true", help="list journeys and stages")
    modes.add_argument("--check", action="store_true", help="validate every curated journey; launch no app")
    modes.add_argument("--journey", help="journey key, for example J14")
    parser.add_argument("--stage", help="stage name")
    parser.add_argument("--run", type=Path, help="run root; stage evidence must be fresh")
    parser.add_argument("--profile-root", type=Path, help="override the persistent isolated profile directory")
    parser.add_argument("--var", action="append", default=[], metavar="NAME=VALUE", help="required stage variable; repeat as needed")
    parser.add_argument("--binary", type=Path, help="executable override (default: build/<stage program>)")
    parser.add_argument("--timeout", type=float, default=120, help="maximum driver runtime in seconds (default: 120)")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        catalog = load_catalog()
    except (OSError, json.JSONDecodeError, PlanError) as exc:
        print(f"catalog error: {exc}", file=sys.stderr)
        return 2
    if args.list:
        for journey, entries in catalog.items():
            for stage in entries:
                print(f"{journey}\t{stage['stage']}\t{stage['driver']}\tprofile={stage['profile']}")
        return 0
    if args.check:
        print(f"validated {sum(map(len, catalog.values()))} stages in {len(catalog)} journey files")
        return 0
    if not args.stage or not args.run:
        print("--journey runs require --stage and --run", file=sys.stderr)
        return 2
    if args.journey not in catalog:
        print(f"unknown journey: {args.journey}", file=sys.stderr)
        return 2
    stage = next((item for item in catalog[args.journey] if item["stage"] == args.stage), None)
    if stage is None:
        print(f"unknown stage: {args.journey}/{args.stage}", file=sys.stderr)
        return 2

    if args.timeout <= 0 or not math.isfinite(args.timeout):
        print("--timeout must be positive and finite", file=sys.stderr)
        return 2
    run_root = args.run.expanduser().resolve()
    evidence = run_root / args.journey / args.stage
    profile = (args.profile_root.expanduser().resolve() if args.profile_root
               else run_root / "profiles" / stage["profile"])
    if evidence.exists():
        print(f"refusing to reuse stage evidence: {evidence}", file=sys.stderr)
        return 2
    evidence.mkdir(parents=True)
    profile.mkdir(parents=True, exist_ok=True)
    for name in ("data", "config", "cache"):
        (profile / name).mkdir(exist_ok=True)
    mapping = {"repo": str(REPO), "evidence": str(evidence), "profile": str(profile)}
    provided: dict[str, str] = {}
    for assignment in args.var:
        if "=" not in assignment:
            print(f"invalid --var {assignment!r}; expected NAME=VALUE", file=sys.stderr)
            write_json(evidence / "stage-result.json", {"status": "not-launched", "error": "invalid --var"})
            return 2
        name, value = assignment.split("=", 1)
        if name in provided:
            print(f"duplicate --var: {name}", file=sys.stderr)
            write_json(evidence / "stage-result.json", {"status": "not-launched", "error": f"duplicate variable: {name}"})
            return 2
        provided[name] = value
    required: set[str] = set()
    for _, value in walk_strings(stage["plan"]):
        for match in TEMPLATE.finditer(value):
            name = match.group(1)
            if name not in BUILTINS:
                required.add(name)
    missing, extra = required - provided.keys(), provided.keys() - required
    if missing or extra:
        message = []
        if missing:
            message.append("missing required variables: " + ", ".join(sorted(missing)))
        if extra:
            message.append("unknown variables: " + ", ".join(sorted(extra)))
        error = "; ".join(message)
        print(error, file=sys.stderr)
        write_json(evidence / "stage-result.json", {"status": "not-launched", "error": error})
        return 2
    mapping.update(provided)
    try:
        plan = replace_templates(stage["plan"], mapping)
        validate_capture_paths(stage["driver"], plan, evidence.resolve(), profile.resolve())
    except PlanError as exc:
        write_json(evidence / "stage-result.json", {"status": "not-launched", "error": str(exc)})
        print(str(exc), file=sys.stderr)
        return 2

    binary = (args.binary if args.binary else REPO / "build" / stage["program"]).expanduser().resolve()
    resolved_plan = evidence / "resolved-plan.json"
    write_json(resolved_plan, plan)
    env = dict(os.environ)
    env.update(HEADLESS)
    env.update(
        XDG_DATA_HOME=str(profile / "data"),
        XDG_CONFIG_HOME=str(profile / "config"),
        XDG_CACHE_HOME=str(profile / "cache"),
    )
    command: list[str]
    if stage["driver"] == "ui":
        command = [str(binary), "--backend", "opengl", *plan.get("launch_args", []),
                   "--resize", "1440,1000", "--profile", *plan["steps"]]
    else:
        helper = "ux-atspi-client.py" if stage["driver"] == "native" else "ux-pty-client.py"
        python = "/usr/bin/python3" if stage["driver"] == "native" else sys.executable
        command = [python, str(REPO / "tools" / helper), "--binary", str(binary),
                   "--plan", str(resolved_plan), "--evidence", str(evidence),
                   "--profile-root", str(profile), "--timeout", str(args.timeout)]
    launch = {"command": command, "cwd": str(REPO), "profile_root": str(profile),
              "headless_environment": {key: env[key] for key in (*HEADLESS, "XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME")}}
    write_json(evidence / "driver-launch.json", launch)
    result: dict[str, Any] = {"status": "failed", "scope": "stage replay, not journey acceptance"}
    stdout_path, stderr_path = evidence / "driver.stdout.log", evidence / "driver.stderr.log"
    try:
        with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
            process = subprocess.Popen(command, cwd=REPO, env=env, stdout=stdout, stderr=stderr,
                                       start_new_session=True)
            try:
                return_code = process.wait(timeout=args.timeout)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(process.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
                try:
                    process.wait(timeout=12)
                except subprocess.TimeoutExpired:
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    process.wait()
                raw_return_code = process.returncode
                result.update(status="timeout",
                              exit_code=137 if raw_return_code == -signal.SIGKILL else raw_return_code,
                              raw_return_code=raw_return_code,
                              error=f"{args.timeout:g} second driver timeout")
            else:
                result.update(exit_code=return_code,
                              status="passed" if return_code == 0 else "failed")
    except OSError as exc:
        stdout_path.touch(exist_ok=True)
        stderr_path.write_text(str(exc) + "\n", encoding="utf-8")
        result.update(status="not-launched", error=str(exc))
    write_json(evidence / "stage-result.json", result)
    print(json.dumps(result))
    return 0 if result["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
