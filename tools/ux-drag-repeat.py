#!/usr/bin/env python3
"""Repeat the scripted drag-selection interaction in isolated headless trials.

This is a diagnostic probe, not a proven fix or UX acceptance test.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time
from typing import Any

REPO = Path(__file__).resolve().parent.parent
HEADLESS = {
    "SDL_VIDEODRIVER": "offscreen",
    "SDL_AUDIODRIVER": "dummy",
    "LIBGL_ALWAYS_SOFTWARE": "1",
}
LOG_LEVEL = "render.scene=debug,text.layout=debug,xuzz.edit=debug,input.pick=debug"
TIMEOUT_SECONDS = 90


def read_ppm(path: Path) -> tuple[int, int, bytes]:
    """Read binary or ASCII PPM (P6/P3), rejecting malformed/truncated images."""
    raw = path.read_bytes()
    pos = 0

    def token() -> bytes:
        nonlocal pos
        while pos < len(raw):
            if raw[pos] in b" \t\r\n\v\f":
                pos += 1
            elif raw[pos] == ord("#"):
                end = raw.find(b"\n", pos)
                pos = len(raw) if end < 0 else end + 1
            else:
                break
        start = pos
        while pos < len(raw) and raw[pos] not in b" \t\r\n\v\f#":
            pos += 1
        if start == pos:
            raise ValueError("PPM header ended before all fields were read")
        return raw[start:pos]

    magic = token()
    if magic not in (b"P6", b"P3"):
        raise ValueError(f"expected P6/P3 PPM, got {magic!r}")
    width, height, max_value = (int(token()) for _ in range(3))
    if width <= 0 or height <= 0 or max_value != 255:
        raise ValueError(f"unsupported PPM dimensions/max value: {width}x{height}, {max_value}")
    count = width * height * 3
    if magic == b"P6":
        if pos >= len(raw) or raw[pos] not in b" \t\r\n\v\f":
            raise ValueError("missing P6 header/raster separator")
        if raw[pos:pos + 2] == b"\r\n":
            pos += 2
        else:
            pos += 1
        pixels = raw[pos:pos + count]
        if len(pixels) != count:
            raise ValueError(f"truncated P6 raster: expected {count} bytes, got {len(pixels)}")
    else:
        values = raw[pos:].split()
        if len(values) != count:
            raise ValueError(f"wrong P3 sample count: expected {count}, got {len(values)}")
        samples = [int(value) for value in values]
        if any(value < 0 or value > 255 for value in samples):
            raise ValueError("P3 sample outside 0..255")
        pixels = bytes(samples)
    return width, height, pixels


def blue_selection_pixels(width: int, height: int, rgb: bytes) -> int:
    """Count blue selection pixels near white glyph pixels (prior probe rule)."""
    stride = width * 3
    # Column prefixes make the nearby-white check linear rather than rescanning
    # up to 48 rows for every candidate blue pixel.
    white_prefix: list[list[int]] = [[0] * (height + 1) for _ in range(width)]
    for y in range(height):
        row = y * stride
        for x in range(width):
            i = row + x * 3
            white = rgb[i] > 235 and rgb[i + 1] > 235 and rgb[i + 2] > 235
            white_prefix[x][y + 1] = white_prefix[x][y] + int(white)

    blue = 0
    for y in range(height):
        row = y * stride
        lo, hi = max(0, y - 24), min(height, y + 25)
        for x in range(width):
            i = row + x * 3
            red, green, blue_channel = rgb[i], rgb[i + 1], rgb[i + 2]
            if blue_channel <= red + 35 or blue_channel <= green + 12:
                continue
            prefix = white_prefix[x]
            white_near = prefix[hi] - prefix[lo]
            if lo <= y < hi and red > 235 and green > 235 and blue_channel > 235:
                white_near -= 1
            if white_near > 0:
                blue += 1
    return blue


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runs", type=int, default=8, help="number of isolated trials (1..32; default: 8)")
    parser.add_argument("--evidence", type=Path, required=True, help="fresh output directory; must not contain files")
    parser.add_argument("--binary", type=Path, default=REPO / "build/xuzz", help="xuzz executable (default: repo build/xuzz)")
    args = parser.parse_args()

    if not 1 <= args.runs <= 32:
        parser.error("--runs must be between 1 and 32")
    binary = args.binary.expanduser().resolve()
    if not binary.is_file():
        parser.error(f"xuzz binary does not exist: {binary}")
    evidence = args.evidence.expanduser().resolve()
    if evidence.exists() and (not evidence.is_dir() or any(evidence.iterdir())):
        parser.error(f"--evidence must be a fresh empty directory: {evidence}")
    evidence.mkdir(parents=True, exist_ok=True)

    results: list[dict[str, Any]] = []
    for run_index in range(1, args.runs + 1):
        trial = evidence / f"run-{run_index:02d}"
        trial.mkdir()
        data_home, config_home, cache_home = (trial / name for name in ("data", "config", "cache"))
        for directory in (data_home, config_home, cache_home):
            directory.mkdir()
        permascroll = trial / "permascroll"
        capture = trial / "selection.ppm"
        argv = [
            str(binary), "--permascroll", str(permascroll), "--backend", "opengl",
            "--profile", "--chord", "Ctrl+N", "--type", "alpha beta gamma",
            "--chord", "Ctrl+Left", "--chord", "Ctrl+Left",
            "--chord", "Ctrl+Shift+Right", "--capture", str(capture), "--dump-a11y",
        ]
        isolated = {
            **HEADLESS,
            "XDG_DATA_HOME": str(data_home),
            "XDG_CONFIG_HOME": str(config_home),
            "XDG_CACHE_HOME": str(cache_home),
            "SPDLOG_LEVEL": LOG_LEVEL,
        }
        env = dict(os.environ)
        env.update(isolated)
        launch = {"argv": argv, "cwd": str(REPO), "environment": isolated, "timeout_seconds": TIMEOUT_SECONDS}
        (trial / "launch.json").write_text(json.dumps(launch, indent=2) + "\n", encoding="utf-8")
        started = time.monotonic()
        result: dict[str, Any] = {"run": run_index, "argv": argv, "capture": str(capture)}
        try:
            completed = subprocess.run(
                argv, cwd=REPO, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                timeout=TIMEOUT_SECONDS, check=False,
            )
            (trial / "stdout.log").write_bytes(completed.stdout)
            (trial / "stderr.log").write_bytes(completed.stderr)
            result.update(exit_code=completed.returncode, timed_out=False)
            if completed.returncode == 0:
                try:
                    width, height, pixels = read_ppm(capture)
                    blue = blue_selection_pixels(width, height, pixels)
                    result.update(ppm_valid=True, width=width, height=height,
                                  blue_selection_pixels=blue, selection_detected=blue >= 20)
                    if not result["selection_detected"]:
                        result["error"] = "selection pixels absent or below 20-pixel threshold"
                except Exception as exc:
                    result.update(ppm_valid=False, error=f"invalid capture: {exc!r}")
            else:
                result.update(ppm_valid=False, error="application exited nonzero")
        except subprocess.TimeoutExpired as exc:
            (trial / "stdout.log").write_bytes(exc.stdout or b"")
            (trial / "stderr.log").write_bytes(exc.stderr or b"")
            result.update(exit_code=None, timed_out=True, error=f"exceeded {TIMEOUT_SECONDS}s")
        except Exception as exc:  # Keep the trial evidence and continue subsequent bounded runs.
            (trial / "stdout.log").write_bytes(b"")
            (trial / "stderr.log").write_text(f"probe exception: {exc!r}\n", encoding="utf-8")
            result.update(exit_code=None, timed_out=False, ppm_valid=False, error=repr(exc))
        result["elapsed_seconds"] = round(time.monotonic() - started, 3)
        (trial / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        results.append(result)
        print(json.dumps(result, ensure_ascii=False), flush=True)

    summary = {"runs_requested": args.runs, "runs": results}
    (evidence / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    return 0 if all(r.get("exit_code") == 0 and r.get("ppm_valid") and r.get("selection_detected") for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
