# Writer review — R1

**Verdict: NOT CONVERGED.** The guide is careful about distinguishing specification from shipped
behavior, and its annotated examples are a strong foundation. These issues still make it harder than
necessary for a new reader to learn and apply the language.

## Blockers

### 1. The first “complete” workflow introduces many unexplained runtime terms

**Where:** `docs/vagent-user-guide.md:73–82`, with terms repeated at `:112–124`.

**Learner impact:** The first explanation of a turn uses “adapter,” “local work item,” “budgets,”
“settles internal belief/goal events,” “arena writes,” “outbox,” and “hard failure” before the
reader has seen an event actually run. The text makes the execution model sound like a distributed
runtime internals manual. “Save anything you need as a local value” is also misleading without the
later explanation that a plan's locals survive suspension.

**Proposed correction:** Lead with a small observable trace of the counter example: construct with
0, deliver Add(3), count becomes 3; deliver Add(2), count becomes 5. Then introduce the turn as “one
input is handled at a time” and explain rollback/outgoing messages in the later sections where those
ideas are taught. If “local value” remains here, distinguish handler locals from saved plan locals.

### 2. The guide asks readers to validate examples without identifying which examples the current

checker can actually check

**Where:** `:84–114`, `:148–151`, and `:394–415`, `:901–915`.

**Learner impact:** The workflow says to check source after each change and frames each code fence
as an independent source unit, but the guide later says cross-file imports are refused and standard
module imports have checking gaps. The string-scanning example explicitly depends on registrations
that the checker does not supply. A novice following the examples can get a rejection and cannot
tell whether they made a mistake or reached an implementation boundary.

**Proposed correction:** Give each example a compact status label such as “checker-ready,” “syntax
illustration; dependency not implemented,” or “specified runtime trace only.” Add one concrete check
command for a known accepted source unit and say what a rejection means for examples outside that
set. Keep the high-level implementation-status caveat, but connect it directly to the fences that
need it.

### 3. Several foundational sections introduce too many language mechanisms before the reader has

had a chance to consolidate the basic model

**Where:** `:233–305` and `:307–348`; then `:350–415`.

**Learner impact:** The early tutorial moves from Boolean conditions to search semantics, bounded
statements, alternation, negation-as-failure, generators, size, concatenation, full precedence,
reversible assignment, five rollback scopes, loops, contracts, and string scanning. This density
forces a beginner to retain several nonstandard semantics at once. The precedence table even lists
string scanning and assignment before either is needed for the basic event/field examples.

**Proposed correction:** Keep the important `Bool` versus success distinction and one short
alternation example near the beginning. Move the complete precedence table, reversible assignment,
string scanning, and advanced rollback cases to a later “Search and expression reference” section.
Add a short checkpoint after ordinary handlers (one event, one field update, one condition) before
introducing alternatives and rollback.

### 4. The capstone combines concepts but does not isolate the central interaction clearly enough

**Where:** `:917–993`, especially `:956–980`.

**Learner impact:** This example combines a question, timeout fallback, typed optional agent fields,
spawn, `adopt`, `achieve by`, source-bearing belief, `await`, and message ordering. The trace is
valuable, but it makes the reader mentally simulate two independent completion paths and several
turns at once. The phrase “the shortage condition fails, so the total if does nothing” (`:976`) is
hard to parse and can be mistaken for a special “total if” construct.

**Proposed correction:** Split this into two stepped examples: first show a courier goal and its
parent's delegated outcome; then add the delivered belief and explain why its order can differ from
the outcome. In the current walkthrough, say “`available < count` is false, so the shortage block is
skipped.” Keep the full program as an optional synthesis exercise after those steps.

## Optional polish

### 5. The reading path omits explicit wayfinding for several substantial topics

**Where:** `:36–50`.

The path jumps from waiting to the workshop and then operations, while effects, behaviors,
scheduling, modules, and persistence are major sections a reader may need to find. Add those links
or state that these are optional/reference chapters and name them in the path.

### 6. The term “belief” is defined, but the first explanation of a fact's source is abstract

**Where:** `:66–68` and `:456–505`.

The `reading` example is useful, but the important claim that the fact survives while another source
asserts it arrives after a large example and several tables. Add a tiny two-agent trace (A tells a
fact, B sees A as source, A withdraws it) before the keyed-predicate detail. This gives the reader a
concrete anchor for why a belief is different from a field.
