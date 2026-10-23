# Writer critique, round 1: design/vagent-agent-language.md

Mechanical checks: `mdformat --wrap 100` (gfm, dollarmath, frontmatter) reports the file clean.
Over-100-column lines are only table rows, EBNF lines 711/752 and a mermaid line (1057/1060), all in
fences or tables. All 48 fence markers pair up. The document is 1726 lines. The substance is below.

## Findings

1. BLOCKING. Header and sections 4.1, 6.1. The Status paragraph says "asks the C++ core for nothing"
   and the Acceptance Test says "Every valid vagent program must lower entirely to link and value".
   The body introduces things that exist nowhere in the tree:

   - `ArenaSlice` (lines 1397, 1406, 1496). `grep -rn ArenaSlice apps/` finds nothing.
     `ArenaManifold` has mark/release, not per-intention slices, and §4.3 itself says marks are
     linear watermarks.
   - `#Trap` and `#Abort` as opcodes or protocols (1356, 1499). vicon-goal-directed-language.md has
     no `Trap` or `Abort`.
   - The "Grant Renewal Protocol" (985) and the "Ingestion Gate" (1021). Neither appears in
     `mesh_actor.hpp`. A grep for `Authorizer` finds only `setAuthorizer` at hpp:125, which admits
     and does not renew.
   - `HolonGoalFailed` (966, 1514), `query`/`ask`, `scan`, `Schedules` timers and the whole
     `std:agent` module.
   - A population dimension and the new `d.*` dimensions of §2.2. The `SystemDimensions` struct at
     vortex_core.hpp:34-41 has no `agent`, `behavior`, `event` and so on.

   Fix: add a Status table with rows "built" (cite file:line), "built elsewhere, reused" and
   "proposed here". It must list each item above. Soften the "asks for nothing" sentence to "asks
   for nothing in the `link`/`value` kernel; proposed additions are listed in §X". The same split
   applies to §4.1: a feature that needs a new opcode is a priced exception, not an automatic
   rejection.

1. BLOCKING. Section 4.1. The Acceptance Test is circular. It says "the lowering cross-walk proves
   this" (449), but §6.1 is a table of prose lowerings with no opcode listing, and some rows lower
   to "Spool-Mesh actor message-boundary primitives" (`MeshActorRuntime::send`, BEP 46 seeding). The
   test therefore admits an unbounded set of C++ escape hatches beyond `link` and `value`. "Proves"
   is an assertion. Fix: rename §6.1 "Lowering sketch" and drop "proves". Add a column "Needs beyond
   link/value" with explicit entries (send, timer, swarm seed). Reword the test as: reduces to
   link/value plus the enumerated mesh calls {send, seed, checkpointView}.

