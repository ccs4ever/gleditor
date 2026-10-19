"""Regressions for replay isolation, variable handling and timeout cleanup."""

import copy
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import subprocess
import sys
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
REPO = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("ux_journey", REPO / "tools/ux-journey.py")
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)


def stage():
    return dict(journey="J1", stage="author", driver="ui", program="xuzz",
                profile="shared", source_evidence="evidence/plan.json", notes=[],
                variables={"input": dict(description="Input path", example="old-path")},
                plan={"steps": ["--type", "${input}", "--capture", "${evidence}/frame.ppm"]})


class JourneyRunnerTest(unittest.TestCase):
    def test_catalog_covers_every_journey(self):
        catalog = RUNNER.load_catalog()
        self.assertEqual(set(catalog), {f"J{i}" for i in range(1, 17)})

    def test_substitution_preserves_whole_arguments(self):
        plan = RUNNER.replace_templates(stage()["plan"],
                                        {"input": "space `literal` $text", "evidence": "/tmp/a b"})
        self.assertEqual(plan["steps"][1], "space `literal` $text")
        self.assertEqual(plan["steps"][3], "/tmp/a b/frame.ppm")
        with self.assertRaises(RUNNER.PlanError):
            RUNNER.replace_templates("${input}", {})

    def test_pty_assertions_cannot_degrade_to_characters(self):
        item = stage()
        item.update(driver="pty", program="vpl", plan={"prompt": "      ",
                    "steps": [{"line": "data", "contains": "12 8 5"}]})
        self.assertTrue(any("whole strings" in error for error in
                            RUNNER.validate_stage(item, Path("plan.json"))))

    def test_capture_cannot_overwrite_an_input(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaises(RUNNER.PlanError):
                RUNNER.validate_capture_paths("ui", {"steps": ["--capture", str(root / "input")]},
                                              root / "evidence", root / "profile")

    def invoke(self, item, root, binary, extra=()):
        argv = ["ux-journey", "--journey", "J1", "--stage", item["stage"],
                "--run", str(root), "--binary", str(binary), *extra]
        with patch.object(RUNNER, "load_catalog", return_value={"J1": [item]}), \
                patch("sys.argv", argv):
            return RUNNER.main()

    def test_missing_variable_does_not_launch_or_use_example(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.assertEqual(self.invoke(stage(), root, root / "nonexistent"), 2)
            result = json.loads((root / "J1/author/stage-result.json").read_text())
            self.assertEqual(result["status"], "not-launched")
            self.assertIn("missing required", result["error"])

    def test_profile_reused_but_evidence_is_fresh(self):
        with tempfile.TemporaryDirectory(prefix="ux path ") as directory:
            root = Path(directory)
            binary = root / "fake program"
            binary.write_text("#!/usr/bin/env python3\nimport os,json\nfrom pathlib import Path\n"
                              "p=Path(os.environ['XDG_DATA_HOME'])/'launches.json'\n"
                              "v=json.loads(p.read_text()) if p.exists() else []\n"
                              "v.append({k:os.environ[k] for k in ['XDG_DATA_HOME',"
                              "'XDG_CONFIG_HOME','XDG_CACHE_HOME','SDL_VIDEODRIVER',"
                              "'SDL_AUDIODRIVER','LIBGL_ALWAYS_SOFTWARE']})\n"
                              "p.write_text(json.dumps(v))\n")
            binary.chmod(0o700)
            item = stage()
            self.assertEqual(self.invoke(item, root, binary, ["--var", "input=a b"]), 0)
            self.assertEqual(self.invoke(item, root, binary, ["--var", "input=a b"]), 2)
            second = copy.deepcopy(item)
            second["stage"] = "resume"
            self.assertEqual(self.invoke(second, root, binary, ["--var", "input=a b"]), 0)
            launches = json.loads((root / "profiles/shared/data/launches.json").read_text())
            self.assertEqual(len(launches), 2)
            self.assertEqual(launches[0], launches[1])
            self.assertEqual(launches[0]["SDL_VIDEODRIVER"], "offscreen")
            self.assertTrue(launches[0]["XDG_CACHE_HOME"].startswith(str(root)))

    def test_pty_timeout_stops_its_separate_session(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary = root / "fake-repl"
            binary.write_text("#!/usr/bin/env python3\nimport os,time\nfrom pathlib import Path\n"
                              "(Path(os.environ['XDG_DATA_HOME'])/'pid').write_text(str(os.getpid()))\n"
                              "print('ready>',end='',flush=True)\ntime.sleep(60)\n")
            binary.chmod(0o700)
            item = stage()
            item.update(driver="pty", program="vpl", variables={},
                        plan={"prompt": "ready>", "steps": [{"line": "data"}]})
            self.assertEqual(self.invoke(item, root, binary, ["--timeout", "1"]), 1)
            pid = int((root / "profiles/shared/data/pid").read_text())
            status = Path(f"/proc/{pid}/status")
            if status.exists():
                self.assertIn("State:\tZ", status.read_text())
            result = json.loads((root / "J1/author/stage-result.json").read_text())
            self.assertEqual(result["status"], "timeout")

    @unittest.skipUnless(os.name == "posix", "process groups require POSIX")
    def test_timeout_stops_child_process(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary = root / "fake-hanging-program"
            binary.write_text("#!/usr/bin/env python3\nimport subprocess,sys,time\n"
                              "p=subprocess.Popen([sys.executable,'-c','import time;time.sleep(60)'])\n"
                              "print(p.pid,flush=True)\ntime.sleep(60)\n")
            binary.chmod(0o700)
            self.assertEqual(self.invoke(stage(), root, binary,
                                        ["--var", "input=x", "--timeout", "0.5"]), 1)
            pid = int((root / "J1/author/driver.stdout.log").read_text())
            status = Path(f"/proc/{pid}/status")
            if status.exists():
                self.assertIn("State:\tZ", status.read_text())
            result = json.loads((root / "J1/author/stage-result.json").read_text())
            self.assertEqual(result["status"], "timeout")


class DragProbeTest(unittest.TestCase):
    def test_valid_blank_frame_is_a_failure(self):
        spec = importlib.util.spec_from_file_location("ux_drag", REPO / "tools/ux-drag-repeat.py")
        probe = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(probe)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary = root / "binary"
            binary.touch()
            def blank_frame(argv, **kwargs):
                capture = Path(argv[argv.index("--capture") + 1])
                capture.write_bytes(b"P6\n2 2\n255\n" + bytes(12))
                return subprocess.CompletedProcess(argv, 0, stdout=b"", stderr=b"")
            with patch("sys.argv", ["ux-drag", "--runs", "1", "--evidence", str(root / "probe"),
                                    "--binary", str(binary)]), patch.object(probe.subprocess, "run", blank_frame):
                self.assertEqual(probe.main(), 1)
            result = json.loads((root / "probe/run-01/result.json").read_text())
            self.assertTrue(result["ppm_valid"])
            self.assertFalse(result["selection_detected"])
            self.assertEqual(result["blue_selection_pixels"], 0)


if __name__ == "__main__":
    unittest.main()
