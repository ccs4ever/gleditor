# Writer critique, round 5 (final): design/vagent-agent-language.md (v3.2, 2002 lines)

Mechanical checks: `mdformat --wrap 100` (gfm, dollarmath, frontmatter) `--check` passes on a copy;
`mdl` with `.mdl_style.rb` prints nothing; no non-table line exceeds 100 columns. Contents anchors
match the 18 headings plus the appendix. Version 3.2 agrees in header and appendix. Counts recheck:
"thirteen" dimensions agrees in V10-adjacent text, §5.1, §10, §16 and the appendix; "nine changes"
agrees with P1-P9.

## Round-4 findings

| #   | Sev   | Status              | Note                                                                                                                                                                           |
| --- | ----- | ------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| 1   | MAJOR | RESOLVED            | Header and §1 call vicon §6's scanning functions a prerequisite and say `std:goal` registers only `scan`, `subject`, `pos`; §10 says "once it registers vicon §6's functions". |
| 2   | MAJOR | RESOLVED            | `Point` is a record in `mars:hardware`; the appendix records the `std:math` correction.                                                                                        |
| 3   | MINOR | RESOLVED            | The disposition moved `within` into the checker.                                                                                                                               |
| 4   | MINOR | RESOLVED by removal | There is no longer a §11.3 sentence; the "(a)-(h)" scheme was replaced by P1-P9. One stale remnant remains (finding 1 below).                                                  |
| 5   | MINOR | RESOLVED            | Glossary rows for restorer, goal intention and question per the disposition.                                                                                                   |
| 6-8 | MINOR | RESOLVED            | §6.2 marked as an excerpt; the `c & e` sentence removed; §12.4 says how `Warehouse` starts.                                                                                    |

## New findings

1. MINOR. §14, "Agents do not leak slots ... given P7 and (h)". `(h)` was the old §11.3 letter for
   fair pumping and no longer exists. Write "given P7 and P8".

1. MAJOR (register completeness, the owner's final-round check). §10 settings table, §3 V1, §4.2.
   The document introduces eleven `vagent.*` settings "read from `system://settings` by the launcher
   and the adapter" (§10, lines 1396-1408). A setting exists only as a `defaultSettingSpecs()` entry
   in `apps/common/xanadu/system_docs.cpp` (declared `system_docs.hpp:952`; AGENTS.md says adding
   one "is easy to skip"), so the table is a change to existing code that §11.2 does not list, and
   AGENTS.md also requires the schema and notes pages. It is not new code under `vagent/` either.
   Fix: add a proposal row ("P10: seed eleven `vagent.*` settings in `defaultSettingSpecs()`; why it
   cannot be in `std:agent`: the system xanadoc is the single store of configuration and its schema
   validation lives in `system_docs.cpp`; without it: the adapter reads compiled-in defaults, which
   AGENTS.md forbids as naked magic numbers"). The honest "without it" is a real fallback only if
   the owner accepts the magic-number cost; say so. Until then the table's defaults (128, 4096,
   65,536, 16, 30s) are asserted as settings that do not exist.

1. MINOR. §10, the `Logging` paragraph. "`vortex.agent` ... which the logging migration must
   register" is a dependency on an edit to `include/gleditor/logging.hpp`, existing code, stated in
   prose and absent from §11.2. Either fold it into the P10 row (a settings-and-logging registration
   proposal) or say vagent uses an existing category. I found no `vortex` category in `logging.hpp`,
   so the name needs registering.

1. MAJOR. §11.2 row P1 and §3 V13, §9.1. The "Without it" fallback is stated only in the table:
   "methods may not be called from speculative positions". It is not repeated where the language
   rules live (§8.2, §8.3, §8.9 static restrictions), and the §9.1 rows for `if` and `while` also
   emit `#ChoiceDepth` but the "Used by" column lists only "V13, §9.1". A reader of §8 meets a
   language with no such restriction and a design that depends on P1 in every statement. State in
   V13 that the whole of §9.1's statement, `if` and `while` lowering is conditional on P1, give the
   fallback lowering (compile-time depth counters) and what §12.1-§12.4 lose, naming any example
   method called from `&`, `|`, `to`, a comparison or a call operand. If an example uses one, the
   examples depend on an unapproved change and must say so.

1. MINOR. §11.2 row P1, "Where". The Where cell says "Vortex VM" but the Change cell also edits
   `vql/compiler.cpp` and `vpl/compiler.cpp` name tables: two more existing files owned by other
   languages. List them in Where, since they are separately approvable.

1. MINOR. §11.2 row P4. The change makes `ActorExecution::message` a `std::optional` (it is a plain
   `ActorMessage` at `mesh_actor.hpp:81`). Executors in `tests/xuzz/mesh_actor_test.cpp`,
   `mesh_actor_resume_test.cpp` and `mesh_checkpoint_network_test.cpp` use the type. The cell says
   "touching the existing mesh executor and its tests"; name the three test files, and say why a
   second entry point taking no message (leaving `ActorExecution` untouched) was refused, since that
   would be the cheaper change and the register is meant to record refused alternatives.

1. MINOR. §11.2 row P9. "Without it: None: this is how every package is found" is a non-answer and
   P9 fails the section's own test ("impossible or possible only by a workaround"): the adapter
   could load `std:agent` as a prelude module of the program artifact. Either justify why that is
   unacceptable (every package is found by name under `d.stdlib`, §10) or say "without it: the
   loader injects `std:agent` into each program artifact, costing a copy per agent". The design then
   states its fallback.

1. MINOR. §11 header and §1. §1 says vagent "asks for one opcode and no other change to the VM";
   §11.2 P1 also touches two compilers and P4/P2/P3/P5-P8 are all runtime changes. The sentence is
   correct about the VM but a reader skimming §1 meets "one opcode" as the total ask. Add "and eight
   changes to the mesh runtime and the standard library, P2-P9" to that sentence.

1. MINOR. §11.2 rows P2/P3/P7 "Without it". Each states a bound ("4096 messages", "128 agents") but
   no row says whether the design still works as a short-lived demonstration; the closing paragraph
   says the mesh "was built for bounded jobs". Add one sentence: without P2, P3, P7 the language
   runs, for a bounded number of messages and agents per node, so the examples in §12 work and a
   long-lived society does not.

1. MINOR. Proposal citation by number. Several sections use the proposed capability by prose only:
   §6.6 and §7.5 rely on the wake of P4 (table "Used by" lists them), but §4.1's "the first wake
   turn" and §5.6's ready list do not cite P4. The register's "Used by" column and the text should
   agree both ways; a grep for `P4` finds §1, §4.2 and the register only.

## Summary

Counts: 0 BLOCKING, 2 MAJOR (2, 4), 8 MINOR (1, 3, 5-10). The text, structure, tables, status
honesty and mechanical formatting are sound and every earlier writer MAJOR is resolved. The register
is well built (reason, fallback, users per row) but is incomplete in one place that matters to the
owner's ruling: the settings table (and the logging category) changes existing code with no proposal
number, and P1's fallback is stated in the register but not carried into the language sections that
depend on it. Both are short edits; neither needs a design change.

VERDICT: NOT CONVERGED
