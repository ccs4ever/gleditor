# Round 1 critique: Xanadu / ZigZag Purist (vagent-native-agent.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 5 MAJOR, 3 MINOR.

Lane verdict in one line: the split is sound where it matters. The agent's own state (`Grant`,
`NativeHandle`, intentions, outbox) is cells in the control-plane arena (N2), and the genuinely
process-local items (descriptors, `T *` pointers, ring indices, pty masters) correctly stay out of
the checkpoint (C10, §12.2). The gaps are at the seams: the replay can break the 2-degree rank
invariant without detecting it, C6 is specified by an unverifiable criterion, and three kinds of
*structure* (approvals, the manifest, the grant ledger) are kept as strings, bencode or C++ rows.

## BLOCKING

None. The nearest candidate is MAJOR 1, which is downgraded only because the doc's own replay (not
the ABI) is the place the invariant is enforced, and the fix is a verification step.

## MAJOR

### 1. (§6.3, §7.2) Fragment ABI admits fan-in on `d.spin`/`d.grab`; verification never checks it

**Problem:** §6.3 builds fragments with `vn_next(b, from, to)` "on `d.spin`" and operands "on
`d.grab`", with `vn_op(b, kind, inputs…)`. Nothing prevents `vn_next(x, join)` and
`vn_next(y, join)` (a join after a branch, a loop head with a back edge), or one value node named as
an input of two ops. In a ZigZag rank `A.pos(d) = B <=> B.neg(d) = A`, so a second link to the same
target along the same direction evicts the first. Replay would then build a graph that differs from
the op log, and the first the module author hears of it is a wrong program. §7.2's seven steps
(decode, ids, allowlist, targets, effects, replay, bounds) include no rank-shape check, yet §7.2
concludes "Verification establishes that the graph can do nothing a vagent program cannot".

