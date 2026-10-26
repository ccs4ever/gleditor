# README and linked-doc writer review, round 2

## Dialogue and decisions

The first pass left two onboarding questions whose UI steps had not been exercised. I checked the
plain editor source: an existing empty file is accepted, a nonexistent path returns `NotFound`,
`Ctrl+S` writes the active document to its opened path, and there is no Save As path for the unnamed
document created by `Ctrl+N`. `docs/rendering.md` now gives a disposable `/tmp` file recipe, asks
the reader to press `Ctrl+S`, and explains the unnamed-document limitation. README links directly to
that section.

For Xuzz, source review showed that `Ctrl+N` creates a separate untitled store under the XDG
xanadocs directory, not a new document inside an explicitly named store. The documented scratch
workflow therefore imports one prepared text file into an explicitly named store under `/tmp` with
`--headless`, then opens that same path for editing. The reader ran the headless seed and reopen
steps in a fresh `/tmp` directory with isolated XDG roots. `store-dump` showed the expected 17-byte
insert span. The reader confirmed that the normal-exit save and same-path reopen instructions are
clear.

The backup section says to close Xuzz and copy the full store directory together with the full
permascroll directory it uses. The example identifies `$XDG_DATA_HOME/xuzz/permascroll/default`; a
custom `--permascroll PATH` requires backing up that directory instead. It distinguishes this local
backup pair from publishing for another reader. Its guarantee is explicitly scoped to the shown
local-only scratch document; it does not claim to capture other registered scrolls. Both recipes use
a unique directory from `mktemp -d`. README links directly to the section.

The reader has **not** run the manual GUI steps: typing, `Ctrl+S`, normal close, and GUI reopen for
the plain editor or Xuzz remain source-checked, paper-only instructions. The Xuzz batch import and
headless reopen were actually run. This evidence boundary is explicit here; no GUI interaction was
claimed.

The Doxygen audit also found that `make doc` skips successfully if Doxygen is missing and that the
configured input roots omit public headers and tools. `docs/testing.md` now states the prerequisite,
HTML output path, and skip behavior, and links the audit. README's `make doc` claim points to that
note rather than implying that a zero exit always generated docs.

## Files changed

- `README.md` — direct links to first-file, scratch-store/backup, and API documentation guidance.
- `docs/rendering.md` — source-checked plain-file creation and save workflow.
- `docs/xanadu.md` — isolated named-store seed/edit/reopen example and paired backup/publication
  guidance.
- `docs/testing.md` — Doxygen prerequisite, output, skip behavior, and coverage-audit link.

## Validation

- Source review of `apps/gleditor/main.cpp`, `apps/gleditor/editor_config.cpp`,
  `src/text_source.cpp`, and `src/renderer.cpp` checked the plain-editor path and save semantics.
- Source review of Xuzz CLI, `BatchOrchestrator`, `Session`, `Store::load`/`save`, and
  `UserPermascroll` checked the explicit store, headless import, normal-exit save, XDG data paths,
  and permascroll backup pair.
- The reader ran the Xuzz seed command with fresh isolated XDG roots, inspected the generated store
  and permascroll, and reopened the same store headlessly. No GUI was opened.
- The parent independently copied the local store/permascroll pair, moved the originals aside,
  restored both at the documented paths, and reopened the store headlessly with a fresh cache. The
  operation and rendered-text dumps matched before and after restore.
- Makefile and Doxyfile review confirmed Doxygen's optional target, skip message, `docs/html/`
  output, and `src apps` inputs. The linked coverage audit records that Doxygen was absent and no
  HTML generation was validated.
- Markdown formatting, lint, formatter check, and `git diff --check` passed for the assigned docs
  and writer records.
