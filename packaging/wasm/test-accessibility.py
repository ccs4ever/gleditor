#!/usr/bin/env python3
"""Check actual Chromium accessibility delivery and browser action dispatch.

Requires Python Playwright and its Chromium browser. The browser stays headless.
Use --browser to reuse an installed Chromium rather than download another one.
This exercises the DOM adapter; it is not a full WebAssembly application smoke test.
"""

import argparse
import json
from pathlib import Path
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from threading import Thread

from playwright.sync_api import sync_playwright


def node(identifier, role, **properties):
    return {
        "id": str(identifier),
        "role": role,
        "label": "",
        "value": "",
        "description": "",
        "placeholder": "",
        "children": [],
        "actions": 0,
        "focusable": False,
        "readOnly": False,
        "modal": False,
        "live": 0,
        **properties,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--browser", help="Chromium executable path")
    parser.add_argument("--compiled", type=Path, help="Compiled test-accessibility.html fixture")
    arguments = parser.parse_args()
    source = Path(__file__).with_name("accessibility.js").read_text()
    # Owner/local IDs can exceed JavaScript's exact-integer range.
    field_id = "18446744073709551615"
    document_id = "9007199254741001"
    run_id = "9007199254741002"
    snapshot = {
        "focus": document_id,
        "nodes": [
            node(0, "window", label="Gleditor", children=[document_id, field_id, "3", "4", "5", "6", "10", "13"]),
            node(document_id, "multilineTextInput", label="Research", actions=3,
                 focusable=True, children=[run_id],
                 selection={"anchor": {"node": run_id, "character": 2},
                            "focus": {"node": run_id, "character": 3}}),
            node(run_id, "textRun", value="A😀B\nsecond"),
            node(field_id, "textInput", label="Title", actions=5, focusable=True,
                 value="Initial", description="Name this document", placeholder="Untitled"),
            node(3, "button", label="Save <draft>", actions=3, focusable=True),
            node(4, "link", label="Follow source", actions=3, focusable=True),
            node(5, "switch", label="Read aloud", actions=3, focusable=True, toggled=True),
            node(6, "log", label="Status", value="Saved", live=1),
            node(10, "comboBox", label="Format", value="Plain", actions=3, focusable=True,
                 children=["11", "12"]),
            node(11, "listItem", label="Plain", actions=3, focusable=True),
            node(12, "listItem", label="Rich", actions=3, focusable=True),
            node(13, "label", label="Playback position", value="00:01 of 00:10", actions=3,
                 focusable=True),
        ],
    }
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch(headless=True, executable_path=arguments.browser)
        page = browser.new_page()
        errors = []
        page.on("pageerror", lambda error: errors.append(str(error)))
        page.set_content("""<!doctype html><html lang="en"><body>
            <canvas id="canvas"></canvas><script>
            window.Module = {canvas: document.getElementById('canvas')};
            window.keyEvents = 0;
            window.addEventListener('keydown', () => ++window.keyEvents);
            </script></body></html>""")
        page.add_script_tag(content=source)
        page.evaluate("snapshot => Module.gleditorAccessibility.update(snapshot)", snapshot)
        session = page.context.new_cdp_session(page)

        def accessibility():
            return [item for item in session.send("Accessibility.getFullAXTree")["nodes"]
                    if not item.get("ignored")]

        def named(role, name):
            return next(item for item in accessibility()
                        if item.get("role", {}).get("value") == role
                        and item.get("name", {}).get("value") == name)

        document = named("textbox", "Research")
        assert document["value"]["value"] == "A😀B\nsecond"
        assert named("textbox", "Title")["description"]["value"] == "Name this document"
        named("button", "Save <draft>")
        named("link", "Follow source")
        named("combobox", "Format")
        named("StaticText", "Playback position 00:01 of 00:10")
        switch = named("switch", "Read aloud")
        assert any(prop["name"] == "checked" and prop["value"]["value"] == "true"
                   for prop in switch["properties"])
        status = named("log", "Status")
        assert any(prop["name"] == "live" and prop["value"]["value"] == "polite"
                   for prop in status["properties"])
        field = page.locator(f"#gleditor-a11y-{field_id}")
        research = page.locator(f"#gleditor-a11y-{document_id}")
        assert research.evaluate("element => [element.selectionStart, element.selectionEnd]") == [3, 4]
        assert page.locator("#canvas").get_attribute("aria-hidden") == "true"
        assert page.locator("draft").count() == 0

        def drain():
            return page.evaluate("""() => {
                const result = []; let request;
                while ((request = Module.gleditorAccessibility.nextAction())) result.push(request);
                return result;
            }""")

        field.fill("Édited name")
        requested = drain()
        assert {"node": field_id, "action": 0, "value": ""} in requested
        assert any(item["node"] == field_id and item["action"] == 2 for item in requested)
        field.press("a")
        assert page.evaluate("keyEvents") == 0
        research.focus()
        research.press("a")
        assert page.evaluate("keyEvents") == 1

        page.get_by_role("combobox", name="Format").select_option("12")
        assert {"node": "12", "action": 1, "value": ""} in drain()
        page.locator("#gleditor-a11y-13").press("Enter")
        assert {"node": "13", "action": 1, "value": ""} in drain()
        assert page.evaluate("keyEvents") == 1
        assert research.input_value() == "A😀B\nsecond"
        drain()
        page.get_by_role("button", name="Save <draft>").press("Enter")
        assert {"node": "3", "action": 1, "value": ""} in drain()
        page.get_by_role("link", name="Follow source").press("Enter")
        assert {"node": "4", "action": 1, "value": ""} in drain()
        assert page.evaluate("keyEvents") == 1

        page.evaluate(f"window.retained = document.getElementById('gleditor-a11y-{field_id}')")
        snapshot["focus"] = field_id
        snapshot["nodes"][3]["value"] = "Updated"
        page.evaluate("snapshot => Module.gleditorAccessibility.update(snapshot)", snapshot)
        assert field.evaluate("element => element === window.retained")
        assert field.input_value() == "Updated"
        assert field.evaluate("element => element === document.activeElement")
        assert drain() == []  # Publication itself must never request actions.

        modal_snapshot = json.loads(json.dumps(snapshot))
        modal_snapshot["nodes"][0]["children"].append("7")
        modal_snapshot["nodes"].extend([
            node(7, "dialog", label="Confirm", modal=True, children=["8"]),
            node(8, "button", label="Accept", actions=3, focusable=True),
        ])
        modal_snapshot["focus"] = "8"
        page.evaluate("snapshot => Module.gleditorAccessibility.update(snapshot)", modal_snapshot)
        named("dialog", "Confirm")
        named("button", "Accept")
        assert not any(item.get("name", {}).get("value") == "Research" for item in accessibility())
        assert research.evaluate("element => element.inert")
        page.evaluate("snapshot => Module.gleditorAccessibility.update(snapshot)", snapshot)
        named("textbox", "Research")
        assert not research.evaluate("element => element.inert")

        snapshot["nodes"][0]["children"].append("9")
        secret_node = node(9, "passwordInput", label="Passphrase", value="******",
                           actions=5, focusable=True)
        snapshot["nodes"].append(secret_node)
        page.evaluate("snapshot => Module.gleditorAccessibility.update(snapshot)", snapshot)
        secret = page.locator("#gleditor-a11y-9")
        assert secret.input_value() == ""
        assert "already set" in secret.get_attribute("placeholder")
        secret.fill("new💡secret")
        assert {"node": "9", "action": 2, "value": "new💡secret"} in drain()
        secret_node["value"] = "**********"
        snapshot["focus"] = "9"
        page.evaluate("snapshot => Module.gleditorAccessibility.update(snapshot)", snapshot)
        assert secret.input_value() == "new💡secret"
        assert "new💡secret" not in json.dumps(accessibility(), ensure_ascii=False)
        field.focus()
        assert secret.input_value() == ""
        drain()
        snapshot["focus"] = field_id
        page.evaluate("snapshot => Module.gleditorAccessibility.update(snapshot)", snapshot)
        assert secret.input_value() == ""

        field.fill("Pending")
        snapshot["nodes"] = [item for item in snapshot["nodes"] if item["id"] != field_id]
        snapshot["nodes"][0]["children"].remove(field_id)
        snapshot["focus"] = document_id
        page.evaluate("snapshot => Module.gleditorAccessibility.update(snapshot)", snapshot)
        assert drain() == []  # Removed sources cannot receive stale requests.
        page.evaluate("Module.gleditorAccessibility.close()")
        assert page.locator("#gleditor-accessibility").count() == 0
        assert not errors, errors
        if arguments.compiled:
            class QuietHandler(SimpleHTTPRequestHandler):
                def log_message(self, *_arguments):
                    pass

            handler = partial(QuietHandler, directory=str(arguments.compiled.parent.resolve()))
            server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
            Thread(target=server.serve_forever, daemon=True).start()
            try:
                page.goto(f"http://127.0.0.1:{server.server_port}/{arguments.compiled.name}")
                compiled = page.get_by_role("textbox", name="Compiled text")
                compiled.wait_for()
                assert compiled.input_value() == "A😀B\nsecond"
                native = named("textbox", "Compiled text")
                assert native["description"]["value"].startswith("C++ tree delivered")
                compiled.fill('Édited\n"quoted"\\value')
                actions = page.evaluate("""() => {
                    const actions = [];
                    while (Module._a11y_take_action()) actions.push(Module.lastAction);
                    return actions;
                }""")
                assert {"node": field_id, "action": 2, "value": 'Édited\n"quoted"\\value'} in actions
                assert not errors, errors
            finally:
                server.shutdown()
                server.server_close()
        browser.close()
    print("PASS: browser AX roles/text/state, Unicode selection, focus, actions, modal isolation, secret replacement, stale requests")
    if arguments.compiled:
        print("PASS: compiled C++ tree -> browser AX API -> native ActionRequest round trip")


if __name__ == "__main__":
    main()
