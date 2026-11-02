# Round 5 critique: Native Agent Developer / User (vagent-native-agent.md v0.5)

Paths below are relative to the repo root. Tally: 2 BLOCKING, 5 MAJOR, 2 MINOR.

## BLOCKING

### 1. (§14.2 P20.3, §14.3, apps/vagent/main.cpp) vagent.native.enabled blocks vagent run

**Problem:** §14.3 sets the default value of `vagent.native.enabled` to `false`. §14.2 P20.3
explains that "`vagent run` reads no system store today (`apps/vagent/main.cpp:11-14`), so these
settings reach it only when that general gap closes". `apps/vagent/main.cpp:11-15` explicitly
confirms that settings come strictly from built-in defaults without opening the user's system store.
Because neither P20.8 nor `apps/vagent/main.cpp` specifies a CLI flag (such as `--setting` or
automatic enablement when executing a `.vnprog`), `vagent.native.enabled` will permanently evaluate
to `false` in `vagent run`. Any developer attempting to run their native agent with `vagent run` is
deadlocked: `LocalHost` and `NativeHostSupervisor` will refuse to launch the native agent.

**Evidence:**

- §14.3 line 1244: `vagent.native.enabled` defaults to `false` ("whether the node launches native
  agents at all").
- §14.2 P20.3 line 1229: "`vagent run` reads no system store today (`apps/vagent/main.cpp:11-14`),"
  "so these settings reach it only when that general gap closes".
- `apps/vagent/main.cpp:11-15`: "Settings come from `AgentSettings`' built-in defaults, which equal
  the `vagent.*` defaults of system://settings: this command does not open the user's system store,
  so a changed setting does not reach `run` yet."
- §14.2 P20.8 line 1234 specifies running a `.vnprog` in `vagent run` but does not override
  `vagent.native.enabled` or provide CLI setting overrides.

**Fix:** Update P20.8 and §14.2 to specify that when `vagent run` executes a `.vnprog` artifact, it
automatically sets `hostOptions.nativeSettings.enabled = true`, or add an explicit CLI flag
`--setting <name>=<value>` (or `--enable-native`) to `vagent run` to populate
`LocalHost::Options.nativeSettings`.

### 2. (§5, §7.1, §7.3, §14.2 P20.8) Missing module path prevents control plane locating module.so

**Problem:** §7.1 defines the `.vnprog` file format as a bencoded dictionary containing only
`api_version`, `op_log`, and `module_digest`—it contains neither the path nor the filename of the
module shared object (`module.so`). §7.3 specifies: "At load, and again each time a native host
starts, the control plane opens the module file, reads it, and computes its SHA-256 from the bytes
read. If the digest is not the `.vnprog`'s `module_digest`, it refuses with `ModuleDigestMismatch`."
However, neither §5 (Load), §7.1, §7.3, nor §14.2 P20.8 defines how the control plane locates "the
module file" on disk. If a developer runs `vagent run dist/my_agent.vnprog`, the runner has no way
to locate `my_agent.so` to verify the digest and copy it into the staging directory.

**Evidence:**

- §7.1 lines 430-434: "A `.vnprog` file is `\x89VAGNATV`, format version 1, then a bencoded
  dictionary with `api_version`, `op_log` (list of op records, binding kinds included), and
  `module_digest`."
- §7.3 lines 445-447: "At load, and again each time a native host starts, the control plane opens
  the module file, reads it, and computes its SHA-256 from the bytes read."
- §14.2 P20.8 line 1234: `running a .vnprog` in `apps/vagent/main.cpp`.
- `apps/vagent/main.cpp:490-505`: `runRun` takes only a single positional `file` argument.

**Fix:** Specify how the module path is resolved at load and run time. Either:

1. Record `module_name` (e.g. `my_agent.so`) in `.vnprog` alongside `module_digest`, and search for
   it relative to the `.vnprog` file path and along `vagent.native.library_paths`; or
1. Add a `--module <path.so>` option to `vagent run`, defaulting to replacing `.vnprog` with `.so`
   in the file path. Also specify that `Load` returns a typed error (e.g. `ModuleNotFound`) if the
   module file cannot be opened.

## MAJOR

### 1. (§6.1, §8.1, §11.1, §13.4) Missing C ABI and export symbol for module-native skills

**Problem:** §11.1 states that a native skill can bind "to the module's own C function table (a
`NativeSkill`-shaped `start`/`cancel` pair)" rather than libffi. §13.4 uses this mechanism for
`escape_probe.cpp` and protocol violation gates, and §8.1 step 7 states that the host loads the
module and resolves declared symbols. However, §6.1 defines `vagent_native.h` as containing only
`uint32_t vagent_native_api_version(void)` and
`int vagent_native_build(const vn_api *api, vn_builder *b)`. The document never specifies the C
struct, typedefs, signatures, or symbol name for the "C function table". How does a module author
write `escape_probe.cpp` in C/C++? What are the function pointer signatures for `start` and
`cancel`, how are effect arguments received, and how does the module signal completion with a
wire-encoded result across the C ABI?

