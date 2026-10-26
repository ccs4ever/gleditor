# README first-use dialogue review

The user requested a new-user critique in dialogue with a technical writer. Two Luna subagents
worked directly together: the reader identified obstacles and tried available commands; the writer
translated those observations into edits, replied with explanations, and requested rechecks. A
second pass addressed the initially unresolved first-file and store-backup questions.

- [Current README](../../../README.md)
- [Reader's first pass and responses](user-r1.md)
- [Writer's first-pass changes](writer-r1.md)
- [Reader's second pass and final recheck](user-r2.md)
- [Writer's second-pass changes and evidence](writer-r2.md)
- [Separate Doxygen coverage audit](../doxygen-coverage-audit.md)

The `baseline-*.txt` files preserve the inputs before this dialogue. The `final-*.txt` files
preserve the resulting README and three reference pages. This is a subagent review, not a study with
external participants.

## Questions translated into improvements

| New-user difficulty                                | Concrete change                                                                                                                                                            |
| -------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Hidden batch/headless options                      | README names `--help-all`, `--headless` and `--batch`.                                                                                                                     |
| Unclear first Xuzz store and save behavior         | README explains the default store and normal-exit persistence; linked instructions use an isolated, named scratch store.                                                   |
| Unclear raster input                               | README distinguishes store projection from `--lens PATH` and gives a working filesystem example.                                                                           |
| Dependency list is hard to act on                  | Linked directly to the tested Ubuntu apt package list; other distributions use equivalent packages.                                                                        |
| Dump tool is not on PATH                           | Linked references use `./build/store-dump`.                                                                                                                                |
| No first saved plain file                          | Added an existing-empty-file recipe, Ctrl+S and same-path reopen instructions, including the unnamed-document Save As limitation. README says the initial file must exist. |
| Store copy mistaken for complete backup            | Added paired store/permascroll backup guidance explicitly scoped to the local-only scratch example, and linked publication for sharing.                                    |
| `make doc` success mistaken for generated API docs | Added Doxygen prerequisites, the current successful-skip behavior and the source-scope audit link. Configuration was not changed.                                          |

## Verification and limits

- Reader: ran help, detailed help and a filesystem raster projection with offscreen video, dummy
  audio, software GL and isolated XDG state. The projection succeeded without opening a window.
- Reader: created the documented scratch store by headless import and reopened it headlessly. The
  operations and matching permascroll existed; the dump contained the expected 17-byte insert.
- Parent: copied that kind of local store and permascroll, moved the original directories aside,
  restored both, reopened with a fresh cache, and compared semantic dumps including text. They were
  identical. No GUI was used.
- Writer: checked file loading, Ctrl+S and normal-exit persistence against source. Interactive
  typing, window-close save and GUI reopen remain untested in this task.
- Reader: rechecked the final unique temporary paths, local-only backup scope and API-docs note; no
  actionable documentation gap remained within the first-use scope.
- Markdown formatting/lint, local paths/anchors and Git diff checks pass for the edited documents.
  Only documentation changed in this review.
