# Round 1 critique: VPL (vortex-io-redesign.md)

Paths below are relative to the repo root. Target snapshot: v0.9, SHA-256
`a6325b19e99bdec6f7f6cb6724ed7a6b5ddecf1e7152e345194ae6872eb58535`. Tally: 0 BLOCKING, 2 MAJOR, 0
MINOR.

## Dialogue with other roles

- **VQL:** VQL asked whether short-circuiting/LIMIT stops later admissions while earlier requests
  and read-ahead remain owned. I agree: early stop prevents only subsequent explicit submits;
  already admitted request cells remain service-owned and observable, and framed unread suffixes
  remain endpoint-owned. Trimming a VPL view with `↑` cannot undo source consumption or discard a
  suffix needed by the next read. This sharpens the VPL outcome/buffer example requested below.
- **Wildcard:** Wildcard and VQL agree that independent request identities/outcomes suffice; VPL
  owns array position/shape in lowering. A generic request-cell rank can represent a cohort; do not
  add a batch opcode or imply atomic admission/cancel. Wildcard also confirms framing is an endpoint
  or request capability, transport owns bounded read-ahead/cursor/generation leases, and VPL owns
  pure decoding/lexing/token state. This resolves my pre-review group question; it does not resolve
  the VPL-specific output/input exposure gap below.

## BLOCKING

None.

## MAJOR

### 1. (§6.5, line 549) The VPL example does not establish shape-preserving results

**Problem:** The sketch writes `R ← ⊃ submit¨ Caps ReadSpecs` and then `await¨ R`, while the stated
contract says each request keeps its input position and the VPL result preserves input shape (§6.5,
lines 539–544). Existing VPL defines monadic `⊃` as disclose/first and resolving a clone to its
master (`design/vpl-array-language.md:94, 278–289`); the current lowering reads an enclosed value
through the enclosure link and otherwise returns the input cell
(`apps/common/xanadu/vpl/lower_enclose.cpp:75–104`). The proposal does not say that `submit¨`
creates an enclosed result or that applying `⊃` retains every position. As written, the only
concrete VPL exposure can be read as selecting/disclosing one value before the elementwise await,
which contradicts the very shape guarantee it is meant to demonstrate.

**Evidence:** The sketch is `R ← ⊃ submit¨ Caps ReadSpecs` (line 549); the nearby prose says VPL
preserves input position/shape by retaining request cells (lines 539–544). The current language
assigns `⊃` clone-master semantics (array-language spec lines 94 and 278–289).

**Fix:** Remove the outer `⊃` unless the proposal explicitly defines the mapped result as an
enclosed nested array and proves disclose preserves its full shape. Prefer a proposed sketch such as
`Reqs ← submit¨ Caps ReadSpecs` followed by `Out ← await¨ Reqs`, then state the output shape rule
for empty, regular, and ragged input views. If two operands are zipped, define equal-shape
requirements and scalar extension (or return a typed whole-expression shape refusal); per-position
admission refusal and completion outcomes must remain at their corresponding positions. These are
language-lowering rules, not core array/batch opcodes.

### 2. (§6.5, lines 549–551) VPL has no concrete framed-input or ordered-output trace

**Problem:** The VPL row demonstrates only generic `ReadSpecs` and `await¨`; it does not show how
VPL requests fixed/line/framed bytes, turns binary payload into typed numeric/text arrays, or
handles short blocks and parse/shape failures. Nor does it show how VPL submits a frozen output
buffer, awaits an explicit flush fence, then closes or aborts. The generic section correctly leaves
framing/decoding/tokenization composable (§6.5, lines 556–599), and says VPL's tokenizer remains
frontend-owned, but the VPL bridge never demonstrates that boundary. Without a VPL trace, `↑` or
another early consumer could be mistaken for rewinding a read, dropping the unread suffix, or
cancelling an admitted request; partial/EOF/empty outcomes can also be confused with missing array
elements. The gap matters because §4 explicitly promises asynchronous VPL array results and
deterministic N-cell submissions (lines 219–220, 235–236), and the common review brief requires
language-level buffering/framing examples.

**Evidence:** The only VPL-specific usage is `R ← ⊃ submit¨ Caps ReadSpecs` then `await¨ R` (line
549). Lines 556–599 delegate buffering, framing, incremental decode, endpoint cursor, suffix
ownership, and pure frontend tokenization to generic rules without showing their VPL composition.
The current language has array operations `↑` (take) and `↓` (drop) and a documented ragged view
model (`design/vpl-array-language.md:91–92, 242–253`), so a frontend consumer's view lifetime is
distinct from the service's sequential input cursor.

**Fix:** Add a short explicitly proposed VPL usage trace, without freezing syntax or ABI, that (a)
submits a bounded fixed/line/frame request and awaits the same request, (b) maps the typed binary
payload through a pure incremental VPL numeric/text decode or tokenization step with explicit
shortfall, malformed/truncated, empty-payload, and EOF outcomes, and (c) shows a bounded
`↑`/early-stop retaining any framed unread suffix for the next sequential read while leaving
admitted work service-owned. Add the corresponding output sequence: construct/freeze a bounded
buffer purely, explicitly submit write and flush-after, await that barrier before a dependent
action, and explicitly close/abort with partial/unknown progress. State that unsupported framing,
decoding, flush, or close guarantees are typed refusals. Keep transport buffering and cursor leases
generic; keep VPL tokenization and decoded array shape in VPL lowering/stdlib.

## MINOR

None.

VERDICT: NOT CONVERGED