**Evidence:**

- §6.1 lines 305-346: `vagent_native.h` defines only `vn_builder`, `vn_node`, `vn_status`, `vn_api`,
  `vagent_native_api_version`, and `vagent_native_build`.
- §11.1 lines 786-790: "bound either to the generic libffi runner ... or to the module's own C
  function table (a `NativeSkill`-shaped `start`/`cancel` pair)".
- §8.1 lines 489-492: "Resolve every declared symbol."
- §13.4 lines 1136-1137: "`escape_probe.cpp` is a module-native skill. Each method attempts one
  forbidden act and returns `errno`."
- `apps/common/xanadu/vagent/runtime/host_bindings.hpp:14-22`: `NativeSkill` defines `start` with
  `EffectRequest`, `WireRecord`, and `EffectCompletion`.

**Fix:** In §6.1 or §11.1, define the C ABI for module-native skills in `vagent_native.h`:
`vn_skill_table` with `start` and `cancel` function pointers taking a C-compatible request token,
method name, wire buffer pointer/length, and a completion callback, plus the export symbol name that
`loader.cpp` looks up via `dlsym` (e.g. `const vn_skill_table *vagent_native_skills(void)`).

### 2. (§6.4, §11.2, §11.4, §13.1) Unspecified builder methods for binding kinds and sequences

**Problem:** §11.2 and §11.4 specify binding declarations (`library`, `handle`, `fn`) and `sequence`
using an informal textual "reading form" and grammar, explicitly noting (§11.2 line 808) that on
disk they are bencode nodes. §13.1 shows only 7 lines of plan statements (`b.call`, `b.lit`,
`b.name`, `b.record`, `b.emit`). §6.4 promises that `vagent_native.hpp` "offers typed helpers for
the binding kinds: `library`, `handle`, `fn`, `sequence` and `authority`". However, the actual C++
method signatures are never specified. Crucially, the sequence grammar in §11.4 defines steps as
`step ::= 'let' name ':=' call | 'do' call`. In C++, `do` is a reserved language keyword
(`do { ... } while (...);`), so `seq.do(...)` is invalid C++ syntax. Furthermore, for C developers
using `vagent_native.h`, the node kinds (`VN_KIND_*`), slots (`VN_SLOT_*`), and registration calls
for binding kinds are omitted.

**Evidence:**

- §6.4 lines 415-416: "It also offers typed helpers for the binding kinds: `library`, `handle`,
  `fn`, `sequence` and `authority`."
- §11.2 lines 794-806: Reading form for library, handle, and fn declarations.
- §11.4 lines 855-879: Reading form and grammar for sequences (`do call`, `let name := call`).
- §13.1 lines 1084-1091: Illustrates only plan statements on `ag.plan(...)`.

**Fix:**

1. Specify the C++ wrapper API in §6.4 / §11.2 / §11.4: e.g., `b.authority(name, purpose)`,
   `b.library(name, soname, affinity, capacity, skill)`, `lib.handle(name, c_type, destroy)`,
   `lib.fn(name, params, ret, fail_if, none_if)`, and `lib.sequence(name, params, ret, within)`.
1. For sequence steps, explicitly define the C++ method name that avoids the `do` keyword collision
   (e.g., `seq.step(call)` or `seq.exec(call)` alongside `seq.let(name, call)`, `seq.result(arg)`,
   `seq.finally_call(call)`).
1. Specify the corresponding `VN_KIND_*` and `VN_SLOT_*` names in §6.2.

### 3. (§6.1, §6.4, §13.1) Missing entry-point boilerplate and builder wrapper initialization

**Problem:** §6.1 specifies the C export signatures `uint32_t vagent_native_api_version(void)` and
`int vagent_native_build(const vn_api *api, vn_builder *b)`. §6.4 introduces `vagent_native.hpp` as
a supported header-only wrapper providing "value-style builders on `b`". However, a developer
starting from a blank file does not know how `vagent_native.hpp` wraps the raw C pointer:

- What namespace contains the C++ wrapper types (`vagent::native`?)
- What is the wrapper class name (`Builder`?) and how is it constructed from `(api, b)`?
- Does `vagent_native_api_version()` return a predefined constant (e.g. `VN_API_VERSION`)?
- How are errors reported out of `vagent_native_build`? Does the wrapper check `b.error()`, and what
  integer exit code does `vagent_native_build` return on failure (e.g. `0` on success, `-1` on
  failure)?