**Evidence:** `zigzag/arena_manifold.hpp` documents that `link` displaces an existing neighbour (the
checklist's eviction rule; I did not re-derive the exact call path). The compiler avoids the problem
rather than meeting it: control-flow targets and operand references are `OpHandle` scalar cells made
per use (`vagent/runtime/compiler.cpp:582-609`, `cached(oprefs_, …)` and `stateHandle`), not rank
links, and `runtime_abi.hpp:23` chains list elements on `+d.step` and puts only the first on
`+d.grab`. The native ABI describes a different, link-based encoding ("chained by `vn_next` on
`d.spin`, operands on `d.grab` as the VM reads them") without saying how sharing is represented. I
verified that `#Jump` is handled in `vortex_vm.cpp` and the core, but not the exact operand encoding
the VM reads for every opcode.

**Fix:** (a) State in §6.3 that `vn_next` is a function (at most one successor and one predecessor
per node) and that a join or loop back edge is a `#Jump`/`#Branch` to an `OpHandle`, as the compiler
does; (b) state that a value node used by several consumers is referenced through per-use handles or
a `d.clone` view cell on its `+d.clone` rank (posward of the master), never by a second link along
`d.grab`; (c) add a verification step between "Targets" and "Replay": reject any fragment whose
in-degree exceeds one on any (dimension, direction), with the op ordinal, and add a gate test that
records a diamond and a loop.

### 2. (§12.2, C6) The state-to-program check has no stated criterion and misses shadowed cells

**Problem:** "the adapter walks what is reachable from the agent cell and refuses the checkpoint if
the walk enters a program cell of the shared base." Under P17 the agent's arena is
`ArenaManifold(&programManifold)`, a copy-on-write overlay. A run-time `link` from a state cell to a
base cell calls `shadow(*to)`, which copies the base cell into the overlay *keeping its
non-ephemeral `CellRef`* (`arena_manifold.hpp` overlay comment;
`vagent-shared-program-and-live-upgrade.md` lines 247-248). After that, the walk meets an
arena-resident cell, not a base cell, and a test of "is this a base cell" by storage location
passes. The criterion that works is the ref: any reachable cell with `!isEphemeral(ref)` that is not
on the permitted whitelist (e.g. the dimension cells every state cell legitimately reaches via
`d.dims`).

**Evidence:** P17 line 114 says unshadowed base refs are left untouched and shadowed ones promoted;
`checkpointView` is called at `agent_executor.cpp:4794`. The doc itself notes (§12.2) that it does
not know whether the existing promotion would "silently drag the program into the checkpoint".

**Fix:** Specify the criterion as the ref class, name the whitelist (the dimension cells), and say
what happens to the reverse direction (a base cell holding a link to an ephemeral cell is impossible
by `PROT_READ`, so only state-to-program needs the walk). Price it better: the arena already tracks
which base cells it shadowed, so the check can be O(shadowed cells), not "O(live cells + links)"
(§16); verify that accessor exists before relying on it, which I did not.

### 3. (§10.2, §14.3) Approvals are `programRef:kind` strings in one setting

**Problem:** `vagent.native.approvals` holds "`programRef:kind` pairs". That is a record serialized
into a string: a program identity and an authority kind packed with a colon, parsed by C++ on every
request. The checklist's rule (no foreign serialization inside cells) applies directly, and the
settings machinery cannot even express the intended list well: `SettingSchemaShape` types are
`integer|float|bool|string` with `CellValue = variant<double, int64_t, bool, string>`
(`system_docs.hpp:760-765`), so the only encoding on offer is the packed string.

**Evidence:** `system_docs.cpp:565-600` shows every setting spec is one scalar shape. A pair has two
fields, a ref and a kind, and later fields are likely (granted-at, scope limit, expiry). Each added
field would extend the string grammar.

**Fix:** Model an approval as structure: one cell per approved `programRef` (content = the ref, so
it is quotable and transcludable), with its approved kinds as cells on a rank along a dimension (the
existing `d.values`/`d.groups` geometry of `system://settings` or a new `d.approvals`, declared as a
cell per R12), plus the schema and notes entries P20.3 already owes. P20.3 must then say which
geometry it adds; the setting is a *container*, not a scalar. Refused alternative to record: a flat
string setting, refused for the packing.

### 4. (§7.1, §11.2, §14.1) The manifest is a second program representation outside the cells

**Problem:** The manifest (native skills and binding kind, FFI libraries, function signatures and
types, authorities and purposes, target triple) is bencode inside the `.vnprog`, and the C++
`manifest.{hpp,cpp}` consults it at run time: batch steps are "checked against the manifest when the
batch is staged" (§11.5), `RemoteNativeSkill` is bound from it, and the host resolves symbols from
it. So the program's callable surface lives in a C++ structure that is *not* in the replayed graph,
not in the program store, and not queryable by VQL/VPL or visible in a zigzag view. Everything else
about the agent is a manifold (V10: the graph is data); this is the one place where meaning is kept
in a blob.

**Evidence:** §7.1 (`manifest` is a dictionary key) and §11.2 ("On disk it is bencode"); the vagent
spec's own symbol table (§5.7) and `fieldDefaults`/capacity cells are cells.

**Fix:** At replay, promote the manifest into the program store as cells on dedicated dimensions
(`d.ffi` rank of library cells, each with a rank of signature cells, parameter-type cells on
`d.vars`-style ranks; an `d.authorities` rank), so `programRef` still covers it (it is hashed from
the same bytes) and the runtime reads it from the manifold. The bencode stays as the interchange
form, as source text does. State that explicitly, and cost it against the 262,144 cell bound.

### 5. (§10.3, §12.1) Grant metadata exists only in the broker's C++ rows, with no durable record

**Problem:** Separating the legitimately process-local from the structural: the fd number, the pty
master and the incarnation's handle map must stay in C++ (they cannot be cells and must not
checkpoint). But "kind, incarnation, the control plane's end of the resource" also carries the
*decision*: which program asked, for what purpose string, who or what approved (policy value), when,
and why it ended (`released`, `host_lost`, revoked, lease end). That is the security-relevant
history of a terminal-write authority, and it is lost on node restart or host death. R8's activity
store precedent (`system://activity`) shows the repository already records completed events as
private-store Structure operations.

**Evidence:** §10.3 "Grants are process-local and never enter a checkpoint"; §12.1 revokes and
retires them with no record; `AuthorityRevoked{grant, cause}` is posted to the agent only.

**Fix:** Keep the table for the fds. Add an append-only authority ledger: each request, decision and
revocation is a small record of cells (program ref, kind, purpose, decision, cause) minted as
Structure operations in a private store, mirroring R8's activity store and kept out of any visited
document. Price it (one `MakeCell`/`SetLink`/`SetValue` batch per decision) and say which store. If
the owner rules this out, record the refusal in §15.

## MINOR

### 6. (§10.1, §10.4, §11.2, §13.1) `Grant` and `NativeHandle` are not defined consistently

`record Grant { kind: Str, id: Int, incarnation: Int }` has no `scope`, yet §10.4 says "the grant
record's `scope` carries the terminal type" and §13.1 passes `t.scope` to `newterm`.
`NativeHandle {incarnation, id, type}` is used in §11.2 but declared nowhere. Both are agent-visible
records, so their fields are cells and the declaration is the schema; define `scope` (and decide
whether it is a `Grant` field at all, given that the host never learns a path) and declare
`NativeHandle` in the prelude capacity of §10.1.

### 7. (§3 C9) Dimension count disagrees with the code

C9 says "Genesis, the thirteen dimensions"; `runtime_abi.hpp:50-51` documents "The fifteen
dimensions the adapter registers in every agent arena". The spec section the doc cites may say
thirteen; reconcile the number or cite the spec's section. Confirm which is current before naming
either.

### 8. (§N5, §5 Load) Where the string constants land is not stated

N5 argues the sandbox cannot promote because promotion spools strings into the author's permascroll,
but then says the control plane promotes "into the program store, as `fromSource` does".
`program.cpp:44` builds `std::make_shared<xanadu::Store>()` with no permascroll visible at that
call, while AGENTS.md says a `Store` "is handed one, never makes its own". I could not verify which
scroll the program's string constants (names, `"terminal"`, literal texts) end up in, or whether the
spans are identical on every node for one `programRef`. State it: if they land in the node owner's
permascroll, text constants are authored (and attributed) to the node owner rather than the module,
and the spans differ per node, which must not leak into `programRef` or checkpoint identity. If they
use the scratch scroll, say so.

## Checked and found sound

- Handlers on `d.clause`, the dispatcher and symbol table are emitted by the scaffolding (N4), not
  by native ops, so there is no handler fan-in in module code; `vn_alt`/`vn_fail` are lowered by
  replay.
- R8: the staging context is an ephemeral arena promoted once into a program store; no visited store
  is mutated, and the checkpoint publishes a separate store (`agent_executor.cpp:4794-4820`).
- R12: dimensions are minted with `ensureDimension`, not hardcoded rank indices.
- The handle table, descriptors, ring indices and the pty are correctly *not* cells; the agent holds
  names (V11) that go stale across incarnations.

VERDICT: NOT CONVERGED
