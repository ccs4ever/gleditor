# Guide review r1 — technical writing

Verdict: ACTIONABLE FINDING

## G1 — Name the known gaps in frontend checking

**Location:** `docs/vagent-user-guide.md`, “Implementation status” table, row “Syntax, types and
static restrictions” (around line 15).

**Finding:** The guide says that `build/vagent check` reports diagnostics and exits nonzero for
rejected source, and later correctly limits a successful result to the *implemented* frontend rules.
However, the status table does not identify the known checker gaps. The current progress record says
“Static module aliases/selectors and adopting-agent scope checks still need work.” A reader could
reasonably take the broad syntax/types/static-restrictions row as evidence that those language rules
are already checked.

**Action:** Add a brief status note naming the open static checks (module aliases/selectors and
adopting-agent scope), or link this row directly to the progress record's checkpoint. Keep the
existing distinction between static acceptance and execution.

**Basis:** `design/vagent-implementation.md`, “First iteration commands and evidence,” explicitly
lists these remaining frontend gaps. The guide already handles the larger runtime limitation
accurately: it says the traces are specified behavior, not runtime-validated behavior, and points
readers to implementation progress.

## Review notes

- The guide's explanation of `:=` and `<-` matches design §8.3: “irreversible” is scoped to search
  inside a bounded statement, while failed enclosing statements, handlers, and turns still roll back
  writes.
- The §12.6 sample distinctions are carried into the guide with useful context, especially
  Bool-as-value versus success, first-result versus search, and reversible writes within a
  statement. I found no keyword interaction that contradicts the contract in the sections reviewed.
- The guide labels future run behavior and native bindings as specified scenarios, and the
  implementation record reports 92 frontend, 91 shared-runtime, and 23 scalar-kernel tests without
  presenting these as an executable agent runtime. The draft remains reviewable as teaching material
  without waiting for runtime completion.
