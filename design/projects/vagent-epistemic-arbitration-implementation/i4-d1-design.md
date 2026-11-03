# I4-D1: the numeric design for P19 scoring, pooling, gates and fusion

Revision 1, a draft for review. Author's perspective: ML and numerics. Code baseline: HEAD
`47d00222` (the design reads `apps/common/xanadu/vortex/vortex_vm.cpp` and
`vagent/runtime/epistemic_policy.cpp` at that commit; neither has changed since the plan was
written). Spec authority: P19 v1.5 sections 3.1 to 3.4, 5.2, 5.3 and 8.1. Rulings it applies: D1,
O6, O9, O10 and P2-5 (the label and ledger caps are **not** here; they belong to I4-D2). Inputs: the
I1 record ([I1 validation](i1-validation.md), [ML gate](ml-gate-i1.md)), the I3-2 estimator
([I3-2 design](i3-2-design.md) section 3.2) and the ML plan reviews M4 to M7 and minors
([round 1](plan-review/review-ml.md), [round 2](plan-review/review-ml-r2.md)).

Everything numeric below was computed, not asserted. The scripts and the prototype of the vector
file are under `/tmp/p19-work/i4d1/` (`ref/` holds the reference, the vectors and a stand-in test;
`gate_bound*.py`, `nums.py`, `smoke.py` and `lcb_table.py` hold the gate and lcb figures). The
prototype is evidence for the format and the tolerances, not a deliverable; section 4 says where the
real files go.

## 1. What this design decides

| Item | Decision                                                                                                                                                                           | Section  |
| ---- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------- |
| D1   | Replay is bit-equal on one host. Every routine that uses no `Exp`/`Ln` is bit-equal on every IEEE-754 host. The rest agree within a stated cap, and a decision reports `marginal`. | 5        |
| O9   | `risk: "lcb"` shrinks the warrant toward 1/2 by `c * sqrt(v)` and never past it. Proposed amendment A13.                                                                           | 6        |
| O10  | Gate rejections drive an event-indexed fraction (half-life 64), SUSPECT at 0.30, FAULTED at 0.70 with a minimum evidence weight. False latch at most 1e-6 per device-year, proved. | 7        |
| O6   | Drop the profile-variance path for v1 (proposed amendment A15). `t_star` and the reference are fixed in 8.2.                                                                       | 8.2      |
| -    | `decay` is the predicate's freshness half-life (already built as `decayMs`); absent means no decay. `score_tolerance` is an absolute difference of log-odds (proposed A14).        | 8.1, 3.6 |
| -    | The reference is an mpmath program with a running error bound per case, a hex-float input format, 50-digit outputs, and exact bits for the libm-free routines.                     | 4        |

Findings that shaped the design, each checked against the code:

- **The VM does not guard division.** `Div` by a zero divisor yields `0.0` and succeeds
  (`vortex_vm.cpp:516-523`). Only `Exp`, `Ln` and `Sqrt` refuse (`:922-945`). Every `Div` in section
  3 therefore has a divisor proved positive, or a refusal before it.
- **The VM has no `isfinite`.** A finite test is `Abs` then `Lte` against the largest double, which
  is false for infinity and NaN. Section 3 calls it `finite(v)`.
- **Only `Exp` and `Ln` reach libm.** `+ - * / Sqrt Min Max Abs Neg` and the comparisons are single
  IEEE operations, one per opcode, with no fast-math or contraction flag in the Makefile (grep: only
  `-O0` and `-O3` appear). So most of P19's arithmetic is bit-identical across hosts, and the
  cross-host question is confined to a small, nameable set (section 5).
- **`decay` and `score_tolerance` already exist as policy keys** (`epistemic_policy.cpp:45,47`;
  `decay` is a duration in milliseconds, `score_tolerance` defaults to 1e-9). The plan's "per-source
  `decay`" is the predicate policy's half-life; nothing is stored per source.

## 2. Notation and rounding model

`u = 2^-53` is the unit roundoff. `+ - * /` and `Sqrt` are correctly rounded: relative error at most
`u`. `Exp` and `Ln` are the host libm. This design assumes at most 2 ulp, relative error `L = 4u`,
which glibc and musl meet. The assumption is not trusted: the vector file carries libm probes
(section 4.4) so each host proves it.

A **pair** `(p, q)` carries a warrant `w` and its complement `1 - w`, each to relative accuracy.
Computing `1 - w` from `w` near `w = 1` loses digits (a rounding of `5e-17` in `w` is `5e-11`
relative to `q = 1e-6`). The pair never forms the complement by subtraction. Section 3 gives the
sequences in which both entries are sums or ratios of non-negative numbers.

Error bounds are **absolute** bounds on the computed value, from a running first-order analysis that
also keeps the second-order terms for `+ - *` (section 4.2). A "cap" is the largest bound the
generator may write for a routine.

## 3. The routines

Every routine is a pure sequence of VM opcodes over `Float` values. Operation counts are VM opcodes
(`Add Sub Mul Div Min Max Abs Neg`, comparisons, `Sqrt`, and the libm calls themselves); the Libm
column says how many of them are `Exp` or `Ln`. "T0" means no libm call is reached (bit-identical
across hosts), "T1" means one is (section 5).

### 3.1 Table

| Routine      | Formula (spec)                                                         | Operations              | Libm | Class | Cap (abs unless noted)                        |
| ------------ | ---------------------------------------------------------------------- | ----------------------- | ---- | ----- | --------------------------------------------- |
| `betaPair`   | $\mu=\alpha/(\alpha+\beta)$, $1-\mu=\beta/(\alpha+\beta)$              | 3                       | 0    | T0    | rel $2^{-51}$                                 |
| `betaVar`    | $\alpha\beta/((\alpha+\beta)^2(\alpha+\beta+1))$                       | 6                       | 0    | T0    | rel $2^{-49}$                                 |
| `shrink`     | O9, section 6                                                          | 17                      | 0    | T0    | $2^{-49}$                                     |
| `decay`      | $w(a)=\tfrac12+e^{-\lambda a}(w_0-\tfrac12)$                           | 45 (series), 10 (exp)   | 0, 1 | T0/T1 | $2^{-50}$; rel $2^{-49}$                      |
| `clampLogit` | clamp to $[\epsilon_w,1-\epsilon_w]$, then $\ln(w/(1-w))$              | 5                       | 1    | T1    | $2^{-46}$                                     |
| `sigmoid`    | $\sigma(s)$ and $1-\sigma(s)$                                          | 7                       | 1    | T1    | rel $2^{-49}$                                 |
| `sumC`       | Neumaier compensated sum, in the given order                           | 7 per term, 1           | 0    | T0    | see `pool`                                    |
| `pool`       | $s=\sum_G n_{\mathrm{eff}}(G)\bar\ell_G$, $W=\sigma(s)$                | $12n+13g+8$ (n sources) | n+1  | T1    | $n\cdot2^{-46}$                               |
| `nEff`       | $\lvert G\rvert/(1+(\lvert G\rvert-1)\rho_G)$                          | 4                       | 0    | T0    | rel $2^{-49}$                                 |
| `fusion`     | $r_f=(\sum\tilde r_i^{-1})^{-1}$, $\hat x_f=r_f\sum\tilde r_i^{-1}x_i$ | $17k+3$ (k inputs)      | 0    | T0    | $r_f$ rel $2^{-50}$; $\hat x_f$ rel $2^{-47}$ |
| `innovation` | $d^2=\lVert x-x_0\rVert^2/(\tilde r+p_0)$                              | $2k+3$                  | 0    | T0    | rel $2^{-50}$                                 |
| `slew`       | $\lVert\Delta x\rVert^2\le(v_{\max}\Delta t)^2$ (no `Sqrt`)            | $2k+3$                  | 0    | T0    | exact comparison                              |
| `cluster`    | $\lVert x_i-x_j\rVert^2\le\tau^2$, union-find, diameter                | $3k$ per pair           | 0    | T0    | exact comparison                              |
| `fracStep`   | $b=e^{-\ln 2/H}$; $R\leftarrow bR+h$, $N\leftarrow bN+1$               | 3, then 4 per event     | 1    | T1    | $b$ rel $2^{-49}$; $f$ abs $H\cdot2^{-50}$    |

