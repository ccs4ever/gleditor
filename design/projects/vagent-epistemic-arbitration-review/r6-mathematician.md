# Round 6 review: Mathematician & Markdown Math Specialist

Reviewed the frozen v1.3 approved input; it is byte-identical to the current design document.

## Previous finding status

- **R4 K3.1–K3.5:** CLOSED. The score is explicitly a heuristic rather than a Bayes factor;
  convergence claims are conditional; unsupported dependent fusion refuses; numeric domains and
  refusal behavior are specified; and uncertainty is scalar isotropic variance in squared units.
- **R5 ML findings:** CLOSED. The stated SGD update weights both data gradients by `q`, regularizes
  weights and intercept, and limits convergence claims to suitable stationary-epoch assumptions.

## Tally

**0 BLOCKING, 2 MAJOR, 0 MINOR.**

## MAJOR

### 1. (§3.1, lines 149–159) Correlation parameter has no validated domain

**Problem:** `rho_G` is used as an equicorrelation design-effect parameter, but neither the
formula's surrounding text nor the numeric contract gives its valid range. A finite value alone is
insufficient. For a group of two, `rho_G = -1` makes the denominator zero; `rho_G < -1` makes
effective group size negative. Values above 1 are also not correlations and yield nonsensical
attenuation. This can corrupt rankings or cause a numeric refusal where a valid policy was accepted.

**Evidence:** The pool defines `n_eff(G) = |G| / (1 + (|G|-1) rho_G)` (line 151), but only specifies
the fallback `rho = 1` for unknown dependence (lines 156–159). §8.1's parameter-domain list does not
mention `rho_G` (lines 632–635 in the frozen input). A concrete accepted-looking finite value
`rho_G = -1` with `|G|=2` divides by zero.

**Fix:** Define and enforce `0 <= rho_G <= 1` for this initial heuristic, including schema and
runtime validation, and define failure as `InvalidEpistemicPolicy`. If negative dependence is
intentionally supported, give the group-size-dependent positive-semidefinite lower bound and require
`1+(|G|-1)rho_G > 0`; explain that this permits `n_eff > |G|` and validate it before scoring.

### 2. (§5.3, lines 441–445) Oversized single-linkage component has no defined alternative partition

**Problem:** The text defines epsilon-connected components, then says a component over
`max_cluster_diameter` is “preserved as separate clause alternatives,” but does not specify how one
connected component becomes multiple alternatives. Different partitions change cluster scores,
whether `truest` has a unique maximum, and whether `consensus` can fuse. The implementation cannot
determine a unique answer from the specified inputs.

**Evidence:** Points at 0, 0.9 and 1.8 with `cluster_tolerance = 1` form one connected
single-linkage component, but its diameter is 1.8. With maximum diameter 1.2, the document requires
separate alternatives without saying whether to split as `{0,0.9}/{1.8}`, `{0}/{0.9,1.8}`, or
refuse. §5.3 later permits consensus for one accepted unimodal component and fuses its members
(lines 458–478), making this partition decision observable.

**Fix:** Specify a deterministic bounded-diameter partition rule and tie-break, with its cost, or
conservatively make an oversized component ineligible for consensus and preserve its individual
observations as ranked alternatives with an explicit ambiguity result. The latter needs no
unsupported clustering inference.

## Numeric and semantic checks with no finding

The Beta update and fractional-weight caveat are internally consistent. Pooling with one
all-correlated group reduces effective count to one; with independent groups it sums group opinions,
and the neutral prior is `p0=0.5`. The stable sigmoid/logit and finite-domain refusal policy avoid
silently treating NaN/infinity as evidence. SGD uses accepted-label count `n`, applies `q` to both
data gradients, and its stated step-size sums are correct; clamping at finite logits does not affect
the claimed general convergence guarantee because none is made. Decay tends to 0.5 and is distinct
from liveness/physical drift. Tie grouping is anchored and non-transitive, with deterministic
observation-ID ordering. Fusion's precision equations are dimensionally valid for independent,
aligned, scalar-isotropic errors; the design refuses unknown dependence, incompatible times,
multiple components, and unsupported covariance.

VERDICT: NOT CONVERGED
