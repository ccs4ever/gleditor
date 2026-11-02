# Round 1 critique: Programming Language Design & Ergonomics Expert (vagent-native-agent.md)

Paths below are relative to the repo root. Tally: 2 BLOCKING, 6 MAJOR, 4 MINOR.

Method: I read the whole target, then checked the specification's §6.6, §6.7, §8.5–§8.7 and §10
(`design/vagent-agent-language.md:883-965, 1180-1275, 1531-1572`), `host_bindings.hpp` and
`wire.hpp`. I did not build or run anything. The doc's line numbers are used below for the target.

## BLOCKING

### 1. (§6.2, §11.2, §13.1) The first test's module cannot be written from the ABI

**Problem:** §6 claims to be "the whole ABI a module sees" (`vagent-native-agent.md:260`), but the
ops that carry the test's most important content do not exist in it. §11.2 gives FFI libraries and
signatures only as "the manifest's textual form, for reading", with no `vn_*` call that records an
`ffi_library`, an `ffi_fn`, a signature type, `ok_if`, `consumed`, a default or `affinity`. §13.1
calls `vn->ffi_skill`, `vn->authority`, `vn->steps`, `vn->body_adopt`, `vn->local`, `vn->field`,
`vn->index`, `vn->str`, `vn->stmt_emit`, and the macros `VN_STR`, `VN_RECORD`, `VN_LIST`,
`VN_PERFORM`, `VN_NONE`, `VN_DISCARD` and `VN_ARGS`. §6.2's table lists none of them. It lists
`vn_native_skill`, which the example does not call. §13 says "about a hundred lines" and "no glue
compiled for it" (`:576`), yet the module must still say what `ncurses.newterm` takes.

**Evidence:** §6.2 table (`:283-293`) has no manifest family. §7.2 step 5 requires that "every
native skill named by `vn_native_skill` is in the manifest with a binding kind" (`:353`), so the
manifest is produced somewhere. §13.1's `vn->steps` "parses them in the host during the build into
`Step` records against the manifest" (`:760`), so the signatures must already be recorded before the
first batch. The module in §13.1 never records one.

**Fix:** Add a §6.4 "Manifest ops" family (`vn_ffi_library`, `vn_ffi_fn` with a typed parameter
builder, `vn_ffi_global`, `vn_authority`, `vn_native_skill` with its binding kind) and a full
signature table of every call the example uses: argument types, return type, the macros as functions
or enums. Replace the "condensed" §13.1 with a module that compiles against that header. The ABI
header is the artifact a reader of this doc most needs and it is the thing missing.

### 2. The example reads `t.scope` that `Grant` and `request` do not have

(§10.1, §10.4, §11.5, §13.1)

**Problem:** The example passes `"newterm(t.scope, $0, $1)"` where `t` is the `Grant` local. §10.1
declares `record Grant { kind: Str, id: Int, incarnation: Int }` and
`effect def request(kind: Str, purpose: Str): Grant`. There is no `scope` field. §10.4 says the
grant "record's `scope` carries the terminal type" and that the window size comes "from the
request's scope", but `request` takes no scope or size argument. Second, `t` is a plan local, a
runtime value. §13.1 says `vn->steps` parses the strings "in the host during the build into `Step`
records". `Step.args` is "a literal value, or `{result_of: Int}`" (`:642`), so there is no
representation for "the local `t`" or its field. A host-side build cannot see a runtime value.
`stdio.stream(t, "w")` therefore cannot be expressed either.

**Evidence:** `:512`, `:547`, `:642`, `:741-750`, `:760`.

**Fix:** (a) Decide what the terminal parameters are and give `request` a typed signature, for
example a `Terminal` record
`{kind: Str, id: Int, incarnation: Int, term: Str, rows: Int, cols: Int}` returned by
`request_terminal(rows: Int, cols: Int, term: Str)`. (b) Define the Step argument union to include
`{local: …}` or make `vn->steps` return a graph expression (a vn_node) that builds the `[Step]` list
at run time from locals. (c) Say which of these the verifier checks at seal time and which at
staging.

## MAJOR

### 3. (§11.5) The batch effect's types do not exist in the vagent type system

**Problem:** `effect def batch(steps: [Step]): [Result]` uses `Step`, `Arg`, `Value` and
`{result_of: Int}`. §8.7 (`vagent-agent-language.md:1260-1270`) has

```text
Int, Float, Str, Bool, (), Duration, Time, Agent, Space, Intention, Timer, Event, Opt<T>, [T], Map<K,V>
```

and declared events and records. There is no sum type (`Arg`: literal or reference), no dynamic
`Value`, and no anonymous record literal. `Map` is homogeneous and `WireValue` has no map kind
(`wire.hpp:73-76`). The example then does `vn->field(vn->index(r, 5), "value")` and emits it into
`Greeted.text: Str`. That is an unchecked `Value` to `Str` conversion in a language whose "types are
checked statically".

