#!/usr/bin/env python3
"""Build and validate a serial, provenance-bound PGO release pipeline."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
PERF = ROOT / "build/performance"
TARGETS = [
    "all", "shader_assemble", "xudu-swarm-peer", "performance-probe",
    "performance-probe-counts",
]
# Keep these in sync with Makefile's `all` programs plus the explicit helper,
# test-peer, and probe targets below. Workloads prove selected behavior; this
# inventory binds the release executables and compatibility links by digest.
BUILD_ARTIFACTS = [
    "build/gleditor", "build/xuzz", "build/xudu", "build/zigzag",
    "build/store-dump", "build/xudu-dump", "build/vqueryc", "build/vquery",
    "build/vprolog", "build/vplc", "build/vpl", "build/vlace",
    "build/vlacec", "build/vicon", "build/viconc", "build/vagent",
    "build/gleditor_test", "build/xuzz_test", "build/xudu_test",
    "build/zigzag_test", "build/shader_assemble", "build/xudu-swarm-peer",
    "build/performance-probe", "build/performance-probe-counts",
]


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def source_identity() -> str:
    # Include authored inputs and build configuration, not outputs or the
    # pipeline's own generated manifests. Gitlink revisions are included too.
    files = subprocess.check_output(
        ["git", "ls-files", "-co", "--exclude-standard"], cwd=ROOT, text=True
    ).splitlines()
    h = hashlib.sha256()
    included_suffixes = {".cpp", ".hpp", ".h", ".c", ".glsl", ".py", ".sh", ".json", ".yaml", ".yml", ".in", ".mk"}
    for name in sorted(files):
        p = ROOT / name
        if not p.is_file() or name.startswith(("build/", ".worktrees/", "thirdparty/", "tests/samples/")):
            continue
        if name.startswith("assets/") and p.suffix != ".glsl":
            continue
        if p.suffix not in included_suffixes and name not in {"Makefile", "PKGBUILD"}:
            continue
        h.update(name.encode())
        h.update(b"\0")
        h.update(bytes.fromhex(digest(p)))
    dirty_submodules = subprocess.run(
        ["git", "submodule", "foreach", "--quiet", "--recursive",
         "git diff --quiet && test -z \"$(git ls-files --others --exclude-standard)\""],
        cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=False
    )
    if dirty_submodules.returncode:
        raise RuntimeError("refusing profile reuse with dirty or uninitialized submodule inputs")
    submodules = subprocess.check_output(["git", "submodule", "status", "--recursive"], cwd=ROOT, text=True)
    h.update(submodules.encode())
    return h.hexdigest()


def digest_tree(path: Path) -> str:
    h = hashlib.sha256()
    for item in sorted(path.rglob("*")):
        if item.is_file():
            h.update(str(item.relative_to(path)).encode())
            h.update(bytes.fromhex(digest(item)))
    return h.hexdigest()


def compiler() -> tuple[str, bool, str]:
    cxx = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("c++") or "c++"
    real = shutil.which(cxx.split()[0]) or cxx.split()[0]
    version = subprocess.check_output([*cxx.split(), "--version"], text=True, stderr=subprocess.STDOUT).splitlines()[0]
    is_clang = "clang" in version.lower()
    return cxx, is_clang, version + " | " + real


def run(cmd: list[str], *, cwd: Path = ROOT, env: dict[str, str] | None = None) -> None:
    subprocess.run(cmd, cwd=cwd, env=env, check=True)


def llvm_profdata_identity(path: str | None = None) -> dict[str, str]:
    """Identify the exact llvm-profdata invocation used by this pipeline."""
    command_path = path or shutil.which("llvm-profdata")
    if not command_path:
        raise RuntimeError("Clang PGO requires matching llvm-profdata on PATH")
    # Keep the invocation basename and symlink intact: LLVM tool aliases may
    # dispatch behavior based on argv[0]. Resolve PATH once, not the symlink.
    invocation_path = os.path.abspath(command_path)
    executable = Path(invocation_path)
    if not executable.is_file() or not os.access(invocation_path, os.X_OK):
        raise RuntimeError(f"recorded llvm-profdata is unavailable: {invocation_path}")
    try:
        result = subprocess.run(
            [invocation_path, "--version"], text=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, check=True,
        )
    except (OSError, subprocess.CalledProcessError) as error:
        raise RuntimeError(f"cannot identify llvm-profdata at {invocation_path}") from error
    version = result.stdout.strip()
    if not version:
        raise RuntimeError(f"llvm-profdata returned no version: {invocation_path}")
    return {"path": invocation_path, "version": version,
            "sha256": digest(executable)}


def verify_llvm_profdata_identity(generation: dict, is_clang: bool) -> dict[str, str] | None:
    """Verify and return the generation-time LLVM tool; GCC has no such tool."""
    if not is_clang:
        return None
    expected = generation.get("toolchain", {}).get("profile_tool")
    if (not isinstance(expected, dict)
            or not isinstance(expected.get("path"), str)
            or not expected["path"]):
        raise RuntimeError("generation manifest has no llvm-profdata identity")
    try:
        actual = llvm_profdata_identity(expected.get("path"))
    except RuntimeError as error:
        raise RuntimeError("recorded llvm-profdata identity is unavailable") from error
    if actual != expected:
        raise RuntimeError("llvm-profdata path, version, or executable changed since generation")
    return actual


def semantic_build_identity(mode: str = "control", profile: Path | None = None) -> str:
    cmd = ["make", "--no-print-directory", "--silent", "perf-print-flags", f"PERF_MODE={mode}"]
    if profile:
        cmd.append(f"PERF_PROFILE={profile}")
    output = subprocess.check_output(cmd, cwd=ROOT, env=environment(), text=True, stderr=subprocess.STDOUT)
    try:
        lines = output.split("__PERF_FLAGS_BEGIN__\n", 1)[1].split("__PERF_FLAGS_END__", 1)[0].splitlines()
    except IndexError as error:
        raise RuntimeError("Make did not return compiler/linker feature flags") from error
    if len(lines) != 2:
        raise RuntimeError("Make returned an incomplete compiler/linker flag identity")
    return semantic_flags_identity(" ".join(lines), mode)


def semantic_flags_identity(flags: str, mode: str) -> str:
    import re

    flags = re.sub(r"(?:^|\s)(?:-fprofile-instr-generate|-fprofile-generate(?:=[^\s]+)?|-flto=thin|-flto)(?=\s|$)", " ", flags)
    flags = re.sub(r"(?:^|\s)-fprofile-(?:instr-use|use|dir)=[^\s]+", " ", flags)
    # This one warning classification is intentionally less strict in Clang
    # profile-use than profile generation; all other warning policy stays in
    # the identity so an unrelated flag change invalidates the transition.
    if mode in {"pgo-use", "thinlto-pgo-use"}:
        flags = re.sub(r"(?:^|\s)-Wno-error=profile-instr-unprofiled(?=\s|$)", " ", flags)
    return hashlib.sha256(" ".join(flags.split()).encode()).hexdigest()


def unprofiled_translation_units(build_output: str) -> list[str]:
    import re

    files = re.findall(
        r'warning: no profile data available for file ["“](.+?)["”]',
        build_output,
    )
    return sorted(set(files))


def build_artifacts() -> dict[str, str]:
    manifest = json.loads((ROOT / "tests/performance/workloads.json").read_text())
    names = {workload["command"][0] for workload in manifest["workloads"]}
    names.update(BUILD_ARTIFACTS)
    paths = [ROOT / name for name in sorted(names)]
    paths.extend(sorted((ROOT / "build").glob("libgleditor.*")))
    if sys.platform == "darwin":
        expected_library = ROOT / "build/libgleditor.dylib"
    elif os.name == "nt":
        expected_library = ROOT / "build/libgleditor.dll"
    else:
        expected_library = ROOT / "build/libgleditor.so"
    if not expected_library.is_file():
        raise RuntimeError(f"expected shared library link is missing: {expected_library.relative_to(ROOT)}")
    result = {}
    for path in paths:
        if not path.is_file():
            raise RuntimeError(f"expected build artifact is missing: {path.relative_to(ROOT)}")
        result[str(path.relative_to(ROOT))] = digest(path)
    return result


def profile_records(paths: list[Path]) -> list[dict[str, str]]:
    records = []
    for path in sorted(set(paths)):
        if not path.is_file() or path.stat().st_size == 0:
            raise RuntimeError(f"training profile is missing or empty: {path}")
        records.append({"path": str(path), "sha256": digest(path)})
    return records


def verify_profile_records(records: list[dict[str, str]], verify_digest: bool = True) -> list[Path]:
    paths = [Path(record["path"]) for record in records]
    if not records or any(
        not path.is_file() or path.stat().st_size == 0
        or (verify_digest and digest(path) != record.get("sha256"))
        for path, record in zip(paths, records)
    ):
        raise RuntimeError("a recorded raw profile is missing, empty, or modified")
    return paths


def workload_profile_records(report: dict, split: str, field: str, verify_digest: bool = True) -> dict[str, list[dict[str, str]]]:
    result: dict[str, list[dict[str, str]]] = {}
    for workload in report.get("workloads", []):
        if workload.get("split") != split:
            continue
        reported = [record for sample in workload.get("samples", []) for record in sample.get(field, [])]
        paths: dict[Path, str] = {}
        for record in reported:
            if (not isinstance(record, dict) or not isinstance(record.get("path"), str)
                    or not isinstance(record.get("sha256"), str) or len(record["sha256"]) != 64):
                raise RuntimeError(f"workload {workload.get('id')} has malformed {field} evidence")
            path = Path(record["path"])
            if not path.is_absolute():
                path = ROOT / path
            paths[path] = record.get("sha256", "")
        if not paths:
            raise RuntimeError(f"workload {workload.get('id')} produced no raw profiles")
        records = [{"path": str(path), "sha256": sha} for path, sha in sorted(paths.items())]
        verify_profile_records(records, verify_digest=verify_digest)
        result[workload["id"]] = records
    return result


def current_input_identity(paths: list[str]) -> list[dict[str, object]]:
    result = []
    for name in paths:
        path = ROOT / name
        if not path.is_file():
            raise RuntimeError(f"training input or dependency is missing: {name}")
        result.append({"path": name, "available": True, "size_bytes": path.stat().st_size, "sha256": digest(path)})
    return result


def validate_training_report(report_path: Path, expected_digest: str, expected_split: str = "train") -> dict:
    if not report_path.is_file() or digest(report_path) != expected_digest:
        raise RuntimeError("training report is missing or modified")
    try:
        report = json.loads(report_path.read_text())
        manifest_path = ROOT / "tests/performance/workloads.json"
        manifest = json.loads(manifest_path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError("training report or workload manifest is unreadable") from error
    if report.get("schema_version") != 1 or report.get("split") != expected_split or not report.get("success"):
        raise RuntimeError("training report has an unsupported schema, split, or failed status")
    if report.get("manifest_version") != manifest.get("version") or report.get("manifest_sha256") != digest(manifest_path):
        raise RuntimeError("training workload manifest changed after profile collection")
    if expected_split != "train":
        raise RuntimeError("profile provenance validation expects the train split")
    expected_workloads = {item["id"]: item for item in manifest["workloads"] if item.get("split") == "train"}
    actual_workloads = {item.get("id"): item for item in report.get("workloads", [])}
    if set(actual_workloads) != set(expected_workloads):
        raise RuntimeError("training report workload set differs from the current manifest")
    if (report.get("warmups_requested") != manifest.get("warmups")
            or report.get("repetitions_requested") != manifest.get("repetitions")):
        raise RuntimeError("training repetition recipe changed after profile collection")
    for workload_id, expected in expected_workloads.items():
        actual = actual_workloads[workload_id]
        repetitions = manifest["repetitions"] * expected["weight"]
        expected_format = expected.get("result_format", "gtest_json")
        expected_inputs = current_input_identity(expected.get("inputs", []))
        expected_dependencies = current_input_identity(expected.get("dependencies", []))
        if (actual.get("split") != "train" or actual.get("weight") != expected["weight"]
                or actual.get("training_weight") != expected["weight"]
                or actual.get("repetitions_requested") != manifest["repetitions"]
                or actual.get("realized_measured_iterations") != repetitions
                or actual.get("result_format") != expected_format
                or actual.get("scenario") != expected.get("scenario")
                or actual.get("command_identity", {}).get("argv") != expected["command"]
                or actual.get("inputs") != expected_inputs
                or actual.get("dependencies") != expected_dependencies
                or not actual.get("success")):
            raise RuntimeError(f"training recipe or input identity changed for workload {workload_id}")
        if any(not sample.get("success") for sample in actual.get("samples", [])):
            raise RuntimeError(f"training report contains a failed sample for workload {workload_id}")
    return report


def verify_build_artifacts(gen: dict) -> None:
    if gen.get("artifacts") != build_artifacts():
        raise RuntimeError("instrumented executables or shared library changed between pipeline stages")


def validate_stage_state(gen: dict, action: str, mode: str, source: str, compiler_id: str, target: str) -> None:
    expected = {"train": {"generated", "trained", "training"},
                "merge": {"trained"}, "release": {"merged"}}[action]
    if gen.get("status") not in expected:
        raise RuntimeError(f"stage requires pipeline status in {sorted(expected)!r}; found {gen.get('status')!r}")
    if gen.get("source") != source or gen.get("compiler") != compiler_id or gen.get("target") != target:
        raise RuntimeError("generation is stale for current source/compiler/target")
    if gen.get("mode") != mode:
        raise RuntimeError(f"pipeline was generated in mode {gen.get('mode')!r}, not {mode!r}")


def validate_profile_manifest(meta: dict, source: str, compiler_id: str, profile: Path) -> None:
    if meta.get("version") != 1:
        raise RuntimeError("unsupported release profile manifest version")
    if meta.get("source") != source or meta.get("compiler") != compiler_id:
        raise RuntimeError("merged profile source/compiler provenance is stale")
    actual = digest(profile) if profile.is_file() else digest_tree(profile) if profile.is_dir() else None
    if actual != meta.get("profile_sha256"):
        raise RuntimeError("merged profile is missing, corrupt, or modified")


def atomic_json(path: Path, obj: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + f".{os.getpid()}.tmp")
    tmp.write_text(json.dumps(obj, indent=2, sort_keys=True) + "\n")
    os.replace(tmp, path)


def environment() -> dict[str, str]:
    env = os.environ.copy()
    env.update({
        "SDL_VIDEODRIVER": env.get("SDL_VIDEODRIVER", "offscreen"),
        "SDL_AUDIODRIVER": env.get("SDL_AUDIODRIVER", "dummy"),
        "LIBGL_ALWAYS_SOFTWARE": env.get("LIBGL_ALWAYS_SOFTWARE", "1"),
        "XDG_DATA_HOME": env.get("XDG_DATA_HOME", str(ROOT / "build/xdg/data")),
        "XDG_CONFIG_HOME": env.get("XDG_CONFIG_HOME", str(ROOT / "build/xdg/config")),
        "XDG_CACHE_HOME": env.get("XDG_CACHE_HOME", str(ROOT / "build/xdg/cache")),
    })
    for key in ("XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME"):
        Path(env[key]).mkdir(parents=True, exist_ok=True)
    return env


def probe(cxx: str, is_clang: bool, with_lto: bool) -> dict:
    """Exercise instrumented link, profile merge, and profile-use link first."""
    profile_tool = llvm_profdata_identity() if is_clang else None
    with tempfile.TemporaryDirectory(prefix="gleditor-profile-probe-") as td:
        d = Path(td)
        src = d / "p.cpp"
        src.write_text("int main(){return 7==7?0:1;}\n")
        exe = d / "probe"
        raw = d / "probe-%m-%p.profraw"
        data = d / "probe.profdata"
        if is_clang:
            gen = ["-fprofile-instr-generate"]
            use = [f"-fprofile-instr-use={data}"]
            lto_gen = ["-flto=thin", "-fuse-ld=lld"] if with_lto else []
            lto_use = lto_gen
        else:
            gen = [f"-fprofile-generate={d / 'gcc-profile'}"]
            use = [f"-fprofile-use={d / 'gcc-profile'}"]
            lto_gen = ["-flto"] if with_lto else []
            lto_use = lto_gen
        run([*cxx.split(), "-O2", *lto_gen, *gen, str(src), "-o", str(exe)])
        env = environment()
        env["LLVM_PROFILE_FILE"] = str(raw)
        run([str(exe)], cwd=d, env=env)
        if is_clang:
            profiles = list(d.glob("*.profraw"))
            if not profiles:
                raise RuntimeError("compiler probe produced no raw profile")
            run([profile_tool["path"], "merge", *map(str, profiles), "-o", str(data)])
            if not data.is_file() or data.stat().st_size == 0:
                raise RuntimeError("llvm-profdata produced an empty profile")
            run([*cxx.split(), "-O2", *lto_use, *use, str(src), "-o", str(exe)])
        else:
            run([*cxx.split(), "-O2", *lto_use, *use, str(src), "-o", str(exe)])
        run([str(exe)])
    if is_clang:
        kind = "clang-thinlto-pgo" if with_lto else "clang-pgo"
    else:
        kind = "gcc-lto-pgo-portability" if with_lto else "gcc-pgo-portability"
    return {"kind": kind, "profile_tool": profile_tool}


def make(mode: str, profile: Path | None = None) -> list[str]:
    cmd = ["make", f"-j{os.cpu_count() or 1}", *TARGETS, f"PERF_MODE={mode}"]
    if profile:
        cmd.extend([f"PERF_PROFILE={profile}", f"PERF_PROFILE_DIGEST={digest(profile) if profile.is_file() else digest_tree(profile)}"])
    if mode.endswith("-use") and profile:
        completed = subprocess.run(
            cmd, cwd=ROOT, env=environment(), text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False,
        )
        log = PERF / "profile-use-build.log"
        log.write_text(completed.stdout)
        sys.stdout.write(completed.stdout)
        if completed.returncode:
            raise subprocess.CalledProcessError(completed.returncode, cmd)
        return unprofiled_translation_units(completed.stdout)
    run(cmd, env=environment())
    return []


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("action", choices=["generate", "train", "merge", "release", "digest"])
    parser.add_argument("--mode", choices=["pgo", "thinlto-pgo"], default="pgo")
    parser.add_argument("--path")
    args = parser.parse_args()
    if args.action == "digest":
        if not args.path:
            raise RuntimeError("digest action requires --path")
        path = Path(args.path)
        if not path.exists():
            raise RuntimeError(f"profile path is missing: {path}")
        print(digest(path) if path.is_file() else digest_tree(path))
        return 0
    PERF.mkdir(parents=True, exist_ok=True)
    lock = PERF / "release-profile.lock"
    try:
        fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    except FileExistsError:
        raise RuntimeError(f"profile pipeline already active or interrupted; inspect and remove {lock}")
    os.write(fd, f"pid={os.getpid()} action={args.action} time={time.time()}\n".encode())
    os.close(fd)
    state = PERF / "pipeline.json"
    try:
        cxx, is_clang, version = compiler()
        profile_tool = None
        if args.action != "generate":
            if not state.exists():
                raise RuntimeError("no generation manifest; run make perf-pgo-generate first")
            gen = json.loads(state.read_text())
            validate_stage_state(gen, args.action, args.mode, source_identity(), version, subprocess.check_output([*cxx.split(), "-dumpmachine"], text=True).strip())
            profile_tool = verify_llvm_profdata_identity(gen, is_clang)
        if args.action == "generate":
            info = probe(cxx, is_clang, args.mode == "thinlto-pgo")
            source = source_identity()
            if args.mode == "pgo":
                mode = "pgo-generate"
            elif is_clang:
                mode = "thinlto-pgo-generate"
            else:
                mode = "lto-pgo-generate"
            atomic_json(state, {"version": 1, "status": "generating", "source": source, "compiler": version, "target": subprocess.check_output([*cxx.split(), "-dumpmachine"], text=True).strip(), "toolchain": info, "mode": mode})
            make(mode)
            if source_identity() != source:
                raise RuntimeError("source or build inputs changed during instrumented generation")
            atomic_json(state, {"version": 1, "status": "generated", "source": source_identity(), "compiler": version, "target": subprocess.check_output([*cxx.split(), "-dumpmachine"], text=True).strip(), "semantic_build": semantic_build_identity(mode), "buildflags_sha256": digest(ROOT / "build/.buildflags"), "artifacts": build_artifacts(), "toolchain": info, "mode": args.mode, "build_mode": mode, "started": time.time()})
        elif args.action == "train":
            verify_build_artifacts(gen)
            if gen.get("buildflags_sha256") != digest(ROOT / "build/.buildflags"):
                raise RuntimeError("instrumented build flags changed before training")
            # Reset raw counts even on a repeated or recovered training stage.
            gen["status"] = "training"
            atomic_json(state, gen)
            out = PERF / "train-report.json"
            rawdir = PERF / "raw"
            shutil.rmtree(rawdir, ignore_errors=True)
            rawdir.mkdir(parents=True)
            gccdir = PERF / "gcc-profile"
            if not is_clang:
                shutil.rmtree(gccdir, ignore_errors=True)
                gccdir.mkdir(parents=True)
            env = environment()
            env["LLVM_PROFILE_FILE"] = str(rawdir / "%m-%p.profraw")
            if not is_clang:
                # -fprofile-generate=<dir> already relocates GCC output. GCOV_PREFIX
                # corrupts GCC's LTO runtime profile path on affected toolchains.
                env["GLEDITOR_GCC_PROFILE_DIR"] = str(gccdir)
            run([sys.executable, "tools/performance-report.py", "run", "--split", "train", "--output", str(out)], env=env)
            report = json.loads(out.read_text())
            if report.get("split") != "train" or report.get("schema_version") != 1:
                raise RuntimeError("training runner returned an unsupported report")
            if not report.get("success") or any(
                not sample.get("success")
                for workload in report.get("workloads", [])
                for sample in workload.get("samples", [])
            ):
                raise RuntimeError("training report contains a failed workload run")
            if source_identity() != gen.get("source"):
                raise RuntimeError("source or build inputs changed during training")
            verify_build_artifacts(gen)
            if gen.get("buildflags_sha256") != digest(ROOT / "build/.buildflags"):
                raise RuntimeError("instrumented build flags changed during training")
            profile_field = "profile_outputs" if is_clang else "gcc_profile_outputs"
            workload_profiles = workload_profile_records(report, "train", profile_field, verify_digest=is_clang)
            if not workload_profiles or any(not records for records in workload_profiles.values()):
                raise RuntimeError("one or more training workloads produced no raw profile")
            raw_records = profile_records(sorted({Path(record["path"]) for records in workload_profiles.values() for record in records})) if is_clang else profile_records(sorted(gccdir.rglob("*.gcda")))
            gen.update({"status": "trained", "train_report": str(out), "train_report_sha256": digest(out), "raw_profiles": raw_records, "workload_profiles": workload_profiles, "profile_dir": str(gccdir) if not is_clang else None})
            atomic_json(state, gen)
        elif args.action == "merge":
            if gen.get("status") != "trained":
                raise RuntimeError("training stage is missing or incomplete")
            report_path = Path(gen["train_report"])
            report = validate_training_report(report_path, gen.get("train_report_sha256", ""))
            if gen.get("semantic_build") != semantic_build_identity(gen["build_mode"]):
                raise RuntimeError("compile/link feature flags changed since profile generation")
            if gen.get("buildflags_sha256") != digest(ROOT / "build/.buildflags"):
                raise RuntimeError("instrumented build flags changed before profile merge")
            verify_build_artifacts(gen)
            if is_clang:
                raw_records = gen.get("raw_profiles", [])
                raw = verify_profile_records(raw_records)
                if not gen.get("workload_profiles") or any(not records for records in gen["workload_profiles"].values()):
                    raise RuntimeError("a training workload has no recorded profile output")
                for records in gen["workload_profiles"].values():
                    verify_profile_records(records, verify_digest=is_clang)
                tmp = PERF / "release.profdata.tmp"
                assert profile_tool is not None
                run([profile_tool["path"], "merge", *map(str, raw), "-o", str(tmp)])
                if not tmp.is_file() or tmp.stat().st_size == 0:
                    raise RuntimeError("profile merge did not produce data")
                final = PERF / "release.profdata"
                os.replace(tmp, final)
                profile = final
            else:
                # GCC's .gcda set is consumed in place; hash the whole tree.
                profile = Path(gen["profile_dir"])
                raw_records = gen.get("raw_profiles", [])
                verify_profile_records(raw_records)
                if not gen.get("workload_profiles") or any(not records for records in gen["workload_profiles"].values()):
                    raise RuntimeError("a training workload has no recorded profile output")
                for records in gen["workload_profiles"].values():
                    verify_profile_records(records, verify_digest=False)
            if source_identity() != gen.get("source"):
                raise RuntimeError("source or build inputs changed during profile merge")
            validate_training_report(report_path, gen["train_report_sha256"])
            meta = {"version": 1, "source": gen["source"], "compiler": version, "profile": str(profile), "profile_sha256": digest(profile) if profile.is_file() else digest_tree(profile), "training_report_sha256": gen["train_report_sha256"], "toolchain": gen["toolchain"], "mode": gen["mode"]}
            atomic_json(PERF / "release-profile.json", meta)
            gen["status"] = "merged"
            gen["profile_manifest_sha256"] = digest(PERF / "release-profile.json")
            atomic_json(state, gen)
        else:
            meta_path = PERF / "release-profile.json"
            if not meta_path.is_file():
                raise RuntimeError("merged profile manifest missing; refusing unprofiled release")
            meta = json.loads(meta_path.read_text())
            if gen.get("profile_manifest_sha256") != digest(meta_path) or meta.get("mode") != args.mode:
                raise RuntimeError("merged profile manifest is stale or belongs to another pipeline mode")
            prof = Path(meta["profile"])
            if meta.get("training_report_sha256") != gen.get("train_report_sha256"):
                raise RuntimeError("merged profile references a different training report")
            report = validate_training_report(Path(gen["train_report"]), gen.get("train_report_sha256", ""))
            validate_profile_manifest(meta, source_identity(), version, prof)
            if args.mode == "pgo":
                use_mode = "pgo-use"
            elif is_clang:
                use_mode = "thinlto-pgo-use"
            else:
                use_mode = "lto-pgo-use"
            if gen.get("semantic_build") != semantic_build_identity(use_mode, prof):
                raise RuntimeError("compile/link feature flags changed since profile generation")
            if gen.get("buildflags_sha256") != digest(ROOT / "build/.buildflags"):
                raise RuntimeError("instrumented build flags changed before profile-use transition")
            verify_build_artifacts(gen)
            release_source = source_identity()
            unprofiled_tus = make(use_mode, prof) or []
            if source_identity() != release_source or release_source != gen.get("source"):
                raise RuntimeError("source or build inputs changed during profile-use build")
            release_artifacts = build_artifacts()
            validation_report = PERF / "validation-report.json"
            run([sys.executable, "tools/performance-report.py", "run", "--split", "validation", "--output", str(validation_report)], env=environment())
            validation = json.loads(validation_report.read_text())
            if source_identity() != release_source:
                raise RuntimeError("source or build inputs changed during held-out validation")
            validate_training_report(Path(gen["train_report"]), gen["train_report_sha256"])
            if not validation.get("success"):
                raise RuntimeError("profile-use release failed held-out validation workloads")
            if release_artifacts != build_artifacts():
                raise RuntimeError("release binaries or shared library changed during held-out validation")
            bins = [{"path": name, "sha256": sha} for name, sha in sorted(release_artifacts.items())]
            build_log = PERF / "profile-use-build.log"
            atomic_json(PERF / "release-artifacts.json", {"version": 1, "source": source_identity(), "profile_manifest_sha256": digest(meta_path), "validation_report_sha256": digest(validation_report), "compiler": version, "profile_use_policy": {"unprofiled_translation_units": unprofiled_tus, "build_log": str(build_log.relative_to(ROOT)), "build_log_sha256": digest(build_log), "profile_instr_out_of_date_is_fatal": True, "entrypoint_workloads_are_coverage_only": True}, "artifacts": bins})
            print("Built profile-use release artifacts:", len(bins))
    finally:
        lock.unlink(missing_ok=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.CalledProcessError, OSError, json.JSONDecodeError) as exc:
        print(f"release-profile: {exc}", file=sys.stderr)
        raise SystemExit(1)
