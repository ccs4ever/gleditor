#!/usr/bin/env python3
"""Validate native AT-SPI text, controls and Xuzz navigation in isolated sessions."""

import contextlib
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time


def main():
    if len(sys.argv) < 2:
        raise SystemExit(
            "usage: check-accessibility-linux.py <binary> [backend] [--walks|--navigation]"
        )
    navigation = "--navigation" in sys.argv
    keyboard = navigation or "--walks" in sys.argv
    if sys.argv[1] != "--session":
        binary = str(Path(sys.argv[1]).resolve())
        with tempfile.TemporaryDirectory(prefix="gleditor-atspi-") as directory:
            env = dict(os.environ)
            env.update(
                SDL_VIDEODRIVER="x11" if keyboard else "offscreen",
                SDL_AUDIODRIVER="dummy",
                LIBGL_ALWAYS_SOFTWARE="1",
                GSETTINGS_BACKEND="memory",
                XDG_DATA_HOME=directory + "/data",
                XDG_CONFIG_HOME=directory + "/config",
                XDG_CACHE_HOME=directory + "/cache",
            )
            command = ["dbus-run-session"]
            if env.get("DBUS_SESSION_BUS_CONFIG"):
                command += ["--config-file", env["DBUS_SESSION_BUS_CONFIG"]]
            command += [
                "--",
                sys.executable,
                __file__,
                "--session",
                binary,
                *sys.argv[2:],
            ]
            if keyboard:
                command = ["xvfb-run", "-a", "-s", "-screen 0 1280x1024x24", *command]
            result = subprocess.call(command, env=env)
            if env.get("GLEDITOR_A11Y_EVIDENCE"):
                shutil.copytree(directory, env["GLEDITOR_A11Y_EVIDENCE"])
            return result

    import gi

    gi.require_version("Atspi", "2.0")
    from gi.repository import Atspi, Gio, GLib

    binary = sys.argv[2]
    backend = (
        sys.argv[3]
        if len(sys.argv) > 3 and not sys.argv[3].startswith("--")
        else "opengl"
    )
    walks = keyboard
    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    bus.call_sync(
        "org.a11y.Bus",
        "/org/a11y/bus",
        "org.freedesktop.DBus.Properties",
        "Set",
        GLib.Variant(
            "(ssv)", ("org.a11y.Status", "ScreenReaderEnabled", GLib.Variant("b", True))
        ),
        None,
        Gio.DBusCallFlags.NONE,
        5000,
        None,
    )
    address = bus.call_sync(
        "org.a11y.Bus",
        "/org/a11y/bus",
        "org.a11y.Bus",
        "GetAddress",
        None,
        GLib.VariantType.new("(s)"),
        Gio.DBusCallFlags.NONE,
        5000,
        None,
    ).unpack()[0]
    os.environ["AT_SPI_BUS_ADDRESS"] = address
    registry_path = next(
        (
            path
            for path in [
                shutil.which("at-spi2-registryd"),
                "/usr/lib/at-spi2-registryd",
                "/usr/libexec/at-spi2-registryd",
                "/usr/lib/at-spi2-core/at-spi2-registryd",
            ]
            if path and Path(path).is_file()
        ),
        None,
    )
    if not registry_path:
        raise RuntimeError("Install the AT-SPI registry daemon")
    # Start it directly: dbus-broker activation can require a user systemd
    # instance that an isolated CI session deliberately does not have.
    registry = subprocess.Popen(
        [registry_path], env=dict(os.environ, DBUS_SESSION_BUS_ADDRESS=address)
    )
    application = None
    try:
        Atspi.init()
        if walks:
            fixture = Path(os.environ["XDG_DATA_HOME"]).parent / "fixture"
            fixture.mkdir()
            workspace = contextlib.nullcontext(str(fixture))
        else:
            workspace = tempfile.TemporaryDirectory(prefix="gleditor-native-text-")
        with workspace as work:
            directory = Path(work)
            text = "Native accessibility validation sample."
            sample = directory / "sample.txt"
            sample.write_text(text + "\n", encoding="utf-8")
            args = [binary, "--backend", backend]
            if not walks:
                args += ["--no-present"]
            if Path(binary).name in ("xuzz", "xudu", "zigzag"):
                if navigation:
                    text = "alpha bravo charlie"
                    # Preserve both documents before marking immutable endpoint versions.
                    args += [
                        str(directory / "document"),
                        "--type",
                        text,
                        "--chord",
                        "Ctrl+S",
                        "--select",
                        "0,5",
                        "--chord",
                        "Ctrl+Alt+[",
                        "--select",
                        "12,19",
                        "--chord",
                        "Ctrl+Alt+[",
                        "--chord",
                        "Ctrl+N",
                        "--type",
                        "one two three",
                        "--chord",
                        "Ctrl+S",
                        "--key",
                        "enter",
                        "--select",
                        "0,3",
                        "--chord",
                        "Ctrl+Alt+]",
                        "--select",
                        "4,7",
                        "--chord",
                        "Ctrl+Alt+]",
                        "--select",
                        "8,13",
                        "--chord",
                        "Ctrl+Alt+]",
                        "--chord",
                        "Ctrl+1",
                        "--chord",
                        "Ctrl+Alt+L",
                        "--chord",
                        "Ctrl+S",
                        "--chord",
                        "Alt+Shift+N",
                        "--chord",
                        "Alt+Shift+W",
                    ]
                elif walks:
                    # These are UI automation hands using the default bindings.
                    # This focused fixture tests native metadata and note controls;
                    # it does not claim a complete branching journey.
                    args += [
                        str(directory / "document"),
                        "--type",
                        text,
                        "--select",
                        "0,6",
                        "--chord",
                        "Ctrl+Alt+[",
                        "--select",
                        "7,20",
                        "--chord",
                        "Ctrl+Alt+]",
                        "--chord",
                        "Ctrl+Alt+L",
                        "--chord",
                        "Alt+Shift+N",
                        "--chord",
                        "Alt+Shift+X",
                        "--chord",
                        "Alt+Shift+Return",
                        "--chord",
                        "Alt+Shift+W",
                    ]
                else:
                    args += ["--import", str(sample), str(directory / "document")]
            else:
                args += [str(sample)]
            with (directory / "application.log").open("w+") as log:
                application = subprocess.Popen(
                    args, cwd=directory, stdout=log, stderr=log
                )

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
                    raise RuntimeError(
                        "Native accessibility readback timed out\n" + log.read()
                    )

                def application_node():
                    desktop = Atspi.get_desktop(0)
                    return next(
                        (
                            node
                            for node in nodes(desktop)
                            if node.get_process_id() == application.pid
                            and node.get_role() == Atspi.Role.APPLICATION
                        ),
                        None,
                    )

                root = wait_for(application_node)

                def document_text():
                    for node in nodes(root):
                        if "Text" in node.get_interfaces():
                            if text in Atspi.Text.get_text(node, 0, -1):
                                return node
                    return None

                wait_for(document_text)
                print("PASS: native AT-SPI exposes document text", flush=True)
                if walks:

                    def named(name):
                        return next(
                            (node for node in nodes(root) if node.get_name() == name),
                            None,
                        )

                    def click(name):
                        control = wait_for(lambda: named(name))
                        if not Atspi.Action.do_action(control, 0):
                            raise RuntimeError("AT-SPI refused " + name)

                    def snapshot(step):
                        from PIL import ImageGrab

                        def native_tree():
                            native_nodes = []
                            for node in nodes(root):
                                node.clear_cache()
                                native_nodes.append(node)
                            tree = [
                                {
                                    "role": node.get_role_name(),
                                    "name": node.get_name(),
                                    "interfaces": list(node.get_interfaces()),
                                }
                                for node in native_nodes
                            ]
                            for entry, node in zip(tree, native_nodes):
                                if "Text" in entry["interfaces"]:
                                    entry["text"] = Atspi.Text.get_text(node, 0, -1)
                                    entry["caret"] = Atspi.Text.get_caret_offset(node)
                                    entry["selections"] = []
                                    for index in range(
                                        Atspi.Text.get_n_selections(node)
                                    ):
                                        selected_range = Atspi.Text.get_selection(
                                            node, index
                                        )
                                        entry["selections"].append(
                                            [
                                                selected_range.start_offset,
                                                selected_range.end_offset,
                                            ]
                                        )
                            return tree

                        tree = wait_for(native_tree)
                        (directory / (step + ".json")).write_text(
                            json.dumps(tree, indent=2) + "\n", encoding="utf-8"
                        )
                        painted = 0

                        def painted_frame():
                            nonlocal painted
                            frame = ImageGrab.grab(xdisplay=os.environ["DISPLAY"])
                            colors = frame.getcolors(maxcolors=32)
                            painted = painted + 1 if colors is None else 0
                            return frame if painted >= 2 else None

                        wait_for(painted_frame).save(directory / (step + ".png"))

                    if navigation:
                        # Close can also name a setup dialog; wait for the final
                        # Walks marker so native actions do not race the UI hands.
                        wait_for(lambda: named("Walks"))
                        ready = wait_for(
                            lambda: next(
                                (
                                    node
                                    for node in nodes(root)
                                    if node.get_name() == "Close"
                                ),
                                None,
                            )
                        )
                        if not Atspi.Action.do_action(ready, 0):
                            raise RuntimeError("AT-SPI refused closing Walks")
                        wait_for(
                            lambda: next(
                                (
                                    node
                                    for node in nodes(root)
                                    if node.get_name() == "Enter"
                                ),
                                None,
                            )
                        )

                        def doc(value):
                            return next(
                                (
                                    n
                                    for n in nodes(root)
                                    if "Text" in n.get_interfaces()
                                    and Atspi.Text.get_text(n, 0, -1) == value
                                ),
                                None,
                            )

                        def selected(value, start, end):
                            n = doc(value)
                            if not n or not Atspi.Text.get_n_selections(n):
                                return False
                            selection = Atspi.Text.get_selection(n, 0)
                            return (
                                selection.start_offset == start
                                and selection.end_offset == end
                            )

                        stores = [
                            directory / "document",
                            *directory.glob("doc_*.xanadoc"),
                        ]
                        if len(stores) != 2:
                            raise RuntimeError(
                                "Fixture did not preserve exactly two documents"
                            )
                        if named("Preserve Temporary Xanadoc"):
                            raise RuntimeError(
                                "Fixture left its preservation dialog open"
                            )

                        def hashes():
                            return {
                                str(store / name): hashlib.sha256(
                                    (store / name).read_bytes()
                                ).hexdigest()
                                for store in stores
                                for name in ("ops.nodes", "store.tables")
                            }

                        original = hashes()
                        source = wait_for(lambda: doc(text))
                        before = Atspi.Text.get_caret_offset(source)
                        click("left member 2 of 2")
                        click("right member 3 of 3")
                        target_name = "occurrence 1 of 1, document 1, bytes 8 to 13"
                        occurrence = wait_for(lambda: named(target_name))
                        if not Atspi.Component.grab_focus(occurrence):
                            raise RuntimeError("AT-SPI refused occurrence focus")
                        if Atspi.Text.get_caret_offset(source) != before:
                            raise RuntimeError(
                                "Occurrence preview moved the source caret"
                            )
                        print(
                            "PASS: native 2x3 member selection and occurrence Focus preview",
                            flush=True,
                        )
                        snapshot("endpoints-preview")
                        click(target_name)
                        wait_for(lambda: selected("one two three", 8, 13))
                        print(
                            "PASS: native occurrence Enter selects exact target range",
                            flush=True,
                        )
                        snapshot("endpoints-enter")
                        click("Back")
                        wait_for(
                            lambda: doc(text)
                            and Atspi.Text.get_caret_offset(doc(text)) == before
                        )
                        click("right member 1 of 3")
                        click("occurrence 1 of 1, document 1, bytes 0 to 3")
                        wait_for(lambda: selected("one two three", 0, 3))
                        windows = subprocess.check_output(
                            [
                                "xdotool",
                                "search",
                                "--onlyvisible",
                                "--pid",
                                str(application.pid),
                            ],
                            text=True,
                        ).splitlines()
                        subprocess.run(
                            ["xdotool", "windowfocus", "--sync", windows[0]], check=True
                        )
                        subprocess.run(["xdotool", "key", "alt+shift+w"], check=True)
                        wait_for(lambda: named("Walks"))
                        if not doc("one two three"):
                            raise RuntimeError("Walks shortcut changed document text")
                        second = wait_for(
                            lambda: next(
                                (
                                    n
                                    for n in nodes(root)
                                    if n.get_name().startswith("Visit 2 · parent 1")
                                ),
                                None,
                            )
                        )
                        if not Atspi.Component.grab_focus(second):
                            raise RuntimeError(
                                "AT-SPI refused saved-visit preview focus"
                            )
                        wait_for(
                            lambda: next(
                                (
                                    n
                                    for n in nodes(root)
                                    if n.get_name().startswith("Preview Visit 2:")
                                ),
                                None,
                            )
                        )
                        wait_for(
                            lambda: next(
                                (
                                    n
                                    for n in nodes(root)
                                    if n.get_name().startswith("Visit 3 · parent 1")
                                    and "current" in n.get_name()
                                ),
                                None,
                            )
                        )
                        snapshot("walks-branch-preview")
                        click("Reference (R)")
                        wait_for(lambda: named("Visit referenced"))
                        click("Restore visit")
                        wait_for(lambda: selected("one two three", 8, 13))
                        print(
                            "PASS: native Back branches, Walks preview preserves current, reference and Restore return exact range",
                            flush=True,
                        )
                        snapshot("walks-restored")
                        if hashes() != original:
                            raise RuntimeError(
                                "Navigation changed visited-document operations or tables"
                            )
                        (directory / "document-hashes.json").write_text(
                            json.dumps(original, indent=2) + "\n", encoding="utf-8"
                        )
                        print(
                            "PASS: native navigation preserves visited-document store hashes",
                            flush=True,
                        )
                        windows = subprocess.check_output(
                            [
                                "xdotool",
                                "search",
                                "--onlyvisible",
                                "--pid",
                                str(application.pid),
                            ],
                            text=True,
                        ).splitlines()
                        subprocess.run(
                            ["xdotool", "windowfocus", "--sync", windows[0]], check=True
                        )
                        subprocess.run(["xdotool", "key", "ctrl+q"], check=True)
                        application.wait(timeout=15)
                        application = subprocess.Popen(
                            [binary, "--backend", backend, str(directory / "document")],
                            cwd=directory,
                            stdout=log,
                            stderr=log,
                        )
                        root = wait_for(application_node)

                        def new_window():
                            result = subprocess.run(
                                [
                                    "xdotool",
                                    "search",
                                    "--onlyvisible",
                                    "--pid",
                                    str(application.pid),
                                ],
                                text=True,
                                capture_output=True,
                            )
                            return (
                                result.stdout.splitlines()
                                if result.returncode == 0
                                else None
                            )

                        windows = wait_for(new_window)
                        subprocess.run(
                            ["xdotool", "windowfocus", "--sync", windows[0]], check=True
                        )
                        subprocess.run(["xdotool", "key", "alt+shift+w"], check=True)
                        wait_for(lambda: named("Walks"))
                        second = wait_for(
                            lambda: next(
                                (
                                    n
                                    for n in nodes(root)
                                    if n.get_name().startswith("Visit 2 · parent 1")
                                    and "reference" in n.get_name()
                                ),
                                None,
                            )
                        )
                        if not Atspi.Component.grab_focus(second):
                            raise RuntimeError(
                                "Native saved-visit focus failed after restart"
                            )
                        wait_for(
                            lambda: next(
                                (
                                    n
                                    for n in nodes(root)
                                    if n.get_name().startswith("Preview Visit 2:")
                                ),
                                None,
                            )
                        )
                        snapshot("walks-restarted")
                        wait_for(
                            lambda: next(
                                (
                                    n
                                    for n in nodes(root)
                                    if n.get_name().startswith("Target unavailable")
                                ),
                                None,
                            )
                        )
                        click("Restore visit")
                        wait_for(
                            lambda: next(
                                (
                                    n
                                    for n in nodes(root)
                                    if n.get_name().startswith("target unavailable;")
                                ),
                                None,
                            )
                        )
                        wait_for(
                            lambda: next(
                                (
                                    n
                                    for n in nodes(root)
                                    if n.get_name().startswith("Visit 2 · parent 1")
                                    and "current" in n.get_name()
                                ),
                                None,
                            )
                        )
                        visits = [
                            n
                            for n in nodes(root)
                            if n.get_name().startswith("Visit ")
                            and n.get_role() == Atspi.Role.LIST_ITEM
                        ]
                        if len(visits) != 3:
                            raise RuntimeError("Unavailable Restore appended a visit")
                        snapshot("walks-restarted-refused")
                        restarted_hashes = hashes()
                        if restarted_hashes != original:
                            raise RuntimeError(
                                "Restart or unavailable Restore changed visited documents"
                            )
                        (directory / "restart-document-hashes.json").write_text(
                            json.dumps(
                                {"before": original, "after": restarted_hashes},
                                indent=2,
                            )
                            + "\n",
                            encoding="utf-8",
                        )
                        print(
                            "PASS: native branches/reference survive restart and unavailable Restore is refused",
                            flush=True,
                        )
                        return 0

                    wait_for(lambda: named("Walks"))
                    wait_for(
                        lambda: next(
                            (
                                node
                                for node in nodes(root)
                                if node.get_name().startswith("Preview Visit ")
                            ),
                            None,
                        )
                    )
                    wait_for(
                        lambda: next(
                            (
                                node
                                for node in nodes(root)
                                if node.get_name().startswith("Target available")
                            ),
                            None,
                        )
                    )
                    print(
                        "PASS: native Walks preview and availability have readable names",
                        flush=True,
                    )
                    snapshot("walks-preview")
                    click("Edit note (N)")

                    def editable_note():
                        node = named("Edit visit note")
                        # The snapshot queried this node while it was static text.
                        # Refresh the client's interface cache after the role change.
                        if node:
                            node.clear_cache()
                        if (
                            node
                            and "Component" in node.get_interfaces()
                            and "Text" in node.get_interfaces()
                        ):
                            return node
                        return None

                    note = wait_for(editable_note)
                    windows = subprocess.check_output(
                        [
                            "xdotool",
                            "search",
                            "--onlyvisible",
                            "--pid",
                            str(application.pid),
                        ],
                        text=True,
                    ).splitlines()
                    subprocess.run(
                        ["xdotool", "windowfocus", "--sync", windows[0]], check=True
                    )
                    if not Atspi.Component.grab_focus(note):
                        raise RuntimeError("AT-SPI refused note focus")
                    if Atspi.Text.get_text(note, 0, -1) != "":
                        raise RuntimeError("New visit note is not empty")
                    note_text = "native branch note"
                    if not Atspi.generate_keyboard_event(
                        0, note_text, Atspi.KeySynthType.STRING
                    ):
                        raise RuntimeError("AT-SPI refused keyboard input")
                    wait_for(lambda: Atspi.Text.get_text(note, 0, -1) == note_text)
                    snapshot("walks-note-input")
                    click("Save note")
                    wait_for(lambda: named("Visit note: " + note_text))
                    snapshot("walks-note-saved")
                    print(
                        "PASS: native note focus, keyboard input, text readback and Save note",
                        flush=True,
                    )
                    return 0
                if Path(binary).name in ("xuzz", "xudu", "zigzag"):
                    button = wait_for(
                        lambda: next(
                            (
                                node
                                for node in nodes(root)
                                if node.get_name() == "New Document"
                            ),
                            None,
                        )
                    )
                    before = sum(
                        node.get_role() == Atspi.Role.LIST_ITEM for node in nodes(root)
                    )
                    if not Atspi.Action.do_action(button, 0):
                        raise RuntimeError("AT-SPI refused New Document action")
                    wait_for(
                        lambda: sum(
                            node.get_role() == Atspi.Role.LIST_ITEM
                            for node in nodes(root)
                        )
                        > before
                    )
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
                                Atspi.StateType.EDITABLE
                            ):
                                empty_editable = True
                        return empty_editable

                    wait_for(readable_empty_document)
                    print(
                        "PASS: native AT-SPI reads the empty editable document",
                        flush=True,
                    )
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
