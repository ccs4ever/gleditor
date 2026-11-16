#!/usr/bin/env python3
"""Run versioned headless performance workloads and retain raw observations."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import resource
import secrets
import shlex
import shutil
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parent.parent
MANIFEST = ROOT / "tests/performance/workloads.json"
HEADLESS_ENV = {
    "SDL_VIDEODRIVER": "offscreen",
    "SDL_AUDIODRIVER": "dummy",
    "LIBGL_ALWAYS_SOFTWARE": "1",
    "XDG_DATA_HOME": "build/performance/xdg/data",
    "XDG_CONFIG_HOME": "build/performance/xdg/config",
    "XDG_CACHE_HOME": "build/performance/xdg/cache",
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def git(*args: str, cwd: Path = ROOT) -> str:
    result = subprocess.run(["git", *args], cwd=cwd, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            check=False)
    return result.stdout.strip() if result.returncode == 0 else ""


def porcelain_paths(raw: bytes) -> list[tuple[str, str, str]]:
    """Parse porcelain-v1 -z entries as (status, path, role)."""
    entries = raw.split(b"\0")
    parsed = []
    index = 0
    while index < len(entries) and entries[index]:
        item = entries[index]
        index += 1
        code = os.fsdecode(item[:2])
        parsed.append((code, os.fsdecode(item[3:]), "path"))
        if "R" in code or "C" in code:
            if index >= len(entries) or not entries[index]:
                raise ValueError("incomplete git rename/copy status record")
            parsed.append((code, os.fsdecode(entries[index]), "renamed_from"))
            index += 1
    return parsed


def source_identity(repository: Path = ROOT) -> dict[str, Any]:
    status_result = subprocess.run(
        ["git", "status", "--porcelain=v1", "-z", "--untracked-files=all"],
        cwd=repository, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        check=False)
    if status_result.returncode != 0:
        raise ValueError(f"cannot inspect git status at {repository}")
    dirty: list[dict[str, str]] = []
    for code, item_name, item_role in porcelain_paths(status_result.stdout):
        path = repository / item_name
        record = {"path": item_name, "status": code, "role": item_role}
        if path.is_file():
            record["sha256"] = sha256(path)
        elif path.is_dir():
            if (path / ".git").exists():
                record["is_submodule"] = True
            else:
                record["tree_sha256"] = directory_digest(path)
        dirty.append(record)
    submodules = []
    dirty_submodules = []
    for line in git("submodule", "status", "--recursive", cwd=repository).splitlines():
        if line:
            submodules.append(line)
            module_path = line[1:].strip().split(None, 1)[1].split(" (")[0]
            module_root = repository / module_path
            module_status = subprocess.run(
                ["git", "-C", str(module_root), "status", "--porcelain=v1",
                 "-z", "--untracked-files=all"], cwd=ROOT,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
            if module_status.returncode != 0:
                raise ValueError(f"cannot inspect submodule {module_path}")
            module_files = []
            for status_code, relative, item_role in porcelain_paths(module_status.stdout):
                path = module_root / relative
                entry = {"path": relative, "status": status_code, "role": item_role}
                if path.is_file():
                    entry["sha256"] = sha256(path)
                elif path.is_dir():
                    entry["tree_sha256"] = directory_digest(path)
                module_files.append(entry)
            if module_files:
                dirty_submodules.append({"path": module_path,
                                         "revision": subprocess.run(
                                             ["git", "-C", str(module_root),
                                              "rev-parse", "HEAD"], cwd=ROOT,
                                             text=True, stdout=subprocess.PIPE,
                                             check=False).stdout.strip(),
                                         "dirty_files": module_files})
    return {
        "revision": git("rev-parse", "HEAD", cwd=repository),
        "dirty": bool(dirty),
        "dirty_files": dirty,
        "submodules": submodules,
        "dirty_submodules": dirty_submodules,
        "buildflags": (repository / "build/.buildflags").read_text(errors="replace")
        if (repository / "build/.buildflags").is_file() else None,
    }


def directory_digest(path: Path) -> str:
    digest = hashlib.sha256()
    for child in sorted(path.rglob("*")):
        if child.is_file() and ".git" not in child.parts:
            digest.update(str(child.relative_to(path)).encode())
            digest.update(sha256(child).encode())
    return digest.hexdigest()


def tool_version(command: list[str]) -> dict[str, Any]:
    executable = shutil.which(command[0])
    if not executable:
        return {"command": command, "available": False}
    result = subprocess.run([executable, *command[1:]], cwd=ROOT, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            check=False, timeout=10)
    return {"command": command, "path": executable,
            "version": result.stdout.strip().splitlines()[:3],
            "returncode": result.returncode}


def linker_selector(buildflags: str | None) -> str | None:
    if not buildflags:
        return None
    for flag in shlex.split(buildflags):
        if flag.startswith("-fuse-ld="):
            return flag.partition("=")[2] or None
    return "ld"


def linker_program(selector: str, buildflags: str | None) -> str:
    """Turn Clang's -fuse-ld spelling into the program whose version matters."""
    explicit_programs = {
        "lld": "ld.lld",
        "gold": "ld.gold",
        "bfd": "ld.bfd",
        "mold": "ld.mold",
    }
    if selector != "lld":
        return explicit_programs.get(selector, selector)
    target = next((token.partition("=")[2] for token in shlex.split(buildflags or "")
                   if token.startswith("TARGET=")), "")
    if "windows" in target or "mingw" in target or "msvc" in target:
        return "lld-link"
    if "darwin" in target or "apple" in target:
        return "ld64.lld"
    return explicit_programs["lld"]


