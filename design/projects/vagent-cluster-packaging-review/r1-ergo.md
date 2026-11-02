# Round 1 critique: Programming Language Design & Ergonomics Expert (vagent-cluster-packaging.md)

Paths below are relative to the repo root. Tally: 1 BLOCKING, 2 MAJOR, 4 MINOR.

## BLOCKING

### 1. (§4 K1/K3, §5.2, §7.4) Cross-platform soname resolution fails in multi-target packages

**Problem:** Under Ruling K1, a `.vnpkg` cluster package possesses a single canonical `op_log` in
`program_body` that is shared across all target triples in `targets` (`x86_64-linux-gnu`,
`aarch64-apple-darwin`, `x86_64-pc-windows-msvc`). In `vagent-native-agent.md` §6.4 and §11.2,
module authors declare dynamic library dependencies via `b.library(name, soname, ...)` using a
concrete platform library filename (e.g. `b.library("ncurses", "libncursesw.so.6", ...)` or
`b.library("engine", "libengine.so.1", ...)`). That literal string is recorded into the canonical
`op_log`.

In `.vnpkg` §5.2, `targets[<target-triple>].bundled_libs` records the target's platform-specific
library name as `soname` (e.g. `libengine.dylib` on macOS or `engine.dll` on Windows), but does not
record the logical library name (`"engine"`) or the canonical op-log soname (`"libengine.so.1"`).
When `vagent-native-host` executes on macOS or Windows (§7.4 Step 7), the loader attempts to resolve
the binding manifest's declared libraries against staged files in `<staging>/lib/`. Searching for
`"libengine.so.1"` on macOS fails because the file staged on disk is `libengine.dylib`. Similarly,
for system libraries (K3), resolving `"libncursesw.so.6"` on macOS (`libncurses.dylib`) or Windows
(`ncurses.dll` / `pdcurses.dll`) fails. Because the op-log is canonical and immutable across targets
to preserve `programRef` identity, multi-target execution and heterogeneous migration cannot work as
specified without a formal library mapping mechanism.

**Evidence:**

- `design/vagent-cluster-packaging.md:157–176`: K1 specifies a single canonical `op_log` across all
  targets to ensure an invariant logical `programRef`.
- `design/vagent-cluster-packaging.md:192–206`: K3 states "The sandboxed host loader resolves
  declared `soname`s against staged bundled libraries in `<staging_dir>/lib/` before falling back to
  system paths."
- `design/vagent-cluster-packaging.md:314–323, 352–356`: `bundled_libs` records only
  `{soname, sha256, size, info_hash}`, omitting the logical library name (`"engine"`) or canonical
  alias.
- `design/vagent-cluster-packaging.md:495–498`: Step 7 calls `dlopen("<staging>/lib/<soname>")`.
- `design/vagent-native-agent.md:1131–1133`: Acknowledges non-ELF naming differences for two stock
  libraries (`libncursesw.so.6`, `libc.so.6`), but offers no mechanism for user-bundled or arbitrary
  system libraries in multi-target packages.

**Fix:** Add an explicit library mapping mechanism to each target slice in `.vnpkg` §5.2:

1. In `targets[<triple>].bundled_libs`, add `name: "<string>"` (the logical library identifier
   passed to `b.library`) and/or `canonical_soname: "<string>"` (the soname recorded in the op-log).
   During staging (§7.3), the supervisor creates a compatibility symlink
   `<staging>/lib/<canonical_soname> -> <soname>` (or the loader maps `canonical_soname` to `soname`
   in memory before Step 7 `dlopen`).
1. In `targets[<triple>]`, add an optional `system_library_aliases` dictionary mapping canonical
   sonames to platform equivalents (e.g. `{"libncursesw.so.6": "libncurses.dylib"}`).

## MAJOR

### 1. (§5.1, §10.1, §12 P21.6) Underspecification and ergonomic gaps of source+native packages

**Problem:** §5.1 introduces `"source+native"` as a first-class package kind answering Open Question
NQ2 from `vagent-native-agent.md` §17 ("Vagent source programs carrying declared native skills").
However, the developer workflow, CLI syntax, and package-time validation for `"source+native"` are
missing:

1. §10.1 contains no CLI example or explanation for packaging `"source+native"`. A developer with
   `app.vagent` and `build/x86_64/myskills.so` has no documented command to package them.
1. §10.1 does not explain how `kind` is inferred (e.g. whether passing `.vagent` with `--target`
   automatically selects `"source+native"`, or if a `--kind` flag is required).
1. `vagent package` specifies no package-time inspection of the native module for `"source+native"`.
   Unlike native agents where `vagent native seal` verifies the op-log against `checker.cpp`, a
   `"source+native"` package could ship a module that fails to export `vagent_native_skills()` or
   omits skills declared as `native skill` in `app.vagent`, deferring errors to runtime crash.
