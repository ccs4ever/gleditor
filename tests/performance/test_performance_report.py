"""Focused checks for workload manifest and report distribution semantics."""

import importlib.util
import json
import os
import subprocess
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "performance_report", ROOT / "tools/performance-report.py"
)
performance_report = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(performance_report)


class PerformanceReportTest(unittest.TestCase):
    def test_manifest_labels_concrete_files_and_synthetic_test_sources_separately(self):
        manifest = performance_report.load_manifest()
        train = [item for item in manifest["workloads"] if item["split"] == "train"]
        validation = [item for item in manifest["workloads"] if item["split"] == "validation"]
        self.assertTrue(train)
        self.assertTrue(validation)
        train_inputs = {name for item in train for name in item["inputs"]}
        validation_inputs = {name for item in validation for name in item["inputs"]}
        self.assertFalse(train_inputs & validation_inputs)
        self.assertTrue(any(name.endswith("editor.txt") for name in train_inputs))
        self.assertTrue(any(name.endswith("editor.txt") for name in validation_inputs))
        self.assertIn("no held-out corpus claim", manifest["corpus_identity_claim"])
        train_dependencies = {name for item in train
                              if item["result_format"] == "gtest_json"
                              for name in item.get("dependencies", [])}
        validation_dependencies = {name for item in validation
                                   if item["result_format"] == "gtest_json"
                                   for name in item.get("dependencies", [])}
        self.assertFalse(train_dependencies & validation_dependencies)
        probe_train = {item["probe_scenario"]: item["seed"] for item in train
                       if item["result_format"] == "probe_json"}
        probe_validation = {item["probe_scenario"]: item["seed"] for item in validation
                            if item["result_format"] == "probe_json"}
        self.assertEqual(probe_train.keys(), probe_validation.keys())
        self.assertTrue(all(probe_train[key] != probe_validation[key]
                            for key in probe_train))
        self.assertTrue(all(isinstance(item["weight"], int) and item["weight"] > 0
                            for item in manifest["workloads"]))

    def test_training_weight_controls_realized_repetitions(self):
        self.assertEqual(performance_report.effective_repetitions("train", 3, 2), 6)
        self.assertEqual(performance_report.effective_repetitions("validation", 3, 2), 3)

    def test_run_loop_uses_weight_for_actual_child_sample_count(self):
        with tempfile.TemporaryDirectory(prefix="perf-report-loop-") as temp:
            repo = Path(temp)
            subprocess.run(["git", "init", "-q", str(repo)], check=True)
            workloads = [{
                "id": "weighted_train", "split": "train", "weight": 2,
                "command": [sys.executable, "-c", "pass"], "inputs": [],
                "covers": ["runner loop"], "result_format": "exit_status"}, {
                "id": "unweighted_validation", "split": "validation", "weight": 5,
                "command": [sys.executable, "-c", "pass"], "inputs": [],
                "covers": ["runner loop"], "result_format": "exit_status"}]
            manifest = {"version": 1, "warmups": 0, "repetitions": 2,
                        "input_identity_kind": "test source files",
                        "corpus_identity_claim": "synthetic runner test",
                        "workloads": workloads}
            args = SimpleNamespace(split="train", repetitions=2, warmups=0,
                                   output=str(repo / "train.json"),
                                   variant="test", build_label="test",
                                   pairing_id="pair-test")
            with patch.object(performance_report, "ROOT", repo), \
                    patch.object(performance_report, "load_manifest", return_value=manifest):
                self.assertEqual(performance_report.run(args), 0)
                train = json.loads((repo / "train.json").read_text())
                train_result = train["workloads"][0]
                self.assertEqual(train_result["realized_measured_iterations"], 4)
                self.assertEqual(len(train_result["samples"]), 4)
                args.split = "validation"
                args.output = str(repo / "validation.json")
                self.assertEqual(performance_report.run(args), 0)
                validation = json.loads((repo / "validation.json").read_text())
                validation_result = validation["workloads"][0]
                self.assertEqual(validation_result["realized_measured_iterations"], 2)
                self.assertEqual(len(validation_result["samples"]), 2)
                malformed_source = (
                    "import pathlib,sys; "
                    "p=next(x.split(':',1)[1] for x in sys.argv "
                    "if x.startswith('--gtest_output=json:')); "
                    "pathlib.Path(p).write_text('[]')"
                )
                bad_manifest = {**manifest, "workloads": [{
                    "id": "malformed_gtest", "split": "train", "weight": 1,
                    "command": [sys.executable, "-c", malformed_source],
                    "inputs": [], "covers": ["malformed result"],
                    "result_format": "gtest_json"}]}
                with patch.object(performance_report, "load_manifest",
                                  return_value=bad_manifest):
                    args.split = "train"
                    args.output = str(repo / "malformed.json")
                    self.assertEqual(performance_report.run(args), 1)
                malformed_report = json.loads((repo / "malformed.json").read_text())
                self.assertFalse(malformed_report["success"])
                self.assertFalse(malformed_report["workloads"][0]["samples"][0]["success"])

    def test_separate_counter_child_does_not_inflate_primary_timing(self):
        with tempfile.TemporaryDirectory(prefix="perf-counter-loop-") as temp:
            repo = Path(temp)
            subprocess.run(["git", "init", "-q", str(repo)], check=True)
            common = {"schema_version": 2, "scenario": "fold-text",
                      "wall_time_scope": "synthetic timing scope",
                      "work_items": 3, "checksum": 9}
            timing_payload = {**common, "measure": "timing", "wall_time_ns": 20_000_000}
            allocation_payload = {
                **common, "measure": "allocations", "wall_time_ns": None,
                "scenario_total_cpp_new_calls": 4,
                "scenario_total_cpp_new_requested_bytes": 512}
            timing_command = [sys.executable, "-c",
                              f"import json; print(json.dumps({json.dumps(timing_payload)}))"]
            counter_program = (
                "import json,time; time.sleep(0.15); "
                f"print({json.dumps(json.dumps(allocation_payload))})"
            )
            manifest = {"version": 1, "warmups": 0, "repetitions": 1,
                        "input_identity_kind": "synthetic probe parameters",
                        "corpus_identity_claim": "synthetic test",
                        "workloads": [{
                            "id": "probe_timing", "split": "train", "weight": 1,
                            "command": timing_command,
                            "counter_command": [sys.executable, "-c", counter_program],
                            "inputs": [], "covers": ["counter isolation"],
                            "result_format": "probe_json", "probe_scenario": "fold-text",
                            "measure": "timing", "size": 3, "seed": 4}]}
            args = SimpleNamespace(split="train", repetitions=1, warmups=0,
                                   output=str(repo / "report.json"),
                                   variant="test", build_label="test",
                                   pairing_id="counter-test", collect_counters=True)
            with patch.object(performance_report, "ROOT", repo), \
                    patch.object(performance_report, "load_manifest", return_value=manifest):
                self.assertEqual(performance_report.run(args), 0)
            report = json.loads((repo / "report.json").read_text())
            workload = report["workloads"][0]
            self.assertEqual(workload["samples"][0]["wall_seconds"], 0.02)
            self.assertEqual(workload["counter_iterations_requested"], 1)
            self.assertEqual(workload["counter_iterations_completed"], 1)
            self.assertIsNone(workload["counter_sample"]["wall_seconds"])
            self.assertGreater(workload["counter_sample"]["runner_wall_seconds"], 0.15)
            self.assertEqual(workload["counter_metrics"]["scenario_total_cpp_new_calls"], 4)

    def test_summary_uses_successful_run_wall_time_and_reports_spread(self):
        samples = [
            {"phase": "warmup", "success": True, "wall_seconds": 999.0},
            {"phase": "run", "success": True, "wall_seconds": 1.0},
            {"phase": "run", "success": False, "wall_seconds": 2.0},
            {"phase": "run", "success": True, "wall_seconds": 3.0},
        ]
        self.assertEqual(
            performance_report.summarize(samples),
            {"successful_runs": 2, "median_wall_seconds": 2.0,
             "min_wall_seconds": 1.0, "max_wall_seconds": 3.0,
             "spread_wall_seconds": 2.0},
        )

    def test_no_successful_runs_has_no_fabricated_summary(self):
        summary = performance_report.summarize(
            [{"phase": "run", "success": False, "wall_seconds": 1.0}]
        )
        self.assertEqual(summary["successful_runs"], 0)
        self.assertIsNone(summary["median_wall_seconds"])

    def test_headless_environment_preserves_driver_values_and_isolates_xdg(self):
        env = performance_report.headless_environment({"KEEP_ME": "yes"})
        self.assertEqual(env["SDL_VIDEODRIVER"], "offscreen")
        self.assertEqual(env["SDL_AUDIODRIVER"], "dummy")
        self.assertEqual(env["LIBGL_ALWAYS_SOFTWARE"], "1")
        self.assertTrue(env["XDG_DATA_HOME"].startswith(str(ROOT)))
        self.assertEqual(env["KEEP_ME"], "yes")

    def test_rss_units_match_platform_conventions(self):
        self.assertEqual(performance_report.rss_bytes(123, "darwin"), 123)
        self.assertEqual(performance_report.rss_bytes(123, "linux"), 123 * 1024)

    def test_linker_identity_uses_the_configured_driver_selection(self):
        self.assertEqual(performance_report.linker_selector("-O3 -fuse-ld=lld"), "lld")
        self.assertEqual(performance_report.linker_selector("-O3 -g"), "ld")
        self.assertIsNone(performance_report.linker_selector(None))
        self.assertEqual(performance_report.linker_program("lld", "-fuse-ld=lld"),
                         "ld.lld")
        self.assertEqual(performance_report.linker_program(
            "lld", "-fuse-ld=lld TARGET=x86_64-w64-mingw32"), "lld-link")
        completed = subprocess.CompletedProcess(
            ["clang++", "-print-prog-name=ld.lld"], 0,
            stdout="/tmp/test-ld.lld\n", stderr="")
        with patch.dict(os.environ, {"CXX": "clang++"}), \
                patch.object(performance_report.shutil, "which",
                             side_effect=["/usr/bin/clang++", "/tmp/test-ld.lld"]), \
                patch.object(performance_report.subprocess, "run",
                             return_value=completed), \
                patch.object(performance_report, "tool_version",
                             return_value={"available": True, "returncode": 0}) as versions:
            identity = performance_report.configured_linker_identity(
                "-O3 -fuse-ld=lld")
        self.assertEqual(identity["selection"], "lld")
        self.assertEqual(identity["driver_program"], "ld.lld")
        self.assertEqual(identity["driver_reported_linker"], "/tmp/test-ld.lld")
        self.assertEqual(identity["linker_path"], "/tmp/test-ld.lld")
        self.assertEqual(versions.call_count, 2)

    @unittest.skipUnless(performance_report.shutil.which("clang++") and
                         performance_report.shutil.which("ld.lld"),
                         "host Clang and ld.lld are required")
    def test_host_lld_identity_resolves_the_real_linker_not_dispatcher(self):
        with patch.dict(os.environ, {"CXX": "clang++"}):
            identity = performance_report.configured_linker_identity(
                "-O3 -fuse-ld=lld")
        self.assertEqual(identity["status"], "resolved")
        self.assertEqual(Path(identity["linker_path"]).name, "ld.lld")
        self.assertEqual(identity["linker"]["returncode"], 0)

    def test_nonzero_linker_version_is_reported_as_unknown(self):
        completed = subprocess.CompletedProcess(
            ["clang++", "-print-prog-name=ld.lld"], 0,
            stdout="/tmp/test-ld.lld\n", stderr="")
        with patch.dict(os.environ, {"CXX": "clang++"}), \
                patch.object(performance_report.shutil, "which",
                             side_effect=["/usr/bin/clang++", "/tmp/test-ld.lld"]), \
                patch.object(performance_report.subprocess, "run",
                             return_value=completed), \
                patch.object(performance_report, "tool_version",
                             side_effect=[{"returncode": 0}, {"returncode": 1}]):
            identity = performance_report.configured_linker_identity("-fuse-ld=lld")
        self.assertEqual(identity["status"], "unknown")

    def test_probe_counter_argv_uses_separate_allocation_binary(self):
        manifest = performance_report.load_manifest()
        probes = [item for item in manifest["workloads"]
                  if item["result_format"] == "probe_json"]
        self.assertTrue(probes)
        for workload in probes:
            self.assertEqual(workload["command"][0], "build/performance-probe")
            self.assertEqual(workload["command"][-1], "timing")
            self.assertEqual(workload["counter_command"][0],
                             "build/performance-probe-counts")
            self.assertEqual(workload["counter_command"][-1], "allocations")

    def test_gtest_result_count_excludes_skipped_tests(self):
        path = ROOT / "build/performance/gtest-result-unit.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        try:
            path.write_text(json.dumps({"testsuites": [{"testsuite": [
                {"status": "RUN", "result": "COMPLETED"},
                {"status": "RUN", "result": "SKIPPED"},
            ]}]}))
            self.assertEqual(performance_report.parse_gtest_result(path),
                             {"executed": 1, "skipped": 1})
        finally:
            path.unlink(missing_ok=True)

    def test_malformed_gtest_json_shapes_are_failed_samples_not_exceptions(self):
        with tempfile.TemporaryDirectory(prefix="perf-gtest-json-") as temp:
            path = Path(temp) / "result.json"
            for invalid in ([], {"testsuites": ["not a suite"]},
                            {"testsuites": [{"testsuite": [None]}]}):
                path.write_text(json.dumps(invalid))
                with self.subTest(invalid=invalid):
                    with self.assertRaises(ValueError):
                        performance_report.parse_gtest_result(path)

    def test_zero_executed_gtest_and_nonzero_child_are_failures(self):
        run_id = "performance-report-unit-test"
        log_dir = ROOT / "build/performance/unit-test-logs"
        shutil.rmtree(log_dir, ignore_errors=True)
        log_dir.mkdir(parents=True)
        env = performance_report.headless_environment(os.environ)
        skipped_program = (
            "import json,pathlib,sys; "
            "path=next(x.split(':',1)[1] for x in sys.argv if x.startswith('--gtest_output=json:')); "
            "pathlib.Path(path).write_text(json.dumps({'testsuites':[{'tests':["
            "{'status':'RUN','result':'SKIPPED'}]}]}))"
        )
        no_execution = performance_report.run_one(
            {"id": "all_skipped", "result_format": "gtest_json",
             "command": [sys.executable, "-c", skipped_program]},
            "run", 1, log_dir, env, run_id, run_id, "baseline")
        self.assertEqual(no_execution["returncode"], 0)
        self.assertEqual(no_execution["test_counts"], {"executed": 0, "skipped": 1})
        self.assertFalse(no_execution["success"])

        failing_program = "raise SystemExit(9)"
        failed = performance_report.run_one(
            {"id": "fails", "result_format": "gtest_json",
             "command": [sys.executable, "-c", failing_program]},
            "run", 1, log_dir, env, run_id, run_id, "baseline")
        self.assertEqual(failed["returncode"], 9)
        self.assertFalse(failed["success"])

        probe_payload = {
            "schema_version": 2, "scenario": "arena-links", "measure": "both",
            "wall_time_scope": "synthetic test timing scope",
            "wall_time_ns": 12345, "cpp_new_calls": 4,
            "cpp_new_requested_bytes": 128, "scenario_total_cpp_new_calls": 4,
            "scenario_total_cpp_new_requested_bytes": 128,
            "by_ref_entries_per_cell": 1.0,
            "work_items": 8, "checksum": 77}
        probe_program = f"import json; print(json.dumps({json.dumps(probe_payload)}))"
        probe = performance_report.run_one(
            {"id": "probe", "result_format": "probe_json",
             "probe_scenario": "arena-links", "measure": "both",
             "command": [sys.executable, "-c", probe_program]},
            "run", 1, log_dir, env, run_id, run_id, "baseline")
        self.assertTrue(probe["success"])
        self.assertEqual(probe["probe_metrics"], probe_payload)
        self.assertAlmostEqual(probe["wall_seconds"], 0.000012345)
        shutil.rmtree(log_dir, ignore_errors=True)

    def test_porcelain_nul_hashes_renamed_and_quoted_paths(self):
        with tempfile.TemporaryDirectory(prefix="perf-git-status-") as temp:
            repo = Path(temp)
            subprocess.run(["git", "init", "-q", str(repo)], check=True)
            subprocess.run(["git", "-C", str(repo), "config", "user.name", "Perf Test"], check=True)
            subprocess.run(["git", "-C", str(repo), "config", "user.email", "perf@example.invalid"], check=True)
            (repo / "original.txt").write_text("same content\n")
            subprocess.run(["git", "-C", str(repo), "add", "original.txt"], check=True)
            subprocess.run(["git", "-C", str(repo), "commit", "-qm", "fixture"], check=True)
            renamed = repo / 'renamed "quoted".txt'
            (repo / "original.txt").rename(renamed)
            quoted = repo / 'untracked "name".txt'
            quoted.write_text("dirty bytes\n")
            subprocess.run(["git", "-C", str(repo), "add", "-A"], check=True)
            identity = performance_report.source_identity(repo)
            records = {item["path"]: item for item in identity["dirty_files"]}
            self.assertEqual(records[renamed.name]["sha256"],
                             performance_report.sha256(renamed))
            self.assertEqual(records[quoted.name]["sha256"],
                             performance_report.sha256(quoted))
            self.assertIn("renamed_from", {item["role"] for item in identity["dirty_files"]})


if __name__ == "__main__":
    unittest.main()