def configured_linker_identity(buildflags: str | None) -> dict[str, Any]:
    selector = linker_selector(buildflags)
    if selector is None:
        return {"status": "unknown", "reason": "build flags unavailable"}
    compiler = shlex.split(os.environ.get("CXX", "clang++"))
    driver_path = shutil.which(compiler[0]) if compiler else None
    if not driver_path:
        return {"status": "unknown", "selector": selector,
                "reason": "configured compiler driver unavailable"}
    compiler[0] = driver_path
    program = linker_program(selector, buildflags)
    command = [*compiler, f"-print-prog-name={program}"]
    try:
        result = subprocess.run(command, cwd=ROOT, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                check=False, timeout=10)
    except (OSError, subprocess.TimeoutExpired) as error:
        return {"status": "unknown", "selector": selector,
                "driver_path": driver_path, "driver_reported_command": command,
                "reason": str(error)}
    reported = result.stdout.strip() if result.returncode == 0 else ""
    resolved = (reported if Path(reported).is_absolute() else shutil.which(reported))
    # Preserve the reported basename: generic lld dispatches by argv[0], so
    # resolving ld.lld's symlink would turn it into the generic lld driver.
    resolved_path = str(Path(resolved).absolute()) if resolved else None
    driver_identity = tool_version([*compiler, "--version"])
    linker_identity = (tool_version([resolved_path, "--version"])
                       if resolved_path else None)
    identity_known = (bool(resolved_path) and linker_identity is not None and
                      linker_identity.get("returncode") == 0)
    return {
        "status": "resolved" if identity_known else "unknown",
        "selection": selector,
        "driver_program": program,
        "driver": driver_identity,
        "driver_path": driver_path,
        "driver_reported_command": command,
        "driver_reported_linker": reported or None,
        "linker_path": resolved_path,
        "linker": linker_identity,
        "reason": None if identity_known else "selected linker path/version could not be verified",
    }


def machine_identity() -> dict[str, Any]:
    return {"platform": platform.platform(), "machine": platform.machine(),
            "processor": platform.processor(), "python": sys.version,
            "cpu_count": os.cpu_count()}


def input_identity(workload: dict[str, Any]) -> list[dict[str, Any]]:
    result = []
    for name in workload["inputs"]:
        path = ROOT / name
        if not path.is_file():
            result.append({"path": name, "available": False})
        else:
            result.append({"path": name, "available": True,
                           "size_bytes": path.stat().st_size,
                           "sha256": sha256(path)})
    return result


