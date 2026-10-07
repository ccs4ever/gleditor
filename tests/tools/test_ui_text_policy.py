"""Acceptance checks for the production UI policy scanner."""

import importlib.util
from pathlib import Path
import unittest
import subprocess
import sys
import tempfile

SPEC = importlib.util.spec_from_file_location(
    "ui_text_policy", Path(__file__).resolve().parents[2] / "tools/check-ui-text-policy.py")
POLICY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(POLICY)


class UiTextPolicyTest(unittest.TestCase):
    def test_multiline_byte_ellipsis_and_nested_count(self):
        for source in ('text.substr(0, 10) + "..."',
                       'text.substr(\n0, min(10, n))\n + "…"',
                       '(text.substr(0, count)) + "\\u2026"'):
            self.assertEqual(len(POLICY.violations(source)), 1)

    def test_comments_and_non_presentation_substrings(self):
        source = '// text.substr(0, 10) + "..."\n/* "Sans 12" */\n'
        source += 'text.substr(0, split); text.substr(2, n) + "...";'
        self.assertEqual(POLICY.violations(source), [])

    def test_cli_rejects_ui_source_and_reports_line(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bad_overlay.cpp"
            path.write_text('// harmless example: "Sans 12"\n"Sans 12";')
            result = subprocess.run([sys.executable, SPEC.origin, str(path)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)
            self.assertIn("bad_overlay.cpp:2:", result.stdout)
            path.write_text('text.substr(0, count) + "...";')
            result = subprocess.run([sys.executable, SPEC.origin, str(path)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)
            self.assertIn("TextFit", result.stdout)

    def test_fonts_and_scope(self):
        self.assertEqual(len(POLICY.violations('"Sans 12"; "Sans Bold 10.5";')), 2)
        self.assertEqual(POLICY.violations('"Sans"; "Monospace";'), [])
        self.assertEqual(POLICY.violations('"Sans 12";', fonts=False), [])
        for path in ('apps/xudu/satelloid.cpp', 'include/gleditor/form.hpp',
                     'apps/common/ui/new_widget.cpp', 'apps/xudu/new_overlay.hpp'):
            self.assertTrue(POLICY.is_ui(Path(path)))
        self.assertFalse(POLICY.is_ui(Path('apps/common/xanadu/system_docs.cpp')))


if __name__ == '__main__':
    unittest.main()
