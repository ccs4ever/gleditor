# Vortex I/O frontend critique brief

## Assignment and target snapshot

- Repository:
  `/home/ccs4ever/.codex/worktrees/eced7532-310c-4b04-8ad9-6e2ab242f574/gleditor-restore`
- Target: `design/vortex-io-redesign.md` frozen round-one snapshot v0.9, SHA-256
  `a6325b19e99bdec6f7f6cb6724ed7a6b5ddecf1e7152e345194ae6872eb58535`.
- R1 editor revision: v0.10, SHA-256
  `fcfa22892d147405f78b42a94ff6357727e4173bcdbdf5c19a1b881ee1d8a855`; the round-one reports stay
  anchored to the v0.9 target above.
- R2 input snapshot: v0.10, SHA-256
  `fcfa22892d147405f78b42a94ff6357727e4173bcdbdf5c19a1b881ee1d8a855`; see `r2-disposition.md`.
- Current editor revision: v0.11, SHA-256
  `7a4a0d94ea6e0c36f45bad608f0df1c943d8e63c6304d3fb984620ed660e0e07`. R3 is focused on VPL and the
  technical writer.
- Historical v0.8 SHA-256 before this review:
  `28e5aca38135e335774add9b6d72de888ef8820609ca7b643bb331b395aadfb3`.
- All six language needs briefs and writer/wildcard pre-review synthesis informed the examples. The
  v0.8 convergence in the older review directory is historical only. This stakeholder review starts
  fresh.
- Workflow source:
  `/data/git/gleditor-restore/.claude/worktrees/bridge-cse_01JyeD4AMjEywzundTw7BvJ7/.claude/skills/vortex-design-critique/SKILL.md`.
  Apply its evidence/severity/report discipline, adapted to the assignments below.
- Every critic uses `gpt-6-luna` at high effort, is read-only, and writes only the assigned report.
  The editor alone changes target, common brief, disposition, and README.
- At most three critics run concurrently. Each frontend has a new stakeholder reviewer; add a
  wildcard reviewer and technical writer. Reviewers proceed in rounds until each reports
  `VERDICT: CONVERGED` and the editor records zero unresolved BLOCKING/MAJOR findings. Reports are
  in dialogue: read the needs and current reports of other roles, reply to at least one substantive
  point in your own report, and note whether that exchange changed your finding.

## Exhaustive current frontend roster

| Stakeholder              | Current status and placement                                                                                                                                                        | CLI variants and source anchors                                                                                                            |
| ------------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------ |
| VQL                      | Implemented query language in `apps/common/xanadu/vql/`; embedded use in Xuzz/evidence UI and host, with runner/REPL and compiler.                                                  | `apps/vquery/main.cpp`, `apps/vqueryc/main.cpp`; `apps/common/xanadu/vql/vql_engine.cpp`, `compiler.hpp`; Makefile VQUERY/VQUERYC targets. |
| VPL                      | Implemented array language in `apps/common/xanadu/vpl/`; runner/REPL and compiler.                                                                                                  | `apps/vpl/main.cpp`, `apps/vplc/main.cpp`; `apps/common/xanadu/vpl/compiler.hpp`; Makefile VPL/VPLC targets.                               |
| vprolog / Vlog           | Implemented logic language over Vortex/Vlog; standalone REPL/runner, compiler/parser in shared engine; Prolog resolver also used by engine hosts.                                   | `apps/vprolog/main.cpp`; `apps/common/xanadu/vprolog/compiler.cpp`, `parser.cpp`; Vortex host and mesh evaluator call sites.               |
| VIcon                    | Implemented goal-directed language; standalone runner/REPL and compiler CLI.                                                                                                        | `apps/vicon/main.cpp`, `apps/viconc/main.cpp`; `apps/common/xanadu/vicon/{driver,compiler}.cpp`; Makefile VICON/VICONC targets.            |
| VLace                    | Implemented functional/lazy language; standalone runner/REPL and compiler/type-checker CLI.                                                                                         | `apps/vlace/main.cpp`, `apps/vlacec/main.cpp`; `apps/common/xanadu/vlace/{driver,lower_io}.cpp`; Makefile VLACE/VLACEC targets.            |
| VAgent                   | Implemented typed agent language with checker and single-node runner; adapter handles staged post-commit effects.                                                                   | `apps/vagent/main.cpp`; `apps/common/xanadu/vagent/{frontend,adapter}.cpp`; Makefile VAGENT target.                                        |
| Wildcard / direct Vortex | Direct Vortex is the protocol baseline, not an extra language. Reviewer finds unrepresented transports, adapters, hosts, or future execution shapes beyond the six known languages. | `apps/common/xanadu/vortex/{vortex_core,vortex_vm,vortex_host}.cpp`; inspect all relevant service and source adapters.                     |
| Technical writer         | Cross-cutting reader of proposed contract, examples, terminology, acceptance mapping, and clarity; not a seventh language.                                                          | Check exact source refs and ensure every example is marked proposed/pseudocode.                                                            |

