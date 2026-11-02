# Round 2 critique: Vortex & Spool-Mesh Expert (vagent-native-agent.md v0.2)

Paths below are relative to the repo root. Doc line numbers are `design/vagent-native-agent.md`;
code paths are under `apps/common/xanadu/`. Tally: 1 BLOCKING, 4 MAJOR, 5 MINOR.

## Status of previous BLOCKING/MAJOR findings

- R1.1 (no seam to splice fragments): RESOLVED. N4 now builds an `ast.hpp` tree and lets the
  existing compiler lower it. The new problem this exposes is B1 below.
- R1.2 (call/return/frame ABI): RESOLVED. Moot; no fragments.
- R1.3 (opcode allowlist proves nothing): RESOLVED. Moot; the claim is now the checker's and
  compiler's, and §15 records the refusal.
- R1.4 (replay once per ref): RESOLVED. N1 and §16 price compilation per agent and per resume, as
  `instantiate` does (`program.cpp:148-160`).
- R1.5 (C6 reachability walk): RESOLVED. Withdrawn; C6 is the compiler's.
- R1.6 (Queue serialises all agents): RESOLVED. N11 binds `Shared`, with contention on the host's
  library thread; P20.1 adds the agent to the request.
- R1.7 (incarnation is a counter): PARTLY. The durable epoch is used (`mesh_epoch.hpp:17-24`; the
  node name `author:device:epoch` is `NodeId::str()`, `spool_mesh.cpp:19-21`, and a pid carries it,
  `mesh_actor.cpp:198`). But §12.3 and §10.3/§12.4 now say opposite things about what a checkpoint
  does to grants (M1).
- R1.8 (no lifecycle signal): PARTLY. P20.6 adds `stopped` and `checkpointed`. `checkpointed` cannot
  mean what §12.4 needs (M1), and two transitions are still unsignalled (M2, M3).
- R1.9 (multiprocess by analogy): RESOLVED. §1 item 5 says plainly that Vortex offers distribution,
  not isolation; the peer alternative is refused with a reason (§15) and put to the owner (NQ7).
- R1.10 (`programRef` construction, module digest): RESOLVED. Length-prefixed parts (N6) and module
  staged from the hashed bytes (§7.3).
- R1.11 (ABI unspecified): RESOLVED. Generic node and slot setters over an X-macro table (§6.1-6.2).
  One gap remains (minor 1).

## BLOCKING

### 1. (N1, N4, C3, §10.1, §11.2, §13.1) The actor compiler cannot lower effects or native skills, and no proposal builds that

**Problem:** The whole design rests on "a native program is a `CompiledProgram`" whose synthesized
`capacity … effect def … within`, `native skill … implements …` and effect calls are "checked and
compiled exactly as source" (N1, C3, §10.1, §13.1). The checker accepts all of that. The compiler
rejects every one of those constructs today, and it is the compiler, not the checker, that would
have to produce the outbox entries the executor's drain consumes. The register (P20.1-P20.8) has no
entry for this, and §3's C3 row says "Checker", which hides it.

**Evidence:**

- `vagent/runtime/compiler.cpp:6761-6763`:
  `if (sd.native) unsupported("native skill " + name + " (its methods are effects)", sd.loc)`. Any
  agent that installs a native skill fails `compileAgent`.
  `tests/xuzz/vagent_skill_test.cpp:370-384` (`ANativeSkillIsRefusedByName`) asserts the refusal for
  the exact shape of §10.1 (`capacity` with an `effect def … within`, `native skill … implements`,
  `agent { uses …; install … }`).
- `compiler.cpp:6822`: `capacityCall` does `if (sig->isEffect) unsupported("effect methods", loc)`,
  so `open_terminal(24, 80)` in the §13.1 plan cannot compile. Same at `:6102` for effect methods in
  member sets.
- `Gen::effectCall` (`compiler.cpp:1097`, `:3827`) exists but has no caller (the only two matches
  are its declaration and definition). It is also incomplete against the drain: it writes
  `kOutboxEffectSkill`, `Method`, `Id`, `Args` and `Intention`, but not `kOutboxEffectCapacity` or
  `kOutboxEffectDeadline`. `agent_executor.cpp:2386-2392` counts an entry without a capacity or
  deadline as malformed and skips it after the turn has committed. So even wiring up the stub would
  produce effects that silently never start.