1. MAJOR. Section 1.1 (line 96), §7.2 (1004), §11.1 (1671-1673). These are overclaims stated as
   fact:

   - "rock-solid failure boundaries" is marketing tone.
   - "Deadlocks are mathematically impossible". §10.3 itself defines an "Epistemic Query Deadlock"
     and has `query`/`ask` suspend the intention until the reply arrives. That is an RPC-style
     block, contradicting §11.1.2 "Synchronous RPC-style blocking cycles cannot be constructed". Two
     agents asking each other, each suspended, form a wait cycle, which is a livelock if not a
     deadlock.
   - "structurally impossible" for data races. This is true only between arenas. Within an agent,
     the `d.clone` master write (2.5) is shared by behaviors.
   - "liberates" (18).
   - "seamlessly" (1040).

   Fix: restate as claims with conditions ("no shared mutable memory between arenas, so no data race
   between agents; wait cycles via `ask` are possible and are bounded by timeout T"). Add a timeout
   or `maxWait` row to the grant (§7.1) or refuse the cycle. Strike the adjectives.

1. MAJOR. Whole document, house style. There are no priced rulings and no refused alternatives, and
   few `file:line` citations for design choices (C++ refs appear only in §3.2/§7). The owner's
   standing style is "argued, not asserted". Missing:

   - A "Rulings" section (V1..Vn) with a cost per ruling: why a new `d.*` dimension per concept, why
     `emit` inside a plan is staged, why `commit;` is a `#Cut`.
   - A "Refused alternatives" section: embed in Vlog rather than a new surface; actors-only without
     SARL spaces; one overloaded dimension for behavior/skill/capacity; dynamic typing of events.
   - "Open questions" (the U1-U3 style of store-slice-convergence.md).

   Fix: add them after §1.4, and cite the sibling-doc rulings they rest on (R8 for ephemerality,
   vicon §8.1).

1. MAJOR. Section 2.2 / 1.4 versus the "more cells and dimensions" preference. Seventeen new
   dimensions are declared with no cost and no check against dimension budgets or `d.*` bootstrap
   conventions in `SystemDimensions`. Conversely, §3.2 and §7 do not say which agent data (priority,
   `maxInstructions`, guard predicate) is stored as cells versus packed in the payload.
   `priority 100`, the `CRITICAL_THRESHOLD` (1303), "80% of lease" (986) and "instruction quota" are
   naked magic numbers, which AGENTS.md forbids in algorithms. Fix: list each as a setting in
   `system://settings` (or an agent-level cell) with a default, and say which.

1. MAJOR. Terminology drift. Examples:

   - "Agent Headcell", "Agent cell", "root" and "Headcell".
   - "Space" versus "context" versus "inner_context/outer_context", which the grammar treats as
     identifiers while §5.4 treats them as space expressions and §3.1 as sets of spaces.
   - "Mailbox ring", "mailbox", "queue" and "`+d.event` rank".
   - "Intention" versus "plan" versus "goal" cell.
   - "Skill rank", "`d.skill` rank" and "ring": dimensions are called ranks and rings
     interchangeably, though §2.1 says only rank.
   - "Event" (immutable cell) versus `ActorMessage` (envelope).
   - "Epistemic query" versus "ask" versus "query". §7.4 gives the pair, §8.2 omits both capacities,
     and §5.5's grammar has no `query` or `ask` production.
   - Dimension headings use "`d.agent`" for both a population spine and a child-sibling link, and
     §2.2 says it is a "peer chain" while §2.3 uses it for peers and §6.5 uses `+d.holon` plus
     `+d.agent` for children. This confuses a reader (and `d.holon` is not 2-degree-safe, see
     finding 8).

   Fix: a short glossary after §1.4 with one name per concept, then a pass.

1. MAJOR. Examples versus grammar (section 5.5). Evidence for each:

   - `emit E into X to Y` is allowed by `EmitStmt`, but §5.4 describes the clauses as alternatives.
     Define ordering and exclusivity.
   - `on TargetLogged(drone, target) from inner_context` is fine, but
     `on RawTelemetryPacket(packet: String)` (1528) uses a typed pattern. `PatternField` has no
     `':' Type` form (its `Identifier ':'` is a field name). This does not parse.
   - `scan packet { ... }` parses as `ScanExpr`, but `tab(...)`/`move`/`many`/`upto`/`match` are
     never declared. Where do they come from? Neither `std:agent` nor the imports list them.
   - `import std:agent;` (1567) does not match `ImportDecl` (`Identifier | StringLiteral`; `:` is
     not an identifier char) and `QualifiedIdent` uses `::`. Likewise `module mars::exploration`.
     `import std:math (Point, distance)`.
   - The `goal`/`plan` bodies call `PlanDirectPath(loc) | PlanRidgeBypass(loc)`, so plans are called
     as expressions, yet a `PlanDecl` is not an expression-callable `MethodDef`.
     `achieve X & Y else {...}` binds `else` ambiguously against
     `AchieveStmt ::= 'achieve' Expr ['else' Block]`.
   - `{ ... } & drive(loc)` uses a `Block` as a `PrimaryExpr` operand. Blocks end in `commit;`, a
     `CommitStmt`, which `PrimaryExpr 'commit'` allows. The document is inconsistent on `commit` as
     statement versus expression.
   - `check_feasibility(dest) & commit; & navigate_to(dest)` (1375): the `;` after `commit` ends the
     expression, so `& navigate_to` is a parse error.
   - `every $sample := ...` (1351) uses `$` and `:=` that are not in the lexical rules. §5.5 says
     `every Expr do Block`.
   - `uses ThrusterNavigation as Navigation` with `UsesDecl ::= 'uses' Identifier 'as' Identifier`,
     but §9.2's `setSkill(SwarmTransport as Transport)` is the form `SkillSwapStmt` takes. Also
     `SkillSwapStmt` is a statement where the prose uses it like a call.
   - `[String; [String]]` (1177) is not an `ArrayType` (the length slot needs an `IntegerLiteral`).
     It looks like an intended map type, which the grammar lacks, and
     `pendingQueries.insert/get/set` are undefined.
   - `abort intention;` is fine. `idx.toString()` and `Point(0.0,0.0)` are not covered by any
     declared library.
   - `self`, `inner_context`, `outer_context`, `DefaultSpace` and `Initialize`/`Destroy` appear
     nowhere in the grammar or lexical rules.
   - In §9.1 `WorkerDrone` emits `into outer_context` but the coordinator subscribes
     `from inner_context`. By §5.4 the worker's outer context is the coordinator's inner context, so
     the names flip by viewpoint without the document saying so.
   - §9.1 aggregation checks `currentHits.len() >= expectedParts * 2`, which silently hard-codes
     each stub worker's two hits.

   Fix: either fix the grammar or run every example mentally through it and keep a table of the
   "ambient names". Ideally compile the examples against a grammar file in a test.

1. MAJOR. Sections 2.1, 2.3, 6.5 against the 2-degree invariant. §2.1 says each cell has at most one
   posward and one negward neighbour per dimension. §2.3's figure shows `Agent -> +d.agent`? (only
   children), and `Child1 -(+d.agent)-> Child2` while the headcell's own `d.agent` population link
   is also used, so the same dimension is serving as population spine and holon sibling chain.
   §2.4's example splices at the head with `link(agent_cell, d.behavior, +1)` returning `old_b` and
   then `link(agent_cell, d.behavior, +1, B_new)` without relinking `-1` of `old_b`. §2.3 also shows
   both `S1 -(+d.skill)-> S2` and `S1 -(+d.capacity)->` fine, but `Agent -(+d.skill)->` two skills
   mapped to the same capacity in §6.3 are unspecified. `d.capacity` is described as "points from a
   skill to the capacity" yet the §8.1 diagram chains capacity cells along `+d.capacity`, which
   makes it both a pointer and a rank. Fix: rename the dimensions (population `d.agent`, siblings
   `d.holonmate`) or state the pointer-versus-rank convention. Specify what happens when the second
   skill for a capacity is mounted.