def command_identity(argv: list[str]) -> dict[str, Any]:
    executable = resolve_executable(argv[0])
    result: dict[str, Any] = {"argv": argv}
    if executable.is_file():
        result["executable"] = {"path": str(executable), "size_bytes": executable.stat().st_size,
                                "sha256": sha256(executable)}
        libraries = []
        try:
            dep_command = (["otool", "-L", str(executable)] if sys.platform == "darwin"
                           else ["ldd", str(executable)])
            ldd = subprocess.run(dep_command, cwd=ROOT, text=True,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 check=False, timeout=10)
            for line in ldd.stdout.splitlines():
                fields = line.strip().split(" => ")
                if sys.platform == "darwin":
                    resolved = line.strip().split(" (")[0]
                    if resolved == str(executable):
                        continue
                else:
                    resolved = fields[-1].split(" (")[0] if len(fields) > 1 else ""
                if resolved.startswith("/") and Path(resolved).is_file():
                    libraries.append({"path": resolved, "sha256": sha256(Path(resolved))})
            result["libraries"] = libraries
        except (OSError, subprocess.TimeoutExpired):
            result["libraries"] = None
    else:
        result["executable"] = {"path": argv[0], "available": False}
    return result


def resolve_executable(name: str) -> Path:
    path = Path(name)
    if path.is_absolute():
        return path
    if len(path.parts) > 1:
        return ROOT / path
    found = shutil.which(name)
    return Path(found) if found else ROOT / name


def parse_gtest_result(path: Path) -> dict[str, int]:
    """Count executed and skipped tests in GoogleTest's JSON result."""
    data = json.loads(path.read_text())
    if not isinstance(data, dict):
        raise ValueError("GoogleTest JSON root must be an object")
    executed = 0
    skipped = 0

    def visit(node: Any) -> None:
        nonlocal executed, skipped
        if not isinstance(node, dict):
            raise ValueError("GoogleTest suite/test entry must be an object")
        if "status" in node:
            if not isinstance(node.get("status"), str) or not isinstance(node.get("result"), str):
                raise ValueError("GoogleTest test status/result must be strings")
            if node.get("result") == "SKIPPED":
                skipped += 1
            elif node.get("status") == "RUN":
                executed += 1
            return
        tests = node.get("tests", [])
        if not isinstance(tests, (int, list)):
            raise ValueError("GoogleTest suite tests field has an invalid shape")
        for key in ("testsuite", "testsuites"):
            if key in node and not isinstance(node[key], list):
                raise ValueError(f"GoogleTest {key} field must be a list")
        if isinstance(tests, list):
            for test in tests:
                if not isinstance(test, dict):
                    raise ValueError("GoogleTest test entry must be an object")
                if "tests" in test or "testsuite" in test or "testsuites" in test:
                    visit(test)
                    continue
                if "status" not in test or "result" not in test:
                    raise ValueError("GoogleTest test entry misses status/result")
                if test.get("status") == "RUN" and test.get("result") != "SKIPPED":
                    executed += 1
                elif test.get("result") == "SKIPPED":
                    skipped += 1
        elif not isinstance(tests, (int, float, str)):
            raise ValueError("GoogleTest suite tests field has an invalid shape")
        for suite in node.get("testsuite", []):
            visit(suite)
        for suite in node.get("testsuites", []):
            visit(suite)

    visit(data)
    return {"executed": executed, "skipped": skipped}


def headless_environment(parent: dict[str, str]) -> dict[str, str]:
    env = parent.copy()
    for key, value in HEADLESS_ENV.items():
        env[key] = str(ROOT / value) if key.startswith("XDG_") else value
    return env


def rss_bytes(max_rss: float, target_platform: str) -> int:
    """resource.ru_maxrss is bytes on macOS and KiB on Linux/other Unix."""
    return int(max_rss if target_platform == "darwin" else max_rss * 1024)


def effective_repetitions(split: str, repetitions: int, weight: int) -> int:
    return repetitions * weight if split == "train" else repetitions


def gcc_profile_snapshot(directory: Path) -> dict[str, str]:
    if not directory.is_dir():
        return {}
    return {str(path): sha256(path) for path in directory.rglob("*.gcda") if path.is_file()}


