# Round 4 disposition (orchestrator) — document is now version 3.2

NEW OWNER RULING (from the user, binding on reviewers): nothing is in production, so changes to the
Vortex core, the Vortex stdlib, the mesh runtime and other existing code are allowed where strictly
needed — but each must carry a detailed justification for why it has to be made at that level, and
must receive SEPARATE APPROVAL before being incorporated. §11 now separates new code (§11.1, not a
proposal) from a register of proposals P1–P9 to existing code (§11.2), each "pending approval", each
with "why it cannot live in std:agent or the adapter" and "without it" columns. Reviewers should
check that every dependency on existing-code change is listed there, justified, and cited by number,
and that the design states what happens without it.

All round-4 BLOCKING/MAJOR findings accepted:

- Vortex F1 (BLOCKING): requestId = sender-name hash (16 hex) : turn (16) : outbox pos (4) (§5.5).
- Vortex F2: sender-declared floor in every envelope replaces a high-water watermark (§7.3, P3).
- Vortex F3: `redeliver(requestId)` added to P2.
- Vortex F4: each statement gets its own #Choice (V13, §9.1, §6.7); SFAIL context-dependent.
- Vortex F5: mark first; scratch and cell limits relative to m0 (§4.2 steps 1-2).
- Vortex F6: P7 erases spawns\_ and monitors\_ entries too.
- Vortex F7: parameters ride in the issuer-signed capability; Initialize is a local event in the
  first wake turn; no gating (§4.1).
- Vortex F8 + Writer #1, #2: vicon §6 scanning functions listed as prerequisite; Point is a record
  declared in mars:hardware.
- Vortex minors: #Call/#Return with #Nop after calls (§5.8); contract violations and cycle
  exhaustion classification (§4.2); loopback Sender, trustNode, hashcash, launcher pid (§11.1);
  symbol-table entries for methods/answers/defs; citations replaced by function names.
- AOP #1: drop of a dead id is a no-op success; intentions() is top-level only; dropping Survey
  doesn't run its else.
- AOP #2: settle loop: drain d.pending and reconsider until fixpoint within belief_event_depth.
- AOP #3: await captures payload at match time into `waiting`; await guards isolated with the
  intention's frame.
- AOP #4: keyed predicates (`key` params); keyed revise; believes yields every source's value.
- AOP minors: restorer success = holds; maintain adopted once beliefs exist (rover adopts on first
  Battery via `Opt<Intention>`); recharge awaits battery with a guard (term awaits take guards);
  dropped restorer sets back-off; ConditionFault once; cancel-before-achieve remembered in
  $cancelled; drop takes effect at a statement boundary; no interleaving within plan segment (§14);
  effect deadline elapse stages cancel; no-applicable-plan fails immediately (stated).
- ZigZag #1: d.source minted by std:agent; d.extends added; thirteen dimensions.
- ZigZag #2: §5.9 values layout (lists as Vlog compound terms on +d.grab/+d.step, maps as lists of
  records, Opt).
- ZigZag #3: `ready` stamp attribute from $turn_count; ready list derived from cells.
- ZigZag minors: guard on +d.grab, body on +d.spin; extends via d.extends; instance content is
  qualified name; reserved fields completed ($parent $outer $terminating $cancelled $stats);
  d.subgoal is a chain; intention checkpoint cost (~30 cells); legend wording.
- Ergo #1: top-level `question` declarations type `ask`/`answer`.
- Ergo #2: ParamList takes Ident; Word only after '.', fields, record keys; accepts(sender,
  goal_name).
- Ergo minors: transitive purity of defs in conditions; return as an action, fail/return
  bottom-typed; case convention makes record literals unambiguous; `x == 1 | 2` warning; `for r`
  takes one name or a list; SARL emit self-delivery row; var initialiser timing; Map from std:agent;
  std capacities in scope without `uses`; newline rule for declarations; belief triggers take no
  `as`.
- Writer minors: within required on effect methods (checker); glossary rows for restorer, goal
  intention, question; §6.2 marked as excerpt; `c & e` sentence removed; §12.4 says how Warehouse
  starts.
