# Writer critique, round 7: design/vagent-agent-language.md (v3.4, 2118 lines)

Mechanical checks: no non-table line over 100 columns except one code-block line (inside a fence,
exempt); mdl not installed here, so not run (I did not verify it). Counts agree: "fourteen" changes
(header, §1, §11, appendix) match P1-P14; "thirteen" dimensions agree in V10, §5.1, §10, §16 and the
appendix. Version 3.4 is in the header and appendix; the stale v3.2 mentions are gone.

## Round-6 BLOCKING/MAJOR findings

None were raised (verdict CONVERGED). All five round-6 minors: version strings, §11 intro, §5.1
lead-in, §1 framing and P9 notes pages are RESOLVED.

## New BLOCKING or MAJOR findings

### 1. MAJOR, §11.2 lead-in: contradicts the status it sits under

Line 1531 says "P12-P14 ... are **pending approval**. The design depends on them as approved
changes; the 'Without it' column remains as the record of what each buys." The sentence was correct
for P1-P11 in v3.3 and was not updated when P12-P14 were added. It contradicts the §11 intro (line
1477: "P12-P14 are not [approved], and the design is written to work without them"), the header, §1
and §4.4, which all state the pending ones are optional. An implementer reading the register alone
would treat P12-P14 as prerequisites and might delete the §4.2 Dewey reset or the periodic
`compact()` fallbacks, which are the only thing bounding the cost until they land. This is the
status-honesty rule of the house style.

Fix: replace the two sentences with "The design depends on P1-P11 as approved changes. It does not
depend on P12-P14: each 'Without it' cell states the cost and the fallback the design carries until
the owner decides, and §4.4 and §15 repeat them."

## MINOR

- §11 intro "A proposal is incorporated into the design only after its own approval" now reads oddly
  beside P1-P11 being incorporated; say "P12-P14 will be incorporated only after approval".

VERDICT: NOT CONVERGED