**Evidence:**

- §6.1 lines 344-346: C exports.
- §6.4 lines 407-417: Description of `vagent_native.hpp`.
- §13.1 lines 1081-1092: Shows snippet using `b` and `ag` without showing function signature,
  wrapper instantiation, or return code.

**Fix:** In §6.4 or §13.1, provide the exact canonical C++ module skeleton:

```cpp
#include <vagent/vagent_native.hpp>

extern "C" uint32_t vagent_native_api_version() {
  return VN_API_VERSION;
}

extern "C" int vagent_native_build(const vn_api *api, vn_builder *raw) {
  vagent::native::Builder b(api, raw);
  // ... define authorities, libraries, agents ...
  return b.ok() ? 0 : -1;
}
```

### 4. (§10.2, §10.5, §14.2 P20.8) Unspecified CLI syntax and options for vagent native commands

**Problem:** Proposal P20.8 lists four distinct CLI additions in `apps/vagent/main.cpp`:
`vagent native seal`, `vagent native approve`, `vagent native withdraw`, and running a `.vnprog` in
`vagent run`. However, the document gives no syntax or flag specification for any of them:

- `vagent native seal`: Does it take `-o <out.vnprog>`? What is the default output path if omitted?
- `vagent native approve` and `withdraw`: Do they accept a path to `.vnprog` or a hex `programRef`?
  What are the flags for specifying authority kind (e.g. `--kind terminal`)? How is the authority
  ledger directory specified if not the default (e.g. `--store <path>`)?
- `vagent run`: Does it accept `--module <path.so>`? Does it support
  `--grant-policy <deny|ask|approved>`? How does the interactive `ask` prompt prompt the user?

**Evidence:**

- §10.2 lines 670-680: Policy table (`deny`, `ask`, `approved`).
- §10.5 lines 757-758: "`vagent native approve` and `vagent native withdraw` append events (P20.8)".
- §14.2 P20.8 line 1234: "`vagent native seal`, `vagent native approve`, `vagent native withdraw`,
  running a `.vnprog`, an approval prompt for `ask`, and `reserveNodeId` for the node's epoch in
  `vagent run`".

**Fix:** In §14.2 P20.8, document the complete command grammar, arguments, defaults, and exit codes
for `vagent native seal`, `vagent native approve`, `vagent native withdraw`, and
`vagent run <file.vnprog>`.

### 5. (§5, §13) Lack of an end-to-end developer walkthrough from source to execution

**Problem:** §13 is structured as an internal regression test specification (`VagentNativeAgentTest`
in `tests/xuzz/`) that tests programmatic execution with mock pty masters and programmatic
`LocalHost` setup. An external developer trying to build a native agent from scratch has no
end-to-end guide. To build, seal, approve, and run an agent, the developer must piece together
fragments from §5 (Architecture), §6.1/6.4 (ABI/wrapper), §7.1/7.3 (sealing/staging), §10.2/10.5
(policy/ledger), §11.2/11.4 (declarations/sequences), §13.1 (sample agent), and §14.2 P20.8 (CLI).

**Evidence:**

- §5 lines 282-295: Summarizes seal/load/run in 12 lines.
- §13.1–13.2 lines 1051-1126: Test harness focus.

**Fix:** Add a dedicated "Developer Walkthrough" subsection in §5 or §13 detailing the sequential
5-step lifecycle: (1) authoring `my_agent.cpp`, (2) compiling to `my_agent.so`, (3) sealing to
`my_agent.vnprog`, (4) approving authorities with `vagent native approve`, and (5) running with
`vagent run`.

## MINOR

### 1. (§6.4, §14.2 P20.5) Missing compilation recipe and compiler flags for native shared objects

**Problem:** The specification does not document the recommended compiler invocation, C++ standard
(C++20), compiler flags (`-shared -fPIC -fvisibility=hidden`), or pkg-config invocation for
compiling `my_agent.cpp` into `my_agent.so`.

**Evidence:** §6.4 lines 407-425; §14.2 P20.5 line 1231.

**Fix:** Add a sample compile command in §6.4 (e.g.
`c++ -std=c++20 -shared -fPIC -O2 $(pkg-config --cflags vagent-native) agent.cpp -o agent.so`).

### 2. (§11.4) Grammar token `do` in sequence steps collides with C++ keyword

**Problem:** §11.4 defines `step ::= 'let' name ':=' call | 'do' call`. In the C++ wrapper, `do`
cannot be a member function name (`builder.do(...)`).

**Evidence:** §11.4 line 874.

**Fix:** Explicitly note in §11.4 and §6.4 that the C++ wrapper names this method `step` or `exec`.

VERDICT: NOT CONVERGED
