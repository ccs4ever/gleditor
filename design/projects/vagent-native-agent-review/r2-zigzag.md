# Round 2 critique: Xanadu / ZigZag Purist (vagent-native-agent.md v0.2)

Tally: 0 BLOCKING, 2 MAJOR, 4 MINOR.

## Status of previous BLOCKING/MAJOR findings

- R1.1 (fragment fan-in): RESOLVED. Native ops now build a `Unit` tree (shape check in §6.3 step 2:
  every id used as exactly one child) and the compiler's own emission is unchanged. No link-based
  ABI is left.
- R1.2 (C6 shadowed-cell walk): RESOLVED. The walk is withdrawn and C6 is the compiler's.
- R1.3 (approvals as `programRef:kind` string): PARTLY. Approvals are now cells on `d.authority`,
  and the string is refused in §15. The topology has gaps, below (R2.1, R2.2).
- R1.4 (manifest outside cells): RESOLVED by the partial refusal. See below.
- R1.5 (no durable grant record): RESOLVED in intent (§10.5 ledger). Which permascroll holds its
  text is still open (R2.2).

## M4 refusal: accepted

The agent-visible half (capacities, records, events, authority kinds) is synthesized into the
`Unit`, so it becomes cells. The C half (sonames, C types, `fail_if`) is consumed only by the host's
libffi runner and is hashed into `programRef`. Promoting it to cells would only be read back out to
send to the host, and the doc prices that in §15. I have no new evidence. One residue: the authority
purposes and kinds appear in both halves (the manifest and the synthesized declarations); say once
that the declaration is authoritative and the manifest copy is hashed but never consulted.

## MAJOR

### R2.1 (§10.5) Decision-to-approval and decision-to-purpose relations are unspecified, and the obvious encoding is fan-in

The ledger says a decision records "program, kind ... purpose (transcluded from the approval's
purpose text)" but never says how a decision refers to the approval cell that justified it. The
natural encoding, a link from each decision to its approval or kind cell along some dimension, is
many-to-one. A kind cell then has one neighbour along that dimension per decision, and under the
2-degree rank invariant each new link evicts the previous one. The same holds for "program": one
`programRef` is shared by every decision about it. The purpose text is safe if it is a content span
quoted many times (spans are not links), but only if the doc says so.

**Fix:** State the encoding. Either (a) each decision cell carries the `programRef` and kind as
content spans (quoted from the approval's cells; no link, no fan-in), or (b) the approval is reached
through a `d.clone` view cell per decision. Say that a ledger query "all decisions for this program"
is a Spanfilade stab on the quoted span, not a rank walk. Add a gate test that records two decisions
against one approval and checks both decisions still resolve the approval.

### R2.2 (§10.5) No approval withdrawal, and the ledger's permascroll is unnamed

(a) Policy `approved` is granted "if the ledger holds an approval". The ledger is append-only and
has records only for approvals, decisions and revocations of *grants*. Nothing records an owner
withdrawing an approval, nor an approval's second kind added later (a new tail cell on the
`d.authority` rank is fine; an un-approve is not representable). The policy test therefore has no
defined answer for "approved, then withdrawn". Add a withdrawal record (a `d.revocation` link from
the approval's kind cell, or a withdrawal marker cell) and state that the policy check reads the
latest state along the rank.

(b) A store holds no primedia. The ledger's text (programRef, kind names, purpose text, agent names,
causes) must be spooled into some permascroll. The doc says "plus the purpose text spooled once per
program" but not where. If it is the node owner's `UserPermascroll`, the ledger text is authored and
attributed to the node owner, which is right for decisions, but the purpose text originates in the
module. Say which scroll, and say which `programRef` or purpose bytes the ledger must not let leak
into checkpoint identity. If the store is private and never published (as R8's activity store), say
that it is never adopted by another node, so the permascroll binding in `store.tables` is local.
Also state the `authorities/` store's load failure behaviour per the on-disk rules (typed refusal,
not an empty ledger that silently approves nothing or everything).

## Other checks in lane

- **Rank shape.** One approval cell heading a rank of kinds on `d.authority` is sound: many ranks on
  one dimension are fine, each a path. One global `d.decision` rank is sound if it is a
  chronological chain (append at the tail). `d.revocation` from a decision is a one-step rank per
  decision; if a decision can have more than one revocation event (`released` then `host_lost`), the
  doc must say it is a chain with at most one tail, or restrict to one revocation per decision. As
  written ("each revocation linked from its decision") the second would evict the first.
- **R7.** Revocation chain and cell identity rely on the `MakeCell` index being the ref; no sentinel
  appears. Fine.
- **R8.** A private deployment store no reader visits: respected. R12: dimensions by
  `ensureDimension`: respected.
- **Synthesized declarations and tokens (V11).** Consistent. The synthesized capacities are ordinary
  compiled cells, and the token is a Str value in the agent arena (an opaque name that goes stale
  across incarnations), with the live table process-local in C++. The authority cause strings and
  tokens never persist in the ledger, as §10.5 says.

## MINOR

- §10.5: "about a dozen operations per decision" is not derived. List them (cell, scalar time cell,
  links, values) so the price can be checked.
- Duplicate approval of one `programRef` mints a second head cell; say whether approval is
  idempotent (find the existing head) or an append.
- `vagent native approve` should name the author whose key signs the ledger operations.
- `d.authority`, `d.decision` and `d.revocation` need schema and notes entries if the ledger store
  is ever shown in xuzz; say they are not system xanadoc dimensions.
- The ledger retention default (NQ5, "keep everything") is fine, but say the store's own segment
  growth is the bound.

VERDICT: NOT CONVERGED (0 BLOCKING, 2 MAJOR)
