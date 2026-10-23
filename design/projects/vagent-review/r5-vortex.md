# Round 5 critique (vortex role): vagent-agent-language.md v3.2

Paths are relative to `apps/common/xanadu/` unless stated. All checked against the worktree.

## 1. Status of round-4 BLOCKING and MAJOR findings

| R4  | Topic                             | Status   | One line                                                                                                       |
| --- | --------------------------------- | -------- | -------------------------------------------------------------------------------------------------------------- |
| F1  | Request id lacks sender           | RESOLVED | 16-hex sender-name hash : turn : position (5.5); 38 chars fits `boundedText(..,128)` (actor_wire.cpp).         |
| F2  | Watermark drops fresh messages    | PARTLY   | The floor is sound as a rule, but how the runtime reads it is unspecified and unregistered (N1 below).         |
| F3  | `send` cannot resend acknowledged | RESOLVED | `redeliver` added to P2; the early return at mesh_actor.cpp `send` is now cited correctly.                     |
| F4  | Failing statement undoes the turn | RESOLVED | Per-statement `#Choice` (9.1, 9.2). The failure alternative is context dependent and stated.                   |
| F5  | Absolute scratch limit            | RESOLVED | Mark first, limits relative to `m0` (4.2 steps 1-2). Field names are stale (N7).                               |
| F6  | `spawns_`/`monitors_` grow        | RESOLVED | P7 erases both. Erasing during `pumpOne` is a hazard the register does not state (N2).                         |
| F7  | `Initialize` gating               | RESOLVED | Parameters ride in the capability, which `ActorContinuation::spawnRequest` keeps. Cap 16 KiB is unstated (N8). |
| F8  | `std:goal`/`std:math` exports     | RESOLVED | Prerequisites listed; `Point` moved to `mars:hardware`.                                                        |

## 2. New and remaining findings

### N1. MAJOR (5.5, 7.3, P3): the runtime cannot read the floor, and P3 hides a wire or …

Full title: N1. MAJOR (5.5, 7.3, P3): the runtime cannot read the floor, and P3 hides a wire or
id-format dependency

- 5.5 puts the floor inside the bencoded `value` of the `ActorSend`. The runtime treats `value` as
  opaque; `enqueue` and `receive` (mesh_actor.cpp) never decode it. `ActorMessage` has no spare
  field: `sequence` must be zero for `ActorSend` (`isValid`, identity/actor_wire.cpp `noBulk`), and
  `infoHash`/`programRef`/`hashcashStamp`/`capability` must be empty for it.
- P3 says only "forget remembered ids below the sender's floor". To do that the runtime must (a)
  parse vagent's value dictionary, a layering inversion, or (b) gain a floor field in `ActorMessage`
  and its encode/decode/`isValid`/`deliveryContentId`, which is a wire-format change to `identity/`
  that is not in the register, or (c) call an adapter-supplied hook. It must also split a
  `requestId` into sender hash and turn to decide "below", so the id format of 5.5 becomes runtime
  knowledge.
- Fix: choose (c) and say so. P3 becomes "an optional
  `std::function<std::optional<Floor>(const ActorMessage&)>` hook in `ActorOptions`; the runtime
  erases remembered entries for that sender whose ids compare lower, and refuses a lower fresh id
  with a typed error". The hook, not the runtime, knows the id layout. If (b), list the identity
  change as its own proposal and bump the wire version.

### N2. MAJOR (11.2 P3, P7): erasure during `pumpOne` is a crash, and the register does …

Full title: N2. MAJOR (11.2 P3, P7): erasure during `pumpOne` is a crash, and the register does not
constrain it

- `pumpOne` iterates `for (auto &[key, actor] : actors_)` and, after the executor returns, does
  `actor->remembered.at(message.requestId).response = response` outside the `try` (mesh_actor.cpp,
  `pumpOne`). The adapter drains inside the executor (4.2 step 9), calling `send`, `spawn`,
  `monitor` and, with P7, `stop`, all re-entrantly on the same runtime.
- If a same-turn `receive` (loopback) carries a floor that trims the running message's own
  `remembered` entry, `.at` throws `std::out_of_range` through `pumpOne`. If P7 erases the running
  actor (a `terminate` turn stopping itself) the range-for holds a dangling reference.
- Fix: add to P3 and P7 "never erases an entry of a message that is executing, or an actor that is
  `running`; erasure is deferred to the end of `pumpOne`". State in 4.2 that step 9 runs inside the
  executor and is re-entrant on the runtime, which the runtime's header comment ("owner-thread
  runtime") permits but does not promise.