1. The ergonomics of `"source+native"` are not bounded: surface `.vagent` syntax has no grammar for
   FFI `library` or `sequence` declarations (so `"source+native"` cannot use libffi) and no syntax
   for `authority` declarations (leaving authority purpose strings undefined).

**Evidence:**

- `design/vagent-cluster-packaging.md:287–289`: §5.1 defines `"source+native"` in one sentence.
- `design/vagent-cluster-packaging.md:688–705`: §10.1 shows examples for pure `.vagent` and
  `.vnprog`, but zero examples for `.vagent` with native target binaries.
- `design/vagent-cluster-packaging.md:794`: P21.6 does not mention `"source+native"` handling.
- `design/vagent-native-agent.md:1714`: NQ2 notes `"source+native"` is "§§8–12 without §6".

**Fix:**

1. Document the CLI invocation in §10.1:
   `vagent package app.vagent --target x86_64-linux-gnu=build/x86_64/myskills.so -o app.vnpkg`.
1. Specify that `vagent package` infers `kind = "source+native"` when the input is `.vagent` and
   `--target` is provided (or when the AST contains `native skill` declarations).
1. Require `vagent package` to verify that each target module exports `vagent_native_skills()` and
   implements all `native skill` symbols declared in the source AST before completing the package.
1. State explicitly in §5.1 that `"source+native"` programs interface with native code exclusively
   through `vagent_native_skills()` function tables, not libffi sequences.

### 2. (§9.1, §10, §12 P21.6) Authority approval CLI breaks for .vnpkg cluster packages

**Problem:** In `vagent-native-agent.md` §5.1 (step 4) and P20.8, authority approvals are recorded
via `vagent native approve <file.vnprog | programRef> --kind <kind> [--ledger <dir>]`. Under Ruling
K1, a `.vnpkg` package calculates its logical `programRef` using format tag `vagent-package/1` and
`targets_digest`, producing a different digest from the single-target `.vnprog`.

However, P21.6 only extends `vagent run` to accept `.vnpkg` files. It fails to extend
`vagent native approve` and `vagent native withdraw`. An operator who packages `greeter.vnpkg`
cannot run `vagent native approve greeter.vnpkg --kind terminal`. If they approve `greeter.vnprog`,
the approval is recorded under the single-target `programRef`. When they subsequently run
`vagent run greeter.vnpkg` with `--grant-policy approved`, the broker checks the K1 multi-target
`programRef`, finds no matching approval in the ledger, and denies execution.

**Evidence:**

- `design/vagent-cluster-packaging.md:271–275`: K10 states authority approvals bind directly to the
  logical `programRef`.
- `design/vagent-cluster-packaging.md:655–668`: §9.1 describes how approvals bind to `.vnpkg`
  packages, but gives no CLI command to execute the approval.
- `design/vagent-cluster-packaging.md:794`: P21.6 specifies "Add `vagent package` CLI subcommands
  and extend `vagent run` to accept `.vnpkg` packages", omitting `vagent native approve`.
- `design/vagent-native-agent.md:1588–1601`: P20.8 limits `approve` to `<file.vnprog | programRef>`.

**Fix:** Update §10 and P21.6 to explicitly extend `vagent native approve` and
`vagent native withdraw` to accept `<file.vnpkg>` files:
`vagent native approve <file.vnprog | file.vnpkg | programRef> --kind <kind> [--ledger <dir>]`.
Specify that when given a `.vnpkg`, the command computes `programRef` according to Ruling K1.

## MINOR

- In §10.1 line 693, `vagent package app.vnprog -o app.vnpkg` packages a native agent for the local
  host without passing `--target`. Specify that when `--target` is omitted for a `.vnprog`, the CLI
  defaults to the host's detected normalized target triple and resolves `module_filename` in the
  `.vnprog`'s parent directory or working directory.
- In §10.2 lines 724–726 and 733–735, the sample `vagent package inspect` output includes
  `System Libs:` for each target, but §5.2 does not include `system_libs` in the `targets` schema.
  Clarify in §10.2 whether `System Libs` is dynamically inferred by inspecting dynamic sections
  (`DT_NEEDED` minus `bundled_libs`), extracted from `op_log`, or read from a manifest field.
- In §10.1 lines 698–702, `--bundle-lib <triple>=<path>` must be repeated for every library and
  every target. Consider supporting `--bundle-dir <triple>=<dir>` or allowing `--bundle-lib <path>`
  scoped under a preceding `--target` to reduce CLI flag verbosity for complex dependency graphs.
- In §10.3 line 745, `vagent run greeter.vnpkg --grant-policy ask` illustrates running a package.
  Clarify that `vagent run` on `.vnpkg` accepts all standard P20.8 options (`--ledger`,
  `--grant-policy`) and automatically enables `vagent.cluster.enabled = true` and
  `vagent.native.enabled = true`.

VERDICT: NOT CONVERGED
