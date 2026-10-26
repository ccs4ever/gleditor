# r4 technical writer

## Verdict: CONVERGED

No actionable findings remain. The R4 comparison shows only the revision label and the Contract-Net
result-deadline clarification changed from R3. The protocol now requires
`now < result_until <= expires_at` both when creating `Accept` and when accepting the work; invalid
or elapsed deadlines close/refuse the offer without adopting a goal
(`design/vagent-open-question-resolutions.md:362–368`). The acceptance gate explicitly tests an
acceptance whose execution deadline has elapsed and verifies that no work is adopted (`:410–421`).
This closes the final minor from the R3 review without introducing a new issue within the proposed
same-node, same-owner, shared-clock scope.

The R3 disposition of all fourteen questions remains valid. No owner approval or implementation
claim is implied (`design/vagent-open-question-resolutions.md:3–6, 421–462`). The gates remain
proposed, not executed.