Positional bookkeeping is the second cost. `$0`, `$2`, `index(r, 5)` and the comment "5: reads the
line back" are magic numbers (house rule 2 forbids them in algorithms). Reordering a step silently
breaks later steps.

**Fix:** Do not expose a stringly `Step`. Let the manifest declare typed named sequences:
`ffi_seq draw(g: Grant, y: Int, x: Int, text: Str): DrawResult`, whose steps name earlier results
(`let out := stdio.stream(g, "w")`) and whose result is a declared record the module lists. The
agent sees an ordinary typed effect. Refused alternative to record: `[Step]` as the source-level
surface. Keep the generic list form only if a use case needs runtime-built sequences, and then give
it a real declared `Arg` encoding (a record with optional fields, using `Opt<T>`).

### 4. Failure semantics contradict each other, and the example leaks on failure

(§11.2, §11.5, §13.1)

**Problem:** §11.2 says `ok_if` "turns a C error convention into effect failure (`ffi_error` …) so a
plan's `else` handles a failed `refresh`". §11.5 says a batch "stops at the first failure, and the
effect completes with every result so far", and `Result` has `ok: Bool` and `cause`. Which is it for
a failed `refresh` inside a batch: `EffectFailed`/`else`, or `Result.ok == false` in a completed
effect? The example does not check `ok` and does not use `else`. If `newterm` fails, `index(r, 5)`
fails and the plan fails with the grant never released and no `endwin`/`delscreen`/`stdio.close`
run.

The agent author must pair `request`/`release`, `stream`/`close`, `newterm`/`delscreen`, and nothing
releases them on failure, drop or stop. §12.1 revokes grants only when the host dies. That is the
bookkeeping the design could remove.

**Fix:** Pick one rule (an `ok_if` failure inside a sequence fails the effect with `cause` naming
the step, and the completed prefix is reported as a field of the failure). Tie grants and handles to
the requesting intention: when the intention ends (success, failure, drop, stop), the broker revokes
its grants and the runner closes its `consumed`-pending handles. `release` stays for early release.
Show the example with `else` recovery.

### 5. The ABI is not versionable or checkable as specified, and uses C features unusable from Rust

(§6.1)

**Problem:** (a) "a module built against API version 1 runs on any host that still offers version 1,
and a host refuses a module that asks for a version it lacks" (`:271`), but the module never asks:
`vagent_native_build(const vn_api *, vn_builder *)` carries no version, and `VN_API_VERSION` is a
macro in the header. The table has no size or version member, so a host cannot tell how many entries
the module expects, and a module cannot tell how many the host has. (b) Ids and absence:
"`VN_NONE`", "`VN_DISCARD`" and an "invalid call sets the builder's error. Further calls are
ignored" mean every later call returns something. What is `vn_sym{0}`? Is 0 valid ("dense")? These
are in-band sentinels at an API boundary the doc declares C. House rule 3 allows them only at the
wire, and the doc should say that the ABI is that boundary and define them (an explicit
`vn_node {id}` with 0 invalid). (c) There is no `vn_error(b)` for the module to read the message at
the failing call, so a module returning 0 after an ignored error looks successful. (d)
`VN_ARGS(...)` and `vn->steps(b, "…", …)` are variadic. A function-pointer table slot cannot be a
safe variadic from Rust or most FFI users, and §11.3 refuses variadic foreign calls for this very
reason.

**Fix:** Export `uint32_t vagent_native_api_version(void)` and make `vn_api` start with
`{uint32_t version; uint32_t size;}` and be append-only. Define id 0 as invalid and give `vn_error`.
Replace variadics with `(const vn_node *args, size_t n)` forms; keep macros as optional C sugar.

### 6. (§6.3, §13.1) The statement and suspension ops are inconsistent with the doc's own rules

**Problem:** §6.3 says "`vn_stmt_begin`/`vn_stmt_end` bracket each statement" and that
`vn_stmt_effect` and friends build suspension statements. The example never brackets: is
`vn_stmt_effect` self-bracketing? A reader cannot tell which ops need brackets.
`vn->handler(…, body_adopt(greet))` takes a fragment, but `vn->plan(…)` is given statements built in
place with `stmt_*(b, plan, …)`: two different body conventions. `vn_local` returns a `vn_node`,
which §6.1 defines as "an opcode cell in a fragment", but a frame slot is not a cell. §6.1 says ids
are passed, not names, and the example passes names everywhere (`"Greeted"`, `"text"`, `"request"`,
`"Curses.batch"`, `"std:native/Authorities"`). The handler-versus-plan rule of the specification
(§6.6: handlers stage effects without waiting and cannot use `else`) is not carried over: nothing
says `vn_stmt_effect` in a handler fragment records a non-suspending stage, or is refused. The
`else cause` recovery of §6.6 (`:896-909`) has no parameter, so the C API cannot express the
specification's own recovery form. `vn->uses(b, agent, "Curses")` uses a skill name; `uses` names a
capacity (`vagent-agent-language.md:1193-1200`).