- `tests/xuzz/vagent_effects_test.cpp:1-5`: "driven with hand-built outbox entries and intention
  cells: the compiler side comes later." `vagent-implementation.md:49` lists milestone 6 "Effects
  and composition" (native asynchronous effects) as Open.
- `agent_executor.cpp:4919-4922` states the compiler "cannot lower/resume BDI plans yet" in the
  upgrade path. §13.1 is a plan with a suspension point.

**Fix:** Say so, and price it. Either (a) add a numbered proposal, P20.x, "effect and native-skill
lowering in the actor compiler": `prepareExtensions` accepts native skills as effect-only containers
with no members, `capacityCall` routes an effect to the installed native skill and calls
`effectCall`, `effectCall` writes capacity and deadline (from `sig.withinMs`), result decode for
record results, and the step-entry and resume protocol for the suspension. Note it overlaps
milestone 6 of the vagent plan and must be sequenced with it. Or (b) state that the design is
blocked on that milestone and that §13 cannot run until it lands, and drop "works unchanged" from
N1. Replace the C3 row's "Checker" by "Checker and the P20.x lowering". Add a gate that the first
test's graph stages an outbox entry the drain accepts (capacity and deadline present). The current
`ANativeSkillIsRefusedByName` test must be changed in the same proposal.

## MAJOR

### 2. (§12.3 vs §10.3, §12.4, P20.6) A checkpoint is a snapshot of a live agent, yet the doc both keeps and revokes its grants

**Problem:** §12.3 says the host "may stay alive across a checkpoint, and the running agent keeps
its grants". §10.3 (`agent_ended`: "checkpointed for migration") and §12.4 (`checkpointed(pid)`:
"revokes the agent's grants, shuts down its host") say the opposite. Both cannot hold, and the
second cannot be implemented as described.

**Evidence:** `AgentExecutor::checkpoint` (`agent_executor.cpp:4770-4827`) leaves the source agent
running: it changes no state and returns an `AgentCheckpoint`. Nothing says whether the caller means
to migrate or only to back up, so a `checkpointed(pid)` observer would revoke the terminal of an
agent that goes on running. A `stopped(pid, cause)` follows only if the caller stops it afterwards.
Resume mints a fresh pid and requires `epoch_ > continuation->pid.epoch` on the same node
(`mesh_actor.cpp:241-243`, `:322-324`), so the copy's tokens are stale by the pid binding alone; no
revocation is needed for that.

**Fix:** Drop revocation on `checkpointed`. State that a checkpoint changes nothing for the live
agent, the copy's stale tokens come from the pid binding, and grants end at `stopped` (including
`terminate` after a migration). Remove "checkpointed for migration" from `agent_ended` and
`checkpointed(pid)` from P20.6, or keep it as information only with no supervisor action.

### 3. (§12.1, §12.5, §7.3) Nothing handles the control plane dying: hosts, ptys and staging directories are orphaned

**Problem:** Every lifecycle row assumes a live supervisor. If the node process dies, each
`vagent-native-host` is left running with a blocked library thread, the pty slave, and a staging
directory under `$XDG_RUNTIME_DIR/vagent-native/<node-serial>/` that "the control plane removes when
the host exits" (§7.3). The restart then reserves a new epoch (the only way a same-node resume
works, see item 2) and never looks at the old directory.

**Evidence:** §12.1 covers a host's death only; §8.1-8.3 set no `PR_SET_PDEATHSIG` and give the host
no rule for end-of-file on fd 3. A call blocked inside ncurses cannot see the socket close.
`reserveNodeId` journals epochs durably (`mesh_epoch.hpp:17-24`) but the staging path is keyed by a
"node-serial" nobody defines.

**Fix:** Specify in §8.1: the host's I/O thread treats EOF on fd 3 or a failed doorbell read as
`_exit`, and the spawn sets `PR_SET_PDEATHSIG(SIGKILL)` (state that it is per spawning thread and so
the supervisor must spawn from one long-lived thread). Key the staging directory by the epoch, and
sweep directories of earlier epochs when the supervisor starts. Add a gate: SIGKILL the test's
control plane process and assert the host is gone and the directory is swept after restart.

