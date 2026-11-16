# M1 target-mode wiring review

**Disposition:** The current `perf-release` recipe is corrected for the reported Clang ThinLTO+PGO
stage mismatch. This verifies target wiring only; it is not a full release or runtime pass.

## Verification

- The Makefile now invokes `tools/release-profile.sh release --mode thinlto-pgo`, matching the
  generate, train, and merge recipes. The Python parser defaults to `pgo`, so this explicit argument
  is necessary.
- `release_profile.py` validates the stage state and merged manifest against the selected mode. For
  `thinlto-pgo`, Clang selects `thinlto-pgo-use`; GCC selects `lto-pgo-use`. It then calls Make with
  that mode and the validated profile path. The recipe therefore selects the intended profile-use
  configuration instead of silently taking PGO-only defaults.
- `docs/testing.md` documents the serial stage sequence and says `make perf-release` is the
  supported provenance-checking entry point. The corrected Makefile now agrees with that contract.

## Adjacent mode note

The documented Make recipes are hardwired to the `thinlto-pgo` driver mode. That is appropriate for
the documented default Clang path. The driver contains a GCC translation to full-LTO+PGO
(`lto-pgo-*`), and the docs call GCC a portability alternative, but the current documentation does
not show an alternate invocation for selecting that path. This is a discoverability gap, not a
defect in the corrected Clang target. If GCC support is meant to be directly usable through Make,
expose or document a mode-selectable target/driver invocation; do not claim the standard hardwired
sequence itself exercises GCC's alternative.

No builds, profile stages, tests, or release validation were run for this review. No full release
pass is claimed.
