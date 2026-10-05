#!/usr/bin/env python3
"""Read an isolated application's native AT-SPI text and activate a control."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time


def main():
    if len(sys.argv) < 2:
        raise SystemExit("usage: check-accessibility-linux.py <binary> [backend]")
    if sys.argv[1] != "--session":
        binary = str(Path(sys.argv[1]).resolve())
        with tempfile.TemporaryDirectory(prefix="gleditor-atspi-") as directory:
            env = dict(os.environ)
            env.update(SDL_VIDEODRIVER="offscreen", SDL_AUDIODRIVER="dummy",
                       LIBGL_ALWAYS_SOFTWARE="1", GSETTINGS_BACKEND="memory",
                       XDG_DATA_HOME=directory + "/data",
                       XDG_CONFIG_HOME=directory + "/config",
                       XDG_CACHE_HOME=directory + "/cache")
            return subprocess.call(
                ["dbus-run-session", "--", sys.executable, __file__,
                 "--session", binary, *sys.argv[2:]], env=env)

    import gi
    gi.require_version("Atspi", "2.0")
    from gi.repository import Atspi, Gio, GLib

    binary = sys.argv[2]
    backend = sys.argv[3] if len(sys.argv) > 3 else "opengl"
    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    bus.call_sync("org.a11y.Bus", "/org/a11y/bus",
                  "org.freedesktop.DBus.Properties", "Set",
                  GLib.Variant("(ssv)", ("org.a11y.Status", "ScreenReaderEnabled",
                                         GLib.Variant("b", True))),
                  None, Gio.DBusCallFlags.NONE, 5000, None)
    address = bus.call_sync(
        "org.a11y.Bus", "/org/a11y/bus", "org.a11y.Bus", "GetAddress", None,
        GLib.VariantType.new("(s)"), Gio.DBusCallFlags.NONE, 5000, None).unpack()[0]
    os.environ["AT_SPI_BUS_ADDRESS"] = address
    registry_path = next((path for path in [
        shutil.which("at-spi2-registryd"), "/usr/lib/at-spi2-registryd",
        "/usr/libexec/at-spi2-registryd", "/usr/lib/at-spi2-core/at-spi2-registryd"
    ] if path and Path(path).is_file()), None)
    if not registry_path:
        raise RuntimeError("Install the AT-SPI registry daemon")
    # Start it directly: dbus-broker activation can require a user systemd
    # instance that an isolated CI session deliberately does not have.
    registry = subprocess.Popen([registry_path], env=dict(
        os.environ, DBUS_SESSION_BUS_ADDRESS=address))
    application = None
    try:
        Atspi.init()
        with tempfile.TemporaryDirectory(prefix="gleditor-native-text-") as work:
            directory = Path(work)
            text = "Native accessibility validation sample."
            sample = directory / "sample.txt"
            sample.write_text(text + "\n", encoding="utf-8")
            args = [binary, "--backend", backend, "--no-present"]
            if Path(binary).name in ("xuzz", "xudu", "zigzag"):
                args += ["--import", str(sample), str(directory / "document")]
            else:
                args += [str(sample)]
            with (directory / "application.log").open("w+") as log:
                application = subprocess.Popen(args, cwd=directory, stdout=log,
                                               stderr=log)

                def nodes(root):
                    yield root
                    for index in range(root.get_child_count()):
                        child = root.get_child_at_index(index)
                        if child:
                            yield from nodes(child)

                def wait_for(predicate):
                    deadline = time.monotonic() + 30
                    while time.monotonic() < deadline:
                        if application.poll() is not None:
                            break
                        while GLib.MainContext.default().pending():
                            GLib.MainContext.default().iteration(False)
                        try:
                            found = predicate()
                            if found:
                                return found
                        except GLib.GError:
                            pass
                        time.sleep(0.1)
                    log.seek(0)
                    raise RuntimeError("Native accessibility readback timed out\n" + log.read())

                def application_node():
                    desktop = Atspi.get_desktop(0)
                    return next((node for node in nodes(desktop)
                                 if node.get_process_id() == application.pid and
                                 node.get_role() == Atspi.Role.APPLICATION), None)

                root = wait_for(application_node)

                def document_text():
                    for node in nodes(root):
                        if "Text" in node.get_interfaces():
                            if text in Atspi.Text.get_text(node, 0, -1):
                                return node
                    return None

                wait_for(document_text)
                print("PASS: native AT-SPI exposes document text", flush=True)
                if Path(binary).name in ("xuzz", "xudu", "zigzag"):
                    button = wait_for(lambda: next((node for node in nodes(root)
                                                   if node.get_name() == "New Document"), None))
                    before = sum(node.get_role() == Atspi.Role.LIST_ITEM for node in nodes(root))
                    if not Atspi.Action.do_action(button, 0):
                        raise RuntimeError("AT-SPI refused New Document action")
                    wait_for(lambda: sum(node.get_role() == Atspi.Role.LIST_ITEM
                                         for node in nodes(root)) > before)
                    print("PASS: native AT-SPI action creates a document", flush=True)

                    def readable_empty_document():
                        empty_editable = False
                        # Read every native text interface: an omitted value
                        # on an empty TextRun crashes AccessKit's consumer.
                        for node in nodes(root):
                            if "Text" not in node.get_interfaces():
                                continue
                            value = Atspi.Text.get_text(node, 0, -1)
                            if value == "" and node.get_state_set().contains(
                                    Atspi.StateType.EDITABLE):
                                empty_editable = True
                        return empty_editable

                    wait_for(readable_empty_document)
                    print("PASS: native AT-SPI reads the empty editable document", flush=True)
    finally:
        for process in (application, registry):
            if process and process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
