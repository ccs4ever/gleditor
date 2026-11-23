# Round 1 critique: Programming Language Design & Ergonomics Expert (vortex-io-redesign.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 2 MAJOR, 0 MINOR.

## BLOCKING

None.

## MAJOR

### 1. (§4, lines 180–199) Shared effect rules stop at the runtime boundary

**Problem:** The document defines the crucial distinction between a fresh explicit submission and
observing an admitted request, but it does not state a language-neutral static contract for where
those actions may occur or which language constructs can trigger them. The bridge table delegates
multiplicity to each frontend, and §4 says evaluation models determine notation and multiplicity.
Without common rules for guards, predicates, read-only answers, render/display, and repeated
expressions, two frontends can assign different effect behavior to equivalent programs. A read or
write reached during query traversal, a retried goal, or rendering could submit more than once or
perform I/O in a context that sibling languages promise is pure. This is especially consequential
for the mandated fresh-submit-on-backtrack rule.

**Evidence:** The document says “Their evaluation models determine notation and multiplicity, not
separate resource implementations” (§4, lines 187–189), and says “A backtracked explicit read or
write is a fresh submission when executed again” (§4, lines 191–194), but gives no shared static
legality or evaluation-count rule. Existing VAgent semantics explicitly prohibit effects in methods,
conditions, and answers (design/vagent-agent-language.md:1130–1135), require transitive read-only
answers (design/vagent-agent-language.md:970–971), and make guards pure
(design/vagent-agent-language.md:2177–2178). VQL's bridge separately needs a positional static rule
because a mutating step in a predicate/join may run multiple times or not at all
(design/vql-lens-bridge.md:108–124, 139–147). The target identifies VIcon's host-rendered yields as
a current seam but never specifies whether rendering is observational only (§2.3, lines 126–129).

**Fix:** Add a language-neutral effect contract in §4 before the frontend matrix. Define submit as
an explicit effectful action that creates a new request each time that action is evaluated; define
await as observation of a named retained request; define render/display as observational and
forbidden from initiating work. State which categories of contexts must be pure (at minimum guards,
predicates/filters, and read-only answers), and require each bridge to document statically checked
submission sites and evaluation multiplicity according to its evaluation model. Include diagnostics
for rejected effect placement without prescribing new syntax.

### 2. (§6.2 and §7.1, lines 261–266, 316–323) Terminal outcomes have no language-level result rule

**Problem:** The service outcome vocabulary is listed, and Pending is explicitly separated from
success and logical failure, but the design never specifies how a ready terminal outcome is consumed
by a program. In particular, it is unclear whether EOF terminates a stream read as a normal value,
whether rejection/error/cancellation/overflow become typed result cells or fail the current goal, or
how a partial payload/count is exposed alongside an error. An implementer could collapse transport
failure into ordinary logical failure or expose a different convention in each frontend, making
backtracking and recovery behavior unpredictable despite the stated typed cell protocol.

**Evidence:** The Outcome table lists “Payload/result, EOF, rejection, error, cancellation, partial
completion, overflow, or unknown outcome” (§6.2, lines 258–266); it distinguishes empty payload from
EOF and says cancellation cannot be represented as logical failure, but does not define the
program-visible terminal shape. §7.1 says a ready request “materializes its outcome” and only
defines Pending's execution behavior (§7.1, lines 316–323). By comparison, VAgent specifies that an
awaited event timeout fails the wait statement and that an answer returns either a value or typed
fault (design/vagent-agent-language.md:960–968, 2462–2466).

**Fix:** Specify a language-neutral ready-outcome contract: the owner materializes a typed Outcome
cell for every terminal state, preserves any partial payload and progress with its terminal reason,
and does not implicitly translate transport outcomes into logical failure. Define stream EOF as a
distinct normal terminal observation, then require each frontend bridge to state how its control
model consumes/recoveries from those typed outcomes. Keep timeout/cancel of a language-level wait
separate from cancellation of the underlying request.

## MINOR

None.

VERDICT: NOT CONVERGED