### 4. (N1, §12.3, §10.2) Live upgrade is claimed to work unchanged, but a new `programRef` changes the module, the manifest and the approvals under a running host

**Problem:** N1 lists "live upgrade (`AgentExecutor::upgrade` takes a `CompiledProgram`)" among what
works unchanged. For a native program a new ref can carry a different module digest, binding
manifest and authority set. The supervisor keys the host and grants by agent, and approvals by
`programRef` (§10.2), so after an upgrade the agent runs new sequences against a host that loaded
the old module, and holds grants approved for the old ref.

**Evidence:** `upgrade` checks quiescence and installs the new program
(`agent_executor.cpp:4893-4923`), and requires `old.flights.empty()`, but it reports nothing to the
supervisor. P20.6 signals `stopped` and `checkpointed` only.

**Fix:** Add `upgraded(pid, oldRef, newRef)` to P20.6 and specify: the supervisor compares module
digest and manifest. If equal, keep the host and grants (only the agent's program changed). If not,
revoke grants (`program_changed` in §10.3), restart the host from the new module, and require the
approvals for `newRef`. Or refuse the upgrade between refs with different modules in v1 and put it
in §17.

### 5. (§11.6, §11.4) A handler cannot read the result of the sequence the design has it run

**Problem:** §11.6 says "The agent's handler runs a sequence that reads (`getch`), and re-arms".
Effect results are delivered to an intention. A call made from a handler has none, so its value is
dropped.

**Evidence:** `agent_executor.cpp` `finishEffect`: "A handler made the call: nobody waits, so only a
failure is news" (`if (!flight.intention)`). The compiler's handler path stages the entry "without
an intention or waiting record and returns immediately" with a unit result
(`compiler.cpp:3870-3874`). The checker permits a handler effect only in a bounded position
(`checker.cpp:1706-1712`), and does not reject a non-unit one, so the §11.6 handler would
type-check, run, and lose every keystroke.

**Fix:** Rewrite §11.6: the `TerminalReadable` handler adopts a goal, and a plan of that goal calls
the reading sequence, uses its value, and calls `watch_terminal` again. Put that in the example of
§13.1 or as a second test (inject input into the master and assert the agent sees the key).

## MINOR

- **ABI table coverage (§6.2, N4).** "One kind per `ast.hpp` struct" omits `PolicyArg` and
  `ArbitrationClause` (`ast.hpp`, with `PredicateDecl`). Either list them or N4's "anything vagent
  source can describe" is false for arbitrated predicates (P19). The generated table should assert
  full coverage, not a hand list.
- **Unit equality test has no comparator (§13.2 step 2).** `ast.hpp` has no `operator==` and there
  is no printer or dump. The X-macro already enumerates every field, so generate the comparator from
  `node_kinds.def` and make it ignore `loc`. Verified in support of the design: the compiler uses
  `SourceLocation` only in error text (`compiler.cpp` `bail`/`unsupported`, `program.cpp:59-62`) and
  the checker only to sort diagnostics (`checker.cpp:343-344`), so a synthesized location (line = op
  ordinal) changes no graph. State that declaration order (user declarations, then synthesized) is
  part of what the test fixes.
- **Resume in the test (§13.2 step 8).** A same-node resume is refused `WrongEpoch` in the same
  epoch (`mesh_actor.cpp:241-243`), so the step needs a second `LocalHost` with a greater epoch. Say
  so, and say which supervisor and broker the second host uses.
- **Cancelled `open_terminal` (§10.1, §10.4).** The deadline is a human's decision time (60s). If it
  expires or the effect is cancelled after the pty exists, `cancel(token)` must destroy the pair and
  record a decision of `unavailable`/`timeout`; as written the ledger and broker table can hold a
  grant nobody can name.
- **`pid` in a frame vs `(node, serial)` (§9.2, §12.2).** A pid already contains node and epoch
  (`mesh_actor.cpp:198`). Say the incarnation `serial` distinguishes host restarts within one pid,
  so the `(node, serial)` pair is not duplicated information.

VERDICT: NOT CONVERGED