The caps are the largest bounds the committed vectors produced, rounded up to a power of two (the
prototype's measured maxima: `betaPair` $2.2\times10^{-16}$, `betaVar` $8.9\times10^{-16}$ relative,
`shrink` $8.9\times10^{-16}$, `decay` $6.6\times10^{-16}$, `logit` $6.3\times10^{-15}$, `sigmoid`
$8.9\times10^{-16}$ relative, `pool` $8.0\times10^{-13}$ at $n=128$). The generator refuses to write
a case whose bound exceeds the cap, so a loose analysis cannot hide an error (section 4.3).

### 3.2 Beta mean, variance and the pair

`betaPair(alpha, beta)`: refuse unless both are finite, positive, and `alpha + beta <= 1e100` (so
`s*s*(s+1)` cannot overflow). Then `s = alpha + beta`, `p = alpha / s`, `q = beta / s`. The divisor
`s` is positive, so the `Div` quirk cannot arise. `betaVar` is `(alpha*beta) / ((s*s)*(s+1))`. The
refusal is `InvalidEpistemicState` (new, for values read from a record; section 3.10).

### 3.3 Decay toward 1/2 (spec 3.2)

With `tau` the policy half-life and `a` the age, both in milliseconds as Floats (integers below
$2^{53}$, so exact):

```text
hl = a / tau                      # half-lives; tau > 0 checked at policy load
if hl > 1100:  return (1/2, 1/2)  # exp(-x) is zero; decided before any product can overflow
x  = hl * LN2                     # LN2 is the double nearest ln 2 (3.4e-17 relative off, in the bound)
if x < 1/2:  em = expm1_series(-x); E = 1 + em;  h = 1/2 * (-em)
else:        E = Exp(-x);           h = 1/2 * (1 - E)
p' = E*p + h;   q' = E*q + h
```

`p' + q' = E(p+q) + 2h = 1` exactly, and both are sums of non-negatives, so neither loses digits.
The textbook form `1/2 + E*(p - 1/2)` loses the small side: for $w_0=1-10^{-6}$ and a short age its
complement carries an error near $5\times10^{-11}$ (the prototype's naive mutant fails 14 vector
cases by factors up to $4\times10^{7}$). `expm1_series(y)` is a Horner polynomial of 17 terms,
$y(1+y/2!+y^2/3!+\dots)$, on $|y|\le\tfrac12$ with coefficients $1/k!$ as doubles. Its truncation is
under $10^{-24}$ and is added to the bound. It needs no libm, so for ages below about 0.72
half-lives the whole routine is T0. The branch is decided by a T0 comparison, so every host takes
the same branch. At age 0 the result is bit-identical to the input pair (`E = 1`, `h = 0`).

The expensive part is the 17-term series, 35 opcodes per observation. At 128 sources that is about
4,500 opcodes per query, against about 1,700 for the pool itself. If the measured cost matters, I4-2
may drop the series and accept the cap-widened bound for $w$ near 0 and 1 (section 4.3 shows how the
bound moves); the design recommends keeping it.

### 3.4 Clamp, logit, sigmoid

`clampLogit(p, q)`: if `p < eps` use `(eps, 1 - eps)`; else if `q < eps` use `(1 - eps, eps)`. Then
`ell = Ln(p / q)`. One `Ln` of the ratio, not `Ln(p) - Ln(q)`: the difference cancels near
$w=\tfrac12$ (by the bound analysis, two libm logarithms near $-0.69$ leave up to $6\times10^{-16}$
absolute error; the ratio form leaves $1\times10^{-16}$ plus the libm term in $\lvert\ell\rvert$),
and the ratio is in $[\epsilon/(1-\epsilon),(1-\epsilon)/\epsilon]$, so `Div` and `Ln` are safe.
$\epsilon_w$ is the policy value (default $10^{-6}$), validated in $(0,\tfrac12)$.

`sigmoid(s)`: `E = Exp(-Abs(s))`; `den = 1 + E`; `big = 1/den`; `small = E/den`; return
`(big, small)` if `s >= 0` else `(small, big)` as $(W,1-W)$. The exponent is never positive, so
`Exp` cannot overflow; for $|s|>745$ it underflows to zero, which the VM allows (spec 8.1). $W$
rounds to exactly 1 once $s>36.7$; ordering is therefore done on $s$, never on $W$ (section 3.6).

### 3.5 Compensated sum, pooling and $n_{\mathrm{eff}}$ (spec 3.1)

`sumC` is Neumaier's variant of Kahan summation: for each term `t = s + x`; if `Abs(s) >= Abs(x)`
`c += (s - t) + x` else `c += (x - t) + s`; `s = t`; result `s + c`. Seven opcodes per term. Bound:
$2u\lvert S\rvert+2n^2u^2\sum\lvert x_i\rvert$ plus the input bounds, so the vector of 128
mixed-sign logits of size 13.8 that cancels to $-1.8\times10^{-9}$ is exact to its own rounding. A
plain `Add` loop is bounded by $n u\sum\lvert x_i\rvert$, about $2.5\times10^{-11}$ here, which is
more than the $7\times10^{-12}$ guard of section 5.3.

The summation order is the **canonical order**: groups by stable group ID, sources within a group by
stable source ID, as spec 5.3 says "stable source/event-ID order". The query layer sorts; the
routine sums what it is given. A permutation of arrival order then changes nothing bit for bit; a
change of the IDs changes the result only within the cap.

`pool(groups, rho)` for each group $G$: for each member run `clampLogit`; $\bar\ell_G$ is
`sumC(ells) / g`; $n_{\mathrm{eff}}$ is `g / (1 + (g-1)*rho)` (the divisor is at least 1; for
$\rho=1$ it equals $g$ and the result is exactly 1, for $\rho=0$ it is exactly $g$); the group's
term is `n_eff * mean`. $s$ is `sumC(terms)`; the constant $\operatorname{logit}(p_0)$ is exactly
zero at the spec's $p_0=\tfrac12$ and is omitted. Then `sigmoid(s)`. Checks: every $\rho$ finite in
$[0,1]$; $g\ge1$; `finite(s)`.

