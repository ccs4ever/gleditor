# Round-14 disposition — orchestrator rulings on v1.13 findings

0 blocking, 1 major, ~22 minors. **Six of seven lanes converged** — the strongest round yet.
R13-A verified sound end-to-end in four lanes (content-carried endpoints close ≥3 sharers,
chains, and cross-head copies with zero edges into shared referents). The remaining work is
stale-vocabulary cleanup plus two real specification gaps.

## Major — adopted

**R14-A — same-revision collision: the agent-legs sentence still claims reachability "through
capture edges"** (codebase major; zigzag + meta + architect minors — four lanes hit the same
stale text). :2114-2121 justifies agent-`d.vars` reachability via "a fragment's slot link to an
agent-`d.vars` cell" — the edge class R13-A deleted, and §6.1's `-d.values`-live rule forbids
for `d.vars` cells anyway. The conclusion (legs classify state-side) survives by unreachability;
the mechanism claim is wrong twice over. **Ruling:** restate — agent-`d.vars` cells are reached
through the agent cell's own `±d.vars` rank (adapter-bound agent root), never through fragment
correlation; the walk is the two-root vocabulary already stated. Sweep all residual
"capture edge/link" vocabulary: :137 (glossary), :1170, :1840-1847, :1863-1864, :2114-2121,
:2192-2194 — replace with the content/map/name three-arm phrasing.

## Minors — all adopted

- **`closed`'s content-read lacks the program-side `+d.name` carve-out** (architect + vortex):
  a `dimConstant`'s program-side dim referent carries `+d.name` on the *recipe record*, not a
  `+d.refs` name-record — strictly read, the exemption is missing and every `cells"…"` path
  refuses at `learn` re-verify. State it.
- **`closed` frontier stale enumeration** (vortex + ergo): :1863-1864 still says "capture edges
  on record dims" — restate as the content/record-keyed frontier; :1840-1847 arm (a) must not
  read legal field captures out of the admitted class.
- **(d)-time endpoint read is `*handleTarget`, not `int64`** (vortex): donor slots are
  `OpHandle`-kind at (d) — :2216-2219.
- **Recipe tuples' `+d.values`→endpoints** (ergo): sits beside "one record, one edge, only
  where `-d.values` unclaimed" — shared tuple endpoints hit the same M14 class; name the
  conditional or the encoding.
- **Changelog program-side misgrouping** (ergo + writer): :3555-3557 groups program-side under
  `int64`/`*handleTarget` — the mechanism keeps it on `+d.name`→`ensureDimension`; fix.
- **§10.2 (v) un-narrated + cite** (writer): add to the v1.13/1.14 changelog; `store.cpp:124`
  shows the ctor initializer — the no-id signature is `:123` (and `store.cpp:4450` writes it on
  directory load — codebase nit).
- **"Displaced-vs-appended auditable" bound** (ergo): holds only for *declared* victims —
  session-grafted victims mint no `d.patch` record; state the bound.
- **Grafted-twin reduce-to-append drops the replay caveat** (writer): :1235 — a dropped
  displacer replays as a *displace*; echo fix into changelog :3572.
- **"`documentId_` is `Store`-ctor state"** (codebase nit): `store.cpp:4450` writes it on
  directory load too — narrow the claim.
- **Mirror the "no session resolver" qualifier** (writer): glossary :136 and §6.1 :1231.
- **Agent-legs nit** (meta): name §5.3's clone/`masterOf` mechanism for any remaining
  reachability phrase.
- **Zigzag's other minors** (:1743 direction slip verified fixed; residual "capture links" at
  :1863 frontier enumeration — covered above).

## Round status

v1.14 to follow. Trajectory: r12 ~6M → r13 1M (5 lanes) → r14 1M (6 lanes converged). The
mechanism is closed; what remains is vocabulary coherence around the removal.