**Fix:** State once: statement ops are self-bracketing, or brackets are mandatory and shown. Give
`vn_plan` a body fragment like `vn_handler`. Add `vn_stmt_effect(..., else_fragment)`. Say that
handler-context effect ops stage without a result and are rejected with a named diagnostic if a
result target is given. Resolve effect and capacity references as ids.

### 7. (§11.2) The FFI manifest has gaps an implementer would fill differently

**Problem:** (a) `ffi_fn` has no `within`, yet §7.2 step 5 and C3 refuse an effect method without
one, and `batch` takes "the library's batch deadline" that `ffi_library` has no field for. (b)
`cstring_out(256)` appears among the C parameters but not among the agent's arguments
(`mvinnstr(2, 4)`); nothing says out-params are hidden, how they map to the result, or that
`n: i32 = 256` (a default, which the type vocabulary does not list) must equal the buffer size. The
same number is written twice. The result is `i32`, but the string is "in the result record": its
shape is not defined. (c) `ok_if(!= -1)` has no grammar, and a `NULL` handle result from `newterm`
(ncurses' failure value) is not covered by it. (d) A failure cause carries "the return value" and
`errno` as text, so the plan parses a string; `cause: Str` is the vagent form, but a structured
`errno: Int` would be typed. (e) Does `consumed` remove the handle if the call fails? (f) `in` is a
reserved word of vagent (`vagent-agent-language.md:1084`); using it as a parameter name in the
manifest invites copying it into source. (g) The skill-to-library cardinality is open: `Curses` is a
skill over library `ncurses`, `stdio` is a built-in library, and `Curses.batch` is addressed "per
library". (h) `handle<T>` is checked "by name" at run time, but the agent side is a single
`NativeHandle`, so a screen can be passed where a window is expected and only fails when run.

**Fix:** Add `within` to each `ffi_fn` (or a library default) and to `ffi_library`'s batch deadline.
Specify out-params as result fields, the `ok_if` grammar (comparison on return, plus `non_null`),
`consumed` on failure, and give handles per-type records
(`record Screen { id: Int, incarnation: Int }`) so mismatches are caught by the checker.

### 8. (§10.1, §11.6) The authority surface is stringly and under-specified against vagent's surface

**Problem:** `kind: Str` makes "undeclared" a run-time failure that the verifier could prove when
the argument is a literal (as in the example). `AuthorityRevoked` is declared as an `event`, while
§11.6's `std:native/Readable{grant}` is called a percept and is not declared in §10.1; whether a
plan can `await AuthorityRevoked as e [e.grant.id == t.id] within …` or must have a handler is not
shown. `Grant` exposes `incarnation`, a host detail the agent must treat as opaque; forging
`Grant{id: 0}` is a test (§13.3) rather than prevented by construction. `purpose` is declared in the
manifest (`vn->authority(b, "terminal", "…")`) and again in each request: two sources of truth.
`request` can fail with `refused`, `undeclared`, `unavailable` or the deadline, but no capacity type
lists causes, so an `else` cannot match them (cause is a free `Str`; matching is by string compare).
Only `release` returns `()`; the doc should state that a `release` of an already revoked grant
succeeds.

**Fix:** Make the verifier reject literal undeclared kinds at seal time. Declare `Readable` beside
`AuthorityRevoked` in §10.1 with its delivery class. Take `purpose` from the manifest only. Provide
the set of cause strings as a table in §10.1 (as §6.7 does for the core) and keep `incarnation` out
of the agent-visible record.

## MINOR

### 9. (§11.2) Syntax of the manifest text is irregular

`consumed` is a parameter suffix, `ok_if(...)` a function suffix, `affinity:` a map, `= 256` a
default, and `handle<T>` a generic, inside a form that is said to be bencode on disk. Choose one
modifier syntax (`name: type [modifier]*`) and show the bencode shape of one `ffi_fn`.

### 10. (§11.5) `Result` collides with `EffectResult`

`record Result { ok, value, cause }` repeats the host binding's `EffectResult` (`host_bindings.hpp`:
`ok`, `value`, `cause`) under a name that suggests the language's own. Prefer `StepOutcome`, and
keep `Value` out of it per finding 3.

### 11. (§10.1) Package naming

The `/` in `std:native/Authorities` reads as module path then name (`vagent-agent-language.md:1285`)
and is consistent. But the other prelude capacities (`Schedules`, `Clock`, `Logging`) are bare, and
`uses "std:native/Authorities"` in the example is a string where source writes `uses Authorities`.
State how an unqualified use resolves and what happens on a clash.

### 12. (§11.2) `bool` is C `int`

Say whether a C `_Bool`/`bool` is also accepted, or name the type `c_int_bool`. The width matters
for the libffi type chosen.

VERDICT: NOT CONVERGED
