# Round 2 disposition: revised as R3

The language and Vortex reviewers converged, each with a minor request to carry protocol expiry
explicitly. The writer closed all first-round findings but found a major missing participant-side
cancellation transition, plus minor incorporation-map and loss-observability issues.

R3 accepts all remaining findings. The worked protocol now states participant request, rejection and
active-cancellation transitions, terminal-result races, late completions, expiry, closed-state
retention and resource limits. It confines the illustrative absolute-deadline comparisons to one
node with a shared nondecreasing injected clock, adds explicit deadline fields, and leaves
multi-node clock semantics as a distributed-library prerequisite. Loss handling distinguishes
observable orderly stops from unreported local crashes, which still depend on deadlines.

The U-A3 map includes checkpoint sections. Required ordinary P4 arbitration moves into Now;
priority/urgency alone remains Later. No deferred extension is represented as built or approved.
R2's exact input remains in `r2-input.txt`; R3's input is archived as `r3-input.txt`.
