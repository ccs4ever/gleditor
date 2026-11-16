#!/usr/bin/env python3
"""Collect perf stat counters around validated manifest workloads.

This report is supplemental to performance-report.py. Counters cover each
whole child process, including startup and setup; they do not isolate a probe's
internally timed region. Counter-run wall times are diagnostic only.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import importlib.util
import json
import math
import os
import secrets
import shutil
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_ARTIFACT_MANIFEST = ROOT / "build/performance/release-artifacts.json"
COUNTERS = ("cycles:u", "instructions:u")


def import_sibling(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot import {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


performance_report = import_sibling(
    "performance_report_for_hardware", ROOT / "tools/performance-report.py")
release_profile = import_sibling(
    "release_profile_for_hardware", ROOT / "tools/release_profile.py")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def selected_manifest(path: Path | None) -> Path:
    candidate = path or DEFAULT_ARTIFACT_MANIFEST
    if not candidate.is_absolute():
        from_cwd = candidate.resolve()
        candidate = from_cwd if from_cwd.is_file() else (ROOT / candidate).resolve()
    return candidate


def verify_artifact_manifest(path: Path | None) -> dict[str, Any]:
    manifest_path = selected_manifest(path)
    manifest = json.loads(manifest_path.read_text())
    artifacts = manifest.get("artifacts")
    if not isinstance(artifacts, list):
        raise ValueError("artifact manifest must contain an artifacts array")
    records = []
    mismatches = []
    for artifact in artifacts:
        if not isinstance(artifact, dict) or not {
                "path", "sha256"} <= artifact.keys():
            raise ValueError("artifact record must contain path and sha256")
        artifact_path = Path(artifact["path"])
        if not artifact_path.is_absolute():
            artifact_path = ROOT / artifact_path
        actual = sha256(artifact_path) if artifact_path.is_file() else None
        record = {**artifact, "actual_sha256": actual,
                  "matches": actual == artifact["sha256"]}
        records.append(record)
        if not record["matches"]:
            mismatches.append(artifact["path"])
    supplied_source = manifest.get("source")
    actual_source = release_profile.source_identity() if supplied_source is not None else None
    source_matches = supplied_source is None or supplied_source == actual_source
    result = {"path": str(manifest_path), "sha256": sha256(manifest_path),
              "source": supplied_source, "actual_source": actual_source,
              "source_matches": source_matches, "artifacts": records,
              "matches": not mismatches and source_matches,
              "mismatches": mismatches}
    if not result["matches"]:
        detail = []
        if mismatches:
            detail.append(f"artifacts={mismatches}")
        if not source_matches:
            detail.append("source identity differs")
        raise ValueError("artifact/source identity mismatch: " + ", ".join(detail))
    return result


def parse_perf_csv(path: Path) -> dict[str, Any]:
    """Parse perf -x, rows; unavailable and multiplexed values stay null."""
    raw = path.read_text(errors="replace") if path.is_file() else ""
    parsed_rows = []
    by_event: dict[str, dict[str, Any]] = {}
    for row in csv.reader(raw.splitlines()):
        if not row or row[0].lstrip().startswith("#"):
            continue
        fields = row + [""] * max(0, 7 - len(row))
        raw_value, unit, event, runtime_text, percent_text = fields[:5]
        try:
            value = int(raw_value.replace(" ", ""))
            value_status = "counted"
            if value < 0:
                value = None
                value_status = "invalid_negative_count"
        except ValueError:
            value = None
            value_status = "not_counted_or_unsupported" if raw_value else "missing"
        validation_status = None
        try:
            runtime_ns = int(runtime_text.replace(" ", ""))
            if runtime_ns < 0:
                runtime_ns = None
                validation_status = "invalid_negative_runtime_ns"
        except ValueError:
            runtime_ns = None
            validation_status = "runtime_ns_unavailable"
        try:
            runtime_pct = float(percent_text)
            if not math.isfinite(runtime_pct) or not 0.0 <= runtime_pct <= 100.0:
                runtime_pct = None
                validation_status = "invalid_runtime_pct"
        except ValueError:
            runtime_pct = None
        row_data = {"event": event, "raw_value": raw_value, "value": value,
                    "unit": unit or None, "runtime_ns": runtime_ns,
                    "runtime_pct": runtime_pct, "value_status": value_status,
                    "validation_status": validation_status,
                    "raw_fields": row}
        parsed_rows.append(row_data)
        by_event[event] = row_data

    events = {}
    for event in COUNTERS:
        row = by_event.get(event)
        if row is None:
            events[event] = {"value": None, "status": "missing_event",
                             "runtime_pct": None, "runtime_ns": None}
        elif row["value"] is None:
            events[event] = {"value": None, "status": row["value_status"],
                             "runtime_pct": row["runtime_pct"],
                             "runtime_ns": row["runtime_ns"],
                             "raw_value": row["raw_value"]}
        elif row["validation_status"] is not None:
            events[event] = {"value": None,
                             "status": row["validation_status"],
                             "runtime_pct": row["runtime_pct"],
                             "runtime_ns": row["runtime_ns"],
                             "raw_value": row["raw_value"]}
        elif row["runtime_pct"] is None:
            events[event] = {"value": None, "status": "runtime_pct_unavailable",
                             "runtime_pct": None, "runtime_ns": row["runtime_ns"],
                             "raw_value": row["raw_value"]}
        elif row["runtime_pct"] < 100.0:
            events[event] = {"value": None, "status": "multiplexed",
                             "runtime_pct": row["runtime_pct"],
                             "runtime_ns": row["runtime_ns"],
                             "raw_value": row["raw_value"]}
        else:
            events[event] = {"value": row["value"], "status": "counted",
                             "runtime_pct": row["runtime_pct"],
                             "runtime_ns": row["runtime_ns"], "unit": row["unit"]}
    return {"path": str(path), "sha256": sha256(path) if path.is_file() else None,
            "events": events, "rows": parsed_rows, "raw_text": raw}


def run_sample(workload: dict[str, Any], phase: str, index: int,
               run_id: str, variant: str, log_dir: Path, raw_dir: Path,
               env: dict[str, str], perf_path: str) -> dict[str, Any]:
    raw_path = raw_dir / f"{workload['id']}-{phase}-{index:02d}.csv"
    original_argv, _ = performance_report.expand_workload_command(
        workload, run_id, phase, index)
    original_argv[0] = str(performance_report.resolve_executable(original_argv[0]))
    wrapped = dict(workload)
    wrapped["command"] = [perf_path, "stat", "-x,", "--no-big-num", "-e",
                          ",".join(COUNTERS), "-o", str(raw_path), "--",
                          *workload["command"]]
    started = time.perf_counter()
    sample = performance_report.run_one(
        wrapped, phase, index, log_dir, env, run_id,
        pairing_group=f"{variant}:{run_id}", variant=variant)
    sample["counter_wrapper_returncode"] = sample.get("returncode")
    sample["original_command_argv"] = original_argv
    sample["original_command_identity"] = performance_report.command_identity(original_argv)
    sample["perf_command_argv"] = sample.pop("command_argv", None)
    sample["perf_csv"] = parse_perf_csv(raw_path)
    sample["counter_wrapper_elapsed_seconds"] = time.perf_counter() - started
    sample["counter_status"] = (
        "counted" if all(sample["perf_csv"]["events"][name]["status"] == "counted"
                         for name in COUNTERS)
        else "unavailable_or_multiplexed")
    return sample


def run(args: argparse.Namespace) -> int:
    manifest = performance_report.load_manifest()
    workloads = [item for item in manifest["workloads"] if item["split"] == args.split]
    if not workloads:
        raise ValueError(f"manifest has no {args.split} workloads")
    repetitions = args.repetitions if args.repetitions is not None else manifest["repetitions"]
    warmups = args.warmups if args.warmups is not None else manifest["warmups"]
    if repetitions < 1 or warmups < 0:
        raise ValueError("repetitions must be positive and warmups cannot be negative")
    output = args.output.resolve()
    if output.exists():
        raise FileExistsError(f"refusing to overwrite existing output {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    raw_dir = output.parent / f"{output.stem}.raw-perf-csv"
    raw_dir.mkdir(parents=True, exist_ok=False)
    run_id = f"{datetime.now(timezone.utc):%Y%m%dT%H%M%S%fZ}-{os.getpid()}-{secrets.token_hex(4)}"
    artifacts_before = verify_artifact_manifest(args.artifact_manifest)
    source_before = performance_report.source_identity()
    perf_path = args.perf or shutil.which("perf")
    if not perf_path:
        raise FileNotFoundError("perf was not found; use --perf to select it")
    if args.perf is None:
        perf_path = str(Path(perf_path).resolve())
    perf_version = subprocess.run([perf_path, "--version"], cwd=ROOT, text=True,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  check=False)
    if perf_version.returncode != 0:
        raise RuntimeError("perf --version failed")
    log_dir = ROOT / "build/performance/logs" / f"hardware-{args.split}-{run_id}"
    log_dir.mkdir(parents=True, exist_ok=False)
    env = performance_report.headless_environment(os.environ)
    samples = []
    failures = []
    for workload in workloads:
        measured = performance_report.effective_repetitions(
            args.split, repetitions, workload["weight"])
        indices = [("warmup", n + 1) for n in range(warmups)]
        indices.extend(("run", n + 1) for n in range(measured))
        for phase, index in indices:
            sample = run_sample(workload, phase, index, run_id, args.variant,
                                log_dir, raw_dir, env, perf_path)
            samples.append({"workload_id": workload["id"], **sample})
            if not sample.get("success"):
                failures.append(f"{workload['id']} {phase} {index}: workload validation failed")
    counter_failures = [
        {"workload_id": sample["workload_id"], "phase": sample["phase"],
         "index": sample["index"], "events": sample["perf_csv"]["events"]}
        for sample in samples if sample["counter_status"] != "counted"]
    if counter_failures:
        failures.append(f"{len(counter_failures)} samples had unavailable/multiplexed events")
    artifacts_after = verify_artifact_manifest(args.artifact_manifest)
    source_after = performance_report.source_identity()
    identity_unchanged = (
        artifacts_before["sha256"] == artifacts_after["sha256"] and
        artifacts_before["artifacts"] == artifacts_after["artifacts"] and
        artifacts_before["source_matches"] and artifacts_after["source_matches"] and
        source_before == source_after)
    if not identity_unchanged:
        failures.append("source or artifact identity changed during collection")
    report = {
        "schema_version": 1, "kind": "supplemental_hardware_counters",
        "variant": args.variant, "run_id": run_id,
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "split": args.split, "manifest_path": str(performance_report.MANIFEST),
        "manifest_sha256": performance_report.sha256(performance_report.MANIFEST),
        "workload_count": len(workloads), "warmups_requested": warmups,
        "repetitions_requested": repetitions,
        "release_artifact_manifest": artifacts_before,
        "release_artifact_manifest_after": artifacts_after,
        "source_identity_before": source_before,
        "source_identity_after": source_after,
        "identity_unchanged": identity_unchanged,
        "perf": {"path": perf_path, "version": perf_version.stdout.strip(),
                 "argv_template": ["perf", "stat", "-x,", "--no-big-num", "-e",
                                   ",".join(COUNTERS), "-o", "RAW.csv", "--", "COMMAND"]},
        "counter_scope": "whole user-space child process and inherited subprocesses, including startup/setup; not an internally timed probe region",
        "primary_timing_policy": "hardware-counter-run wall times are diagnostic only; use performance-report.py timing samples for timing conclusions",
        "counter_policy": "values remain null when missing, unsupported, not counted, or multiplexed; runtime percentages and raw CSV are retained",
        "samples": samples, "counter_failures": counter_failures,
        "failures": failures, "success": not failures,
        "output_path": str(output), "raw_csv_directory": str(raw_dir),
        "log_directory": str(log_dir),
    }
    temporary = output.with_name(f".{output.name}.{run_id}.tmp")
    temporary.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    os.replace(temporary, output)
    print(f"wrote {output}")
    return 0 if report["success"] else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--split", choices=("train", "validation"), default="validation")
    parser.add_argument("--repetitions", type=int)
    parser.add_argument("--warmups", type=int)
    parser.add_argument("--artifact-manifest", type=Path,
                        default=DEFAULT_ARTIFACT_MANIFEST)
    parser.add_argument("--variant", default="current-release",
                        help="measurement label; it does not claim a build mode")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--perf", help="perf executable path; primarily useful for controlled tests")
    args = parser.parse_args()
    try:
        return run(args)
    except (OSError, ValueError, json.JSONDecodeError, RuntimeError) as error:
        print(f"performance-hardware-report: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
