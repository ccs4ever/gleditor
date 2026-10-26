# Technical writer review — vagent user guide

Reviewed `docs/vagent-user-guide.md` at SHA256
`6adf9a4e099fc1890f78b6d9080de7c7604b8eac53e257a2f1092c94b2eb8667`, against
`design/vagent-agent-language.md`, `design/vagent-implementation.md`, the CLI implementation, and
the guide-test source. I did not run tests or inspect earlier review reports.

## Finding

### The guide never teaches how to declare a record

**Location:** `docs/vagent-user-guide.md:225-235`, with the keyword reference at `1360-1362`.

The value table lists “Declared record/event” and says records have statically typed named fields,
and the keyword table lists `record`; however, no section gives the `record Name { field: Type }`
declaration form or a complete example that constructs, reads, or passes a record. The only “Point”
mention is explicitly hypothetical. This leaves a named part of the type system discoverable only by
consulting the design document, which weakens the guide’s stated role as a standalone teaching path.

**Suggested fix:** Add a short checkable source unit defining a record, constructing it, and reading
a field; explain that a record is a local typed value rather than a wire event. Link it from the
values section and keyword reference. Mark the execution story as predicted under the existing
convention.

## Checks and terminology

- The title’s v3.9 reference matches the specification’s version.
- The `build/vagent check FILE...` usage, `-` stdin form, successful output, and exit-status
  descriptions match `apps/vagent/main.cpp`.
- The guide consistently distinguishes accepted frontend syntax from complete conformance and
  runtime execution, including imports, scanning, native effects, schedules, checkpoints, and the
  workshop trace.
- The inspected examples and runtime claims use the specification’s terms consistently: source-aware
  belief facts, bounded statement rollback, intention/plan distinction, await-before-handler
  matching, within-event priority, stable agent name versus pid, and cancellation versus physical
  effect reversal.
- Internal cross-references in the reading path point to the corresponding section headings.

CONVERGED on technical correctness and terminology, with the record-teaching completeness finding
above for the author to resolve.
