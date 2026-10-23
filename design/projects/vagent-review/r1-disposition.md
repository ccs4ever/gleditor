# Round 1 disposition (orchestrator)

The document was rewritten as version 2.0. Almost every round-1 finding was accepted. The main
decisions:

- V1: one agent = one MeshActorRuntime actor with its own arena; no cross-arena links; holon
  children are separate actors referenced by handle cells on d.holon; no population rank.
- V2/V3: speculation is turn-bounded; ArenaSlice, d.trail, d.shadow, d.choice, #Abort, #Trap
  withdrawn; one cursor per agent.
- V4: cross-turn intentions persist as data (goal, plan ordinal, tried list, step path, frame);
  suspension points restricted (§6.6).
- V5: all emissions staged on d.outbox, sent by the (owed) adapter at turn end.
- V6: `effect` methods only in bounded positions; free-standing `commit` removed.
- V6a (found by the orchestrator in vicon/compiler.cpp:358-361): the arena trail undoes every write
  in an Icon `every` body, so vagent has no `every`; it iterates with deterministic `for`. findall /
  `for believes` walk the clause rank with per-clause copies.
- V7: beliefs are Vlog clauses (d.belief → predicate → d.clause); performatives tell/untell/
  achieve-by/ask added.
- V8: agent state is arena state; no implicit promote; explicit StoreWriter capacity.
- V9/V10: ten dimensions minted by std:agent; fan-in only via clone views.
- Deliberation cycle, selection functions (fixed, stated), single-minded commitment, goal kinds
  (achieve/perform/maintain), plans `for` goals, failure taxonomy (§6).
- Grammar rewritten (§8.9), precedence table (§8.4), newline rule, SARL differences table,
  sibling-language table, ambient names, built-in events.
- Owed C++ listed and priced (§11), including two mesh_actor.cpp retention fixes the orchestrator
  verified: `deliveries_` and `remembered` are never erased (no `erase` in the file), so a runtime
  can send, and an actor can receive, at most 4096 messages in its lifetime.

Partly accepted or deferred, with reasons:

- AOP #11 (protocol declarations, FIPA/Contract-Net): performatives added; a `protocol` declaration
  is not added in 2.0. Deferred, not refused.
- AOP #5 (overridable selection functions): selection functions are stated and fixed in 2.0.
- AOP commitment variants: single-minded only; others are U-A5.
- Ergonomics #25 (test harness): U-A10.
- ZigZag #7 (intentions on d.cursors): refused in §5.1 and §13 — a cursor is a VM process with a
  program counter, and V4 forbids persisting one.
- Writer #13 (comparison matrix): replaced by a BDI-oriented matrix with a "where vagent is weaker"
  paragraph.
