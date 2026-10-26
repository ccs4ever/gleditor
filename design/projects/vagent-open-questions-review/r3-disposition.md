# Round 3 disposition: revised as R4

The writer and language expert converged with no actionable findings. The Vortex expert converged
with one minor lower-bound check: an Accept whose result deadline has already passed must not start
work, even while its offer deadline remains in the future.

R4 accepts that finding. Both construction and acceptance require
`now < result_until <= expires_at`; an invalid/elapsed deadline closes or refuses the offer without
adopting work. The scripted trace gate includes this case. No other recommendation semantics
changed. R3 is archived in `r3-input.txt`; R4 in `r4-input.txt`.
