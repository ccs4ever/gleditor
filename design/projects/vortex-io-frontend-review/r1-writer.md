# Round 1 critique: Technical Writer (vortex-io-redesign.md)

Paths below are relative to the repo root. Target v0.9 SHA-256:
`a6325b19e99bdec6f7f6cb6724ed7a6b5ddecf1e7152e345194ae6872eb58535`. Tally: 0 BLOCKING, 2 MAJOR, 1
MINOR.

## BLOCKING

None.

## MAJOR

### 1. (§6.5:549) VPL `⊃` conflicts with its existing meaning

**Problem:** The proposed VPL row uses `R ← ⊃ submit¨ Caps ReadSpecs`, then says results retain
input positions and shape. The example reuses `⊃`, which in VPL means disclose/resolve a clone to
its master, not preserve a mapped array. The row is labeled a proposal, but gives no proposed
redefinition of this established glyph; a reader can reasonably implement a scalar/master result and
lose the per-position request mapping the prose requires.

**Evidence:** The draft's VPL row and shape promise are at `design/vortex-io-redesign.md:539-550`.
The language defines `⊃` as “disclose / first” and “resolve a clone to its master” at
`design/vpl-array-language.md:93-94`; its clone-master semantics are explained at `:278-289`. The
current lowering returns the input when it is not enclosed and reads through the enclosure link
otherwise (`apps/common/xanadu/vpl/lower_enclose.cpp:75-104`).

**Fix:** Remove the outer `⊃` and show `Reqs ← submit¨ Caps ReadSpecs` followed by
`Out ← await¨ Reqs`. State that lowering preserves each input position and shape (including empty
and ragged views), independent of completion order; define or defer operand shape-mismatch rules.
Keep any new operator spelling explicitly proposed and avoid borrowing an existing glyph with a
different meaning.

### 2. (§6.5, lines 582-616) Message boundaries are not a normative receive/send contract

**Problem:** The draft distinguishes empty payload from EOF, and mentions zero-length datagrams in
an acceptance trace, but the operations and framing rules are byte-oriented. They do not say whether
a message receive returns exactly one message, whether a valid zero-length datagram is a complete
message rather than stream EOF, or whether a send request names one message. An adapter could
flatten or discard message boundaries and still satisfy the written generic rules.

**Evidence:** Empty payload and EOF are distinguished at `design/vortex-io-redesign.md:388-391`;
`Read`, fixed/line/frame policy and cursor rules are at `:469-482, 556-599`. The datagram
requirement appears only as a test/coverage item at `:613-615`, without defining its semantics.

**Fix:** Define an optional message-mode capability: each successful receive returns one whole
message, including an empty one; message transports do not synthesize stream EOF; each send request
names one message and retains its identity/outcome through buffering or coalescing. Adapters without
that capability return typed refusal. Add the two-receive trace (empty message, then nonempty
message). Keep framing grammar and tokenization frontend-local.

## MINOR

### 1. (§6.5, line 554) The Direct Vortex table cell breaks the Markdown table

**Problem:** The row contains escaped literal backticks and an unescaped `|` between `Pending` and
`Outcome`, so Markdown treats the type union as another column delimiter and the code span does not
render as intended.

**Evidence:** `design/vortex-io-redesign.md:554`.

**Fix:** Use a normal code span and escape the inner pipe, e.g.
`Submit(...) -> requestCell; Await(...) -> Pending \| Outcome`.

## Cross-role dialogue

- **VPL:** The VPL report raises the same shape-preservation concern. I verified the operator
  against the language reference and lowering: its established disclose/clone-master behavior does
  not support the example's implied array-preserving meaning. The exchange confirms the finding; the
  fix belongs in the VPL sketch, not shared protocol semantics.
- **Wildcard, VProlog, and VIcon:** Wildcard asks for explicit message-mode boundaries. VProlog and
  VIcon independently agree that empty-vs-EOF and “adapter-supported frame policy” do not establish
  one-message-per-receive or boundary preservation. This confirms the text gap; it does not require
  all adapters to implement message mode.
- **Vlace/VAgent:** I asked whether the local buffer builder, explicit `perform(flush)`, and staged
  actor intent boundaries are clear enough, and whether their perspectives reveal message ambiguity.
  No response was available when this report was written. The generic contract itself distinguishes
  these stages at `:564-580, 601-606`, so I have no separate finding on them.
- ABI freedom is explicit at `:60`, and §6 marks the generic names and operations proposed at
  `:332-333, 463-511`; I found no accidental promise of frozen production APIs.

VERDICT: NOT CONVERGED