### N3. MAJOR (11.2 P1 "Without it"): the fallback is wrong; static depths cannot work

- The fallback says "static depths only: methods may not be called from speculative positions". But
  `cut(cutBarrier)` takes an absolute index into `choiceStack_` (vortex_vm.cpp `cut`,
  `case OpcodeKind::Cut`), and `#Choice` outputs the constant `true`, not its depth (the `Choice`
  case). A method's entry depth is the caller's depth plus whatever its call chain pushed, so it is
  static for no method that is called from two sites, or recursively, even from non-speculative
  positions. The compiler could only recover it by cloning every method per call chain.
- So without P1, V13 (bounded statements) cannot be implemented for methods at all, and the
  dispatcher of 9.2 (which also uses `#ChoiceDepth`) falls with it. The register must say that P1 is
  load-bearing, not a convenience.
- Also not considered: `std:goal` already ships `if`, `while`, `every` and `limit` as host routines
  that bound evaluation with "a second cursor and `cut()`" (vicon/compiler.cpp:12-22 comment;
  vortex_stdlib.cpp `hostRoutine("if"...)`, `"while"`). The register should record why a
  per-statement host routine of that kind (existing code, no new opcode) is refused: presumably the
  trail undoing the body's writes (13, "every e1 do e2"), but then say so.
- P1's touch list is incomplete: `mesh_evaluator.cpp` has a closed allowlist `pureOpcode()` (the
  `case OpcodeKind::Cut: return true;` block) and an artifact decoder that rejects any opcode
  `> OpcodeKind::Cut` (the "Unknown Vortex artifact opcode" check). `Cut` is the last enumerator
  (vortex_vm.hpp), so a `ChoiceDepth` after it is refused in every sealed program, and the
  `convertsInteger` list wants an entry. Add `mesh_evaluator.cpp` to P1's "Where", and note that a
  new enumerator shifts or extends the artifact opcode numbering.

### N4. MAJOR (11.1, 11.2 P9, 10): `programRef` and `std:agent` ignore the existing …

Full title: N4. MAJOR (11.1, 11.2 P9, 10): `programRef` and `std:agent` ignore the existing
sealed-program format

- `mesh_evaluator.hpp` already has `sealVortexProgram` (canonical, content-hashed bencode of a
  closed pure instruction graph) and the loader in `mesh_evaluator.cpp`, with the same "opcode
  allowlist, no native callback" stance 11.1 describes. 11.1 invents `program.cpp` as new code and
  never mentions it. Either extend it (an existing-code change that needs a proposal) or say why a
  second artifact format is cheaper.
- That format has `SyntaxLimits{bytes 1 MiB, tokens 512, nesting 64}`, and `validateProgram` refuses
  a program with more than `tokens` cells or instructions. A vagent program plus `std:agent` is far
  larger. 11.1 must say which `SyntaxLimits` the adapter passes (a host policy per its header
  comment), or the first real agent is refused.
- References in the artifact are "artifact-local ordinals" and no stdlib or native callback can be
  named, yet 10 says `std:agent` reuses `std:logic`, `std:goal`, `std:collections`, `std:string`,
  whose functions are native opcode graphs minted by `VortexStdLib::build*Module`. How does a sealed
  agent program reference them? Say so (for example: the compiler links the Vortex-source parts into
  the artifact and only the `#`-opcodes are allowlisted).
- P9 is wrong in its "Without it" cell. Every std package is a C++ `build*Module` in
  vortex_stdlib.cpp (`bootstrap`, 1,455-1480), not Vortex source; "written in Vortex" for
  `std:agent` therefore means either hand-minting a BDI engine in C++ inside a 5,000-line file,
  which AGENTS.md requires justifying, or compiling a vagent/vicon-source prelude into the artifact,
  which needs no P9 and is the fallback. State which, and make P9's "Without it" the prelude route
  with its cost (the 512-token ceiling above, one copy of the prelude per artifact).

### N5. MAJOR (11.2 P4): the refused alternative is the wrong one

- 13 refuses self-sent messages for wake-ups, correctly (the reply collides with the actor's own
  `remembered`). P4's "why" repeats only that. The unrefused alternative is a wake message from a
  distinct host source pid: the adapter owns a host pid (it mints one for the launcher, 11.1), sends
  `ActorSend` from it with a fresh id, the reply goes to the host pid, and nothing collides. That
  needs no change to existing code; it costs one mailbox slot and one `remembered` entry per wake,
  which P3's floor trims.
