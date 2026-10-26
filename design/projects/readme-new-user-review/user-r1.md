# New user review, round 1

I know general programming but am new to this repository. This pass used the current README and the
linked `docs/rendering.md`, `docs/xanadu.md`, and `docs/testing.md`. I did not read prior review
reports or architecture/design notes.

## Actions and observed results

- Read `README.md` and followed its links to the three user-facing references above.
- Confirmed binaries already present: `build/gleditor`, `build/xuzz`, `build/vquery`, `build/vpl`,
  `build/vprolog`, and `build/vagent`. The dump binaries present are both `build/store-dump` and
  `build/xudu-dump`.
- Ran `./build/xuzz --help` with SDL offscreen/dummy audio/software GL and XDG data, config, and
  cache redirected into this worktree's `build/xdg/`. This exited with usage output. It describes
  the positional store as a directory “created if it is not there” and gives a default below
  `$XDG_DATA_HOME/xuzz/xanadocs/default`. The short help omitted `--headless` and the batch options.
- Ran `./build/gleditor --help` in the same isolated environment. It printed usage; it does not
  explain an initial file workflow.
- Ran `./build/vquery --help` in the same isolated environment. It printed usage; unlike xuzz's
  short help, it includes `--headless` and describes query inputs, result stores, and output
  formats.
- Ran `./build/xuzz --help-all` in the isolated environment and searched its output. This confirmed
  `--headless` is an alias for `--batch`, says it exits without GUI, and that `--raster` prints a
  slice projection. It also says `--lens PATH` projects a filesystem path and is used with
  `--raster`.
- Read testing guidance. It clearly shows how Make isolates XDG state, and how to set the same
  headless variables for direct test-binary runs. The README says `make test` runs headlessly with
  isolated XDG state.
- Did not launch either GUI, mutate a store, create a document, run the build, or run tests. No
  first-document or store-persistence workflow was executed.

## Concrete points that slowed me down

1. README says “Each program's `--help` shows everyday options and `--help-all` shows batch and
   diagnostic options. `xuzz --headless` runs batch commands...” But the normal `xuzz --help` omits
   `--headless`. A new user has to infer the `--help-all` flag before discovering the advertised
   no-window mode. State that xuzz batch options are under `--help-all`, or give a small concrete
   example.
1. `./build/xuzz [store]` is a risky-looking first command because a missing positional store is
   created automatically. The README gives the default location but does not offer a disposable
   first-use location or say how to distinguish an existing store from a new one. A starter workflow
   should explicitly use a scratch store and isolated XDG paths if intended for exploration.
1. The README says `--raster` prints a text projection but doesn't state what input it projects. The
   full help shows `--raster` for a slice and `--lens PATH` for a filesystem path. A command example
   would resolve this ambiguity.
1. Setup names a long list of development libraries and points to CI for a working Linux dependency
   set, but doesn't say how to install dependencies on a common platform. This may be enough for an
   experienced contributor, but it is a missing step for a first-time build unless the CI list is
   easy to map to a local system.
1. The program table calls the store inspection tool `build/store-dump`; `build/xudu-dump` is also
   present in the checkout. The references use the `store-dump` command without `./build/`, so the
   invocation/path is not immediately clear to a new user. Clarify the canonical binary and show the
   in-tree invocation.
1. README does not have a concrete “first file” example for the plain editor.
   `./build/gleditor file.txt` is named as a start command, but that opens a window, and the basic
   help output doesn't explain file creation or where settings/persistence live. The new user path
   currently stalls at the command line unless they already know how to use the editor.
1. The persistence description for Xuzz is conceptually useful (store has no primedia), but gives no
   ordinary-user instruction for keeping a document portable or restoring it. I did not attempt
   export/publish because that requires more setup and could affect user data; docs describe
   publication, but they don't form a simple local backup/restore recipe in the README.

## Limits of this pass

I did not exercise UI editing, save/reopen, importing, query mutation, or publication. The
observations above about those journeys are missing-documentation questions, not claims that the
underlying features fail. The only live command executions were `--help`/`--help-all` with isolated
XDG directories; I did not create or alter a store.

## Recheck of the writer's README revision

The writer revised the startup paragraph to explain the default Xuzz path, positional store
behavior, `--help-all`, `--raster`, and `--lens PATH`. I executed its proposed
`./build/xuzz --lens docs --raster` example with SDL offscreen, dummy audio, software GL, and XDG
data/config/cache redirected into this worktree's `build/xdg/`. It exited without opening a window
and printed `lens: docs -> 5 cells, 1 root(s)`, followed by the `docs` directory and four files. The
example is correct and useful.

The paragraph still calls the target the “default document” even though it identifies a store
directory. I asked the writer to use “default store” and clarify whether typing persists
automatically and how a new user can safely reopen it. I have not tested editing or reopening
because that requires launching the GUI, which this review was instructed not to do.

The final paragraph now calls this a “default store,” states edits save on normal exit and reopen
from the same path, exposes hidden `--headless`/`--batch` discovery, and gives the verified lens
command. This answers the first-use and persistence questions at a documentation level. I did not
launch the GUI to validate an edit/save/reopen cycle, consistent with the no-window constraint.
`git diff --check -- README.md` produced no output.

## Remaining suggestions

The first-use path, persistence statement, option discovery, and filesystem projection now make
sense. Two lower-priority onboarding issues remain: the README names `build/store-dump`, while the
linked references invoke bare `store-dump` (a fresh checkout may not have `build/` on `PATH`); and
dependency names plus a CI link do not give a newcomer a direct platform-specific package
installation route. I sent these to the writer as non-blocking suggestions. This review cannot
validate interactive editing/save/reopen without launching a GUI, which was explicitly out of scope.

The writer then fixed the two remaining navigation points: both linked references now show
`./build/store-dump`, and README links directly to the CI job's tested Ubuntu apt package list. I
rechecked these references and ran `git diff --check` over the changed README/docs; it produced no
output. The previously noted onboarding gaps are resolved. The only test limitation is deliberate: I
did not open a real window, so interactive save/reopen remains a paper check based on the documented
normal-exit persistence statement.
