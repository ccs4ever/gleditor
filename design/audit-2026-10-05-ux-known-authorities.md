# UX follow-up: known local formatting authorities

This continues the [previous audit](audit-2026-10-04-ux-resume-navigation-followup.md), addressing
J17 quotations whose formatting authority is saved outside the default xanadoc directory.

| Journey | Step                                                                        | Affordance used                                                               | Outcome                                                   | Evidence                                                                                        |
| ------- | --------------------------------------------------------------------------- | ----------------------------------------------------------------------------- | --------------------------------------------------------- | ----------------------------------------------------------------------------------------------- |
| J17     | Author source and quote                                                     | Type; select alpha; Ctrl+Alt+B, Ctrl+Alt+I; Ctrl+D; Ctrl+N; pouch card insert | Pass; quote shares the formatted alpha                    | `/tmp/ux-known-authority-ui/prepare.png`, `quote-created.png`, argument files and logs          |
| J17     | Remember an external source and close it                                    | Launch existing store path; Ctrl+W; Ctrl+Q                                    | Pass; closed checkpoint records the source path           | `/tmp/ux-known-authority-ui/remember-source.log`                                                |
| J17     | Reopen quotation alone, before fix                                          | Launch destination; Ctrl+Home                                                 | Fail; alpha is plain despite the source checkpoint        | `/tmp/ux-known-authority-ui/before-fix.png`                                                     |
| J17     | Reopen quotation alone, after fix                                           | Launch destination; Ctrl+Home                                                 | Pass; alpha inherits bold and italic without a source tab | `/tmp/ux-known-authority-ui/after-fix.png`, `after-fix.log`, `comparison.png`                   |
| J17     | Read without saving the authority                                           | Open destination; capture; Ctrl+Q                                             | Pass; source operations and tables stay byte-identical    | `/tmp/ux-known-authority-ui/source-hashes.json`, `/tmp/ux-known-authority-replay.py`            |
| J17     | Remove bold and resume                                                      | Select alpha; Ctrl+Alt+B; Ctrl+Q; reopen quotation                            | Pass; italic remains and bold stays removed               | `/tmp/ux-known-authority-ui/toggle-off.png`, `restarted.png`, `source.ops.txt`, `quote.ops.txt` |
| J17     | Discover never-opened stores outside the default directory or remote stores | No discovery affordance added                                                 | Remaining gap                                             | Explicit paths must already be in reader history for local discovery                            |

The previous temporary audit fixtures had expired. This run creates new work through the UI, using
independent XDG data and configuration directories. To represent an existing store saved elsewhere,
the UI-authored source directory is relocated outside the default directory and the initial activity
store is removed before the source is explicitly opened and closed. This setup does not fabricate
document operations. Captures were inspected, including the enlarged plain/bold-italic/italic
comparison. Accessibility dumps verify the destination is the only open document; native assistive
technology is not exercised.

Discovery reads all session and closed-document checkpoints, including their slice paths, and
deduplicates the recorded paths. An empty latest session does not erase older known documents. The
history lookup appends no operations and is cached until the activity operation count changes.
Unopened authorities remain separate from the session's saving lifecycle; a decoration edit loads
matching authorities for persistence. Both operations and deployment-table modification times
invalidate disk snapshots on the next text-source resolution. The default document directory still
supplies discovery for work not yet represented in history; arbitrary neighboring folders are not
scanned. The retained source hashes show that removing bold changes `ops.nodes` while `store.tables`
remains identical, so snapshot refresh must observe operations as well as tables.

The permanent binary regression uses engine-authored fixtures separately from the UI evidence. It
covers default-directory inheritance, an unknown external source, discovery after Open/Close,
unchanged authority tables while reading, a remembered source becoming unavailable, and removing
bold through the quote across restart. The engine test verifies deduplicated paths from older
sessions, closed checkpoints and slice-only views after saving, with no operations appended by
lookup.

Validation passes: full `make -j$(nproc)` build, repository formatting and lint gates, focused
`clang-analyzer-*` checks on Session and reading-place code, and 23 focused formatting and activity
tests. The external-authority binary regression also passes on GLES and software Vulkan
(SwiftShader). Logs:
`/tmp/ux-known-authority-{build-final,format,lint,analysis,tests,gles,vulkan}.log`. The binary
regression uses independent XDG directories and explicit permascroll binding. Its captures in
`build/integration_workspace_unopened_formatting/` are overwritten by subsequent backend runs; the
separately retained UI captures above use OpenGL.

Remaining work: discovery of unseen external and remote authorities, visit annotation/reference and
Walks controls, fully coincident link selection, and native accessibility delivery. This follow-up
does not establish formatting inheritance in unopened ZigZag cell views.
