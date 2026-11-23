# Round 1 critique: VQL (vortex-io-redesign.md)

Target verified: v0.9, SHA-256 `a6325b19e99bdec6f7f6cb6724ed7a6b5ddecf1e7152e345194ae6872eb58535`.

Tally: 0 BLOCKING, 0 MAJOR, 0 MINOR.

## Current VQL behavior checked

VQL currently compiles queries to Vortex and returns a synchronous
`expected<vector<CellRef>, QueryExecutionError>` (`apps/common/xanadu/vql/execution.hpp:38-50`). Its
direct engine evaluates a `for` input path once, then recurses through bindings per row;
`executeEffectClause` explicitly iterates and evaluates nested effects for each input cell
(`apps/common/xanadu/vql/vql_engine.cpp:1691-1732, 1778-1804`). Boolean conjunction/disjunction
short-circuit (`vql_engine.cpp:1668-1681`). Therefore an I/O-bearing generator, predicate, or
per-row expression would have data-dependent submission count unless the compiler rejects it or
makes its multiplicity explicit. The proposed contract addresses that at the correct layer: shared
rules prohibit implicit submit from traversal/rendering, and VQL's compiler owns submission-site and
traversal multiplicity (`design/vortex-io-redesign.md:509-511, 546-549`). The exact new grammar is
explicitly left open (`:592-599`), which is appropriate for this protocol design.

## BLOCKING

None.

## MAJOR

None.

## MINOR

None.

## Cross-role dialogue status

- **VPL:** The shared draft says requests have independent identities and outcomes, completion may
  reorder, VPL preserves input shape/position, VQL preserves row association through retained
  request cells, and a generic request-cell collection avoids a batch opcode unless group
  selection/accounting is justified (`design/vortex-io-redesign.md:539-544`). VPL agrees this
  resolves the generic correlation/grouping concern. We also agree that query `LIMIT`/short-circuit
  can stop future admissions but cannot retroactively cancel or erase already-admitted requests or
  endpoint-owned prefetched suffix bytes. This confirms the draft's service-owned request and
  bounded read-ahead rules (`:573-606, 682-694`). No finding remains.
- **Wildcard:** The direct-Vortex dashboard counterexample does not require group identity for VQL
  rows. Wildcard agrees independent request refs and per-request cancellation/outcomes suffice; VQL
  owns row mapping, while a group is optional only for genuinely shared deadline/accounting/bulk
  selection. This matches `:539-544`; no finding remains.
- **Technical writer / needs-source correction:** The v0.9 examples clearly label syntax as
  proposed, distinguish frontend-local pure construction, admission, await, rendering, capture, and
  buffered transfer, and keep tokenization out of the core (`:509-511, 546-562, 564-606`). I correct
  one statement in my pre-review note: `vquery` reuses one `VQLEngine` across calls to `runQuery`
  (`apps/vquery/main.cpp:468, 471-491`); the `VortexVM` and `QueryExecutor` are stack-local to each
  call (`:487-491`). The draft requires the VM to retain parked execution state and makes frontend
  continuation state an adapter concern (`design/vortex-io-redesign.md:335-355, 621-633`). Thus an
  implementation must keep the parked run alive in the host when Pending is returned, but the
  protocol already states the needed ownership and resumption behavior. This source correction does
  not create a target finding.

## VQL coverage check

The draft's generic service policy covers VQL's reads and lenses without making VQL syntax a core
concern: all public values remain cells (`:377-390`); capability and request admission is distinct
from worker progress and await (`:495-507`); explicit `await` observes a retained request and
backtracking can rematerialize its outcome (`:621-644`); transient file/media ranges retain
observation identity under leases (`:743-771`); and resource ownership includes requests, payload
cells, continuations, and query/VM state (`:661-694`).

The VQL sketch is intentionally minimal and proposed (`:546-549`), but the normative contract is
sufficiently concrete for this round: submission cannot be implied by row traversal, projection, or
rendering; the compiler defines effect placement and multiplicity; request refs preserve row
association; admission order is separate from completion order; framing/read-ahead and raw/framed
cursor interactions are bounded and explicit; and outcomes preserve EOF, empty payload, partial
progress, cancellation, and unknown effects. These requirements leave VQL free to choose its syntax
and exact mapping in its later frontend bridge without leaving core semantics ambiguous.

VERDICT: CONVERGED