def run_one(workload: dict[str, Any], label: str, index: int,
            log_dir: Path, env: dict[str, str], run_id: str,
            pairing_group: str, variant: str) -> dict[str, Any]:
    argv = workload["command"]
    log_path = log_dir / f"{workload['id']}-{label}-{index:02d}.log"
    usage_path = log_path.with_suffix(".usage.json")
    result_path = log_path.with_suffix(".gtest.json")
    artifact_dir = ROOT / "build/performance/artifacts" / run_id / workload["id"] / f"{label}-{index:02d}"
    artifact_dir.mkdir(parents=True, exist_ok=True)
    input_paths = [str(ROOT / name) for name in workload.get("inputs", [])]
    placeholders = {"{run_id}": run_id, "{workload_id}": workload["id"],
                    "{sample_index}": str(index), "{artifact_dir}": str(artifact_dir)}
    if input_paths:
        placeholders["{input}"] = input_paths[0]
        placeholders.update({f"{{input{number}}}": path
                             for number, path in enumerate(input_paths)})
    expanded_argv = []
    for value in argv:
        for placeholder, replacement in placeholders.items():
            value = value.replace(placeholder, replacement)
        expanded_argv.append(value)
    result_format = workload["result_format"]
    child_argv = [str(resolve_executable(expanded_argv[0])), *expanded_argv[1:]]
    if result_format == "gtest_json":
        child_argv.append(f"--gtest_output=json:{result_path}")
    probe_path = log_path.with_suffix(".probe.json")
    capture_mode = "stdout-json" if result_format == "probe_json" else "inherit"
    command = [sys.executable, str(Path(__file__).resolve()), "_measure_child",
               str(usage_path), capture_mode, str(probe_path), *child_argv]
    sample_env = env.copy()
    sample_state = ROOT / "build/performance/xdg" / run_id / workload["id"] / f"{label}-{index:02d}"
    xdg_paths = {}
    for suffix in ("data", "config", "cache"):
        path = sample_state / suffix
        path.mkdir(parents=True, exist_ok=True)
        key = f"XDG_{suffix.upper()}_HOME"
        sample_env[key] = str(path)
        xdg_paths[key] = str(path.relative_to(ROOT))
    profile_dir = ROOT / "build/performance/raw" / workload["id"]
    profile_dir.mkdir(parents=True, exist_ok=True)
    profile_before = {path: sha256(path) for path in profile_dir.glob("*.profraw")}
    sample_env["LLVM_PROFILE_FILE"] = str(profile_dir / "%m-%p.profraw")
    gcc_profile_dir_value = sample_env.get("GLEDITOR_GCC_PROFILE_DIR")
    gcc_profile_dir = None
    gcc_before: dict[str, str] = {}
    if gcc_profile_dir_value:
        gcc_profile_dir = Path(gcc_profile_dir_value)
        if not gcc_profile_dir.is_absolute():
            gcc_profile_dir = ROOT / gcc_profile_dir
        gcc_before = gcc_profile_snapshot(gcc_profile_dir)
    started = time.perf_counter()
    try:
        with log_path.open("wb") as log:
            completed = subprocess.run(command, cwd=ROOT, env=sample_env,
                                       stdout=log, stderr=subprocess.STDOUT,
                                       check=False)
        wall = time.perf_counter() - started
        rss = None
        if usage_path.exists():
            try:
                rss = json.loads(usage_path.read_text()).get("peak_rss_bytes")
            except (ValueError, OSError, json.JSONDecodeError):
                pass
        child_wall = wall
        try:
            child_wall = json.loads(usage_path.read_text())["wall_seconds"]
        except (KeyError, ValueError, OSError, json.JSONDecodeError):
            pass
        test_counts = None
        if result_format == "gtest_json" and result_path.exists():
            try:
                test_counts = parse_gtest_result(result_path)
            except (OSError, ValueError, json.JSONDecodeError):
                pass
        success = completed.returncode == 0
        if result_format == "gtest_json":
            success = success and test_counts is not None and test_counts["executed"] > 0
        probe_metrics = None
        probe_error = None
        if result_format == "probe_json":
            try:
                probe_metrics = json.loads(probe_path.read_text())
                expected = workload["probe_scenario"]
                if not isinstance(probe_metrics, dict):
                    raise ValueError("probe JSON root must be an object")
                if (probe_metrics.get("schema_version") not in {1, 2} or
                        probe_metrics.get("scenario") != expected or
                        probe_metrics.get("measure") != workload["measure"]):
                    raise ValueError("probe JSON schema/scenario/measure mismatch")
                if (probe_metrics["schema_version"] == 2 and
                        workload["measure"] in {"timing", "both"} and
                        not isinstance(probe_metrics.get("wall_time_scope"), str)):
                    raise ValueError("timing probe has no wall_time_scope")
                if not {"work_items", "checksum"} <= probe_metrics.keys():
                    raise ValueError("probe JSON misses work_items/checksum")
                if workload["measure"] in {"timing", "both"} and not isinstance(probe_metrics.get("wall_time_ns"), int):
                    raise ValueError("timing probe has no integer wall_time_ns")
                calls = probe_metrics.get("scenario_total_cpp_new_calls",
                                          probe_metrics.get("cpp_new_calls"))
                requested_bytes = probe_metrics.get(
                    "scenario_total_cpp_new_requested_bytes",
                    probe_metrics.get("cpp_new_requested_bytes"))
                if workload["measure"] in {"allocations", "both"} and not isinstance(calls, int):
                    raise ValueError("allocation probe has no integer cpp_new_calls")
                if workload["measure"] in {"allocations", "both"} and not isinstance(requested_bytes, int):
                    raise ValueError("allocation probe has no integer cpp_new_requested_bytes")
            except (OSError, ValueError, json.JSONDecodeError) as error:
                probe_error = str(error)
                probe_metrics = None
            success = success and probe_metrics is not None
        artifacts = []
        artifact_errors = []
        for pattern in workload.get("required_artifacts", []):
            expanded = pattern
            for placeholder, replacement in placeholders.items():
                expanded = expanded.replace(placeholder, replacement)
            path = Path(expanded)
            if not path.is_absolute():
                path = ROOT / path
            if not path.is_file() or path.stat().st_size == 0:
                artifact_errors.append(f"required artifact is missing or empty: {expanded}")
            else:
                artifacts.append({"path": str(path.relative_to(ROOT)) if path.is_relative_to(ROOT) else str(path),
                                  "size_bytes": path.stat().st_size, "sha256": sha256(path)})
        if workload.get("required_artifacts"):
            success = success and not artifact_errors
        output_checks = []
        if workload.get("stdout_contains"):
            try:
                stdout_text = log_path.read_text(errors="replace")
            except OSError:
                stdout_text = ""
            output_checks = [{"text": expected, "matched": expected in stdout_text}
                             for expected in workload["stdout_contains"]]
            success = success and all(check["matched"] for check in output_checks)
        gcc_after = gcc_profile_snapshot(gcc_profile_dir) if gcc_profile_dir else {}
        gcc_changed = [{"path": path, "sha256": digest}
                       for path, digest in gcc_after.items()
                       if gcc_before.get(path) != digest]
        profile_after = {path: sha256(path) for path in profile_dir.glob("*.profraw")}
        llvm_changed = [{"path": str(path.relative_to(ROOT)), "sha256": digest}
                        for path, digest in profile_after.items()
                        if profile_before.get(path) != digest]
        probe_wall = (probe_metrics.get("wall_time_ns") / 1e9
                      if probe_metrics and isinstance(probe_metrics.get("wall_time_ns"), int)
                      else None)
        sample_wall = probe_wall if result_format == "probe_json" else child_wall
        return {"phase": label, "index": index, "wall_seconds": sample_wall,
                "runner_wall_seconds": wall,
                "peak_rss_bytes": rss, "peak_rss_source": "isolated helper RUSAGE_CHILDREN" if rss is not None else "unavailable",
                "returncode": completed.returncode, "success": success,
                "test_counts": test_counts,
                "probe_metrics": probe_metrics, "probe_error": probe_error,
                "artifacts": artifacts, "artifact_errors": artifact_errors,
                "stdout_checks": output_checks,
                "command_argv": child_argv,
                "variant": variant,
                "pairing_key": f"{pairing_group}:{workload['id']}:{label}:{index:03d}",
                "xdg_paths": xdg_paths,
                "profile_outputs": llvm_changed,
                "gcc_profile_outputs": gcc_changed,
                "log": str(log_path.relative_to(ROOT))}
    except OSError as error:
        return {"phase": label, "index": index,
                "wall_seconds": time.perf_counter() - started,
                "peak_rss_bytes": None, "peak_rss_source": "unavailable",
                "returncode": None, "success": False, "error": str(error),
                "log": str(log_path.relative_to(ROOT))}