Compiler/runner variants of one language are a single stakeholder. Xuzz embeddings belong to the
relevant language reviewer. The plain `gleditor` is not a Vortex frontend. `VWeave` has a design
document, `design/vweave-topological-language.md`; exhaustive source/Makefile inspection found no
compiler, runtime, embedded frontend, or executable, so it is proposal-only and is not a current
stakeholder. VWeave may inform wildcard/future-language cases, clearly labeled proposed.

## Owner constraints

- Every runtime and lens I/O path is asynchronous and off evaluation/UI threads.
- Public program protocol is cell-only; native handles, pointers, descriptors, and buffers stay
  private.
- Incoming bytes are transient until an explicit capture action.
- Retained memory and work are bounded with backpressure. No initial spill backend. Unpausable
  producers report gaps/overflow through reserved capacity.
- Explicit submission freezes/pins output and creates a fresh effect identity every time the submit
  site executes, including after backtracking. Await/resume observes the same admitted request and
  never resubmits.
- Preserve declarative lens transactions, VAgent staged intent/post-commit submission, Evidence
  grants/durable attempts/receipts/no-silent-retry, partial progress and uncertainty.
- Vortex core/VM stays language-neutral. New generic service/capability/request/outcome/suspension
  semantics are shared; frontend syntax, typing, purity/effect placement, multiplicity/order, and
  frontend-local continuation state stay in each compiler/adapter.
- There are no production users or production ABI to preserve. ABI may change freely for a cleaner
  design; no obsolete ABI shim. Existing on-disk/wire versioning and the 64-byte `CompactOpNode`
  invariants remain.
- Expose composable buffering/framing choices: unbuffered, line-delimited, fixed-size, and
  adapter-supported framed/tokenized byte delivery; distinguish service coalescing from framing,
  decoding, and frontend tokenization. Unbuffered never means synchronous or bypassing OS buffering.
- Distinguish pure frontend-local buffer construction (no admission) from explicit VLace
  `perform(flush)` and VAgent staged/commit admission. A service-managed buffered write already has
  request identity at admission; later native coalescing preserves each request’s identity/outcome.
  In all cases admission is distinct from bytes transferred. Flush is an explicit asynchronous
  ordering barrier, durable sync is a distinct backend guarantee, and graceful close differs from
  abort/cancel with unsent/partial/unknown bytes.
- Input read-ahead retains bounded unread suffixes; define raw/framed cursor mixing, seek/reset,
  delimiter/UTF-8/token splits, oversized frame policy, and lease ownership. Keep frontend
  lexer/grammar outside core. Test full-duplex pressure with per-direction progress/credits,
  terminal reserve, and sibling request cancellation. A generic collection of request cells suffices
  for fan-out unless common group ownership/deadline/accounting/cancel selection is proved
  necessary; no batch opcode or atomic batch admission is assumed.
