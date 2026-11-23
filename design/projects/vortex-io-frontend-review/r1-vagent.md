# Round 1 critique: VAgent (vortex-io-redesign.md)

Paths below are relative to the repository root. Target verified before the sandbox interruption:
v0.9, SHA-256 `a6325b19e99bdec6f7f6cb6724ed7a6b5ddecf1e7152e345194ae6872eb58535`.

Tally: 0 BLOCKING, 1 MAJOR, 0 MINOR.

## Scope and current VAgent behavior

VAgent has two effect contexts that matter here. Plan effect calls stage an outbox entry and suspend
the intention; handler effects stage without waiting and later report failure as `EffectFailed`
(`design/vagent-agent-language.md:187-199,860-885`). The executor creates an intent id, stores its
correlation and deadline, then parks plan intentions
(`apps/common/xanadu/vagent/executor.cpp:1264-1328`). Effect placement is checked by the VAgent
frontend; effects are limited to plan suspension points and bounded handler positions, and excluded
from conditions, methods, and answers (`design/vagent-agent-language.md:1123-1135`; checker contexts
at `apps/common/xanadu/vagent/checker.cpp:2709-2731`).

Current adapter behavior is only a migration seam: it drains after the executor returns, marks an
effect dispatched, invokes the native binding inline with mutable `WorkerContext`, then delivers a
cell result or `EffectFailed` (`apps/common/xanadu/vagent/adapter.cpp:406-408,662-710`). That is not
a safe async worker boundary or proof of external exactly-once completion. Existing outbox payloads
point to arena cells (`apps/common/xanadu/vagent/executor.cpp:362-369`), so the new design must
enforce the frozen-byte semantics it states before a successful commit; no worker may retain a raw
`CellRef` or actor context.

The current `vagent run` host is a single-node `MeshActorRuntime` (`apps/vagent/main.cpp:155-160`),
rather than a separate direct interpreter. This does not block a future local-runner profile: the
draft distinguishes local explicit `perform` from atomic actor callbacks and assigns frontend
continuation state to the frontend/adapter. Actor turns remain atomic and reject direct suspending
`Submit`/`Await`; a local runner can pump and resume a VAgent plan continuation if that profile is
explicitly selected.

## BLOCKING

None.

## MAJOR

### 1. (§6.5, lines 502-507, 556-562, 613-615) Message operations lack a normative boundary

**Problem:** VAgent native effects can stage endpoint operations, but the shared protocol describes
byte reads/writes and adapter-supported frames without defining a message-oriented operation. The
draft says an empty payload is distinct from EOF and asks an acceptance trace to cover “zero-length
datagrams versus stream EOF,” but does not require one successful receive to return one complete
message, preserve message boundaries under buffering/coalescing, or make one send request denote one
message. An implementation could flatten two staged datagrams, or treat an empty datagram as stream
EOF, while following the current request and buffering text. The actor intent identity and later
outcome would then no longer identify the intended message effect.

**Evidence:** The generic sketch is `Read { offset, maxBytes }` and has no message operation
(`design/vortex-io-redesign.md:469-482`). Empty-success versus EOF is only a payload-length rule
(`:502-507`). The buffering section leaves frame policy “adapter-supported” and defines tokenized as
framing bytes for a caller (`:556-562`). Datagram semantics appear as an acceptance prompt, not as a
normative operation or typed unsupported-capability result (`:613-615`). VAgent's proposed actor row
stages writes and flushes and relies on each request identity/outcome (`:553`); actor intent
identity and durable admission prevent replay from minting another effect (`:272-287`).

**Fix:** Define an optional, cell-only message capability and request contract in the generic
service: one successful `ReceiveMessage` returns exactly one complete message and boundary/length
metadata, including a valid zero-length message; message endpoints do not synthesize stream EOF; one
`SendMessage` names one frozen message. Each request retains its own identity, progress, and typed
outcome through service buffering/coalescing and actor replay. An adapter without the requested
message guarantee returns typed unsupported-capability refusal before admission. Add an acceptance
trace with two independently identified sends and receives where the first received message is
zero-length and the next is nonempty. Keep VAgent policy at the adapter: actor code stages the
message intent, the turn commits, and only then the adapter performs durable admission. This does
not require every backend to support messages or require a batch opcode.

## MINOR

None.

## Cross-role dialogue

- **VLace:** VLace asked whether local buffer construction, `perform(flush)`, service flush, and
  VAgent staged effects were distinct. I agree with its answer: VAgent freezes intent byte/order
  semantics before commit; post-commit admission transfers a bounded immutable lease and records
  metadata before work. VLace's pure local buffer remains inert until `perform(flush buffer)` admits
  one fresh write request. `FlushAfter` is a separate ordered barrier over admitted writes;
  close/abort are separate lifecycle operations. The frozen target states these distinctions in the
  VAgent row and buffer policy (`design/vortex-io-redesign.md:553,564-580,601-606`) and in the host
  prompt trace (`:513-531`). No VAgent buffering finding remains.
- **Technical writer:** The writer asked whether `Flush{after:lastWrite}` is a later barrier intent
  or buffer construction and whether it preserves identity. It is the former: the example separates
  staged Write and Flush intents (`:553`), and the normative rule says flush is an asynchronous
  barrier ordered after earlier writes, while admitted writes already carry their own request
  identities (`:573-580`). The local/VLace/VAgent/service buffer layers are distinguished at
  `:564-571,601-606`. I see no VAgent defect there. The writer's separate VPL `⊃` finding is outside
  this VAgent review.
- **Wildcard:** Wildcard challenged the missing zero-length datagram/message boundary rule. I
  verified the target text above and agree: the existing empty-vs-EOF rule and acceptance prompt are
  not a one-message-per-request contract. This exchange confirms the MAJOR finding. It is generic
  transport semantics; a VAgent adapter may refuse the message capability if unsupported, while
  VAgent syntax and staged intent policy remain frontend-owned.

## Cross-check against the remaining VAgent requirements

No finding remains on actor staging versus direct submit, actor restore refusal, or durable
admission. The target states that intents do not start work before commit; frozen semantics are
fixed before commit; bounded leases and durable metadata admission follow commit; proof is checked
before restore/replay; uncertain admission returns `Unknown`; and completions arrive in a later
owner turn without a `CellRef` (`design/vortex-io-redesign.md:264-287`). Its actor and local-runner
policies remain host-profile choices, while the generic VM/core does not inspect frontend names or
continuations (`:343-356`).

The VAgent example separates staged Write/Flush, and common buffering rules define identity, order,
partial/unknown progress, graceful close, abort, and fresh submission
(`design/vortex-io-redesign.md:553,573-580`). Tokenization stays outside core; a VAgent tokenizer is
a pure frontend operation over bounded bytes and actor completion is delivered in a later turn
(`:282-287,556-599`). Typed outcomes retain status and partial progress rather than becoming logical
failure (`:256-260,469-507`). These requirements match VAgent's purity and suspension positions
(`design/vagent-agent-language.md:872-885,1123-1135`).

VERDICT: NOT CONVERGED
