# R4 Vortex review confirmation

## Verdict: CONVERGED — no actionable findings

R4 closes my sole R3 finding. It now requires `now < result_until <= expires_at` both when
constructing `Accept` and when the participant accepts work, and says an invalid or elapsed result
deadline closes/refuses the offer without adopting a work goal
(`design/vagent-open-question-resolutions.md:362–365`). The worked-protocol test gate now includes
an acceptance whose execution deadline has elapsed and confirms that no work is adopted
(`design/vagent-open-question-resolutions.md:414–418`). This matches the one-node, shared
nondecreasing clock and no-migration scope (`design/vagent-open-question-resolutions.md:341–345`).

I compared the frozen R4 target with archived `r3-input.txt`. Beyond the revision label, the only
substantive changes are this deadline validation and its gate case. The rest of the final set
remains covered by the findings, runtime review, and U-A1–U-A14 dispositions in
[r3-vortex-expert.md](r3-vortex-expert.md); I found no new issue in the R4 changes or the unchanged
recommendations.

This is a specification review confirmation only. No builds or tests were run, and no implementation
or owner approval is implied.