- Make prompt-write → async flush fence → input-read ordering concrete. Distinguish admission,
  accepted/transferred bytes, flush boundary, durable sync, and peer acknowledgement; failure of the
  flush must stop a dependent input submit.
- Be honest about mutable-file consistency, source generations, large random-access media,
  decoder/cache leases, capture identity, and unsupported backend guarantees.

## Reviewer focus and reports

- `r1-vql.md`, `r1-vpl.md`, `r1-vprolog.md`, `r1-vicon.md`, `r1-vlace.md`, `r1-vagent.md`: test each
  language's concrete expression/control model, staging vs. awaiting vs. pure rendering,
  multiplicity, backtracking/resume, and cell-only error/outcome exposure.
- `r1-wildcard.md`: challenge coverage using direct Vortex and unmodeled/future cases (e.g.
  multiplexing, messages vs streams, mutable ranges, host imports); separate generic protocol
  requirements from optional adapter guarantees.
- `r1-writer.md`: audit completeness, consistency, terminology, source accuracy, and whether
  pseudocode makes admission/await/staging/render/capture and terminal outcomes unambiguous.

Cross-role dialogue resolutions: VQL/VPL and wildcard agree that request identity is independent
from completion order; VPL keeps array shape/input position and VQL keeps row association through
request refs. Start with a language-neutral owner collection/rank of request cells; add optional
generic group identity only if common deadline/accounting/bulk cancellation cannot be represented
there, with per-member outcomes and no atomic admission/retry implication. VProlog/VIcon agree that
waiter/continuation ownership is frontend-local; abandoning it unregisters the waiter but does not
silently cancel service-owned admitted work, while bounded unread suffix/read-ahead remains
endpoint-owned and is not backtracked. VLace/VAgent agree that bytes and ordering semantics are
frozen before actor commit can expose an intent; post-commit actor adapter admission transfers a
bounded immutable lease and durably records metadata before work, while VLace explicit `perform`
admits at its own boundary. A VLace explicit flush request and VAgent staged post-commit write
remain distinct policies over the same service. Resolve only shared protocol facts in the common
contract; retain frontend policy in its compiler/adapter. The buffering/framing concern is a
composable-policy question, not a mandate to implement every adapter in the first milestone.

Each report starts `# Round 1 critique: <Role> (vortex-io-redesign.md)`, cites exact
current-worktree `path:line`, quotes the proposal point, and gives a concrete correction. Use
BLOCKING/MAJOR/MINOR, severity tally, and a final convergence verdict. Report current implementation
facts separately from proposed syntax/APIs. Read peer needs and r1 reports before follow-up; include
a short dialogue exchange and disposition of the point addressed. The editor verifies every report
and writes `r<N>-disposition.md` for each round with accepted/deferred/refused, evidence, and target
edits. All reviewers continue until their own findings converge; user-requested redesign constraints
are not reopened as optional preferences.

## Dialogue questions already raised

- VQL and VPL agree that frontend array shape/order and per-operation identity/completion order are
  distinct concerns. Review whether a generic group/batch correlation is needed for ownership,
  cancellation, and aggregation, or whether each frontend should compose independent request cells.
  Do not introduce a batch opcode solely to encode VPL arrays; keep any shared grouping mechanism
  language-neutral and justify its retention/accounting cost. Wildcard and writer reviewers should
  test the examples and state their position; VQL/VPL should reply to those points in follow-up.

### Confirmed VLace/VAgent dialogue

VLace and VAgent agree that “freeze” has two stages: fix the semantic bytes and endpoint order
before an actor turn commits; after successful commit, transfer/retain a bounded immutable payload
lease and durably admit metadata before service work. The actor ledger does not capture transient
bytes. VLace stream submission freezes at its explicit `perform` site instead. An admitted VLace
flush remains service-owned across local continuation abandonment; `close` is a separate ordered
effect, graceful close follows a terminal flush/barrier, `abort` is explicit, and garbage collection
cannot block or claim close succeeded.