The spec's closed forms are inside the bound but not exact. One source at 0.8 and an independent
source at 0.2 (the spec's "restores 0.5") give $s=2.8\times10^{-16}$ and $W=0.5+7\times10^{-17}$. A
test that asserts `W >= 0.5` or `W == 0.5` on that vector is a test of rounding. It is a
**marginal** decision (section 5.3), and the committed vector says so.

### 3.6 Ties and `score_tolerance` (spec 5.3)

The comparison of candidates is on $s$. Spec 5.3 writes the tolerance as an absolute difference in
$W$-space (default $10^{-9}$). $W$-space saturates: $W$ rounds to 1 at $s>36.7$, and a $10^{-9}$
tolerance already ties everything above $s\approx20.7$ (a $10^{-9}$ tolerance equals a log-odds
tolerance of 0.98 there, and 72 at $s=25$). Fifteen independent supporters at 0.8 reach that, so
`truest` would abstain between a candidate with 10 independent supporters and one with 30. The
cross-host noise it was meant to absorb is of the order $n\cdot2^{-44}\approx7\times10^{-12}$ in
$s$, constant in the score.

**Recommendation (A14): `score_tolerance` is an absolute difference of $s$, default $10^{-9}$.** It
is over $100\times$ the guard at 128 sources ($7.3\times10^{-12}$), it never saturates, and at
$W\approx\tfrac12$ it is $2.5\times10^{-10}$ in $W$, tighter than the spec's. `min_confidence` stays
in $W$-space (it is a warrant threshold in $[0,1]$), tested as `at_least(W, W', theta)`:
`W >= theta` when `theta <= 1/2`, else `W' <= 1 - theta` (`1 - theta` is exact for
$\theta\in[\tfrac12,1]$), so a threshold near 1 keeps full resolution through the pair.

Alternative: keep $W$-space and test the saturation (no amendment). Cost: the user guide must say
that confident candidates tie, and the policy hash is unchanged. The recommendation changes the
meaning of an existing key, so it belongs in `vagent-policy-3`, which I4-1a writes once.

### 3.7 Scalar fusion and the innovation gate (spec 5.2, 5.3)

`fusion`: for each input `inv_i = Max(w_i, floor) / r_i`, which is $1/\tilde r_i$ in one `Div`
(`r_i > 0` checked; `finite(inv_i)`). `sum = sumC(inv)`; `r_f = 1 / sum` (positive, so safe);
`x_f = r_f * sumC(inv_i * x_i)`. Mixed-sign $x_i$ are why the second sum is compensated. The refusal
on non-finite is `EpistemicNumericFault`.

`innovation`: `d2 = (sum of dx_k*dx_k in index order) / (r / Max(w, floor) + p0)`. The divisor is
positive. The decision is `d2 <= g_k`. `slew` has no `Sqrt` and no division by $\Delta t$: it tests
`sumsq(dx) <= (vmax*dt)*(vmax*dt)` and requires `dt > 0` first, so duplicate and out-of-order fixes
never reach arithmetic. Both are T0 given bit-equal inputs.

### 3.8 Clustering and the diameter rule (spec 5.3)

Adjacency of two Float vectors is $\lVert a-b\rVert^2\le\tau^2$ with $\tau^2$ computed once by `Mul`
(`finite` checked). No `Sqrt`. Pairs are tested in canonical observation-ID order $(i<j)$ into a
union-find whose representative is the smallest ID, so components and their order are deterministic.
The diameter rule tests every pair in a component against $D^2$, with the same form; a component
over $D^2$ is replaced by one singleton per original observation, all marked diameter-ambiguous.
This is O(N²) in pair tests (about $5\times10^5$ at 1,024 candidates, the spec's accepted naive
cost; the fuel charge is I4-9's). It is T0: **the cluster decision has no margin**, because every
host computes the same bits from the same stored observations.

### 3.9 The event-indexed fraction (I3-2 section 3.2, reused by O10)

`b = Exp(-(LN2 / H))` from the integer `H`; the cap `1/(1-b)` and the rescale-then-update order are
exactly I3-2's (R3-9). One event: `R <- b*R + hit; N <- b*N + 1` after the rescale. I3-2a emits it
parameterised by its two state cells, `b` and the evidence floor (plan, ML minor 5); I4-8 calls the
same routine with `gate_rejected` and `gate_events`. The fraction `f = R/N` divides only when
`N >= min_events`. Cross-host error: `b` differs by at most $5\times10^{-16}$, and the fraction by
at most $\delta b/(1-b)\approx6.6\times10^{-16}H$, so $4\times10^{-14}$ at $H=64$ (section 5.3 turns
this into a guard).

### 3.10 Domain checks and refusals

Spec 8.1 splits invalid static values (the checker) from invalid dynamic ones (a typed refusal
before arithmetic). The numbers below arrive from three places, so three refusals; the first two
exist, the other two are proposed for T1's vocabulary:

| Source of the bad number                                   | Refusal                  | Checked in                             |
| ---------------------------------------------------------- | ------------------------ | -------------------------------------- |
| a policy field (`c`, `epsilon_w`, `rho` of a group, `tau`) | `InvalidEpistemicPolicy` | load and every dynamic update          |
| a variance                                                 | `InvalidVariance`        | ingestion, rechecked before fusion     |
| a record (`alpha`, `beta`, an age, a Float value)          | `InvalidEpistemicState`  | the routine entry (proposed)           |
| a non-finite or overflowing intermediate                   | `EpistemicNumericFault`  | after the routine's last op (proposed) |

`EpistemicNumericFault` carries the routine name and is the typed face of the VM's `Precondition`
contract failure (`vortex_vm.cpp:445-451`); the query layer maps it after scope cleanup, as spec 8.1
requires. A refusal never clamps NaN or infinity into evidence. The `Exp` underflow to a finite zero
is not a fault.

Domain list (spec 8.1, made executable): $\alpha,\beta$ finite and positive with sum at most
$10^{100}$; $c\ge0$ finite; $0<\epsilon_w<\tfrac12$; $0<\epsilon_{\mathrm{floor}}\le1$; $\rho$
finite in $[0,1]$; half-life $\tau>0$; age $a\ge0$ finite; variance finite and positive; thresholds
$\theta\in[0,1]$; $\tau_{\mathrm{score}}\ge0$; cluster tolerance positive and
$D\ge\tau_{\mathrm{cluster}}$; $\Delta t>0$ for slew.

## 4. Reference vectors

### 4.1 The reference program

`tools/vagent-epistemic-reference.py` (new; a developer tool run with `uv run --with mpmath`,
**not** a build dependency) is written from the formulas of section 3 before any IR exists, and is
reviewed as a design artefact by ML and NUM. It uses `mpmath` at 60 digits for `exp`, `log` and
`sqrt`. It imports no `math`, `numpy`, `cmath` or `scipy`; a test greps the file for them (the
prototype's `check_no_libm.sh` is the regular expression; the real test is a gtest reading the file
path from a compile-time macro). `random.Random(19)` supplies the seeded inputs and no input passes
through a floating `log` (the generator draws exponents and raises 2 to them in mpmath).

The program contains two versions of each routine. `exact_*` is the spec formula in mpmath and has
no rounding model. The *sequence* version replays the opcode sequence of section 3 and carries three
things per value: the ideal value, an absolute error bound, and (when no libm call is involved) the
value rounded to 53 bits at every step. The generator asserts the sequence agrees with the exact
formula ($10^{-45}$ for `shrink`, $10^{-13}$ for `decay` and $10^{-9}$ for `pool`, where the input
pairs are rounded doubles), so a transcription error in the sequence is caught at generation.

### 4.2 The error bound

Each operation updates the bound $B$ of its result $z$, from the bounds of its operands:

| Operation | Bound on the result                                                                             |
| --------- | ----------------------------------------------------------------------------------------------- |
| `x + y`   | $B_x+B_y+u(\lvert z\rvert+B_x+B_y)$                                                             |
| `x * y`   | $\lvert x\rvert B_y+\lvert y\rvert B_x+B_xB_y$, plus $u(\lvert z\rvert+\cdot)$                  |
| `x / y`   | $(\lvert x\rvert B_y+\lvert y\rvert B_x)/(\lvert y\rvert(\lvert y\rvert-B_y))$, plus $u(\cdot)$ |
| `Sqrt x`  | $B_x/(\sqrt{x}+\sqrt{x-B_x})$, plus $u(\cdot)$                                                  |
| `Exp x`   | $e^x(e^{B_x}-1)$, plus $L(\cdot)$                                                               |
| `Ln x`    | $-\ln(1-B_x/x)$, plus $L(\cdot)$                                                                |

A division whose divisor the bound cannot separate from zero aborts generation. A branch whose test
is continuous across its edge (the `shrink` three-way choice) adds $8u$ when the result is within
$64u$ of $\tfrac12$, which covers a host taking the other side at the edge. The compensated sum uses
the Higham-style bound of section 3.5. This is a first-order running bound with the second-order
terms kept where the operation is `+ - *`; it is not an interval proof, and a test of the bound is
the perturbation run of section 4.5.

### 4.3 Tolerance, per routine, from the operation count

The tolerance of a vector is its own written bound, so it is derived from that case's operation
sequence and operands. Three more rules fix what the bound may be:

1. **Caps.** The table in 3.1 gives a cap per routine. The generator fails if a bound exceeds the
   cap. A cap is the largest bound seen, rounded up to a power of two (1.3 to 2.3 times it).
1. **The comparison.** A test accepts `got` when
   $\lvert\mathrm{got}-\mathrm{ref}\rvert\le B+u\lvert\mathrm{ref}\rvert+2^{-1073}$, where `ref` is
   the 50-digit decimal rounded to the nearest double by `std::strtod` (the
   $u\lvert\mathrm{ref}\rvert$ term pays for that rounding; the last term is two denormal steps,
   because relative accuracy has no meaning below $2^{-1022}$).
1. **The pool cap scales with the number of sources**: $n\cdot2^{-46}$, so a raised `max_sources`
   raises the guard in step with it (section 5.3).

For orientation, the relative bound of one correctly rounded operation is $1.1\times10^{-16}$, a
libm call $4.4\times10^{-16}$, `betaVar` (six operations) $8.9\times10^{-16}$ and `decay` about
$9.5\times10^{-16}$ in the series branch. A routine with $k$ rounded operations and $m$ libm calls
has a bound near $(k+4m)u$ times its condition number; the generator computes the exact per-case
number.

### 4.4 The file

`tests/samples/vagent/epistemic_vectors.tsv`, one case per line, TAB-separated `key=value` fields,
so the C++ test needs nothing beyond `std::strtod` and string splitting. A header line starts with
`#vagent-epistemic-vectors` and records `version`, `seed`, `dps`, the unit roundoff, the libm
assumption `libm_relative=0x1p-51`, the `mpmath` version and the case count.

```text
beta_mean/001  beta_mean  random
  in.alpha=0x1.0000000000000p+3   in.beta=0x1.0000000000000p+1
  out.mu=0.8   out.one_minus_mu=0.2                      (50 significant digits)
  bound.mu=1.776e-16   bound.one_minus_mu=4.441e-17
  bits.mu=0x1.999999999999ap-1  bits.one_minus_mu=0x1.999999999999ap-3
pool/001  pool  closed
  in.w=0x1.999999999999ap-1;0x1.999999999999ap-3   in.rho=0x1p+0;0x1p+0
  out.s=0.000000000000000277555756156289158...   out.W=0.50000000000000006938893903907228955...
  bound.s=3.3e-15   bound.W=1.047e-15
  decision.name=min_confidence  decision.threshold=0x1p-1  decision.distance=6.939e-17
  decision.guard=1.137e-13  decision.class=marginal
```

(Each case is one physical line; it is wrapped here.) Inputs are hex-float bit patterns so
`std::strtod` reads them back exactly. Outputs are decimals so no float format limits the reference.
Lists are `;`-separated, groups `,`-separated inside a list. A `kind` of `random`, `edge`, `closed`
or `libm` says why the case exists. `bits.*` exists only for T0 outputs (section 5.1) and holds the
exact binary64 result.

The file holds, per routine: seeded random cases, the edges (alpha near $10^{12}$, a logit at the
clamp, an exponential that underflows, 128 mixed-sign logits, ages of 0 and $10^{300}$), the spec's
closed forms, and 40 libm probes for each of `Exp` and `Ln`. `--check` regenerates into a temporary
file and compares byte for byte when the recorded `mpmath` version matches, and to 45 digits
otherwise. One command reproduces it (`vagent-epistemic-reference.py FILE`), and the prototype's
output is reproducible (two runs are byte-equal).

### 4.5 How a test reads it, and the prototype's evidence

The C++ test `VagentEpistemicVectors` parses each line, runs the routine through the VM graph on a
bare `WorkerContext` (the I4-2 path), and asserts: the tolerance rule of 4.3 on every `out.*`; exact
bit equality on every `bits.*`; the cap on every `bound.*`; for a `decision.*`, either the outcome
(class `firm`) or the reported `marginal` flag (class `marginal`), never the outcome of a marginal
case; and, for the libm probes, at most 2 ulp from the reference on this host.

The prototype stands in for that test in Python (`ref/check_vectors.py`) against 481 generated
cases, in three modes:

| Mode      | What it runs                                                         | Result                                              |
| --------- | -------------------------------------------------------------------- | --------------------------------------------------- |
| `host`    | the sequences in IEEE doubles with this host's libm                  | 0 failures; largest error 0.66 of its bound         |
| `perturb` | every `Exp`/`Ln` result moved 0, 1 or 2 ulp at random (another libm) | 0 failures; T0 bits unchanged                       |
| `mutant`  | naive forms: spec `lcb`, textbook decay, `Ln p - Ln(1-p)`, plain sum | rejected: `decay` 14, `logit` 5, `shrink` 184 cases |

The `shrink` mutant fails by design (it is the spec's `lcb`, which section 6 replaces). The
`perturb` mode is the evidence that a bound holds when libm differs; the `mutant` mode is the
evidence that the vectors can fail. The generator's `bits` values for `betaPair`, `betaVar`,
`shrink`, `decay` (series branch), `fusion` and `innovation` are matched exactly by Python doubles,
which also confirms that 53-bit round-to-nearest in mpmath reproduces IEEE `+ - * / sqrt` for these
sequences.

## 5. The cross-host contract (D1)

### 5.1 Statements

1. **Same host, same binary: bit-equal.** A recomputation from the same stored inputs returns the
   same bits as the incremental result. The sum order is canonical, no reduction is parallel, and a
   query reads the committed state. I5-4's replay test compares doubles by `memcmp`, not by
   tolerance (ML M5 part 3).
1. **Stored state is authoritative.** Alpha, beta, weights, the normaliser and the fractions $R,N$
   are stored as bits and are never recomputed on resume, upgrade or migration; a translator copies
   bits (T1's digest compares doubles by bit pattern, so $-0.0\ne0.0$). Hosts therefore diverge only
   on **new** computation.
1. **T0 results are bit-identical on every IEEE-754 host** that uses binary64 with
   round-to-nearest-even, no extended precision and no contraction across VM opcodes. T0 routines:
   `betaPair`, `betaVar`, `shrink`, `decay` below 0.72 half-lives, `sumC` and `nEff` of bit-equal
   inputs, `fusion`, `innovation`, `slew` and `cluster` of bit-equal inputs. The vector file's
   `bits.*` assert this exactly, so a host that fuses a multiply-add or uses x87 fails a test rather
   than drifting. I4-2 adds `static_assert(FLT_EVAL_METHOD == 0)` beside the math opcodes.
1. **T1 results agree within the cap.** `decay` beyond 0.72 half-lives, `clampLogit`, `sigmoid`,
   `pool` and `fracStep` depend on `Exp` or `Ln`; two hosts differ by at most twice the cap. The
   libm probes check the 2 ulp assumption on each host that runs the suite; a host that fails them
   is unsupported for the cross-host claim, and the test names the function and the ulp.
1. **Decisions** use section 5.3.
1. **Rejected alternative.** A VM-owned `Exp` and `Ln` would make T1 bit-identical and costs a
   rewrite of accepted I1 work (the plan records it in 7.2). Not proposed.

### 5.2 Which values are T1, concretely

A source with no `decay` key and a Beta mean or `lcb` is T0 from the counts to the pooled logit's
inputs; the `Ln` in `clampLogit` is the first libm call on its path. The fused point and the
innovation gate are T0 for such a source, and T1 only through $w$ when `decay` is set and the
reading is older than 0.72 half-lives. The cluster, slew and diameter decisions are T0 always.

### 5.3 Decision margins

A decision compares a computed value $x$ with a threshold $\theta$. Let $\mathrm{cap}$ be the
routine's cap at this input size. Two hosts' values of $x$ differ by at most $2\,\mathrm{cap}$. A
decision is **firm** when $\lvert x-\theta\rvert>G$ with $G=4\,\mathrm{cap}$ (twice the largest
possible disagreement, with a factor 2 spare), and **marginal** otherwise. A marginal decision is
still made, deterministically, on this host; it is reported with `marginal: true`, and another host
may decide it differently.

| Decision                                          | Value compared      | Guard $G$                              | Class when T0       |
| ------------------------------------------------- | ------------------- | -------------------------------------- | ------------------- |
| `min_confidence`: $W$ against $\theta$            | $W$ (or $1-W$)      | $n\cdot2^{-44}$ absolute ($n$ sources) | n/a                 |
| tie: $\lvert s_{\max}-s_C\rvert$ against $\tau_s$ | log-odds difference | $(n_a+n_b)\cdot2^{-44}$ absolute       | n/a                 |
| health fraction against a threshold               | $f=R/N$             | $H\cdot2^{-48}$ absolute               | n/a                 |
| innovation $d^2$ against $g_k$                    | $d^2/g_k-1$         | $2^{-46}$ relative                     | firm when $w$ is T0 |
| slew, cluster adjacency, diameter                 | exact comparison    | 0                                      | always firm         |

The guards are $4\times$ the caps of the table in 3.1: $n\cdot2^{-46}\cdot4=n\cdot2^{-44}$, and the
fraction's $H\cdot2^{-50}\cdot4=H\cdot2^{-48}$. At 128 sources the pool guard is
$7.3\times10^{-12}$, nine times the measured worst case of $8\times10^{-13}$. At $H=64$ the fraction
guard is $2.3\times10^{-13}$, five times the $4\times10^{-14}$ derived in 3.9. The innovation guard
is $4\times$ the sum of the `innovation` cap ($2^{-50}$) and the `decay` cap ($2^{-49}$),
$1.1\times10^{-14}$, rounded up to $2^{-46}$.

Rules for the vectors and tests (ML M5 parts 4 to 6):

- Every vector that feeds a decision carries `distance`, `guard` and `class`. The generator keeps a
  case only if `distance > 4*guard` (firm, with its outcome) or `distance <= guard` (marginal, with
  no outcome). A case between the two is dropped.
- A test never pins the outcome of a marginal vector. It pins the `marginal` flag.
- An exact tie built from bit-identical inputs in the same order is exact on every host, because the
  same operations run on the same bits. Such a vector is class `firm`, `distance` 0, `kind=tie`.
- The "about 64 events" wording of I3-2a is correct and stays; no test pins a count on a boundary.
- The user guide (F1) says: a decision within about $10^{-11}$ in score, or $10^{-13}$ in a health
  fraction, of its threshold may differ between hosts, and the result says so.

The result schema carries the flag: a Bool `marginal` on the decision record of `truest`, `ranked`
and the derived health state (I4-5, I4-7). I4-D2's result ABI owns the field; question Q3.

## 6. O9: `lcb` at an uninformative prior

**The problem, computed.** A fresh Beta(1,1) source has $\mu=\tfrac12$, $\sqrt v=0.289$. The spec's
$\max(0,\mu-c\sqrt v)$ is $0.356, 0.211, 0.009, 0$ for $c=0.5, 1, 1.7, 2$, so its logit is negative:
the source votes against its own claim by being new, and three independent fresh sources with $c=1$
pool to $W=0.0189$, below one source alone. `min_confidence` $0.5$ then rejects every candidate
supported only by fresh sources.

**Options**, with the same four sources at $c=1$ ($W$ is the pool of three equal independent
sources):

| Option                                          | Fresh (1,1)     | Mature $\mu=0.875$ | Mature $\mu=0.125$ | Cost                                                                |
| ----------------------------------------------- | --------------- | ------------------ | ------------------ | ------------------------------------------------------------------- |
| spec: $\max(0,\mu-c\sqrt v)$                    | 0.211 (W 0.019) | 0.809              | 0.059              | fresh = veto; documented inversion (option D)                       |
| A: floor at 1/2: $\max(\tfrac12,\mu-c\sqrt v)$  | 0.5 (W 0.5)     | 0.809              | **0.5**            | raises a known-unreliable source to neutral and erases its evidence |
| B: apply the penalty only when $\mu\ge\tfrac12$ | 0.5             | 0.809              | 0.125              | an uncertain bad source is as hostile as a certain one              |
| **C: shrink toward 1/2, never past it**         | 0.5 (W 0.5)     | 0.809              | 0.191 (W 0.013)    | one extra branch                                                    |

**Recommendation: C.** In the pair form of `shrink`:

```math
w_{\mathrm{eff}}=\tfrac12+\operatorname{sgn}\!\left(\mu-\tfrac12\right)
 \max\!\left(0,\ \left|\mu-\tfrac12\right|-c\sqrt v\right).
```

Uncertainty moves a warrant toward ignorance and never past it or away from it. It equals the spec's
$\max(\tfrac12,\mu-c\sqrt v)$ for $\mu\ge\tfrac12$, so the amendment changes the spec only where the
spec's value is below $\tfrac12$. It is continuous, odd about $\tfrac12$ (matching the property
$\operatorname{logit}(w)$ is odd, and decay toward $\tfrac12$), monotone in $c$, and a neutral
source contributes exactly zero. A source known to be wrong 87.5% of the time stays opposition
($0.191$ at $c=1$, $0.257$ at $c=2$), weaker as its evidence is thinner. Option A hides that
evidence; B ignores uncertainty on one side. D, keeping the text, needs the user guide to say that
`lcb` with a symmetric prior inverts the effect of support; it is cheaper but contradicts the
"conservative" purpose the spec gives `lcb`.

**Sequence** (17 opcodes, one of them `Sqrt`; T0): from `betaPair` and `betaVar`: `t = c * Sqrt(v)`;
`dd = p_mu - q_mu`; if `dd > t + t` return `(p_mu - t, q_mu + t)`; else if `-dd > t + t` return
`(p_mu + t, q_mu - t)`; else `(1/2, 1/2)`. Both entries are formed without subtracting from 1.
**Amendment A13** replaces "use $\max(0,\mu-c\sqrt v)$" in 3.4 and the matching sentence in 3.4's
score composition. The checker rule (`lcb` only with `beta_reputation`) is unchanged.

**Tests (I4-2).** A fresh source at any $c$ contributes exactly 0 to $s$; three fresh sources give
$W=0.5$; the shrink is monotone in $c$ and never crosses $\tfrac12$; a source at $\mu=0.125$ keeps a
negative logit. The vectors hold all of these (82 `shrink` cases).

## 7. O10: the gate-rejection rule

### 7.1 Why the streak fails

The spec's default is three consecutive rejections for SUSPECT and five for FAULTED. At an iid
false-alarm rate $p$ the expected number of events to the first run of $r$ is $(1-p^r)/((1-p)p^r)$
(renewal formula, `nums.py`):

| $p$  | run | events | at 100 Hz |
| ---- | --- | ------ | --------- |
| 0.05 | 3   | 8.4e3  | 84 s      |
| 0.05 | 5   | 3.4e6  | 9.4 h     |
| 0.01 | 3   | 1.0e6  | 2.8 h     |
| 0.01 | 5   | 1.0e10 | 2.8e4 h   |

FAULTED latches for life (owner Q2). A healthy sensor behind a 95% gate is latched about every 9
hours at 100 Hz. Spec 5.2 itself says the nominal $p$ does not survive adaptive inflation or model
mismatch, so $p$ is a floor.

### 7.2 The rule

Rejections drive the **same event-indexed exponentially weighted fraction as I3-2's health** (the
routine of 3.9, one function), on separate state:

- State per source and predicate calibration record: Doubles `gate_rejected` ($R_g$) and
  `gate_events` ($N_g$), absent until first written. (Per record because a gate is per predicate;
  the latch below is on the profile. Question Q4.)
- An **event** is one reading that at least one enabled gate **tested** (verdict `ACCEPTED` or
  `REJECTED`). `UNTESTED` (cold start, no reference), duplicates, and readings refused before gating
  are not events. Gates run once, in the ingestion turn (spec 5.1), so no mark or dedup log is
  needed: a duplicate event ID never reaches the gate.
- A **hit** is a rejection by any enabled gate (innovation or slew).
- Update: the I3-2 rescale, then $R_g\leftarrow bR_g+\mathrm{hit}$, $N_g\leftarrow bN_g+1$,
  $b=2^{-1/H}$. A note-style batch update is not needed (one verdict per event).
- $f_g=R_g/N_g$ once $N_g\ge$ the minimum evidence weight.
- **SUSPECT** when $N_g\ge N_S$ and $f_g\ge\theta_S$: displayed, discrete inflation 4 (spec 5.1).
- **FAULTED** when $N_g\ge N_F$ and $f_g\ge\theta_F$: latches (reason code `gate_fraction`, origin
  `evidence`, A6). Only an authorised `reset` unlatches it. A reading accepted afterwards lowers
  $f_g$ and clears nothing, which is the spec's "accepted measurements clear only the streak"
  replaced by "the fraction decays".
- The displayed state is the worst of the health-fraction state, the gate state and the reasons; the
  inflation is the maximum of their factors, applied once.

Settings (actor-wide, as A2 says of health thresholds), with defaults and domains:

| Setting (`vagent.` prefix)          | Default | Domain                                     |
| ----------------------------------- | ------- | ------------------------------------------ |
| `epistemic_gate_half_life`          | 64      | Int in [1, 10^6] events                    |
| `epistemic_gate_suspect_fraction`   | 0.30    | finite, in (0, 1\]                         |
| `epistemic_gate_suspect_min_events` | 16      | [1, floor(0.9/(1-b))] = 83 at H = 64       |
| `epistemic_gate_fault_fraction`     | 0.70    | finite, in (`suspect_fraction`, 1\]        |
| `epistemic_gate_fault_min_events`   | 48      | \[`suspect_min_events`, floor(0.9/(1-b))\] |

`min_events` is the weight $N$ at which $f_g$ starts to decide (I3-2's meaning): at $H=64$ the first
reading that can be SUSPECT is the 18th and the first that can latch is the 68th. The half-life is
**twice** I3-2's health default of 32, and the FAULTED threshold is 0.70: both are what the proof
below needs (a window twice as long halves the effect of correlated runs, section 7.4).

### 7.3 Reuse and what differs from the health fraction

Same: the update, the rescale, the `min_events` meaning, the read-time rescale. Different: the
nominal rate (a content refusal rate is about 0; a gate rejection rate is the gate's false-alarm
rate), the minimum evidence for FAULTED, `N` counting only tested events, and a latch (I3-2's health
fraction never latches). The `reset` zeroes $R_g,N_g$ with the rest of the epoch (I3-2 6.4); a new
incarnation inherits them as it inherits the health fraction. I3-2D must keep its routine free of
the hard-wired profile field names (plan, ML minor 5); this design adds no requirement beyond that.

### 7.4 The proof

**Claim.** If, for a stationary healthy source, the per-event rejection probability is at most $p$
and rejections are independent across events, then the probability that $f_g\ge\theta$ at any one
event is at most

```math
\inf_{t>0}\ \exp(-t\theta W)\prod_i\bigl(1-p+p\,e^{t w_i}\bigr),\qquad
w_i=b^i,\quad W=\sum_i w_i=\frac{1}{1-b}.
```

*Proof.* $f_g\ge\theta$ is $\sum_i w_i(X_i-\theta)\ge0$ with $X_i$ independent Bernoulli of mean at
most $p$ (a larger mean only raises the probability, so take $p$). For $t>0$, Markov's inequality on
$e^{t\sum w_iX_i}$ gives $P\le e^{-t\theta W}\prod_i E\,e^{t w_iX_i}$, and each factor is
$1-p+pe^{tw_i}$. The infimum over $t$ is attained by a convex one-dimensional minimisation
(`gate_bound.py`, golden-section on 3,840 terms; the tail beyond them has weight $2^{-60}$ and moves
the exponent by under $10^{-17}$). $\square$

The **union bound** over events then gives the false-latch probability per device-year as the
per-event bound times the events per year: 3.16e9 at 100 Hz, 3.16e8 at 10 Hz, 3.16e7 at 1 Hz. The
bound holds for any dependence between events *only* in the weak form $P\le p/\theta$ (Markov on the
mean of $f_g$), which is 0.14 at $p=0.10$; so the strong figures need the independence assumption
stated in 7.5, and the stress model of 7.4.2 shows how much correlation they tolerate.

**7.4.1 Independent events, $H=64$, $\theta_F=0.70$, per device-year at 100 Hz.**

| $p$ (true rejection rate) | per event | per device-year |
| ------------------------- | --------- | --------------- |
| 0.05                      | 3.5e-123  | 1.1e-113        |
| 0.10                      | 4.7e-86   | 1.5e-76         |
| 0.15                      | 2.0e-64   | 6.3e-55         |

**7.4.2 Correlated runs.** Model: events fall into independent blocks of $m$ consecutive events that
reject together, each block with probability $p$ (a stress model of a rejection followed by up to
$m-1$ correlated ones, not a worst case over all dependences). The weights become
$w_j=b^{mj}(1-b^m)/(1-b)$ and the same bound applies.

| $p$  | $m=1$   | $m=2$   | $m=4$   | $m=6$   | $m=8$  |
| ---- | ------- | ------- | ------- | ------- | ------ |
| 0.05 | 1e-113  | 1.9e-52 | 7.7e-22 | 1.2e-11 | 1.5e-6 |
| 0.10 | 1.5e-76 | 6.9e-34 | 1.5e-12 | 1.9e-5  | 6.8e-2 |
| 0.15 | 6e-55   | 4.5e-23 | 3.8e-7  | 7.6e-2  | 34     |

Entries are per device-year at 100 Hz (values above 1 mean the bound says nothing). At the shorter
health half-life of 32, $p=0.10$, $m=4$ gives 6.8e-2 per year, which is why the gate window is 64.

**7.4.3 Start-up.** A fresh profile or a reset has fewer terms. The union over the first 1,500
events with $N_F=48$ is $9.2\times10^{-30}$ per start at $p=0.10$, $m=1$ and $2.3\times10^{-7}$ at
$p=0.10$, $m=4$.

**The accepted rate.** **At most $10^{-6}$ false FAULTED latches per device-year at 100 Hz** (a
fleet of $10^4$ such devices sees one per hundred years), proved for the region **$p\le0.15$ and
$m\le4$ (bound $3.8\times10^{-7}$) or $p\le0.05$ and $m\le6$ ($1.2\times10^{-11}$)**, which covers a
95% gate with a factor of 3 of slack in the rejection rate. At 10 Hz and 1 Hz the figures are 10 and
100 times smaller. Outside the region the rate is not claimed, and a source rejected that often is
not obviously healthy. The figure is a fleet-policy number the lead confirms (question Q1); the
bound does not depend on it.

**What the rule detects.** A source whose every reading is rejected reaches SUSPECT at the 18th and
FAULTED at the 68th event from a clean start; from $f_g=0.05$ it takes 29 and 107 events (events,
not seconds: 0.3 s and 1.1 s at 100 Hz, 107 s at 1 Hz). A source that rejects half its readings
stays SUSPECT (inflated, still used). The stationary spread of $f_g$ at a true rate $r=0.8$ is
0.029, so a source rejecting 80% latches within a few dozen events.

### 7.5 What the figure assumes, and the common-cause guard

The bound needs three things, which the test names: independent events (or runs under 4 or 6); a
per-event false-alarm rate at most the stated $p$ (gate quantile at most 0.05, so $p\le0.15$ allows
three times the nominal rate); and a reference that is not itself wrong. The last fails by
construction when the **reference** drifts: every source then rejects together, each reaches
$f_g\ge\theta_F$, and the rule latches the whole predicate. **Recommended guard (Q2):** if at least
$\max(2,\lceil n_t/2\rceil)$ of the $n_t$ sources tested on one predicate key have $f_g\ge\theta_S$,
write no latch this turn and place a degrade-kind reason `reference_suspect` on those sources
instead. Spec 5.2 says a rejection "establishes inconsistency, not which sensor is wrong"; the guard
is that sentence made executable. It costs one count per ingestion turn and removes the case the
per-source bound cannot cover.

### 7.6 Smoke test of the implementation

Zero latches in $n$ events bounds a rate only by about $3/n$: $10^7$ simulated events at $p=0.05$
prove "under 3e-7 per event", one latch per 9 hours at 100 Hz, and cannot tell a 5-streak (1 in
3.4e6) from the proved rule. Simulation is therefore a **smoke test of the implementation against
the bound**, run where the bound is not vanishing. At $p=0.60$, $\theta_F=0.70$, $H=64$ the Chernoff
bound is $1.7\times10^{-2}$ per event; the prototype's Monte Carlo (4,000 chains, 3,600 events each
after a 400-event warm-up, seed 19) gives $2.3\times10^{-3}$. The C++ test runs that configuration
through the IR routine and asserts the observed frequency is at most the bound and at least
bound/100 (so it is not vacuous). Companion tests: the same run at $p=0.55$ (bound
$1.3\times10^{-4}$, observed $1.6\times10^{-5}$); runs of rejections arriving as blocks (7.4.2); the
common-mode drift of 7.5; a rejection trains nothing and blames no sensor (spec 5.2-15).

## 8. `decay`, O6 and the remaining numerics

### 8.1 What `decay` means

`decay` is the **predicate's freshness half-life**, a duration (`decayMs`, built in I3-1a,
`epistemic_policy.cpp:45`). Nothing is stored per source: the calibration record's "decay rate" in
spec 2.1 is reserved and absent, and v1 reads the policy. A per-source rate is additive later (a
Double on the record that overrides the policy when present), and needs no key table change now.

- Present: $\lambda=\ln2/\tau$, evaluated as $x=(a/\tau)\ln2$ in milliseconds (3.3).
- **Absent: $\lambda=0$.** No decay: a reading keeps its full warrant until it crosses `max_age`
  (mandatory, `Need::Always`), then is ineligible. The sharp edge is the point of "static facts".
  The user guide says a source with no `decay` has a cliff, not a ramp.
- Age uses the observation's mapped acquisition time. The mapping uncertainty widens eligibility
  (`max_uncertainty`, built) but is not added to the age: the warrant it would change is bounded by
  $\lambda u_t\lvert w-\tfrac12\rvert$, which is recorded here as a stated limit, not corrected.
- A decayed warrant is never written back: age does not change $\alpha$ or $\beta$ (spec 3.4).

### 8.2 O6: profile variance, $t_\star$ and the reference

**Profile variance: drop for v1 (proposed amendment A15; row 5.2-6 becomes Deferred).** Spec 5.2
allows "an explicitly supplied profile variance only for a stationary observation model whose schema
permits it". Keeping it needs a schema flag declaring the model stationary, an audit that nothing in
I3-3 or I4 performs, a grant for who may set a variance on `d.trust`, and a rule for how a reset and
a calibration epoch treat it. Dropping it needs none: every consensus input carries `with variance:`
(the signed field I3-2's `MissingVariance` already checks), and a datasheet sensor gets its variance
from the adapter that signs the observation. **Consequence:** a sensor that publishes no covariance
needs an adapter supplying one; there is no operator-side default, and `MissingVariance` is the only
answer to its absence. A stationary profile variance can return later as a separate proposal with
those three pieces.

**$t_\star$.** A Float in the actor time basis (milliseconds), declared once in the query snapshot.
A reading is eligible for consensus when its estimate time equals $t_\star$ to within
`alignment + mapping uncertainty`; the arithmetic is a `Sub`, an `Abs` and a comparison, T0. No
averaging across times (spec 5.2's moving-target example is a test).

**The reference.** `(x0, p0)` with $p_0$ finite and positive, for the same key at the measurement
time (equal within `alignment`). It arrives by a host grant (authorization is I4-8's, not numerics);
the numeric rule is that `r/max(w,floor)+p0 > 0` always holds, so the `Div` quirk cannot arise. A
fusion that contains the candidate is never a reference.

### 8.3 Health fraction cross-host margin (I3-2a)

I3-2's accepted `suspect_fraction` decision has the same T1 dependence through `b`. This design
leaves I3-2's text alone and supplies the guard of 5.3 (the fraction guard $H\cdot2^{-48}$) for the
`marginal` flag when I4 reads the state.

## 9. Consequences for the slices

| Slice | What this design gives it                                                                                                                                     |
| ----- | ------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| I4-1a | `score_tolerance` in log-odds (A14) and the gate settings of 7.2 enter `vagent-policy-3` and the settings table once; `decay` absent means $\lambda=0$ (8.1). |
| I4-2  | The sequences of 3.2 to 3.4 and 6, the caps, the vectors and the test of 4.5, the `FLT_EVAL_METHOD` assertion, `finite()`, the typed refusals of 3.10.        |
| I4-3  | `sumC`, `nEff`, `pool`, the canonical order, `at_least`, the tie rule on $s$ (3.5, 3.6); the spec's closed forms asserted as marginal, not as equality.       |
| I4-8  | The rule of 7.2 and the proof 7.4; settings; the smoke test of 7.6; the common-cause guard if the lead accepts Q2; `innovation` and `slew` of 3.7.            |
| I4-9  | `cluster` without `Sqrt`, margin 0 (3.8); `fusion` (3.7); the $t_\star$ rule (8.2).                                                                           |
| I5-4  | The SGD vectors reuse the file format; `Exp` in $\sigma$ is T1; learned weights stay bits (5.1 item 2). Not designed here (I5-D).                             |

## 10. Proposed amendments and questions

| #   | Spec section | Proposed amendment                                                                                                                                                                                                                                     |
| --- | ------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| A12 | 5.1          | Gate rejections drive an event-indexed fraction: SUSPECT at 0.30, FAULTED at 0.70 with a minimum evidence weight and half-life 64, in place of "three consecutive rejections for suspicion, five for fault". Accepted measurements decay the fraction. |
| A13 | 3.4          | `risk: "lcb"` is the warrant shrunk toward 1/2 by $c\sqrt v$ and never past it: $\tfrac12+\operatorname{sgn}(\mu-\tfrac12)\max(0,\lvert\mu-\tfrac12\rvert-c\sqrt v)$.                                                                                  |
| A14 | 5.3          | `score_tolerance` is an absolute difference of the log-odds $s$ (default $10^{-9}$), not of $W$. `min_confidence` stays in $W$-space.                                                                                                                  |
| A15 | 5.2          | Drop the profile-variance sentence for v1; every consensus input carries `with variance:`.                                                                                                                                                             |

Questions for the lead:

1. **Q1.** Confirm the accepted false-latch rate ($10^{-6}$ per device-year at 100 Hz, within the
   region of 7.4) or give another figure; the settings in 7.2 follow from it.
1. **Q2.** Accept the common-cause guard of 7.5 (recommended), or leave it to I4-8's review.
1. **Q3.** The `marginal` Bool on decision records (5.3) is a result-ABI field; I4-D2 or I4-5 owns
   it. Confirm the name and that it is a Bool and not a code per decision.
1. **Q4.** Gate counters per calibration record (this design) or per profile. Per record keeps one
   predicate's frame error from latching a source for others; the latch itself is profile-wide.
1. **Q5.** `InvalidEpistemicState` and `EpistemicNumericFault` are proposed additions to T1's
   refusal vocabulary.
1. **Q6.** Where the vector file and reference live: `tools/vagent-epistemic-reference.py` and
   `tests/samples/vagent/epistemic_vectors.tsv` are proposed; the plan's earlier name
   `tools/vagent-epistemic-vectors.py` appears in older review text only.

## 11. Not done, and limits

- The prototype's vectors cover the routines of 3.1 except `cluster`, `slew` and `fracStep`'s update
  loop (their sequences are `Sub Mul Add` and need only the `bits.*` form, which the generator
  produces the same way). The real generator adds them.
- The bound of 4.2 is a first-order running bound, not an interval proof; the perturbation run is
  its check, and a case that fails it is a finding about the bound.
- The false-latch bound assumes the gate's rejections are independent or in short blocks (7.5). The
  distribution-free bound is 0.14 per event.
- The pool cap and guard scale linearly in $n$, a pessimistic rule: the measured worst case (128
  sources, $|s|$ up to 1768) is 44% of the cap.
- No C++ was written or run. The Python stand-in replays the sequences in IEEE doubles; it checks
  the format and the tolerance, not the VM.
