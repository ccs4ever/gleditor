# Round 6 critique: Native Agent Developer / User (vagent-native-agent.md v0.6)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 2 MINOR.

## Status of previous BLOCKING/MAJOR findings

- R5 B1 (`vagent.native.enabled` blocks `vagent run`): RESOLVED. §5.1, §14.2 (P20.8), and §14.3
  explicitly specify that invoking `vagent run` with a `.vnprog` artifact sets
  `options.nativeSettings.enabled = true` for that invocation, while multi-tenant daemon nodes
  retain the default `false`. `vagent run` also accepts `--module`, `--grant-policy`, and `--ledger`
  overrides.
- R5 B2 (Missing module path prevents control plane locating `module.so`): RESOLVED. §7.1 adds
  `module_filename` (the module shared library's basename at seal time) to the `.vnprog` bencoded
  dictionary. §5, §5.1, §7.3, and §14.2 (P20.8) specify deterministic module location: via an
  explicit `--module <path>` override, or by resolving `<vnprog-dir>/<module_filename>`, with a
  typed `ModuleNotFound` error if the file cannot be opened.
- R5 M1 (Missing C ABI and export symbol for module-native skills): RESOLVED. §6.1 specifies
  `VN_API_VERSION`, `vn_effect_req`, `vn_complete_fn`, `vn_skill_entry`, and the optional export
  symbol `const vn_skill_entry *vagent_native_skills(void)`. §11.1 and §6.4 document how custom
  module-native skills bind through this C table or the C++ `SkillRegistrar` helper.
- R5 M2 (Unspecified builder methods for binding kinds and sequences): RESOLVED. §6.2 provides the
  complete table of `VN_KIND_NATIVE_*` and `VN_SLOT_NATIVE_*` constants for C callers. §6.4
  specifies the C++ builder API: `b.authority`, `b.library`, `lib.handle`, `lib.fn`, and
  `lib.sequence`. In §6.4 and §11.4, unlabelled sequence steps are named `seq.step(call)` to cleanly
  avoid colliding with the C++ `do` keyword.
- R5 M3 (Missing entry-point boilerplate and builder wrapper initialization): RESOLVED. §5.1 and
  §6.4 specify the exact canonical C++ module skeleton in namespace `vagent::native`, wrapping
  `(api, raw)` in `Builder b(api, raw)` and returning `b.finish()` (0 on success, -1 on failure).
- R5 M4 (Unspecified CLI syntax and options for `vagent native` commands): RESOLVED. §14.2 (P20.8)
  specifies the complete CLI grammar, arguments, defaults, and behaviors for `vagent native seal`,
  `vagent native approve`, `vagent native withdraw`, and `vagent run <file.vnprog>`.
- R5 M5 (Lack of an end-to-end developer walkthrough from source to execution): RESOLVED. §5.1
  provides a comprehensive 5-step walkthrough ("Building and running a native agent: five steps from
  source to deployment") covering authoring `greeter.cpp`, compilation, sealing, approval, and
  running via `vagent run`.
- R5 Minor 1 (Compilation recipe and compiler flags): RESOLVED. §5.1 step 2 documents the exact
  compiler flags (`c++ -std=c++23 -shared -fPIC -fvisibility=hidden -O2` with `pkg-config --cflags`
  and `-o greeter.so`).
- R5 Minor 2 (Keyword collision with `do` in sequence steps): RESOLVED. §6.4 and §11.4 document
  `seq.step(call)` as the C++ step method.

## BLOCKING

None.

## MAJOR

None.

## MINOR

- §5.1 step 2 specifies `$(pkg-config --cflags gleditor)` for compilation, whereas §14.1 / §14.2
  P20.5 note `vagent-native.pc` as the dedicated package for native agent headers. Mentioning that
  `$(pkg-config --cflags vagent-native)` is also provided by the package would clarify standalone
  SDK builds.
- In §5.1 step 1 (`greeter.cpp`), `seq.len("text")` computes the length of parameter `text`.
  Explicitly clarifying in §5.1 that `seq.len(...)` takes either a string literal name of a
  parameter or a `let` binding aligns with §11.4 rule 1.

VERDICT: CONVERGED
