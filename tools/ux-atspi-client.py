#!/usr/bin/env python3
"""Run a JSON sequence of native AT-SPI operations in an isolated UI session.

The plan contains launch_args (ordinary automation hands or a store path) and
steps. Supported steps: snapshot, wait_name, absent, action, focus, edit, text,
wait_file (for an ordinary automation capture scheduled in launch_args), and
wait_exit (for graceful application shutdown after a native or keyboard action).
Targets match an exact accessible name unless name_prefix is supplied. Actions
call AT-SPI directly; they never turn a label into a pointer click. Snapshots
are native accessibility evidence, not screenshots or a full journey verdict.
"""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time


def arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--plan", required=True)
    parser.add_argument("--evidence", required=True)
    parser.add_argument("--backend", default="opengl")
    parser.add_argument("--cwd", default=os.getcwd(),
                        help="Application asset/workspace directory; XDG state stays isolated")
    parser.add_argument("--timeout", type=float, default=30)
    parser.add_argument("--session", action="store_true", help=argparse.SUPPRESS)
    return parser.parse_args()


def main():
    args = arguments()
    evidence = Path(args.evidence).resolve()
    evidence.mkdir(parents=True, exist_ok=True)
    binary = str(Path(args.binary).resolve())
    plan_path = Path(args.plan).resolve()
    plan = json.loads(plan_path.read_text())
    if not args.session:
        env = dict(os.environ)
        env.update(
            SDL_VIDEODRIVER="offscreen",
            SDL_AUDIODRIVER="dummy",
            LIBGL_ALWAYS_SOFTWARE="1",
            GSETTINGS_BACKEND="memory",
            XDG_DATA_HOME=str(evidence / "data"),
            XDG_CONFIG_HOME=str(evidence / "config"),
            XDG_CACHE_HOME=str(evidence / "cache"),
        )
        command = [
            "dbus-run-session", "--", sys.executable, str(Path(__file__).resolve()),
            "--session", "--binary", binary, "--plan", str(plan_path),
            "--evidence", str(evidence), "--backend", args.backend,
            "--cwd", str(Path(args.cwd).resolve()),
            "--timeout", str(args.timeout),
        ]
        with (evidence / "session.log").open("w") as log:
            return subprocess.call(command, env=env, stdout=log, stderr=log)

    import gi

    gi.require_version("Atspi", "2.0")
    from gi.repository import Atspi, Gio, GLib

    events = []
    application = None
    registry = None
    result = {"status": "failed", "scope": "native AT-SPI operations only"}
    try:
        bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        bus.call_sync(
            "org.a11y.Bus", "/org/a11y/bus", "org.freedesktop.DBus.Properties",
            "Set", GLib.Variant("(ssv)", (
                "org.a11y.Status", "ScreenReaderEnabled", GLib.Variant("b", True)
            )), None, Gio.DBusCallFlags.NONE, 5000, None,
        )
        address = bus.call_sync(
            "org.a11y.Bus", "/org/a11y/bus", "org.a11y.Bus", "GetAddress", None,
            GLib.VariantType.new("(s)"), Gio.DBusCallFlags.NONE, 5000, None,
        ).unpack()[0]
        os.environ["AT_SPI_BUS_ADDRESS"] = address
        registry_path = next((path for path in (
            shutil.which("at-spi2-registryd"), "/usr/lib/at-spi2-registryd",
            "/usr/libexec/at-spi2-registryd", "/usr/lib/at-spi2-core/at-spi2-registryd",
        ) if path and Path(path).is_file()), None)
        if not registry_path:
            raise RuntimeError("AT-SPI registry daemon is absent")
        with (evidence / "registry.log").open("w") as log:
            registry = subprocess.Popen(
                [registry_path], stdout=log, stderr=log,
                env=dict(os.environ, DBUS_SESSION_BUS_ADDRESS=address),
            )
        Atspi.init()
        launch_args = [str(value).replace("{evidence}", str(evidence))
                       for value in plan.get("launch_args", [])]
        command = [binary, "--backend", args.backend, "--no-present", *launch_args]
        (evidence / "launch.json").write_text(json.dumps(command, indent=2) + "\n")
        with (evidence / "application.log").open("w") as log:
            application = subprocess.Popen(command, cwd=args.cwd, stdout=log, stderr=log)

        def nodes(root):
            yield root
            for index in range(root.get_child_count()):
                child = root.get_child_at_index(index)
                if child:
                    yield from nodes(child)

        def wait_for(predicate):
            deadline = time.monotonic() + args.timeout
            while time.monotonic() < deadline:
                if application.poll() is not None:
                    raise RuntimeError("Application exited before native operation completed")
                while GLib.MainContext.default().pending():
                    GLib.MainContext.default().iteration(False)
                try:
                    value = predicate()
                    if value:
                        return value
                except GLib.GError:
                    pass
                time.sleep(.05)
            raise RuntimeError("Native AT-SPI operation timed out")

        root = wait_for(lambda: next((node for node in nodes(Atspi.get_desktop(0))
                                     if node.get_process_id() == application.pid
                                     and node.get_role() == Atspi.Role.APPLICATION), None))

        def target(step):
            matches = []
            for node in nodes(root):
                node.clear_cache()
                name = node.get_name()
                named = (("name" in step and name == step["name"])
                         or ("name_prefix" in step and name.startswith(step["name_prefix"]))
                         or ("name" not in step and "name_prefix" not in step
                             and "text_contains" in step))
                if not named:
                    continue
                interfaces = node.get_interfaces()
                if step.get("interface") and step["interface"] not in interfaces:
                    continue
                if "text_contains" in step:
                    if ("Text" not in interfaces or step["text_contains"] not in
                            Atspi.Text.get_text(node, 0, -1)):
                        continue
                matches.append(node)
            if len(matches) > 1:
                raise RuntimeError("Ambiguous native target: " + str(step))
            return matches[0] if matches else None

        def snapshot(filename):
            tree = []
            for node in nodes(root):
                node.clear_cache()
                interfaces = list(node.get_interfaces())
                item = {"role": node.get_role_name(), "name": node.get_name(),
                        "interfaces": interfaces,
                        "states": [state.value_nick for state in node.get_state_set().get_states()]}
                if "Action" in interfaces:
                    item["actions"] = [Atspi.Action.get_action_name(node, index)
                                       for index in range(Atspi.Action.get_n_actions(node))]
                if "Text" in interfaces:
                    item["text"] = Atspi.Text.get_text(node, 0, -1)
                    item["caret"] = Atspi.Text.get_caret_offset(node)
                    item["selections"] = []
                    for index in range(Atspi.Text.get_n_selections(node)):
                        span = Atspi.Text.get_selection(node, index)
                        item["selections"].append([span.start_offset, span.end_offset])
                tree.append(item)
            output = evidence / filename
            if output.resolve().parent != evidence:
                raise ValueError("Snapshot must be a filename inside the evidence directory")
            output.write_text(json.dumps(tree, indent=2) + "\n")

        for index, step in enumerate(plan.get("steps", [])):
            operation = step["op"]
            if operation == "snapshot":
                snapshot(step.get("file", f"native-{index:02}.json"))
            elif operation == "wait_file":
                output = evidence / step["file"]
                if output.resolve().parent != evidence:
                    raise ValueError("Expected file must be inside the evidence directory")
                wait_for(lambda: output.is_file() and output.stat().st_size > 0)
            elif operation == "wait_exit":
                code = application.wait(timeout=args.timeout)
                if code != step.get("exit_code", 0):
                    raise RuntimeError("Unexpected application exit code: " + str(code))
                result["exit_code"] = code
            elif operation == "absent":
                wait_for(lambda: target(step) is None)
            else:
                node = wait_for(lambda: target(step))
                if operation == "action":
                    okay = Atspi.Action.do_action(node, step.get("index", 0))
                elif operation == "focus":
                    okay = Atspi.Component.grab_focus(node)
                elif operation == "edit":
                    okay = Atspi.EditableText.set_text_contents(node, step["value"])
                elif operation == "text":
                    def expected_text():
                        current = target(step)
                        if current is None:
                            return False
                        value = Atspi.Text.get_text(current, 0, -1)
                        if "equals" in step:
                            return value == step["equals"]
                        return step["contains"] in value

                    wait_for(expected_text)
                    okay = True
                elif operation == "wait_name":
                    okay = True
                else:
                    raise ValueError("Unknown native operation: " + operation)
                if not okay:
                    raise RuntimeError("Native operation refused: " + str(step))
            events.append({"step": index, "request": step, "status": "passed"})
            (evidence / "events.json").write_text(json.dumps(events, indent=2) + "\n")
        result["status"] = "passed"
    except Exception as error:
        result["error"] = str(error)
    finally:
        for process in (application, registry):
            if process and process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        (evidence / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    return 0 if result["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
