# Formatting and linting, by language

Every language in this tree has at most one formatter and one linter, and
each pair's settings are kept in one place on purpose (see below), so running
the formatter never leaves a file the linter still rejects. `make format`
runs every formatter in place; `make format-check` runs the same tools
`--dry-run`, which is what CI's `format` job gates on; `make lint` runs the
linters. Both run on a checkout with no graphics/build toolchain installed —
see CLAUDE.md's Makefile gotchas for how (`NO_SDL_GOALS`).

| Language                                                 | Formatter                                            | Linter                                                                                                                                   | CI gate                                                                                             |
| ---------------------------------------------------------| ----------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------| ------------------------------------------------------------------------------------------------------|
| C++ (`.cpp`/`.hpp`/`.h`)                                 | clang-format (`make format`)                         | clang-tidy (`.clangd`, editor-only)                                                                                                      | blocking                                                                                             |
| GLSL (`.glsl`)                                           | clang-format, same as C++                            | `glslangValidator` via `make shaders` (compiles every shader)                                                                            | blocking (both)                                                                                      |
| Shell (`.sh`, `PKGBUILD`)                                | shfmt (`make format`)                                | shellcheck (`make lint`)                                                                                                                 | blocking                                                                                             |
| YAML (workflows, dependabot)                             | yamlfmt (`make format`), config in `.yamlfmt`        | yamllint (`make lint`), config in `.yamllint`                                                                                            | blocking                                                                                             |
| Markdown (README, CLAUDE.md, design/)                    | mdformat `--wrap keep` (`make format`)               | mdl (`make lint`), config in `.mdlrc`/`.mdl_style.rb`                                                                                    | blocking                                                                                             |
| Nix (`flake.nix`, `packaging/nix/*.nix`)                 | nixfmt-rfc-style, the flake's own `formatter` output | nix-linter                                                                                                                               | blocking (job `nix`), separate from `format` because it needs Nix installed                          |
| Ruby (`packaging/macos/gleditor.rb`, a Homebrew formula) | `brew style` (needs Homebrew; not run in CI)         | `ruby -c` (syntax only)                                                                                                                  | non-blocking, see `packaging.yml`'s `macos` job                                                      |
| RPM spec (`packaging/fedora/gleditor.spec`)              | —                                                     | rpmlint (needs `rpm`'s native Python binding, so only runs inside the `fedora:latest` container the `fedora` packaging job already uses) | non-blocking until its output has been triaged                                                       |
| Debian (`debian/rules`, `debian/control`, ...)           | —                                                     | —                                                                                                                                         | not covered: `debian/rules` is a `dh`-sequenced Makefile, not a shell script shellcheck understands  |

yamlfmt and mdformat are Go/Python tools rather than apt packages
(`go install`/`pip install` in the CI `format` job); they were picked over
the better-known Node ones (`prettier`, `markdownlint-cli2 --fix`) to avoid
adding a Node toolchain to an otherwise zero-Node tree, the same reasoning
`.yamllint`/`.mdl_style.rb` already used for the linters.

## Why formatter and linter settings live in one place per language

- `.yamlfmt`'s `indentless_arrays: true` matches `.yamllint`'s
  `indent-sequences: false`; both encode the same "`- uses:` sits level with
  `steps:`" convention. `retain_line_breaks: true` is load-bearing but has a
  known bug with folded (`>`) block scalars that leaks a placeholder into the
  string (google/yamlfmt#84) — this repo has none, on purpose; see the
  comment in `.yamlfmt` before adding one back.
- mdformat's `--wrap keep` never reflows a paragraph, so it can't fight
  `.mdl_style.rb`'s `MD013` line-length allowance; the only content it
  rewrites is table cell padding, which mdl doesn't check at all. The one
  failure mode is an inline code span (`` `...` ``) split across a
  hand-wrapped line: mdformat canonicalizes that onto one line regardless of
  `--wrap`, which can push it over the line-length limit — keep code spans on
  one physical line rather than wrapping through them.
- nixfmt-rfc-style and nix-linter check disjoint things (whitespace/layout
  vs. semantic style like unused arguments), so there is nothing for them to
  disagree about.

## `.editorconfig` and `editorconfig-checker`

`.editorconfig` sets each extension's indent style/size, charset, line ending
and trailing-whitespace handling for editors that read it, matching the
settings the formatters above enforce (`.clang-format`'s `IndentWidth: 2`,
shfmt's `-i 2`, `.yamlfmt`'s `indent: 2`, ...). It's a hint for editors, not a
gate — nothing runs `editorconfig-checker` in CI.

A repo-wide `editorconfig-checker` run is noisy on this tree for reasons that
are the checker's limits, not real problems: it doesn't understand
clang-format's alignment-based continuation indents, treats Markdown table
cell padding as "wrong" indentation, and applies `max_line_length` to
LICENSE/Doxyfile text nobody intends to rewrap. Use it to sanity-check
`.editorconfig` itself, not as a gate.
