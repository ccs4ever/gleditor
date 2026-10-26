# Incorporation round 3 disposition: expanded round 4

All four reviewers returned on `r3-input.txt`, confirming the original incorporation corrections and
all fourteen resolutions. The writer and language reviewer found Markdown's interpretation of a
wrapped numeral as a list in the yield explanation. The runtime/codebase reviewers and parent also
identified the reserved plan identifier `event`. Both minor defects are fixed: the plan is
`wait_event`, and the result value is inline code in continuous prose.

To fulfill the user's request for keyword interactions more fully, round 4 adds focused examples for
list/range iteration versus generation, while-loop completion, ordinary calls/early return and pure
pre/postconditions, and string scanning versus logic-variable binding. These bring §12.6 to 23
independent fenced units. All reviewers must check the expanded frozen `r4-input.txt`; no prior
approval is assumed to cover the added examples.