def summarize(samples: list[dict[str, Any]]) -> dict[str, Any]:
    measured = [sample["wall_seconds"] for sample in samples
                if sample["phase"] == "run" and sample["success"] and
                isinstance(sample["wall_seconds"], (float, int))]
    if not measured:
        return {"successful_runs": 0, "median_wall_seconds": None,
                "min_wall_seconds": None, "max_wall_seconds": None,
                "spread_wall_seconds": None}
    ordered = sorted(measured)
    middle = len(ordered) // 2
    median = (ordered[middle] if len(ordered) % 2 else
              (ordered[middle - 1] + ordered[middle]) / 2)
    return {"successful_runs": len(measured), "median_wall_seconds": median,
            "min_wall_seconds": ordered[0], "max_wall_seconds": ordered[-1],
            "spread_wall_seconds": ordered[-1] - ordered[0]}


def load_manifest() -> dict[str, Any]:
    manifest = json.loads(MANIFEST.read_text())
    if manifest.get("version") != 1 or not isinstance(manifest.get("workloads"), list):
        raise ValueError("unsupported performance workload manifest version/shape")
    ids = set()
    for workload in manifest["workloads"]:
        required = {"id", "split", "weight", "command", "inputs", "covers",
                    "result_format"}
        if not required <= workload.keys():
            raise ValueError(f"workload misses fields: {workload.get('id', '<unknown>')}")
        if workload["id"] in ids or workload["split"] not in {"train", "validation"}:
            raise ValueError(f"duplicate id or invalid split: {workload['id']}")
        if not isinstance(workload["command"], list) or not workload["command"]:
            raise ValueError(f"command must be a nonempty argv array: {workload['id']}")
        if not all(isinstance(argument, str) for argument in workload["command"]):
            raise ValueError(f"command must contain only argv strings: {workload['id']}")
        if not isinstance(workload["inputs"], list) or not isinstance(
                workload.get("dependencies", []), list):
            raise ValueError(f"inputs and dependencies must be arrays: {workload['id']}")
        if (not isinstance(workload["weight"], int) or
                workload["weight"] <= 0):
            raise ValueError(f"training weight must be a positive integer: {workload['id']}")
        if workload["result_format"] not in {"gtest_json", "probe_json",
                                               "exit_status", "artifact_file"}:
            raise ValueError(f"unsupported result format: {workload['id']}")
        if workload["result_format"] == "probe_json" and not {
                "probe_scenario", "measure", "size", "seed"} <= workload.keys():
            raise ValueError(f"probe workload misses scenario parameters: {workload['id']}")
        if workload["result_format"] == "probe_json":
            if workload["measure"] not in {"timing", "allocations", "both"}:
                raise ValueError(f"invalid probe measurement mode: {workload['id']}")
            if any(not isinstance(workload[key], int) or workload[key] <= 0
                   for key in ("size", "seed")):
                raise ValueError(f"probe size and seed must be positive integers: {workload['id']}")
            if "counter_command" in workload and (
                    not isinstance(workload["counter_command"], list) or
                    not all(isinstance(argument, str)
                            for argument in workload["counter_command"])):
                raise ValueError(f"counter_command must be an argv array: {workload['id']}")
        if workload["result_format"] == "artifact_file" and not workload.get("required_artifacts"):
            raise ValueError(f"artifact workload has no required artifacts: {workload['id']}")
        ids.add(workload["id"])
        for name in workload["inputs"] + workload.get("dependencies", []):
            if not isinstance(name, str) or not (ROOT / name).is_file():
                raise ValueError(f"missing workload input {name!r}: {workload['id']}")
    source_files_by_split = {
        split: {name for workload in manifest["workloads"]
                if workload["split"] == split for name in workload["inputs"]}
        for split in ("train", "validation")
    }
    overlap = source_files_by_split["train"] & source_files_by_split["validation"]
    if overlap:
        raise ValueError(f"train and validation source files overlap: {sorted(overlap)}")
    return manifest