1. MAJOR. Section 7.1 and 3.2 claims about code. Spot checks:

   - `mesh_actor.cpp:668-682` is correct for the quiescence conditions (checkpointView, lines
     668-680), and the `ActorMessage`, `ActorGrant`, `ActorPid` and `ActorContinuation` locations
     match.
   - But the list in §7.3 adds `deliveries_`, which is right, and the document drops
     `actor->running` and `stopped`.
   - §3.2 quotes `ActorGrant` fields (`expiresAt`, `maxMailboxBytes`) without a code citation for
     each. Please verify the full field list; I confirmed `maxCells`, `maxOutputBytes`,
     `maxSolutions`, `maxInstructions` only.
   - The `ActorPid` description is inconsistent. §3.2 says `node, cursor, epoch, spawnSequence` and
     §7.1 says "Ed25519 public key, local sequential process index, monotonic epoch". I did not
     verify the field types.
   - `zigzag::promote()` at line 834 is accurate and is a free function taking a `Store`, not a
     method, yet §4.3 and the §3.2 class diagram show `ArenaManifold` with no `promote`. §4.3's flow
     also says `promote()` "strips ephemeralBit" and then calls `release()`, which only works if
     marks nest. The header says promote maps refs and does not strip bits.
   - §7.4 "pointer promotion in microsecond latencies": no measurement. AGENTS.md requires empirical
     data for performance claims. Drop it or cite a probe.
   - "BEP 10 local ID 6" (1070) is stated without a citation. Check bittorrent-expert.

   Fix: add a verified-references table and remove unmeasured numbers.

