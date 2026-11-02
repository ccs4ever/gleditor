# VQL beyond network transparency: a Xanadu/ZigZag purist review

Review of checkout `732b8280`; proposals, not implemented guarantees. The specification's banner
says 15.1, while §4.12 and its history introduce v16. Its opening “nothing wired into build” and the
lens document's “no lens implemented” are stale. This review separates these layers.

## Primary principles and present footing

Nelson defines ranks as sequential connections in a dimension, with at most one neighbor in either
direction; dimensions are not global coordinates. Clones present a headcell's content in independent
structural positions. Crucially, he distinguishes abstract structure from the conventions that make
it a useful environment. The repository's dimension names and operational policies therefore deserve
critique rather than treatment as axioms.
[Nelson's ZigZag reference definition](https://xanadu.com/zigzag/ZZdnld/zzRefDef/).

Nelson's ZX proposal separately addresses structure and media, including email fields, and requires
uniform traceability for prose, cell text, titles, and calculation results. His “marginal notes on
anything” is a stronger requirement than making top-level connector objects visible.
[Nelson, *ZX Internals*](https://www.xanadu.net/zigzag/fw99/zxInternals.html). Enfilades implement
reference-based rearrangement and version relationships; their tree internals should not dictate
user-visible object ontology. Calling every cell a “crum” (§4.11.1) conflates those levels.
[Project Xanadu, *Technologies*](https://xanadu.com/tech/).

Built foundations include VQL parser/compiler/interpreter, Structure operations, persistent Manifold
and scratch ArenaManifold, projection sessions, and filesystem/process/network lenses. See
`apps/common/xanadu/vql/{parser,compiler,vql_engine}.cpp`, `zigzag/projection_session.cpp:35`, and
`zigzag/{filesystem,proc,net}_lens.hpp`. But `ProjectionSession` currently exposes projection and
key lookup, not the designed transactional `commit()`/`promoteInto()`; `NodeContent` lacks span-run
content (`projection_lens.hpp:68`). `executeWatch()` is a finite traversal with an explicit catch-up
simulation (`vql_engine.cpp:396`), not the specified durable observer. Parser tests do not establish
implemented distributed semantics.

Already specified: shapes, functions, structural typing, point-free paths, version handles, dynamic
lenses, watches, perspectives, transclusion, action cells, external identity maps, and
partial-failure unprojection. The lens document §8A already proposes SQL NULL, BLOB references,
transactions, edge properties, rekeying, and schema handling. Merely adding “SQL lens” or “email
lens” would repeat this ambition. The extension needed is a common identity, evidence, effect, and
workflow calculus.

## Defects to settle first

1. **Shared bytes are not shared editable identity.** The lens table maps identical file contents to
   `d.clone`. Two independently authored files with equal bytes must not become one mutable thing.
   Reuse immutable spans for actual shared provenance; represent content equality separately.
   Reserve `d.clone` for deliberately shared content authority. The net-lens header already
   recognizes this distinction.

1. **Relationships need addresses.** Lens §8A calls relationship reification “phantom” nodes and
   instead adds hidden edge properties. An annotation on a foreign-key relation, recipient
   occurrence, reaction, or failed delivery needs a stable subject. Make relation occurrences
   ordinary cells; engine edges may remain the efficient topology underneath. Arbitrary many-to-many
   relations need such incidence structure under the two-neighbor rule anyway.

1. **A source key is not an eternal identity.** Inodes, process IDs, row IDs, URLs, account names,
   and message IDs can be reused, renamed, forged, or missing. The version-containing xanadoc key is
   an observation identity, not persistent paragraph identity. Rebinding a cross-device copy to an
   old cell silently decides a disputed continuity question. Preserve identity assertions and their
   evidence in hypertime; do not overwrite them.

1. **Views and destructive edits collide.** §4.8's shape projection severs omitted links, while
   §4.10 treats projection as a non-mutating lens. An “export shape” currently destroys private
   links in its example. A view handle must mask traversal without deleting topology; explicit
   effectful shape application must be different. Perspective fallback also needs an explicit
   suppressed/masked state: absent overlay links currently reveal the master again.

1. **Permanence claims overreach.** Immutable permascroll bytes cannot be modified as the
   transclusion-ripple paragraph claims (§4.9.6). Observe changes to source assembly or live
   selectors; frozen quotations remain frozen. `citing()` can discover known accessible evidence,
   not prove a complete universal backlink set. Private/unpublished links cannot simultaneously
   remain private and be globally discoverable. Repair the sentinel contradictions, clone-chain
   lowering, and unmeasured constant-time claims before extending their guarantees.

## Proposed common model

Give every durable referent a minted local identity, globally nameable using the repository's
operation-reference machinery. Treat source identities as scoped assertions
`(authority, collection, external key, incarnation evidence)`, not as the referent itself. Reconnect
by evidence; expose ambiguity rather than silently attaching old notes to a new object.

Build six ordinary, openly extensible cell structures:

- **Entity:** continuity claim for a file, row, actor, message, resource, or conversation.
- **Observation:** immutable witnessed state with source revision, retrieval time, adapter version,
  completeness, and source-authorship evidence. Observation order and claimed event time are
  separate.
- **Field and occurrence:** field identity belongs to an entity/schema; its observed values belong
  to observations. Membership, order position, recipient, query-row occurrence, and relation
  instances receive identities too.
- **Anchor:** object, field, exact source span, occurrence, event, execution, result, or failure
  target. An anchor records whether it denotes one observation or intentionally follows entity
  continuity. Annotations themselves are ordinary addressable structures, with authorship and
  annotations on their claims.
- **Intent, attempt, receipt:** an action describes requested effects; each execution attempt and
  returned receipt is a separate immutable subject. Unknown outcome is representable; cancellation
  is a request, not proof nothing happened.
- **Derivation:** program/query version, parameters, input observation set, mapping from result
  parts to evidence, execution environment, and output/failure references. Aggregates expose
  contributing members; transformed text records derivation rather than claiming exact transclusion.

These are conventions and structural contracts, not a closed class hierarchy. Use incidence cells
and occurrence facets to place one item in multiple ranks without a degree violation. A rank
enumerates memberships; a membership refers to its entity. Provide a common `children`/`members`
traversal that includes the first child—the current `contains` then `seq` pattern otherwise risks
omitting it because ordinary rank traversal starts after context (`filesystem_lens.cpp:164`, VQL
§4.1).

Persistence must be selective. Browsing can remain scratch. Annotating a virtual field, computed
value, or transient event promotes its identity/evidence closure into a user's evidence/activity
store before the scratch cells disappear. That does not mutate the visited source (convergence R8).
Persist meaningful keys and continuity assertions; lens §5.3's proposed promote-skip for `d.extkey`
cannot also promise stable reconciliation after restart.

External content ordinarily must be **captured once** into retained immutable primedia before exact
durable span annotation is promised. Record that the capturing user witnessed bytes from a source;
do not attribute their signature to the source's author. Subsequent reuse may transclude those
captured spans. A lazy HTTP/body/BLOB reference is useful for browsing, but cannot guarantee future
retrieval or historical character identity. Nelson explicitly presents transclusion as a knowable
relationship with access to original context, rather than one memory-copy mechanism.
[Nelson, *Transclusion*](https://xanadu.com/transclusion/).

## Full source families

**Databases:** expose schema objects, row entities, field values, constraints, FK relation
occurrences, transactions, query executions, duplicate result occurrences, ordering, and execution
failures. Stable primary keys need database incarnation and key-reuse handling; keyless rows have
observation-local identity unless a continuity witness exists. Writes become guarded intents with
expected revisions and source-local transactions; a constraint rejection is an annotatable receipt.
NULL, absent, unavailable, redacted, and empty remain distinct.

**REST and mutable resources:** expose requests, pagination/cursor windows, responses, status,
headers, decoded fields, raw retained bodies, ETags/revisions, and rate-limit/retry failures. A URL
is a locator. Separate resource continuity from each response and decoded view. Every field can be
traced to raw bytes where possible; decoding/normalization has an explicit mapping. Mutations use
conditional requests when supported. Partial listings report coverage rather than imply absence.

**Shell execution/history:** command text and parsed arguments are cells; invocations reference cwd,
environment declarations, interpreter/program identity, input artifacts, stdin spans, timestamped
stdout/stderr chunks, exit/signal, and observed output artifacts. Pipes connect typed result/input
occurrences in a dependency graph. Branches preserve alternative commands and explanations.
Replaying an invocation creates a new attempt; it never rewrites its old outputs or implies
identical external state.

**Email:** distinguish received byte artifact, interpreted message, mailbox membership, participant
identity claim, MIME part, quoted span, threading assertion, and sending/delivery attempt.
Message-ID is evidence, not sufficient identity. Notes can target one address occurrence, attachment
range, quote, delivery failure, or whole conversation without copying mail between app-owned silos.

**IRC:** capture server/session/channel identity, joins/parts/topic changes, message events, sender
identity evidence, optional protocol IDs, local sequence witnesses, and gaps. A nickname is not a
person. Replies and quoted spans refer to exact witnessed events. Missing history stays an explicit
gap.

**ActivityPub/fediverse:** distinguish actor/resource URI claims, activities, object observations,
revisions, replies, boosts, reactions, delivery receipts, tombstones, and collection memberships.
Duplicate deliveries may reference one witnessed activity while remaining separately annotatable
receipts. Edits append observations; deletion affects the live view while retained evidence and its
access policy remain explicit. Capture and authorship verification are different assertions.

## Non-linear work and strongest objections

A workspace is an ordinary slice of selected entities, observations, questions, action intents, and
result occurrences. Chronological history is one rank; causal dependencies, alternatives,
discussion, topic, and evidence are others. An email quote can support a SQL investigation, whose
result feeds a shell experiment, whose failure becomes a draft reply: one addressable fabric, with
adapters at authority boundaries. Add execution effects/capabilities, explicit observations and
captures, durable subscriptions with coverage/checkpoints, branching plans, and inspectable
provenance as standard functions/contracts before inventing source-specific syntax.

The strongest objection is cost: universal addressability multiplies cells, captures consume
storage, and useful provenance can reveal private context. The answer is on-demand materialization,
bounded promotion, selective retention, indexed summaries with inspectable evidence, and
source-specific access boundaries. None makes unlimited retention free. Another objection is that no
stable external identity or atomic cross-system transaction can be manufactured by topology.
Represent uncertainty, preconditions, partial success, and compensating actions; never advertise
global exactly-once execution. The final objection is cognitive: a universal graph can become less
usable than existing applications. Source-shaped presentations remain valuable; they must be views
over shared identities, not exclusive containers that own the user's notes.

## Final synthesis critique — baseline 7617d3f0

The synthesis incorporates the material Purist findings: durable subjects versus immutable
observations; addressable relationship occurrences; honest capture/transclusion distinction;
promotion into designated private stores under R8; and private intent/attempt/receipt histories
independent of ZZ9P. It properly leaves clone policy, braces, and strict-versus-filtering contracts
open. Earlier banner/type findings above describe historical baseline 732b8280: the v16 banner and
§4.12.5 now document direct-evaluator choices and compiler limitations.

Two material gaps remain:

1. **Field-following anchors need durable field identity.** §2.1's binding ends at
   `observation + field occurrence`, while §3.1 and the SQL journey offer notes about fields across
   refresh. Specify a field subject or equivalent identity assertion scoped to the entity and schema
   edition, with observation-local field values. Column rename, dropped/recreated columns, JSON key
   reuse, and MIME restructuring must yield evidence-backed correspondence or ambiguity. Otherwise a
   subject-following note on “amount” can silently move to a different field even though row
   identity is stable. This can use existing Subject/Occurrence conventions; no additional primitive
   is necessary.

1. **Secret filtering changes evidence.** §7 requires filtering secrets before retention, while
   §§2.3/5 expose raw captured bodies and exact evidence. Those promises need a joint rule: retain
   an exact permitted original under its access policy, or retain a separately identified
   redacted/decoded derivative with a disclosure map, withheld ranges, and explicit unavailable
   original. Never present filtered bytes as an exact raw capture or map quotations across
   redactions without a valid source-offset mapping. A hash alone proves neither recoverability nor
   remote authorship. This matters for email headers/bodies, REST payloads, and terminal output, not
   only credential-broker values.

The remaining specification repairs are already acknowledged. No further conceptual blocker found in
the proposed shared work model.
