# Writer critique, round 6: design/vagent-agent-language.md (v3.3, 2075 lines)

Mechanical checks: `mdformat --wrap 100` (gfm, dollarmath, frontmatter) `--check` passes on a copy;
`mdl -i -s .mdl_style.rb` prints nothing; no non-table line exceeds 100 columns. Counts agree:
"eleven" changes (header, §11.2, appendix) match P1-P11; "thirteen" dimensions agree in V10, §5.1
(table), §10, §16 and the appendix. Version 3.3 is in the header and appendix.

## Round-5 BLOCKING/MAJOR findings

- Finding 2 (settings with no proposal): RESOLVED. P9 and P10 exist, and §10 says "once P9 adds
  them".
- Finding 4 (P1 fallback not carried into the language sections): RESOLVED. P1 is marked
  load-bearing, with the inline-everything alternative refused with a reason, and "Used by" lists
  V13, §6.7, §9.1, §9.2 and every example.

## New BLOCKING or MAJOR findings

None.

## MINOR (not blocking)

- Stale version strings in the body: "vagent v3.2 specifies single-owner societies" (line 281),
  "implements single-minded commitment" (line 732) and "fixed in v3.2" (line 758) disagree with
  version 3.3. Write "this version" or 3.3.
- §11 intro still says a proposal "is incorporated into the design only after its own approval;
  until then the design depends on it only as stated in its Without it column", while §11.2 says all
  eleven are approved. Reword to the present state.
- §5.1 lead-in "Found or minted by `std:agent` at load (V10)" contradicts V10, where the adapter
  calls `ensureDimension` and `std:agent` only receives the `DimRef`s. Write "Found or minted by the
  adapter at load (V10)".
- §1 says everything beyond the one opcode "is at the boundary between the VM and the mesh runtime",
  but P9-P11 are settings, logging and Makefile entries; add "and three registrations, P9-P11".
- §11.2 P1 "Where" lists the two compiler name tables; fine, but P9 does not mention the schema and
  notes pages AGENTS.md requires for system xanadoc settings.

VERDICT: CONVERGED
