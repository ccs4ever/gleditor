# Formatter and linter quirks

Formatter and linter settings are kept in one place per language so `make format` never leaves a
file `make lint` rejects; `tools/check-config-harmony.sh` (run by both `format-check` and `lint`)
enforces that `.editorconfig`, `.clang-format`, `.yamlfmt`, `.yamllint`, `.mdl_style.rb`, `.clangd`
and `.clang-tidy` agree.

- **`yamlfmt` and `mdformat`** are Go/Python tools (`go install`/`pip install`), chosen over Node
  equivalents to keep the tree zero-Node. `.yamlfmt`'s `indentless_arrays` matches `.yamllint`'s
  `indent-sequences: false`; its `retain_line_breaks` has a known bug with folded `>` scalars
  (`google/yamlfmt#84`), so this repo has none.
- **`mdformat` requires three plugins and refuses to start without them:**
  `pip install --user mdformat-gfm mdformat-dollarmath mdformat-frontmatter`. The Makefile passes
  them as `--extensions`, so a missing one fails loudly by name rather than silently corrupting
  `design/` (unplugged `mdformat` rewrites `\text` → `\\text` and reflows tables to prose).
  `dollarmath` keeps `$...$` and `$$...$$` opaque; `frontmatter` keeps `.claude/skills/*/SKILL.md`'s
  YAML header (also visible via `.agents -> .claude`) from being parsed as a heading (`mdl -i` is
  the lint-side half).
- **Missing tools and Ruby `mdl`:** If a linting or formatting tool is missing, install it locally
  when possible and rerun the gate; a skipped tool is not a passing check. For a user-installed Ruby
  `mdl`, `make`'s redirected `XDG_DATA_HOME` may hide the gem, so pass
  `GEM_HOME=$(ruby -e 'print Gem.user_dir')` to `make lint`.
- **`MD013` and math/code fences:** `--wrap 100` matches `MD013`'s limit. An inline code span or
  `$...$` near column 100 is atomic to `mdformat` and can still overflow: shorten the prose, or
  promote `$$...$$` to a ```` ```math ```` fence (exempt from `MD013`). A `$$...$$` block directly
  under a list item needs blank lines on both sides or `mdformat` refuses to write.
- **Unfenced code in `design/`** is invisible to both tools (`mdl` flags `#include` as a bad header,
  `mdformat` reflows it). Fence anything that looks like code.
- **`editorconfig-checker`** is not a gate: it misreads `clang-format` continuation indents and
  table padding. Use it only to sanity-check `.editorconfig` itself.
