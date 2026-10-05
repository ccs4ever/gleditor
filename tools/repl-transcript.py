#!/usr/bin/env python3
"""Drive a REPL (vquery, vpl, vprolog) through a pseudo-terminal, a line at a
time, and write what a person at it would have seen.

A pipe is not a terminal: a REPL fed through one may skip its prompt, buffer
its answers or behave as a script runner, which is not the journey under
test. Under a PTY it runs as it would for a person, and the terminal's echo
puts each typed line in the transcript where it was typed.

Each line is sent once the program has been quiet for --quiet-ms, which is
how a person waits for an answer before typing the next thing. After the last
line the terminal sends end-of-file (Ctrl+D), as a person leaving would. The
exit status is the program's own, or 124 if --timeout ran out.

    tools/repl-transcript.py --input steps.txt -- build/vquery store
    printf ':help\\nfind("x")\\n' | tools/repl-transcript.py -- build/vquery s

Headless like everything else here: SDL is pointed at its offscreen and dummy
drivers, and XDG_DATA_HOME / XDG_CONFIG_HOME at a fresh temporary directory
unless they are already set, so a run never types into your own permascroll.
"""

import argparse
import os
import pty
import re
import select
import sys
import tempfile
import time

ANSI = re.compile(rb"\x1b\[[0-9;?]*[ -/]*[@-~]")


def read_until_quiet(fd, quiet, deadline):
    """Everything the program writes until it is quiet for `quiet` seconds;
    None once it has exited and said everything."""
    out = bytearray()
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return bytes(out)
        ready, _, _ = select.select([fd], [], [], min(quiet, remaining))
        if not ready:
            return bytes(out)
        try:
            chunk = os.read(fd, 65536)
        except OSError:  # EIO: the child has gone and the PTY is closed
            return bytes(out) if out else None
        if not chunk:
            return bytes(out) if out else None
        out += chunk


def main():
    parser = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--input", default="-", help="lines to type, one per line; - is stdin"
    )
    parser.add_argument(
        "--transcript", default="-", help="where to write it; - is stdout"
    )
    parser.add_argument(
        "--quiet-ms",
        type=int,
        default=400,
        help="how long the program must be silent before the next line",
    )
    parser.add_argument(
        "--timeout", type=float, default=120.0, help="seconds for the whole run"
    )
    parser.add_argument(
        "--keep-ansi", action="store_true", help="keep terminal escape sequences"
    )
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("no command to run; put it after --")

    source = sys.stdin if args.input == "-" else open(args.input, encoding="utf-8")
    lines = [line.rstrip("\n") for line in source]

    env = dict(os.environ)
    env.setdefault("SDL_VIDEODRIVER", "offscreen")
    env.setdefault("SDL_AUDIODRIVER", "dummy")
    env.setdefault("LIBGL_ALWAYS_SOFTWARE", "1")
    if "XDG_DATA_HOME" not in env or "XDG_CONFIG_HOME" not in env:
        scratch = tempfile.mkdtemp(prefix="repl-transcript-")
        env.setdefault("XDG_DATA_HOME", os.path.join(scratch, "data"))
        env.setdefault("XDG_CONFIG_HOME", os.path.join(scratch, "config"))
        print(f"repl-transcript: XDG homes under {scratch}", file=sys.stderr)

    pid, fd = pty.fork()
    if 0 == pid:
        try:
            os.execvpe(command[0], command, env)
        finally:
            os._exit(127)

    quiet = args.quiet_ms / 1000.0
    deadline = time.monotonic() + args.timeout
    transcript = bytearray()
    finished = False
    for line in [*lines, None]:
        said = read_until_quiet(fd, quiet, deadline)
        if said is None:
            finished = True
            break
        transcript += said
        if time.monotonic() >= deadline:
            break
        # None is the person leaving: end-of-file at the start of a line.
        os.write(fd, b"\x04" if line is None else line.encode() + b"\n")
    while not finished and time.monotonic() < deadline:
        said = read_until_quiet(fd, quiet, deadline)
        if said is None:
            break
        transcript += said

    status = 124
    for _ in range(50):
        done, wait = os.waitpid(pid, os.WNOHANG)
        if done:
            status = os.waitstatus_to_exitcode(wait)
            break
        time.sleep(0.1)
    else:
        os.kill(pid, 9)
        os.waitpid(pid, 0)
        print("repl-transcript: timed out; killed", file=sys.stderr)

    text = transcript.replace(b"\r\n", b"\n")
    if not args.keep_ansi:
        text = ANSI.sub(b"", text)
    if "-" == args.transcript:
        sys.stdout.buffer.write(text)
    else:
        with open(args.transcript, "wb") as out:
            out.write(text)
    return status


if __name__ == "__main__":
    sys.exit(main())
