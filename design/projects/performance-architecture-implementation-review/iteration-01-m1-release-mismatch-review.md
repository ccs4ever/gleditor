# M1 profile-use failure diagnosis and correction review

## Diagnosis

The merged Clang instrumentation profile had no matching record for `apps/xuzz/main.cpp`'s `main`
function. The corpus executed `xuzz_test`, editor and language binaries, but not `build/xuzz`;
different executables' entry points share the symbol name `main` and may have different CFG hashes.
A build-use profile must retain each relevant main hash, and Clang correctly rejects a source whose
hash is absent when `-Werror` is active.

The xuzz-specific `cli.cpp`, `view_coordinator.cpp`, and `xuzz_app.cpp` TUs also had no data because
the Xuzz application was never run. Globally suppressing `profile-instr-out-of-date` would hide
genuine source/profile incompatibility, so that is not done.

## Correction

- Added one headless Xuzz startup workload using the report runner's fresh isolated XDG roots.
- Added entrypoint-only train smoke workloads for the other distinct `main` functions among Make's
  `all` outputs, including VQL/VPL/Vlace/Vicon/Vagent CLIs, `store-dump`, `shader_assemble`, and
  `gleditor_test`. Their manifest descriptions distinguish entrypoint profile coverage from
  representative performance evidence. Existing VPL, VProlog, editor, and test workloads already
  exercise their entry points.
- Expanded the final artifact digest inventory to include every executable and compatibility symlink
  produced by `all`, plus the two performance probes built by the release driver.
- In Clang PGO-use modes only, made `-Wprofile-instr-unprofiled` a visible warning instead of an
  error for wholly cold translation units. `-Wprofile-instr-out-of-date` remains fatal. The release
  artifact report records that policy and labels entrypoint-only workload scope.
- Added a compiler regression proving two different trained main CFG hashes work in strict profile
  use, that omitting a needed main hash still fails, and that a wholly unprofiled cold TU remains a
  visible warning under the narrowly relaxed policy.

## Evidence

- `/tmp/perf-m1-entrypoint-smoke.log`: all newly added entrypoint commands succeeded against the
  existing binaries with offscreen/dummy devices and temporary XDG state.
- `/tmp/perf-m1-focused-tests.log`: 32 profile-provenance and performance-report tests passed.

No project build, fresh generation, training, merge, release build, or full-release gate was run.
The fresh profile pipeline is still required because the manifest/source changed.
