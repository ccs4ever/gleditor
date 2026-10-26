# New user review, round 2

This pass was narrowly scoped to the plain editor's first saved file and Xuzz local store
backup/restore. I judged the written workflows as a programmer new to the repository. I did not open
a UI or make production edits.

## Plain editor first file

README now links directly to [First saved file](../../../docs/rendering.md#first-saved-file). The
steps first create an empty file, open its exact path, type, press Ctrl+S, exit, and rerun the same
path. The guide explains that an empty existing file is accepted, a missing path is not, and Ctrl+N
creates an untitled document without a Save As command yet. This answers how to start with a
disposable file and how to save/reopen it.

The steps are source-backed: `FileTextSource` opens an existing path, the editor binds Ctrl+S to
`save`, and the save item writes to the active document. They are not GUI-executed in this review
because opening a real window was out of scope. The report makes this limitation explicit rather
than claiming an interactive save test.

## Xuzz disposable store and backup

README links directly to
[scratch-store recipe](../../../docs/xanadu.md#create-and-back-up-a-disposable-store). It sets
`XDG_DATA_HOME`, `XDG_CONFIG_HOME`, and `XDG_CACHE_HOME` under `/tmp`, creates a sample text file,
imports it headlessly into a named scratch store, and then opens that same store path for editing.
It says to exit normally to save and to use the same store path and XDG settings to reopen. The
backup instructions require closing Xuzz, copying the full store directory and full
`$scratch/data/xuzz/permascroll/default` directory together, and restoring both at those paths. It
explains that `--permascroll PATH` changes the permascroll directory to back up and that a
store-only copy lacks primedia. It routes sharing to publication.

I ran the documented headless import command with a fresh unique `/tmp` directory and all three XDG
roots isolated. It completed without output or a window. The resulting files included
`store/ops.nodes`, `store/store.tables`, and `data/xuzz/permascroll/default/active.primedia`.
`./build/store-dump --section=ops` showed an Insert operation spanning bytes `[0,17)`. Reopening
that same store with the same XDG roots and `--headless` exited successfully. This verifies the seed
and reopen paths; it does not exercise typing in the UI or normal window-close behavior.

## Recheck result

The README pointers are direct, and the linked recipes now answer both novice workflows. The backup
pair and restore paths are concrete enough to follow. Remaining limitation: interactive editor and
Xuzz typing/save/reopen were paper-checked against source and documentation only; no GUI was
launched, as instructed. No actionable documentation gap remains within this pass's scope.

## Final wording delta

A final paper recheck found the temporary examples now use `mktemp -d` unique directories. Both
recipes keep their generated path in a shell variable, so the commands to reopen or copy the backup
are usable in that same shell. The Xuzz backup claim is explicitly scoped to the shown local-only
scratch document, and calls out that other registered scrolls require additional backup. Restoring
both the entire store and its matching full default permascroll at the recorded paths is concrete;
this does not imply the same pair is sufficient for more complex multi-scroll stores.

README directly links to both first-use sections. The API-doc note now states that `make doc` needs
Doxygen, writes `docs/html/`, can report a successful skip when Doxygen is absent, and currently
scans `src/` and `apps/` while omitting `include/` and tools. It labels that as the current scope
and links the coverage audit, so it does not imply complete public API coverage. `git diff --check`
over the reviewed docs and report was clean.

No actionable documentation gap remains within this limited first-use scope. The normal GUI
edit/save/reopen steps are still paper-checked only; no UI was launched and no additional runtime
check was needed for this final wording delta.
