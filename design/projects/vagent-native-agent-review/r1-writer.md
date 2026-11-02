# Round 1 critique: Technical Writer (vagent-native-agent.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 6 MAJOR, 7 MINOR.

Checks run:
`mdformat --check --wrap 100 --extensions gfm --extensions dollarmath --extensions frontmatter` and
`mdl -s .mdl_style.rb` both pass with no output. Source citations I spot-checked are accurate:
`agent_executor.cpp:3271` (`dynamic_cast<const CompiledProgram *>`), `:3895` (`bindings_->start`),
`agent_executor.hpp:334-341` (`percept`), `program.cpp:18-27` (`refOf`), `mesh_value.hpp:28`
(`operations{65536}`), `identity_layout.hpp:30` (`kMaxPayloadBytes`), and the spec's V1–V16, §4.2,
§6.6, §7.3, §11.3 and §18.3 all exist as cited. The problems are internal to the document.

## BLOCKING

None.

## MAJOR

### 1. (§10.1, §10.4, §13.1) `Grant` has no `scope`, yet three places use it

**Problem:** §10.1 declares `record Grant { kind: Str, id: Int, incarnation: Int }` and
`request(kind: Str, purpose: Str)`. §10.4 then says "sets the window size from the request's scope"
and "The grant record's `scope` carries the terminal type". The §13.1 batch calls
`newterm(t.scope, $0, $1)`. There is no `scope` parameter on `request`, no `scope` field on `Grant`,
and the window size has no source the agent can name. The terminal type is the only way the example
tells ncurses what terminal it has, so the first test depends on an undefined field.

**Evidence:** `design/vagent-native-agent.md:511-513` (record and request), `:544-547` (§10.4),
`:743` (`t.scope`).

**Fix:** Add `scope: Str` to `Grant` and either a `scope: Str` parameter to `request` or state that
the broker fills it and the agent cannot ask for a size. Say where rows and columns come from (the
`vagent.native.*` settings or a `Grant` field). Then the §13.2 assertion "under the granted terminal
type" has a defined meaning.

### 2. (§11.5, §13.1) Batch `Arg` cannot express what the example's step strings do

**Problem:** §11.5 defines `Arg` as "a literal value, or `{result_of: Int}`". The §13.1 steps pass
`t` (a plan local holding a `Grant`) to `stdio.stream(t, "w")` and `t.scope` (a field of it) to
`newterm`. Neither is a literal or a prior result. The prose says `vn->steps` "parses them in the
host during the build into `Step` records", but a step string naming a plan local can only be bound
at run time, when the control plane stages the batch. The string grammar (positions, `$n`,
`local.field`, quoting) is not specified anywhere, and how `grant_fd` receives a `Grant` argument
(§11.2) is not tied to it.

**Evidence:** `:642` (`Arg` comment), `:741-750` (example), `:760-761`.

**Fix:** Add a third `Arg` form (`{local: Sym, path: [Str]}`, or a staged-argument index), give the
step-string grammar a short section or table, and say the binding happens when the batch is staged
(which §11.5 already says is when signatures are checked).

### 3. (§11.2, §11.5, §6.2, §7.2) `ffi_fn` and `batch` have no `within`, against C3

**Problem:** C3 says every external wait has a deadline, §6.2 says `vn_effect_method` "requires a
`within` duration", and §7.2 step 5 refuses "every effect method" lacking one. But the `ffi_fn`
syntax of §11.2 has no deadline, and `ffi_library` has only `soname` and `affinity`. §11.5 writes
the batch deadline as the placeholder `within <the library's batch deadline>`, which no declaration
provides. As written an `ffi` skill cannot pass its own verifier, or the verifier has an unstated
exemption.

**Evidence:** `:107`, `:295-297`, `:353-354`, `:582-589`, `:641`.

**Fix:** Add `within <duration>` to `ffi_fn` and a `batch_within` (or per-step budget rule) to
`ffi_library`; state whether `batch` inherits the sum of its steps' deadlines. Name the setting or
manifest field the placeholder stands for.

### 4. (§6.3 vs §7.2) `#Choice` is both allowed and reserved in fragments

**Problem:** §6.3 says "`#Jump`, `#Branch` and `#Choice` targets must be nodes of the same
fragment", which presumes fragments may contain `#Choice`. §7.2 step 3 removes `Choice` (with
`ChoiceDepth`, `Cut`, `Fail`) from the allowlist as "replay reserves", and says fragments use
`vn_alt`/`vn_fail` instead. Neither `vn_alt` nor `vn_fail` appears in §6.2 or §6.3.

**Evidence:** `:320-321`, `:345-348`.

**Fix:** Make §6.3 say "`#Jump`, `#Branch` and the targets `vn_alt` records", add `vn_alt` and
`vn_fail` to the §6.3 list, and keep one list of reserved opcodes in one place.

### 5. (§6.2, §13.1) The op families cannot declare what the first test needs