def run(args: argparse.Namespace) -> int:
    manifest = load_manifest()
    workloads = [w for w in manifest["workloads"] if w["split"] == args.split]
    if not workloads:
        raise ValueError(f"manifest has no {args.split} workloads")
    reps = args.repetitions if args.repetitions is not None else manifest["repetitions"]
    warmups = args.warmups if args.warmups is not None else manifest["warmups"]
    if reps < 1 or warmups < 0:
        raise ValueError("repetitions must be positive and warmups cannot be negative")
    run_id = f"{datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')}-{os.getpid()}-{secrets.token_hex(3)}"
    perf_dir = ROOT / "build/performance"
    (perf_dir / "raw").mkdir(parents=True, exist_ok=True)
    (perf_dir / "xdg/data").mkdir(parents=True, exist_ok=True)
    (perf_dir / "xdg/config").mkdir(parents=True, exist_ok=True)
    (perf_dir / "xdg/cache").mkdir(parents=True, exist_ok=True)
    output = Path(args.output)
    if not output.is_absolute():
        output = ROOT / output
    output.parent.mkdir(parents=True, exist_ok=True)
    log_dir = perf_dir / "logs" / f"{args.split}-{run_id}"
    log_dir.mkdir(parents=True, exist_ok=True)
    env = headless_environment(os.environ)
    # PGO-instrumented binaries write distinct profile files when the manifest
    # contains several workloads or parallel child processes.
    env["LLVM_PROFILE_FILE"] = str(perf_dir / "raw/%m-%p.profraw")
    source = source_identity()

    report: dict[str, Any] = {
        "schema_version": 1, "manifest_version": manifest["version"],
        "manifest_sha256": sha256(MANIFEST), "split": args.split,
        "created_utc": datetime.now(timezone.utc).isoformat(), "run_id": run_id,
        "invocation": {"argv": sys.argv, "normalized": {
            "action": "run", "split": args.split, "output": str(output),
            "repetitions_override": args.repetitions, "warmups_override": args.warmups,
            "variant": args.variant, "build_label": args.build_label,
            "pairing_id": args.pairing_id,
            "collect_counters": bool(getattr(args, "collect_counters", False))}},
        "warmups_requested": warmups, "repetitions_requested": reps,
        "variant": args.variant, "build_label": args.build_label,
        "pairing": {"id": args.pairing_id or run_id,
                    "comparable": bool(args.pairing_id),
                    "method": "same workload/index keys; runs are sequential, not interleaved"},
        "environment": {"headless": HEADLESS_ENV,
                        "xdg_policy": "fresh isolated data/config/cache directories for each warmup and measured sample"},
        "collect_counters": bool(getattr(args, "collect_counters", False)),
        "corpus_identity_claim": manifest["corpus_identity_claim"],
        "input_identity_kind": manifest["input_identity_kind"],
        "identity": {"source": source, "machine": machine_identity(),
                     "toolchain": {"cxx": tool_version([os.environ.get("CXX", "clang++"), "--version"]),
                                   "make": tool_version(["make", "--version"]),
                                   "linker": configured_linker_identity(source.get("buildflags"))}},
        "counters": {
            "timing_probe_fields": ["wall_time_ns", "fold_kernel_ns", "cell_count",
                                    "dimension_run_count", "empty_dimension_run_cells",
                                    "by_ref_entries_per_cell", "wing_arity_histogram",
                                    "choice_points", "selected_snapshot_map_entries"],
            "allocation_probe_fields": ["scenario_total_cpp_new_calls",
                                        "scenario_total_cpp_new_requested_bytes",
                                        "fold_cpp_new_calls", "fold_cpp_new_requested_bytes"],
            "allocation_probe_collection": "separate pass; collected only with --collect-counters",
            "unavailable": ["hardware_cycles", "instructions",
                            "runtime-wide allocator totals", "occurrence fast-path eligibility",
                            "mutable-payload copied elements", "active boundary/table capacity",
                            "root-steal/rebase/move work"],
            "aggregation": "raw per-workload metrics only; no cross-workload totals"},
        "workloads": []}
    overall_success = True
    for workload in workloads:
        workload_reps = effective_repetitions(args.split, reps, workload["weight"])
        record: dict[str, Any] = {
            "id": workload["id"], "split": workload["split"],
            "weight": workload["weight"], "covers": workload["covers"],
            "inputs": input_identity(workload),
            "dependencies": input_identity({"inputs": workload.get("dependencies", [])}),
            "input_identity_kind": manifest["input_identity_kind"],
            "command_identity": command_identity(workload["command"]),
            "repetitions_requested": reps,
            "training_weight": workload["weight"],
            "realized_iterations": warmups + workload_reps,
            "realized_measured_iterations": workload_reps,
            "counter_iterations_requested": int(
                bool(getattr(args, "collect_counters", False) and
                     "counter_command" in workload)),
            "result_format": workload.get("result_format", "gtest_json"),
            "scenario": workload.get("scenario", "synthetic or embedded data defined by the test source"),
            "scenario_parameters": {key: workload[key]
                                    for key in ("probe_scenario", "measure", "size", "seed")
                                    if key in workload},
            "samples": []}
        for index in range(warmups):
            record["samples"].append(run_one(workload, "warmup", index + 1, log_dir, env,
                                             run_id, args.pairing_id or run_id, args.variant))
        for index in range(workload_reps):
            sample = run_one(workload, "run", index + 1, log_dir, env,
                             run_id, args.pairing_id or run_id, args.variant)
            record["samples"].append(sample)
            overall_success &= sample["success"]
        record["summary"] = summarize(record["samples"])
        if getattr(args, "collect_counters", False) and "counter_command" in workload:
            counter_workload = {**workload, "command": workload["counter_command"],
                                "measure": "allocations"}
            record["counter_command_identity"] = command_identity(
                counter_workload["command"])
            counter_sample = run_one(counter_workload, "counter", 1, log_dir, env,
                                     run_id, args.pairing_id or run_id, args.variant)
            record["counter_sample"] = counter_sample
            record["counter_metrics"] = counter_sample.get("probe_metrics")
            record["counter_success"] = counter_sample["success"]
            record["counter_iterations_completed"] = int(counter_sample["success"])
            overall_success &= counter_sample["success"]
        record["profile_outputs"] = [profile for sample in record["samples"]
                                      for profile in sample.get("profile_outputs", [])]
        record["gcc_profile_outputs"] = [profile for sample in record["samples"]
                                          for profile in sample.get("gcc_profile_outputs", [])]
        record["success"] = all(s["success"] for s in record["samples"])
        overall_success &= record["success"]
        report["workloads"].append(record)
    report["success"] = bool(overall_success)
    report["decision"] = "baseline observations only; no optimization claim"
    temporary = output.with_name(f".{output.name}.{run_id}.tmp")
    temporary.write_text(json.dumps(report, indent=2) + "\n")
    os.replace(temporary, output)
    print(f"wrote {output.relative_to(ROOT) if output.is_relative_to(ROOT) else output}")
    return 0 if overall_success else 1