- A register entry that claims "no other way" must price this. If P4 is kept, the reason has to be
  that a wake would occupy a mailbox slot and `maxMailboxMessages`, or that host deliveries must not
  be refused `MailboxFull` (7.5 says "take no mailbox slot"), and the "Without it" cell should read
  "wakes cost a mailbox slot and a remembered entry each", not "no intention runs".

### N6. MINOR (4.2 end, 7.3): the quiescence rule matches the code; one added condition

The list (empty mailbox, no incomplete sent message, `choiceDepth() == 0`,
`outstandingMarks() == 0`, not `running`, not `stopped`) is exactly `checkpointView`. `stopped` is
not in the doc's list, so a stopped agent reports `NotQuiescent` and the adapter must not retry. Add
it. A remote spawn's delivery has the spawn's `source` pid, not the agent's, so it blocks the
spawner (the launcher), which the 4.2 text should say.

### N7. MINOR (4.2 steps 1-2): stale API names

`Mark` has `cellCount` and `scratchSize` (zigzag/arena_manifold.hpp `struct Mark`), not `m0.cells`.
`setExecutionLimits` takes `(uint32_t cells, size_t work, size_t scratchBytes)`. Say "work" is the
allocation work counter, which `consumeWork` draws (arena_manifold.cpp, `intern` and one more call);
VM steps do not, as the doc says.

### N8. MINOR (4.1): parameters in the capability have a hidden ceiling

`capability.size() > 16384` is refused (`isValid`), and the signature and child name share that
space. A holon whose child takes a large list cannot be spawned. State the limit and the fallback
(the parent sends an `achieve` or `tell` after the spawn, with its fresh id) so users have no
workaround to invent.

### N9. MINOR (5.5): reused ids after restore from an older checkpoint

Checkpoints are taken only with no incomplete send, but a node that resumes from an *older*
checkpoint replays turn counters and produces the same id with different content, which receivers
refuse as `DuplicateMismatch` (enqueue). Either seed the turn counter from the wall clock at resume
or state that restore from a stale checkpoint is outside the model.

### N10. MINOR (4.2): hard-failure exception safety

A cell-limit exception thrown inside `step` is caught and `release(m0)` truncates, which is sound
for the arena. `VortexVM` also holds `currentDeweyPath_` and `cursorPC_`; `backtrack` resets the
former from the choice point, but `clearChoicePoints()` does not (vortex_vm.cpp `cut` only fires the
cut callback). Say the adapter resets the cursor opcode (it does, "reset the cursor's opcode") and
that the Dewey path is unused by vagent, or reset it.

## 3. Register audit (section 11.2)

- Each P1-P9 has "why", "without it" and "used by". Dependencies missing from the register: (1) the
  floor hook or wire field (N1); (2) `mesh_evaluator.cpp` allowlist and opcode range (N3); (3)
  `SyntaxLimits` policy or an extension of `sealVortexProgram` (N4). Dependencies relied on without
  a stated fallback: P2, P3 and P6 name consequences, which is adequate; P1 and P9 are wrong (N3,
  N4); P4 omits the alternative (N5).
- Sections that rely on a proposal and cite it: 1, 4.2 (P4), 4.4 (P5), 4.5 (P6, P7), 7.3 (P2, P3),
  9.1 (P1, P7). 9.1 row "capacity call" and "effect call" need no existing-code change, correct.
  `terminate` cites P7 correctly. The header says "nine changes"; the table has nine.
- The 1 sentence "one opcode and no other change to the VM, no change to ArenaManifold" is honest
  for the VM and arena (`Mark` carries the fields, `pushChoicePoint`/`backtrack`/`clearChoicePoints`
  are public, vortex_vm.hpp:125-129), but not for `mesh_evaluator.cpp` (N3), which is Vortex code.

## 4. Stale citations

`vicon/compiler.cpp:12-22` and `identity/identity_layout.hpp:304` are correct. `result_slice.cpp:34`
is `apps/common/xanadu/result_slice.cpp`; say the directory. Function-name citations elsewhere are
accurate (`WrongEpoch`, `pumpOne`, `enqueue`, `checkpointView`, `Mark`).

## 5. Verdict reasoning

The round-4 blockers are fixed and the turn, statement and delivery mechanics now match the VM and
the runtime. What remains is in the register the owner asked for: P1's fallback is false and its
touch list incomplete (N3), P9 and `programRef` ignore the existing sealed-program format and the
fact that std packages are C++ (N4), P3 hides a wire dependency (N1), and the re-entrancy hazard of
P3 and P7 is unstated (N2). Each is a text fix, not a redesign, but an implementer approving P1 or
P9 on the present wording would be misled.

VERDICT: NOT CONVERGED
