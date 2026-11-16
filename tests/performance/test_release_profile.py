"""Focused provenance and mocked lifecycle checks for M1."""
import importlib.util
import json
import os
from contextlib import ExitStack
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

MODULE_PATH = Path(__file__).resolve().parents[2] / "tools/release_profile.py"
spec = importlib.util.spec_from_file_location("release_profile", MODULE_PATH)
release_profile = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(release_profile)


class ProfileProvenanceTest(unittest.TestCase):
    def test_accepts_matching_profile_digest(self):
        with tempfile.TemporaryDirectory() as temporary:
            profile = Path(temporary) / "release.profdata"
            profile.write_bytes(b"valid profile bytes")
            metadata = {"version": 1, "source": "source-id", "compiler": "compiler-id",
                        "profile_sha256": release_profile.digest(profile)}
            release_profile.validate_profile_manifest(metadata, "source-id", "compiler-id", profile)

    def test_rejects_changed_profile_at_same_path(self):
        with tempfile.TemporaryDirectory() as temporary:
            profile = Path(temporary) / "release.profdata"
            profile.write_bytes(b"original")
            metadata = {"version": 1, "source": "source-id", "compiler": "compiler-id",
                        "profile_sha256": release_profile.digest(profile)}
            profile.write_bytes(b"replacement")
            with self.assertRaisesRegex(RuntimeError, "missing, corrupt, or modified"):
                release_profile.validate_profile_manifest(metadata, "source-id", "compiler-id", profile)

    def test_rejects_source_and_compiler_mismatch(self):
        with tempfile.TemporaryDirectory() as temporary:
            profile = Path(temporary) / "release.profdata"
            profile.write_bytes(b"valid")
            metadata = {"version": 1, "source": "source-id", "compiler": "compiler-id",
                        "profile_sha256": release_profile.digest(profile)}
            with self.assertRaisesRegex(RuntimeError, "provenance is stale"):
                release_profile.validate_profile_manifest(metadata, "changed-source", "compiler-id", profile)
            with self.assertRaisesRegex(RuntimeError, "provenance is stale"):
                release_profile.validate_profile_manifest(metadata, "source-id", "changed-compiler", profile)

    def test_enforces_sequential_pipeline_lifecycle(self):
        base = {"source": "s", "compiler": "c", "target": "t", "mode": "thinlto-pgo"}
        transitions = [("generated", "train"), ("trained", "train"),
                       ("training", "train"), ("trained", "merge"), ("merged", "release")]
        for status, action in transitions:
            with self.subTest(status=status):
                release_profile.validate_stage_state({**base, "status": status}, action,
                                                      "thinlto-pgo", "s", "c", "t")
        with self.assertRaisesRegex(RuntimeError, "requires pipeline status"):
            release_profile.validate_stage_state({**base, "status": "generated"}, "merge",
                                                  "thinlto-pgo", "s", "c", "t")
        with self.assertRaisesRegex(RuntimeError, "source/compiler/target"):
            release_profile.validate_stage_state({**base, "status": "generated"}, "train",
                                                  "thinlto-pgo", "different", "c", "t")
        with self.assertRaisesRegex(RuntimeError, "was generated in mode"):
            release_profile.validate_stage_state({**base, "status": "generated"}, "train",
                                                  "pgo", "s", "c", "t")

    @unittest.skipUnless(shutil.which("clang++") and shutil.which("llvm-profdata"),
                         "Clang profile identity check needs Clang and llvm-profdata")
    def test_make_generation_and_use_flags_share_only_expected_warning_policy(self):
        with tempfile.TemporaryDirectory() as temporary:
            profile = Path(temporary) / "profile.profdata"
            profile.write_bytes(b"identity query only")
            with patch.dict(os.environ, {"CXX": shutil.which("clang++")}):
                generation = release_profile.semantic_build_identity(
                    "thinlto-pgo-generate"
                )
                use = release_profile.semantic_build_identity(
                    "thinlto-pgo-use", profile
                )
            self.assertEqual(generation, use)

        expected = release_profile.semantic_flags_identity(
            "-O3 -Wno-error=profile-instr-unprofiled", "thinlto-pgo-use"
        )
        other_warning = release_profile.semantic_flags_identity(
            "-O3 -Wno-error=other-warning", "thinlto-pgo-use"
        )
        plain = release_profile.semantic_flags_identity("-O3", "thinlto-pgo-generate")
        self.assertEqual(expected, plain)
        self.assertNotEqual(other_warning, plain)

    def test_reports_actual_unprofiled_translation_units(self):
        output = (
            'warning: no profile data available for file “apps/xuzz/cold.cpp” '
            '[-Wprofile-instr-unprofiled]\n'
            'warning: profile data may be out of date: one function mismatched '
            '[-Wprofile-instr-out-of-date]\n'
        )
        self.assertEqual(
            release_profile.unprofiled_translation_units(output),
            ["apps/xuzz/cold.cpp"],
        )

    def test_profile_records_detect_same_path_replacement(self):
        with tempfile.TemporaryDirectory() as temporary:
            profile = Path(temporary) / "workload.profraw"
            profile.write_bytes(b"instrumented counters")
            records = release_profile.profile_records([profile])
            self.assertEqual(release_profile.verify_profile_records(records), [profile])
            profile.write_bytes(b"replacement counters")
            with self.assertRaisesRegex(RuntimeError, "modified"):
                release_profile.verify_profile_records(records)

    def test_requires_profiles_per_training_workload(self):
        with tempfile.TemporaryDirectory() as temporary:
            first = Path(temporary) / "first.profraw"
            first.write_bytes(b"first")
            report = {"workloads": [
                {"id": "one", "split": "train", "samples": [{"profile_outputs": [
                    {"path": str(first), "sha256": release_profile.digest(first)}]}]},
                {"id": "two", "split": "train", "samples": [{"profile_outputs": []}]},
                {"id": "heldout", "split": "validation", "samples": []},
            ]}
            with self.assertRaisesRegex(RuntimeError, "two produced no raw profiles"):
                release_profile.workload_profile_records(report, "train", "profile_outputs")
            report["workloads"].pop(1)
            records = release_profile.workload_profile_records(report, "train", "profile_outputs")
            self.assertEqual(set(records), {"one"})

    def test_rejects_missing_profile(self):
        metadata = {"version": 1, "source": "s", "compiler": "c", "profile_sha256": "abc"}
        with self.assertRaisesRegex(RuntimeError, "missing, corrupt, or modified"):
            release_profile.validate_profile_manifest(metadata, "s", "c", Path("missing.profdata"))

    @unittest.skipUnless(shutil.which("clang++") and shutil.which("llvm-profdata"),
                         "cold-function PGO check needs Clang and llvm-profdata")
    def test_default_profile_merge_retains_cold_function_records(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            source = directory / "cold.cpp"
            source.write_text(
                "__attribute__((noinline)) int cold_function(int x) { return x + 7; }\n"
                "__attribute__((noinline)) int warm_function() { return 42; }\n"
                "int main() { return warm_function() == 42 ? 0 : 1; }\n"
            )
            executable = directory / "cold"
            raw = directory / "run.profraw"
            full = directory / "full.profdata"
            sparse = directory / "sparse.profdata"
            subprocess.run([shutil.which("clang++"), "-O2", "-fprofile-instr-generate",
                            str(source), "-o", str(executable)], check=True,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            env = release_profile.environment()
            env["LLVM_PROFILE_FILE"] = str(raw)
            subprocess.run([str(executable)], env=env, check=True,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            profdata = shutil.which("llvm-profdata")
            subprocess.run([profdata, "merge", str(raw), "-o", str(full)], check=True,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            subprocess.run([profdata, "merge", "-sparse", str(raw), "-o", str(sparse)],
                           check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            full_show = subprocess.run([profdata, "show", "--all-functions", "--counts", str(full)],
                                       check=True, text=True, stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE).stdout
            sparse_show = subprocess.run([profdata, "show", "--all-functions", "--counts", str(sparse)],
                                         check=True, text=True, stdout=subprocess.PIPE,
                                         stderr=subprocess.PIPE).stdout
            self.assertIn("_Z13cold_functioni", full_show)
            self.assertIn("Function count: 0", full_show.split("_Z13cold_functioni", 1)[1])
            self.assertNotIn("_Z13cold_functioni", sparse_show)

    @unittest.skipUnless(shutil.which("clang++") and shutil.which("llvm-profdata"),
                         "multi-executable profile check needs Clang and llvm-profdata")
    def test_merged_profiles_cover_distinct_main_hashes_and_keep_mismatch_fatal(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            first_source = directory / "first.cpp"
            second_source = directory / "second.cpp"
            first_source.write_text(
                "int main(int argc, char**) { return argc == 1 ? 0 : 1; }\n"
            )
            second_source.write_text(
                "int main(int argc, char**) { if (argc > 3) return 1; "
                "return argc == 1 ? 0 : 2; }\n"
            )
            first_exe = directory / "first"
            second_exe = directory / "second"
            for source, executable in ((first_source, first_exe),
                                       (second_source, second_exe)):
                subprocess.run(
                    [shutil.which("clang++"), "-O2", "-fprofile-instr-generate",
                     str(source), "-o", str(executable)],
                    check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                )
            env = release_profile.environment()
            for executable, raw in ((first_exe, directory / "first.profraw"),
                                    (second_exe, directory / "second.profraw")):
                sample_env = env.copy()
                sample_env["LLVM_PROFILE_FILE"] = str(raw)
                subprocess.run([str(executable)], env=sample_env, check=True,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            profdata = shutil.which("llvm-profdata")
            merged = directory / "merged.profdata"
            subprocess.run([profdata, "merge", str(directory / "first.profraw"),
                            str(directory / "second.profraw"), "-o", str(merged)],
                           check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

            for source, executable in ((first_source, directory / "first-use"),
                                       (second_source, directory / "second-use")):
                subprocess.run(
                    [shutil.which("clang++"), "-O2", "-Werror",
                     f"-fprofile-instr-use={merged}", str(source), "-o", str(executable)],
                    check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                )

            only_first = directory / "first-only.profdata"
            subprocess.run([profdata, "merge", str(directory / "first.profraw"),
                            "-o", str(only_first)],
                           check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            mismatch = subprocess.run(
                [shutil.which("clang++"), "-O2", "-Werror",
                 f"-fprofile-instr-use={only_first}", str(second_source),
                 "-o", str(directory / "mismatch")],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            )
            self.assertNotEqual(mismatch.returncode, 0)
            self.assertIn("profile data may be out of date", mismatch.stderr)

            cold_source = directory / "cold.cpp"
            cold_source.write_text("int untrained_helper() { return 7; }\n")
            cold = subprocess.run(
                [shutil.which("clang++"), "-O2", "-Werror",
                 "-Wno-error=profile-instr-unprofiled",
                 f"-fprofile-instr-use={merged}", "-c", str(cold_source),
                 "-o", str(directory / "cold.o")],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            )
            self.assertEqual(cold.returncode, 0, cold.stderr)
            self.assertIn("no profile data available for file", cold.stderr)


class LLVMProfileToolIdentityTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.tool = self.root / "bin" / "llvm-profdata"
        self.tool.parent.mkdir()
        self.write_tool("LLVM profile data tool fake-version-1\n")

    def tearDown(self):
        self.temporary.cleanup()

    def write_tool(self, version_output: str, extra: str = ""):
        self.tool.write_text(
            "#!/bin/sh\n"
            f"# {extra}\n"
            f"printf '%s\\n' '{version_output.strip()}'\n"
        )
        self.tool.chmod(0o755)

    def test_identity_preserves_the_invocation_symlink_path(self):
        alias = self.tool.parent / "llvm-profdata-alias"
        alias.symlink_to(self.tool.name)
        identity = release_profile.llvm_profdata_identity(str(alias))
        self.assertEqual(identity["path"], str(alias.absolute()))
        self.assertEqual(identity["version"], "LLVM profile data tool fake-version-1")
        self.assertEqual(identity["sha256"], release_profile.digest(alias))

    def test_rejects_same_path_and_version_replacement_by_digest(self):
        recorded = release_profile.llvm_profdata_identity(str(self.tool))
        self.write_tool("LLVM profile data tool fake-version-1", "replacement binary")
        generation = {"toolchain": {"profile_tool": recorded}}
        with self.assertRaisesRegex(RuntimeError, "path, version, or executable changed"):
            release_profile.verify_llvm_profdata_identity(generation, True)

    def test_rejects_version_replacement_and_preserves_gcc_null_tool(self):
        recorded = release_profile.llvm_profdata_identity(str(self.tool))
        self.write_tool("LLVM profile data tool fake-version-2")
        generation = {"toolchain": {"profile_tool": recorded}}
        with self.assertRaisesRegex(RuntimeError, "path, version, or executable changed"):
            release_profile.verify_llvm_profdata_identity(generation, True)
        self.assertIsNone(release_profile.verify_llvm_profdata_identity(
            {"toolchain": {"profile_tool": None}}, False))


class ReleaseTargetCoverageTest(unittest.TestCase):
    def test_explicit_release_targets_cover_conditional_build_helpers(self):
        self.assertIn("shader_assemble", release_profile.TARGETS)
        self.assertIn("xudu-swarm-peer", release_profile.TARGETS)
        self.assertIn("build/shader_assemble", release_profile.BUILD_ARTIFACTS)
        self.assertIn("build/xudu-swarm-peer", release_profile.BUILD_ARTIFACTS)

        repo = MODULE_PATH.parents[1]
        manifest = json.loads((repo / "tests/performance/workloads.json").read_text())
        peer = next(workload for workload in manifest["workloads"]
                    if workload["id"] == "swarm_peer_entrypoint_train")
        self.assertEqual(peer["command"], ["build/xudu-swarm-peer", "--help"])
        self.assertIn("tools/xudu-swarm-peer.cpp", peer["dependencies"])

        makefile = (repo / "Makefile").read_text()
        rule = next(line for line in makefile.splitlines()
                    if line.startswith("$(OBJDIR)/shader_assemble:"))
        self.assertIn("$(FLAGSTAMP)", rule)
        recipe = makefile.split(rule, 1)[1].splitlines()[1]
        self.assertIn("$(LDFLAGS)", recipe)
        self.assertIn("$(filter %.cpp,$^)", recipe)


class MockedPipelineTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.perf = self.root / "build/performance"
        self.input = self.root / "tests/performance/fixture.txt"
        self.input.parent.mkdir(parents=True)
        self.input.write_text("training fixture v1\n")
        self.dependency = self.root / "tests/performance/driver.cpp"
        self.dependency.write_text("// fixed workload implementation\n")
        self.manifest_path = self.root / "tests/performance/workloads.json"
        self.manifest_path.parent.mkdir(parents=True, exist_ok=True)
        self.manifest = {"version": 1, "warmups": 0, "repetitions": 1, "workloads": [
            {"id": "train-one", "split": "train", "weight": 1,
             "command": ["build/xuzz", "{input}"], "result_format": "exit_status",
             "inputs": ["tests/performance/fixture.txt"],
             "dependencies": ["tests/performance/driver.cpp"], "scenario": "train scenario"},
            {"id": "validation-one", "split": "validation", "weight": 1,
             "command": ["build/gleditor", "{input}"], "result_format": "exit_status",
             "inputs": ["tests/performance/fixture.txt"], "dependencies": [],
             "scenario": "validation scenario"},
        ]}
        self.manifest_path.write_text(json.dumps(self.manifest))
        self.source = ["stable-source"]
        self.profdata_tool = self.root / "bin/llvm-profdata"
        self.profdata_tool.parent.mkdir(parents=True)
        self.write_profdata_tool("fake-llvm-profdata version 1\n")
        self.profile_tool = release_profile.llvm_profdata_identity(str(self.profdata_tool))
        self.validation_success = True
        self.drop_required_release_output = False
        self.run_calls = []
        self.make_calls = []
        self.stack = ExitStack()
        self.stack.enter_context(patch.object(release_profile, "ROOT", self.root))
        self.stack.enter_context(patch.object(release_profile, "PERF", self.perf))
        self.stack.enter_context(patch.object(release_profile, "source_identity", side_effect=lambda: self.source[0]))
        self.stack.enter_context(patch.object(release_profile, "compiler", return_value=("fake-c++", True, "fake clang")))
        self.stack.enter_context(patch.object(release_profile, "probe", return_value={
            "kind": "mock-clang", "profile_tool": self.profile_tool,
        }))
        self.stack.enter_context(patch.object(release_profile, "semantic_build_identity", return_value="feature-id"))
        self.stack.enter_context(patch.object(release_profile.subprocess, "check_output", return_value="fake-target\n"))
        self.stack.enter_context(patch.object(release_profile, "make", side_effect=self.fake_make))
        self.stack.enter_context(patch.object(release_profile, "run", side_effect=self.fake_run))

    def tearDown(self):
        self.stack.close()
        self.temporary.cleanup()

    def write_profdata_tool(self, version_output, extra=""):
        self.profdata_tool.write_text(
            "#!/bin/sh\n"
            f"# {extra}\n"
            f"printf '%s\\n' '{version_output.strip()}'\n"
        )
        self.profdata_tool.chmod(0o755)

    def fake_make(self, mode, profile=None):
        self.make_calls.append((mode, profile))
        (self.root / "build/.buildflags").parent.mkdir(parents=True, exist_ok=True)
        (self.root / "build/.buildflags").write_text(mode + "\n")
        paths = set(release_profile.BUILD_ARTIFACTS) | {"build/libgleditor.so"}
        if mode.endswith("-pgo-use") and self.drop_required_release_output:
            paths.remove("build/gleditor")
            (self.root / "build/gleditor").unlink(missing_ok=True)
        for relative in paths:
            path = self.root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(mode + ":" + relative)
        if mode.endswith("-use"):
            log = self.perf / "profile-use-build.log"
            log.parent.mkdir(parents=True, exist_ok=True)
            log.write_text("fake profile-use build log\n")

    def training_report(self, split):
        workloads = [item for item in self.manifest["workloads"] if item["split"] == split]
        report_workloads = []
        for workload in workloads:
            inputs = release_profile.current_input_identity(workload["inputs"])
            dependencies = release_profile.current_input_identity(workload["dependencies"])
            sample = {"success": True, "profile_outputs": []}
            if split == "train":
                profile = self.root / "build/performance/raw/train-one/run.profraw"
                profile.parent.mkdir(parents=True, exist_ok=True)
                profile.write_bytes(b"profile bytes")
                sample["profile_outputs"] = [{"path": str(profile.relative_to(self.root)),
                                              "sha256": release_profile.digest(profile)}]
            report_workloads.append({
                "id": workload["id"], "split": split, "weight": workload["weight"],
                "training_weight": workload["weight"], "repetitions_requested": 1,
                "realized_measured_iterations": 1, "result_format": workload["result_format"],
                "scenario": workload["scenario"], "command_identity": {"argv": workload["command"]},
                "inputs": inputs, "dependencies": dependencies, "success": True,
                "samples": [sample],
            })
        return {"schema_version": 1, "manifest_version": 1,
                "manifest_sha256": release_profile.digest(self.manifest_path),
                "split": split, "success": self.validation_success if split == "validation" else True,
                "warmups_requested": 0, "repetitions_requested": 1, "workloads": report_workloads}

    def fake_run(self, command, *, cwd=release_profile.ROOT, env=None):
        self.run_calls.append(command)
        if any("performance-report.py" in part for part in command):
            split = command[command.index("--split") + 1]
            output = Path(command[command.index("--output") + 1])
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_text(json.dumps(self.training_report(split)))
        elif "-o" in command:
            Path(command[command.index("-o") + 1]).write_bytes(b"merged profile")

    def invoke(self, action):
        with patch.object(sys, "argv", ["release_profile.py", action, "--mode", "thinlto-pgo"]):
            return release_profile.main()

    def prepare_merged_profile(self):
        self.invoke("generate")
        self.invoke("train")
        self.invoke("merge")

    def test_mocked_full_lifecycle_runs_validation_before_artifact_manifest(self):
        self.assertEqual(self.invoke("generate"), 0)
        self.assertEqual(json.loads((self.perf / "pipeline.json").read_text())["status"], "generated")
        self.assertEqual(self.invoke("train"), 0)
        self.assertEqual(json.loads((self.perf / "pipeline.json").read_text())["status"], "trained")
        self.assertEqual(self.invoke("merge"), 0)
        state = json.loads((self.perf / "pipeline.json").read_text())
        self.assertEqual(state["status"], "merged")
        self.assertEqual(state["profile_manifest_sha256"], release_profile.digest(self.perf / "release-profile.json"))
        profile_meta = json.loads((self.perf / "release-profile.json").read_text())
        self.assertEqual(profile_meta["training_report_sha256"], state["train_report_sha256"])
        self.assertEqual(self.invoke("release"), 0)
        artifacts = json.loads((self.perf / "release-artifacts.json").read_text())
        self.assertEqual(artifacts["validation_report_sha256"], release_profile.digest(self.perf / "validation-report.json"))
        policy = artifacts["profile_use_policy"]
        self.assertEqual(policy["unprofiled_translation_units"], [])
        self.assertEqual(policy["build_log_sha256"], release_profile.digest(self.perf / "profile-use-build.log"))
        self.assertTrue(policy["profile_instr_out_of_date_is_fatal"])
        self.assertEqual({item["path"] for item in artifacts["artifacts"]},
                         set(release_profile.BUILD_ARTIFACTS) | {"build/libgleditor.so"})
        self.assertEqual([call[0] for call in self.make_calls], ["thinlto-pgo-generate", "thinlto-pgo-use"])
        merge_calls = [call for call in self.run_calls if "merge" in call]
        self.assertEqual(len(merge_calls), 1)
        self.assertEqual(merge_calls[0][0], self.profile_tool["path"])
        self.assertNotIn("-sparse", merge_calls[0])

    def test_profile_tool_is_rechecked_before_training(self):
        self.invoke("generate")
        self.write_profdata_tool("fake-llvm-profdata version 1", "replacement")
        with self.assertRaisesRegex(RuntimeError, "path, version, or executable changed"):
            self.invoke("train")
        self.assertFalse(self.run_calls)

    def test_profile_tool_is_rechecked_before_merge(self):
        self.invoke("generate")
        self.invoke("train")
        self.write_profdata_tool("fake-llvm-profdata version 1", "replacement")
        with self.assertRaisesRegex(RuntimeError, "path, version, or executable changed"):
            self.invoke("merge")
        self.assertFalse((self.perf / "release-profile.json").exists())

    def test_profile_tool_is_rechecked_before_release(self):
        self.prepare_merged_profile()
        self.write_profdata_tool("fake-llvm-profdata version 1", "replacement")
        with self.assertRaisesRegex(RuntimeError, "path, version, or executable changed"):
            self.invoke("release")
        self.assertFalse((self.perf / "release-artifacts.json").exists())

    def test_fixture_mutation_after_training_refuses_merge(self):
        self.invoke("generate")
        self.invoke("train")
        self.input.write_text("training fixture changed\n")
        with self.assertRaisesRegex(RuntimeError, "input identity changed"):
            self.invoke("merge")
        self.assertFalse((self.perf / "release-profile.json").exists())

    def test_fixture_mutation_after_merge_refuses_release(self):
        self.prepare_merged_profile()
        self.input.write_text("training fixture changed\n")
        self.dependency.write_text("// changed dependency\n")
        with self.assertRaisesRegex(RuntimeError, "input identity changed"):
            self.invoke("release")
        self.assertFalse((self.perf / "release-artifacts.json").exists())

    def test_source_change_during_generation_does_not_mark_success(self):
        original_make = self.fake_make
        def mutate_source(mode, profile=None):
            original_make(mode, profile)
            self.source[0] = "changed-source"
        with patch.object(release_profile, "make", side_effect=mutate_source):
            with self.assertRaisesRegex(RuntimeError, "changed during instrumented generation"):
                self.invoke("generate")
        self.assertEqual(json.loads((self.perf / "pipeline.json").read_text())["status"], "generating")

    def test_wrong_lock_refuses_stage_without_side_effects(self):
        self.perf.mkdir(parents=True)
        (self.perf / "release-profile.lock").write_text("another process")
        with self.assertRaisesRegex(RuntimeError, "already active or interrupted"):
            self.invoke("generate")
        self.assertEqual(self.make_calls, [])

    def test_failed_validation_or_missing_output_never_writes_release_manifest(self):
        self.prepare_merged_profile()
        self.validation_success = False
        with self.assertRaisesRegex(RuntimeError, "held-out validation"):
            self.invoke("release")
        self.assertFalse((self.perf / "release-artifacts.json").exists())
        self.validation_success = True
        self.prepare_merged_profile()
        self.drop_required_release_output = True
        with self.assertRaisesRegex(RuntimeError, "expected build artifact is missing"):
            self.invoke("release")
        self.assertFalse((self.perf / "release-artifacts.json").exists())
        validation_calls = [call for call in self.run_calls
                            if any("performance-report.py" in part for part in call)
                            and call[call.index("--split") + 1] == "validation"]
        self.assertEqual(len(validation_calls), 1)

    def test_wrong_stage_state_refuses_merge(self):
        self.perf.mkdir(parents=True)
        release_profile.atomic_json(self.perf / "pipeline.json", {
            "version": 1, "status": "generating", "source": self.source[0],
            "compiler": "fake clang", "target": "fake-target", "mode": "thinlto-pgo",
        })
        with self.assertRaisesRegex(RuntimeError, "requires pipeline status"):
            self.invoke("merge")
        self.assertEqual(self.make_calls, [])
        self.assertEqual(self.run_calls, [])


if __name__ == "__main__":
    unittest.main()
