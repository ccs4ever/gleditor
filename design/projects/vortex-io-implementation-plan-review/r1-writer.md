# R1 — Technical writer review

Target: `design/vortex-io-implementation-plan.md`  
Target SHA-256: `f619801374ad6b81638e719014243cbccc354fdfee5ca9c1579b43b93bc80093`  
Review scope: cross-layer terminology, examples, normative versus illustrative status, milestone readability, and end-to-end contract presentation.  
Verdict: **REVISION RECOMMENDED**

## Findings

### MINOR — The direct Vortex row is both outside the stated bridge count and malformed as a Markdown table row

**Evidence:** §2 says “Six bridge APIs in §10” and §10’s introduction calls the syntax the target surface. The table then contains six named language bridges plus a `Direct Vortex` row, making seven rows. The direct row's `Pending | outcomeCell` is an unescaped pipe inside a code span, so Markdown parses it as an additional table column. This is not a seventh language bridge: the redesign §6.1 describes direct Vortex programs as exercising the shared protocol, so the plan should call it the shared-protocol baseline explicitly.

**Why it matters:** The malformed row renders the API in fragments and makes the M7 “six target interfaces” gate ambiguous about whether direct Vortex is included in the count.

**Suggested fix:** Label the row “Direct Vortex (shared-protocol baseline; not counted among the six bridges)”; write “Pending or outcomeCell” or move the result type outside the table cell; explicitly say M7 covers six language bridges plus the direct Vortex conformance path.

### MINOR — Terminal EOF spelling varies between the API and examples

**Evidence:** §3.2 names the outcome variant `EndOfStream`, while the same subsection says “`Eof` is distinct”; §§6.5 and 7.1 use `Eof` in the worked protocol and stream contract. The plan says signatures are the proposed C++ design and ownership/refusal/order semantics are normative, so a reader cannot tell whether `Eof` is shorthand in pseudocode or a second public tag.

**Why it matters:** Frontend lowering and adapter tests need one canonical outcome tag to avoid each bridge inventing its own spelling or mapping.

**Suggested fix:** Use `EndOfStream` consistently in API-facing text and mark shorter spellings in pseudocode as explanatory aliases, or choose `Eof` as the actual variant and update the declaration.

## Positive checks

- The document distinguishes proposed signatures from production ABI and states which ownership/refusal/ordering rules are normative (§1).
- The frontend examples identify their syntax as a target surface rather than claiming current grammar (§10).
- Milestones have named prerequisites and observable exit evidence; the final section retains explicit first-release refusals.
- The proposed plan is tied to the redesign by version and SHA-256, and the review record correctly keeps plan changes separate from implementation authorization.

## Tally

- Blocking: 0
- Major: 0
- Minor: 2
- Nit: 0

No milestone dependency or final-layer acceptance omission was found in this review scope. I did not modify the plan or runtime sources and did not run tests.
