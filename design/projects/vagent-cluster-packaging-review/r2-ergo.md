# Round 2 critique: Programming Language Design & Ergonomics Expert (vagent-cluster-packaging.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 3 MINOR.

## Status of previous BLOCKING/MAJOR findings

- R1 B1 (§4 K1/K3, §5.2, §7.4, §8.4: Cross-platform soname resolution in multi-target packages):
  RESOLVED. v0.3 adds `name` (the logical library identifier) and `canonical_soname` (the soname
  recorded in `op_log`) to `targets[<triple>].bundled_libs`, and introduces `system_library_aliases`
  in `targets[<triple>]`. At load and Step 7 `dlopen` time, the target slice maps `canonical_soname`
  to the staged `<staging>/lib/<soname>` or host system alias, keeping `op_log` and `programRef`
  completely invariant across platforms.
- R1 M1 (§5.1, §10.1, §12 P21.6: Underspecification of `"source+native"` packages): RESOLVED. §5.1
  and §10.1 specify the CLI workflow
  (`vagent package app.vagent --target <triple>=<path> -o app.vnpkg`), automatic inference of
  `kind = "source+native"`, package-time verification that candidate modules export
  `vagent_native_skills()` covering all declared `native skill` symbols in the source AST, and the
  explicit architectural boundary that `"source+native"` binds skills strictly via
  `vagent_native_skills()` function tables (without libffi sequences).
- R1 M2 (§9.1, §10.3, §12 P21.6: Authority approval CLI accepting `.vnpkg` packages): RESOLVED. §4
  K10, §9.1, §10.3, and P21.6 explicitly extend `vagent native approve` and `vagent native withdraw`
  to accept `<file.vnprog | file.vnpkg | programRef>`, computing the K1 logical `programRef` when
  given a `.vnpkg`.

## BLOCKING

None.

## MAJOR

None.

## MINOR

- In §10.3 lines 908 and 911, the sample approval commands
  `vagent native approve greeter.vnpkg --as deployer` and
  `vagent native withdraw greeter.vnpkg --as deployer` omit the authority kind. Under
  `vagent-native-agent.md` §10.5 and P20.8, ledger approvals map `(programRef, kind)` pairs;
  `--kind <kind>` (e.g. `--kind terminal`) is required by the CLI parser. Add `--kind terminal` to
  the illustrated commands.
- In §5.2 schema for `system_library_aliases`, state explicitly that dictionary keys must be sorted
  lexicographically by raw byte value per BEP-3 canonical bencode rules, matching the rest of the
  manifest dictionaries.
- In §10.1 line 860 (`--bundle-dir aarch64-linux-gnu=build/aarch64/libs`), state explicitly that
  `--bundle-dir` scans the given directory for shared libraries matching the K3 SONAME regex
  allowlist, extracts each library's `DT_SONAME` (or Mach-O install name / PE import name), and
  computes the topological dependency order automatically.

VERDICT: CONVERGED