def main() -> int:
    if len(sys.argv) >= 2 and sys.argv[1] == "_measure_child":
        usage_path = Path(sys.argv[2])
        try:
            capture_mode = sys.argv[3]
            captured_output = Path(sys.argv[4])
            child_argv = sys.argv[5:]
            started = time.perf_counter()
            if capture_mode == "stdout-json":
                with captured_output.open("wb") as output_stream:
                    completed = subprocess.run(child_argv, stdout=output_stream,
                                               check=False)
            elif capture_mode == "inherit":
                completed = subprocess.run(child_argv, check=False)
            else:
                raise ValueError(f"unsupported child capture mode: {capture_mode}")
            usage = resource.getrusage(resource.RUSAGE_CHILDREN)
            usage_path.write_text(json.dumps({
                "wall_seconds": time.perf_counter() - started,
                "peak_rss_bytes": rss_bytes(usage.ru_maxrss, sys.platform),
                "peak_rss_source_unit": "bytes" if sys.platform == "darwin" else "KiB",
            }))
            return completed.returncode
        except OSError as error:
            print(f"performance-report child: {error}", file=sys.stderr)
            return 127
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="action", required=True)
    runner = subparsers.add_parser("run", help="run a manifest split")
    runner.add_argument("--split", choices=("train", "validation"), required=True)
    runner.add_argument("--output", required=True)
    runner.add_argument("--repetitions", type=int)
    runner.add_argument("--warmups", type=int)
    runner.add_argument("--variant", default="baseline")
    runner.add_argument("--build-label", default="current-build")
    runner.add_argument("--pairing-id")
    runner.add_argument("--collect-counters", action="store_true",
                         help="run separately declared allocation probes after timing")
    args = parser.parse_args()
    try:
        return run(args)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"performance-report: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
