"""Unit tests for perf CSV interpretation and the external counter wrapper."""

import importlib.util
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "performance_hardware_report_test", ROOT / "tools/performance-hardware-report.py"
)
hardware = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(hardware)


class PerformanceHardwareReportTest(unittest.TestCase):
    def parse(self, directory, content):
        path = Path(directory) / "perf.csv"
        path.write_text(content)
        return hardware.parse_perf_csv(path)

    def test_csv_numeric_counts_and_runtime(self):
        with tempfile.TemporaryDirectory() as temp:
            parsed = self.parse(temp, "10,,cycles:u,1000,100.00,,\n20,,instructions:u,1000,100.00,,\n")
        self.assertEqual(parsed["events"]["cycles:u"]["value"], 10)
        self.assertEqual(parsed["events"]["instructions:u"]["value"], 20)
        self.assertEqual(parsed["events"]["cycles:u"]["status"], "counted")
        self.assertEqual(parsed["events"]["cycles:u"]["runtime_pct"], 100.0)

    def test_zero_is_a_count_not_unavailable(self):
        with tempfile.TemporaryDirectory() as temp:
            parsed = self.parse(temp, "0,,cycles:u,1000,100.00,,\n0,,instructions:u,1000,100.00,,\n")
        for event in hardware.COUNTERS:
            self.assertEqual(parsed["events"][event]["value"], 0)
            self.assertEqual(parsed["events"][event]["status"], "counted")

    def test_unsupported_or_not_counted_values_remain_null(self):
        with tempfile.TemporaryDirectory() as temp:
            parsed = self.parse(
                temp,
                "<not supported>,,cycles:u,0,0.00,,\n<not counted>,,instructions:u,0,0.00,,\n",
            )
        for event in hardware.COUNTERS:
            self.assertIsNone(parsed["events"][event]["value"])
            self.assertEqual(parsed["events"][event]["status"], "not_counted_or_unsupported")

    def test_invalid_counter_values_and_percentages_are_null(self):
        invalid_rows = {
            "nan": "1,,cycles:u,1000,nan,,\n",
            "positive_infinity": "1,,cycles:u,1000,inf,,\n",
            "negative_infinity": "1,,cycles:u,1000,-inf,,\n",
            "percentage_above_100": "1,,cycles:u,1000,100.1,,\n",
            "percentage_below_0": "1,,cycles:u,1000,-0.1,,\n",
            "negative_count": "-1,,cycles:u,1000,100.0,,\n",
            "negative_runtime": "1,,cycles:u,-1,100.0,,\n",
        }
        expected_status = {
            "nan": "invalid_runtime_pct",
            "positive_infinity": "invalid_runtime_pct",
            "negative_infinity": "invalid_runtime_pct",
            "percentage_above_100": "invalid_runtime_pct",
            "percentage_below_0": "invalid_runtime_pct",
            "negative_count": "invalid_negative_count",
            "negative_runtime": "invalid_negative_runtime_ns",
        }
        for name, row in invalid_rows.items():
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temp:
                parsed = self.parse(
                    temp, row + "0,,instructions:u,1000,100.0,,\n")
                event = parsed["events"]["cycles:u"]
                self.assertIsNone(event["value"])
                self.assertEqual(event["status"], expected_status[name])
                self.assertEqual(parsed["events"]["instructions:u"]["value"], 0)
                self.assertEqual(parsed["events"]["instructions:u"]["status"],
                                 "counted")

    def test_multiplexed_value_is_null_but_runtime_is_retained(self):
        with tempfile.TemporaryDirectory() as temp:
            parsed = self.parse(temp, "123,,cycles:u,1000,75.00,,\n456,,instructions:u,1000,100.00,,\n")
        self.assertIsNone(parsed["events"]["cycles:u"]["value"])
        self.assertEqual(parsed["events"]["cycles:u"]["status"], "multiplexed")
        self.assertEqual(parsed["events"]["cycles:u"]["runtime_pct"], 75.0)
        self.assertEqual(parsed["events"]["instructions:u"]["value"], 456)

    def test_corrupt_release_artifact_is_refused(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            artifact = root / "binary"
            artifact.write_text("changed")
            manifest = root / "release.json"
            manifest.write_text(json.dumps({"artifacts": [{
                "path": str(artifact), "sha256": "0" * 64}]}))
            with patch.object(hardware, "ROOT", root):
                with self.assertRaisesRegex(ValueError, "identity mismatch"):
                    hardware.verify_artifact_manifest(manifest)

    def test_fake_perf_wraps_validated_workload_and_preserves_fresh_isolation(self):
        with tempfile.TemporaryDirectory(prefix="hardware-runner-") as temp:
            root = Path(temp)
            report = hardware.performance_report
            original_root = report.ROOT
            report.ROOT = root
            try:
                fake_perf = root / "fake-perf"
                fake_perf.write_text(
                    "#!/usr/bin/env python3\n"
                    "import json,os,subprocess,sys\n"
                    "args=sys.argv[1:]\n"
                    "open(os.environ['FAKE_PERF_ARGV'], 'w').write(json.dumps(args))\n"
                    "out=args[args.index('-o')+1]\n"
                    "open(out,'w').write('101,,cycles:u,5000,100.00,,\\n202,,instructions:u,5000,100.00,,\\n')\n"
                    "child=args[args.index('--')+1:]\n"
                    "raise SystemExit(subprocess.run(child).returncode)\n"
                )
                fake_perf.chmod(0o755)
                workload_program = root / "fake-workload.py"
                workload_program.write_text(
                    "import json,pathlib,sys\n"
                    "args=sys.argv[1:]\n"
                    "artifact=pathlib.Path(args[args.index('--artifact')+1])\n"
                    "artifact.parent.mkdir(parents=True,exist_ok=True)\n"
                    "artifact.write_text('valid')\n"
                    "print(json.dumps({'schema_version':2,'scenario':'synthetic',"
                    "'measure':'timing','wall_time_ns':1,'wall_time_scope':'child',"
                    "'work_items':2,'checksum':4}))\n"
                )
                run_id = "fake-run"
                workload = {
                    "id": "synthetic_probe", "split": "validation", "weight": 1,
                    "command": [sys.executable, str(workload_program), "--artifact",
                                "{artifact_dir}/out.bin"],
                    "inputs": [], "covers": ["fake wrapper"],
                    "result_format": "probe_json", "probe_scenario": "synthetic",
                    "measure": "timing", "required_artifacts": ["{artifact_dir}/out.bin"],
                }
                logs = root / "logs"
                logs.mkdir()
                raw = root / "raw"
                raw.mkdir()
                base_env = report.headless_environment({
                    **os.environ, "FAKE_PERF_ARGV": str(root / "argv.json")})
                warmup = hardware.run_sample(workload, "warmup", 1, run_id, "test",
                                             logs, raw, base_env, str(fake_perf))
                measured = hardware.run_sample(workload, "run", 1, run_id, "test",
                                               logs, raw, base_env, str(fake_perf))
                self.assertTrue(warmup["success"], warmup)
                self.assertTrue(measured["success"], measured)
                self.assertEqual(warmup["artifacts"][0]["size_bytes"], 5)
                self.assertEqual(measured["perf_csv"]["events"]["cycles:u"]["value"], 101)
                self.assertEqual(measured["counter_status"], "counted")
                self.assertNotEqual(warmup["xdg_paths"], measured["xdg_paths"])
                for key in ("XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME"):
                    self.assertNotEqual(warmup["xdg_paths"][key], measured["xdg_paths"][key])
                argv = json.loads((root / "argv.json").read_text())
                self.assertEqual(argv[0], "stat")
                self.assertIn("cycles:u,instructions:u", argv)
                self.assertEqual(argv[argv.index("--") + 1], sys.executable)
            finally:
                report.ROOT = original_root


if __name__ == "__main__":
    unittest.main()