**Problem:** §6.1 calls the header "the whole ABI a module sees", and §6.2 is presented as the
complete declaration set, but the manifest items the design depends on have no op: FFI libraries and
signatures (§11.2), authorities with a purpose string (§10.1), and the binding kind of a native
skill. §13.1 uses `vn->ffi_skill`, `vn->authority`, `vn->steps`, `vn->body_adopt`, `vn->str`,
`vn->field`, `vn->index`, `vn->stmt_emit`, and the `VN_ARGS`/`VN_RECORD`/`VN_DISCARD`/`VN_NONE`
macros; none is in §6.1–§6.3. §6.2 lists `vn_native_skill` where the example calls `ffi_skill`.
§13.1 is "condensed" but an implementer cannot derive the ABI from the tables.

**Evidence:** `:262-269`, `:283-293`, `:727-757`.

**Fix:** Add a manifest-op row (`vn_ffi_library`, `vn_ffi_fn`, `vn_authority`, and the binding kind
on `vn_native_skill`) to §6.2, list the expression and statement ops (`vn_str`, `vn_field`,
`vn_index`, `vn_stmt_emit`, `vn_body_adopt`) in §6.3, define the `VN_*` macros in the §6.1 header
block, and use one spelling in the example. Also reconcile the seal command: §5 says
`vagent native seal module.so`, §13.2 runs `build/vagent-native-host --seal`.

### 6. (§13.2 step 7, §11.2) The post-resume assertion names a handle that does not exist

**Problem:** Step 7 asserts "After `resume`, a `refresh` through the old screen handle fails
`stale_handle`." But `ncurses.refresh()` takes no handle in §11.2 (`ffi_fn ncurses.refresh(): i32`),
and by the time the agent is quiescent the §13.1 batch has already run `endwin()` and
`delscreen($2)` (a `consumed` handle) and closed both streams, so there is no live screen handle to
go stale. The test as written cannot pass or fail on this.

**Evidence:** `:586`, `:588`, `:748-750`, `:783-785`.

**Fix:** Make the module keep a handle across the checkpoint on purpose (a second plan, or a variant
that leaves the screen open) and assert on a signature that takes one (`wrefresh(win)` or
`delscreen(sp)` with the old `SCREEN`). Declare that function in §11.2.

## MINOR

### 1. (§15, §17) A promised open question is missing

§15 says the "native code builds the AST" alternative is "kept as a possible convenience later
(§17)", but no NQ in §17 covers it (`:913`, `:958-968`). Add one, or remove the pointer.

### 2. (§12.1, §13.3, §10.1) Cause and error names are scattered and one is misused

Causes appear in six places (`refused`, `undeclared`, `unavailable`, `native_crashed`,
`native_killed`, `native_unavailable`, `native_protocol`, `too_large`, `stale_handle`,
`stale_grant`, `ffi_*`), and the error types `NativeProgramError`, `NativeAgentError`,
`NativeIpcError`, `NondeterministicBuild` and `CheckpointRefusal` are used without a definition or a
home. §12.1 defines `stale_grant` as a grant from a retired incarnation, but §13.3 uses it for a
fabricated `Grant{id: 0}` that never existed (`:673-674`, `:790-792`). `unavailable` (§10.1) and
`native_unavailable` (§12.1) are near-twins. Add one table of causes and one of error enums, each
with its section; give "unknown grant" its own cause.

### 3. (§10.1, §11.5, §11.6) Types and events used but never declared

`Value`, `Arg`, `NativeHandle`, and the `std:native/Readable{grant}` percept are used but not
declared in the §10.1 prelude block, which declares only `Authorities`, `AuthorityRevoked` and
`Grant` (`:511-513`, `:642-644`, `:657`). `ffi_global` (`:626`) and a default argument
(`n: i32 = 256`, `:586`) appear in prose and one signature but not in the manifest syntax. The
`cstring_out` row says the result goes "in the result record" while `Result.value` is a single
`Value`; say which.

### 4. (§8.1) "Exactly four descriptors" does not count

The text lists the socket, the shared-memory region, "the doorbell eventfds" (plural, or none) and
the log pipe as stderr, then also stdin/stdout as `/dev/null` (`:384-387`). Say "no more than these
roles" or count the descriptors including the eventfd pair.

### 5. (header, §14.2) Proposal count

The header says "P20.1–P20.6 change existing code and each needs its own approval", but P20.4
(logging doc) and P20.6 (conditional, may be withdrawn) are of a different kind, and §14.2
foreshadows a P20.7. Say "six, one conditional, and a possible seventh" once, in the header.

### 6. (§8.2, §13.4) Windows "not planned" yet "Everything but Linux needs a spike"

The sentence "Everything but Linux needs a spike that runs the §13.4 escape probes" (`:411`)
includes Windows, whose row says "not planned; refused (N7)". Also the macOS row's state is "later;
spike; SPI risk" while §17 NQ7 leans "Spike; if refused, macOS joins Windows". Say "every platform
that gets a backend".

### 7. (§13.1, §13.2) Minor example facts

"About a hundred lines" (`:719`) against about thirty shown; `vagent.native.enabled` defaults to
false (`:886`) but the §13.2 recipe never sets it. The seal step produces `.vnprog` files, but the
test's "Load" step does not say it passes the `curses_hello.so` path that §12.3 says the deployment
configuration names. The glossary omits "native skill", "percept driver", `NativeHandle` and
"module-native" versus "ffi" skill, though all are used as terms of art.

VERDICT: NOT CONVERGED