1. MAJOR. Section 1.4 versus §1.3. The "What is Being Claimed" table has a "New Machinery" column
   whose answer is "none" for 11 of 12 rows. That is the strongest claim in the document and is
   argued nowhere (see finding 1). Several rows cite the wrong anchor: "Vortex §3" and "Vortex §6"
   and "Vicon §6" are not shown to exist. Check that those sections say what is claimed. Fix: each
   "none" gets a pointer and a one-line justification, or the cell becomes "new: ...".

1. MINOR. Contents and anchors. The TOC labels differ from headings (1.1, 1.3, 2.5, 2.6, 3.2, 4.1,
   4.3, 5.x, 6.x, 7.x, 8.2, 10.x, 11.1). That is fine, but the reference-style link ids are
   unordered and have gaps (`toc-25`, `toc-33` missing, `toc-26` before `toc-27`, `toc-dims`).
   Switch to inline links, or generate the ids in order. Sections 10.3 and 10.4 contain four-level
   headings (`####`) that the TOC omits. This is acceptable, but "Action Commit versus Goal
   Achievement" deserves a number (10.3.1). mdl passed on first read; confirm with `make lint` (I
   ran mdformat only).

1. MINOR. Redundancy. The 2-degree invariant is restated in §2.1, §2.3, §6.5, §10.5. The
   `+d.grab/-d.grab` wings are described in §2.2, §4.2 and §6.1. The quiescence conditions appear in
   §7.3, §10.1 and §10.4 (three times, same `mesh_actor.cpp:668-682`). §6.2 repeats the §2.3
   diagram. §11.2 repeats §1.3 and §1.4. Fix: define once, link elsewhere.

1. MINOR. Section 11.2 comparison matrix makes unsourced claims about SARL, Erlang and Akka (for
   example "Erlang: Manual Recursion for search", "Akka: FSM", "Janus Network Layer", "State
   Cloning"). Bold in only the vagent column reads as advocacy. Cite or drop, and add a row on costs
   where vagent loses (throughput, tooling, maturity, no compiler exists).

1. MINOR. Section 5.1 is two sentences ("expressive, modern syntax influenced by Rust and Kotlin")
   and says nothing about lexical structure: comments, identifiers, `$`, literals, `//` versus
   `(* *)`, whether `emit E{...}` needs braces. Retitle or fill. "stripped of Java/Xtend
   boilerplate" is rhetoric.

1. MINOR. Section 8.2 declares capacities `Schedules.every` and `in`, but `every` is a keyword in
   §5.5 (`EveryStmt`) and `in` is a keyword in `ForStmt`. These capacity method names cannot be
   called. Also `Logging` is routed through `GLEDITOR_LOG_INFO` with category `vortex.agent`, but
   AGENTS.md wants spdlog categories in `<gleditor/logging.hpp>` and says to avoid logging document
   text. State the category is registered.

1. MINOR. Header block at lines 3-5 is a run-on: "**Document Version:** ... **Compilation Target:**
   ... **Status:**" are joined in one paragraph (the mdformat reflow). Use a list or a table so
   Status stands on its own line. Siblings use a version history; add one.

1. MINOR. The mermaid `classDiagram` in §3.2 shows `+d_agent links` fields on `AgentCell`, which is
   not a C++ type. The prose says "Every entity maps directly to verified C++ types", but
   `AgentCell` is the one that does not exist. Label it conceptual.

## Summary

Counts: 2 BLOCKING, 8 MAJOR, 7 MINOR. The structure is workable and the code citations that I
checked for `mesh_actor` are accurate. The document does not yet separate built from proposed,
overclaims soundness and speed, and its grammar does not parse its own examples.

VERDICT: NOT CONVERGED
