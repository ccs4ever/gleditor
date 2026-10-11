#!/usr/bin/env python3
"""Type a JSON plan at real REPL prompts and retain the complete PTY transcript.

The plan has optional launch_args (store paths only for journey runs), prompt
(literal), and steps with line plus optional contains assertions. No expression
flags, prepared source file or stdin pipe replaces the terminal interaction.
"""

import argparse
import errno
import json
import os
from pathlib import Path
import pty
import re
import select
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--plan", required=True)
    parser.add_argument("--evidence", required=True)
    parser.add_argument("--timeout", type=float, default=15)
    args = parser.parse_args()
    directory = Path(args.evidence).resolve()
    directory.mkdir(parents=True, exist_ok=True)
    plan = json.loads(Path(args.plan).read_text())
    env = dict(os.environ)
    for kind in ("DATA", "CONFIG", "CACHE"):
        env["XDG_" + kind + "_HOME"] = str(directory / kind.lower())
    env.update(SDL_VIDEODRIVER="offscreen", SDL_AUDIODRIVER="dummy",
               LIBGL_ALWAYS_SOFTWARE="1")
    master, slave = pty.openpty()
    command = [str(Path(args.binary).resolve()), *plan.get("launch_args", [])]
    (directory / "launch.json").write_text(json.dumps(command, indent=2) + "\n")
    process = subprocess.Popen(command, stdin=slave, stdout=slave, stderr=slave,
                               env=env, start_new_session=True)
    os.close(slave)
    transcript = bytearray()
    events = []
    result = {"status": "failed", "scope": "PTY harness smoke/plan, not an automatic journey verdict"}

    def receive_until(marker):
        start = len(transcript)
        pattern = re.compile(rb"(?:^|[\r\n])" + re.escape(marker.encode()) + rb"\Z")
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            if pattern.search(transcript[start:]):
                return transcript[start:].decode(errors="replace")
            if select.select([master], [], [], .1)[0]:
                try:
                    data = os.read(master, 65536)
                except OSError as error:
                    if error.errno == errno.EIO:
                        raise RuntimeError("REPL exited before expected prompt") from error
                    raise
                if not data:
                    raise RuntimeError("REPL closed its terminal before expected prompt")
                transcript.extend(data)
                (directory / "transcript.txt").write_bytes(transcript)
        raise RuntimeError("REPL prompt timeout: " + repr(marker))

    try:
        receive_until(plan["prompt"])
        for step in plan.get("steps", []):
            os.write(master, (step["line"] + "\n").encode())
            if step.get("exit"):
                process.wait(timeout=args.timeout)
                # Drain echoed input and final output before the PTY closes.
                while select.select([master], [], [], .1)[0]:
                    try:
                        data = os.read(master, 65536)
                    except OSError as error:
                        if error.errno == errno.EIO:
                            break
                        raise
                    if not data:
                        break
                    transcript.extend(data)
                output = transcript.decode(errors="replace")
            else:
                output = receive_until(step.get("prompt", plan["prompt"]))
            for expected in step.get("contains", []):
                if expected not in output:
                    raise RuntimeError("Expected REPL output absent: " + expected)
            events.append({"request": step, "status": "passed"})
        if process.poll() not in (None, plan.get("exit_code", 0)):
            raise RuntimeError("REPL exited with unexpected status: " + str(process.returncode))
        result.update(status="passed", exit_code=process.poll())
    except Exception as error:
        result["error"] = str(error)
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        os.close(master)
        (directory / "transcript.txt").write_bytes(transcript)
        (directory / "events.json").write_text(json.dumps(events, indent=2) + "\n")
        (directory / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    return 0 if result["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
