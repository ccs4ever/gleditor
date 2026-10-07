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

    def test_character_literals_do_not_change_expression_depth(self):
        for count in ("f(')')", "f('(', n)", r"f('\\', n)", "f(L')')"):
            self.assertEqual(len(POLICY.violations(
                f'text.substr(0, {count}) + "..."')), 1)
        self.assertEqual(POLICY.violations("'\"'; 'S'; '\\u2026';"), [])

    def test_raw_and_adjacent_literals_are_decoded(self):
        for source in ('R"(Sans 12)"', 'u8R"font(Sans Bold 10.5)font"',
                       '"Sans " /* comment */ "12"', r'"Sans\x20" "12"'):
            self.assertEqual(len(POLICY.violations(source)), 1, source)
        for suffix in ('R"(...)"', 'u8R"tag(…)tag"', '"." ".."',
                       '"" "…"', r'"\u2026"', r'U"\U00002026"'):
            self.assertEqual(len(POLICY.violations(
                f'text.substr(0, n) + {suffix}')), 1, suffix)
        self.assertEqual(POLICY.violations(
            'R"(text.substr(0, n) + "...")";'), [])

    def test_zero_integer_bases_and_suffixes(self):
        for zero in ('0u', '0ULL', '0ll', '00UL', '0x0u', '0b00LL', '0z', '0UZ'):
            self.assertEqual(len(POLICY.violations(
                f'text.substr({zero}, n) + "..."')), 1, zero)
        for nonzero in ('1u', '0x10u', '0.0', 'count'):
            self.assertEqual(POLICY.violations(
                f'text.substr({nonzero}, n) + "..."'), [])

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
                     'apps/common/ui/new_widget.cpp', 'apps/xudu/new_overlay.hpp',
                     'src/floating_toolbar_3d.cpp', 'include/gleditor/floating_toolbar_3d.hpp',
                     'apps/xudu/world_card_presentation.hpp', 'src/ui/world_panel.cpp'):
            self.assertTrue(POLICY.is_ui(Path(path)))
        self.assertFalse(POLICY.is_ui(Path('apps/common/xanadu/system_docs.cpp')))


if __name__ == '__main__':
    unittest.main()
