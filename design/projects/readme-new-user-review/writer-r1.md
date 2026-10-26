# README and linked-doc writer review, round 1

## Dialogue and decisions

The independent reader found the first-run CLI harder to discover than the short README implied: the
everyday Xuzz help hides `--headless`, `--raster` needs an input, and the README did not explain
what store a first launch opens. We checked the parser help and `XuzzApp::run()` together, then
changed the Quick Start text to point readers to `--help-all`, explain `--headless`/`--batch`,
distinguish rasterizing a store from the `--lens PATH` filesystem projection, and show
`./build/xuzz --lens docs --raster`.

The reader ran that projection with offscreen SDL, dummy audio, software GL, and isolated XDG
directories. It exited successfully and printed the docs directory and its files. The reader
requested the term “store” and an explanation of persistence. We confirmed `session->saveAll()` runs
after the app loop exits and that the parser's no-argument path is the default xanadoc store. The
README now says edits are saved to the default store on normal exit and reopened from that path. It
calls the path the user's data directory, since the implementation falls back from `$XDG_DATA_HOME`
to `$HOME/.local/share` when XDG is unset.

The reader also noted that bare `store-dump` examples require `build/` on `PATH`; both references
now use `./build/store-dump`. For Linux dependencies, we linked the README directly to the CI build
job's tested Ubuntu apt package list. The reader rechecked these changes and confirmed both gaps
were resolved.

## Files changed

- `README.md` — first-run store behavior, detailed help, batch mode and raster examples; direct link
  to tested Ubuntu package list.
- `docs/xanadu.md`, `docs/testing.md` — executable paths for `store-dump` examples.

## Validation

- Source review of `apps/xuzz/cli.cpp`, `apps/xuzz/xuzz_app.cpp`, and
  `apps/common/ui/xanadoc/session.cpp` confirmed the documented CLI options, raster branches, XDG
  fallback, and normal-exit save path.
- The independent reader executed `./build/xuzz --lens docs --raster` in headless mode with isolated
  XDG directories; it succeeded.
- `mdformat --wrap 100` with the `gfm`, `dollarmath`, and `frontmatter` extensions and `mdl -i` ran
  on the README and four linked reference docs. mdformat required repeated `--extensions` flags;
  passing the comma-separated list was rejected by the installed CLI.
- The reader's `git diff --check` over the edited docs was clean. No UI was opened and no UI
  save/reopen interaction was tested.
