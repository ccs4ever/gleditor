# I4-D1: the numeric design for P19 scoring, pooling, gates and fusion

Revision 2, after round 1 review (both reports FAIL: [ML](i4-d1-review/review-ml.md) 2 MAJOR 6
MINOR, [numerics](i4-d1-review/review-numerics.md) 3 MAJOR 6 MINOR; every finding is dispositioned
in section 13). Author's perspective: ML and numerics. Code baseline: HEAD `47d00222` (the design
reads `apps/common/xanadu/vortex/vortex_vm.cpp` and `vagent/runtime/epistemic_policy.cpp` at that
commit; the I3-2a health estimator, `vagent/runtime/epistemic_emit_health.cpp`, was read again at
`49149e4b` for section 12). Spec authority: P19 v1.5 sections 3.1 to 3.4, 5.2, 5.3 and 8.1. Rulings
it applies: D1, O6, O9, O10 and P2-5 (the label and ledger caps are **not** here; they belong to
I4-D2). Inputs: the I1 record ([I1 validation](i1-validation.md), [ML gate](ml-gate-i1.md)), the
I3-2 estimator ([I3-2 design](i3-2-design.md) section 3.2) and the ML plan reviews M4 to M7 and
minors ([round 1](plan-review/review-ml.md), [round 2](plan-review/review-ml-r2.md)).

Everything numeric below was computed, not asserted. The reference program, its vector file, the
stand-in checker and the gate and lcb scripts are now in the tree (section 4.4):
`tools/vagent-epistemic/` holds the programs and `tests/samples/vagent/` the two data files. Every
figure in this document that changed in revision 2 was recomputed from them; the recipe for each is
in the script named beside it. The Python stand-in is evidence for the format and the tolerances,
not the C++ test; I4-2 writes that.

## 1. What this design decides

| Item | Decision                                                                                                                                                                                                                                                                                           | Section  |
| ---- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------- |
| D1   | Replay is bit-equal within one process. A "host" is a (libm build, CPU feature set) pair. Every routine that uses no `Exp`/`Ln` is bit-equal on every IEEE-754 host. The rest agree within a stated, per-query cap, and a decision reports `marginal`. Stored bits are authoritative.              | 5        |
| O9   | `risk: "lcb"` shrinks the warrant toward 1/2 by `c * sqrt(v)` and never past it. Proposed amendment A13.                                                                                                                                                                                           | 6        |
| O10  | Gate rejections drive event-indexed fractions: SUSPECT from a fast window (half-life 64) and FAULTED only when a fast and a slow window (half-life 4,096) agree, with the gate tested on the uninflated variance. False latch at most 1e-6 per device-year under a stated autocorrelation, proved. | 7        |
| O6   | Drop the profile-variance path for v1 (proposed amendment A15). `t_star` and the reference are fixed in 8.2.                                                                                                                                                                                       | 8.2      |
| -    | `decay` is the predicate's freshness half-life (already built as `decayMs`); absent means no decay. `score_tolerance` is an absolute difference of log-odds (proposed A14).                                                                                                                        | 8.1, 3.6 |
| -    | The reference is an mpmath program with a running error bound per case, a hex-float input format, 50-digit outputs, exact bits for the libm-free routines, and an analytic cap asserted on every case. Every mutant must be rejected.                                                              | 4        |

Findings that shaped the design, each checked against the code:

- **The VM does not guard division.** `Div` by a zero divisor yields `0.0` and succeeds
  (`vortex_vm.cpp:516-523`). Only `Exp`, `Ln` and `Sqrt` refuse (`:922-945`). Every `Div` in section
  3 therefore has a divisor proved positive and in range (not merely positive: a product that
  underflows to zero is a zero divisor, section 3.2), or a refusal before it.
- **The VM has no `isfinite`.** A finite test is `Abs` then `Lte` against the largest double, which
  is false for infinity and NaN. Section 3 calls it `finite(v)`.
- **Only `Exp` and `Ln` reach libm.** `+ - * / Sqrt Min Max Abs Neg` and the comparisons are single
  IEEE operations, one per opcode, with no fast-math or contraction flag in the Makefile (grep: only
  `-O0` and `-O3` appear). So most of P19's arithmetic is bit-identical across hosts, and the
  cross-host question is confined to a small, nameable set (section 5).
- **`decay` and `score_tolerance` already exist as policy keys** (`epistemic_policy.cpp:45,47`;
  `decay` is a duration in milliseconds, `score_tolerance` defaults to 1e-9). The plan's "per-source
  `decay`" is the predicate policy's half-life; nothing is stored per source.
- **The revision-1 vectors did not test what section 3 claimed.** Four of its claims were not true
  of its own files: a plain sum passed (3.5), a pool cap held only because no vector sat at the
  clamp (3.4), `betaVar` was fine only because Python raises where the VM does not (3.2), and the
  caps were stated but never asserted by the generator (4.3). Sections 3.2, 3.5, 4.3 and 4.5 repair
  each, and a mutant that survives now fails the run.

## 2. Notation and rounding model

`u = 2^-53` is the unit roundoff. `+ - * /` and `Sqrt` are correctly rounded: relative error at most
`u`. `Exp` and `Ln` are the host libm. This design assumes at most 2 ulp, relative error `L = 4u`,
which glibc and musl meet. The assumption is not trusted: the vector file carries libm probes
(section 4.4) so each host proves it, and a 3 ulp libm is rejected by name at the probe before any
vector bound is in question (section 4.5).

A **pair** `(p, q)` carries a warrant `w` and its complement `1 - w`, each to relative accuracy.
Computing `1 - w` from `w` near `w = 1` loses digits (a rounding of `5e-17` in `w` is `5e-11`
relative to `q = 1e-6`). The pair never forms the complement by subtraction. Section 3 gives the
sequences in which both entries are sums or ratios of non-negative numbers.

Error bounds are **absolute** bounds on the computed value, from a running first-order analysis that
also keeps the second-order terms for `+ - *` (section 4.2). A "cap" is a formula in a case's own
inputs and outputs that the bound may not exceed (section 4.3). **Range.** The ideal value lives in
an unbounded exponent; the computed one is binary64. The generator tracks every intermediate against
$[2^{-1022}, \text{max double}]$ and records the result as `range=ok` or `range=subnormal` on the
case (an intermediate above the maximum is a refusal, never a vector; a binary64 division by zero
fails generation). A subnormal result adds $2^{-1075}$ to its bound, which is the rounding step
there and is why the comparison rule of 4.3 carries an absolute floor.

## 3. The routines

Every routine is a pure sequence of VM opcodes over `Float` values. Operation counts are VM opcodes
(`Add Sub Mul Div Min Max Abs Neg`, comparisons, `Sqrt`, and the libm calls themselves); the Libm
column says how many of them are `Exp` or `Ln`. "T0" means no libm call is reached (bit-identical
across hosts), "T1" means one is (section 5).

### 3.1 Table

| Routine      | Formula (spec)                                                              | Operations                 | Libm | Class | Cap (a formula, asserted per case)                                                   |
| ------------ | --------------------------------------------------------------------------- | -------------------------- | ---- | ----- | ------------------------------------------------------------------------------------ |
| `betaPair`   | $\mu=\alpha/(\alpha+\beta)$, $1-\mu=\beta/(\alpha+\beta)$                   | 3                          | 0    | T0    | rel $2^{-51}$                                                                        |
| `betaVar`    | $pq/(s+1)$, $p,q$ from `betaPair`, $s=\alpha+\beta$                         | 6                          | 0    | T0    | rel $2^{-49}$                                                                        |
| `shrink`     | O9, section 6                                                               | 17                         | 0    | T0    | abs $2^{-49}$                                                                        |
| `decay`      | $w(a)=\tfrac12+e^{-\lambda a}(w_0-\tfrac12)$                                | 45 (series), 10 (exp)      | 0, 1 | T0/T1 | rel $2^{-49}$ per entry                                                              |
| `clampLogit` | clamp to $[\epsilon_w,1-\epsilon_w]$, then $\ln(w/(1-w))$                   | 5                          | 1    | T1    | abs $8u(1+\lvert\ell\rvert)$                                                         |
| `sigmoid`    | $\sigma(s)$ and $1-\sigma(s)$                                               | 7                          | 1    | T1    | rel $2^{-49}$ per entry                                                              |
| `sumC`       | Neumaier compensated sum, in the given order                                | 7 per term, 1              | 0    | T0    | $2u\lvert S\rvert+2n^2u^2\sum\lvert x_i\rvert$ plus the inputs' bounds               |
| `pool`       | $s=\sum_G n_{\mathrm{eff}}(G)\bar\ell_G$, $W=\sigma(s)$                     | $12n+13g+8$ (n sources)    | n+1  | T1    | $s$: $16\,u\,A$; $W$: section 5.3                                                    |
| `nEff`       | $\lvert G\rvert/(1+(\lvert G\rvert-1)\rho_G)$                               | 4                          | 0    | T0    | rel $2^{-49}$                                                                        |
| `fusion`     | $r_f=(\sum\tilde r_i^{-1})^{-1}$, $\hat x_f=r_f\sum\tilde r_i^{-1}x_i$      | $17k+3$ (k inputs)         | 0    | T0    | $r_f$ rel $2^{-50}$; $\hat x_f$ abs $2^{-49}\,r_f\sum\lvert\tilde r_i^{-1}x_i\rvert$ |
| `innovation` | $d^2=\lVert x-x_0\rVert^2/(\tilde r+p_0)$                                   | $2k+3$                     | 0    | T0    | rel $2^{-50}$                                                                        |
| `slew`       | $\lVert\Delta x\rVert^2\le(v_{\max}\Delta t)^2$ (no `Sqrt`)                 | $2k+3$                     | 0    | T0    | exact comparison                                                                     |
| `cluster`    | $\lVert x_i-x_j\rVert^2\le\tau^2$, union-find, diameter                     | $3k$ per pair              | 0    | T0    | exact comparison                                                                     |
| `fracStep`   | $b=1+\operatorname{expm1}(-\ln2/H)$; $R\leftarrow bR+h$, $N\leftarrow bN+1$ | about 40 once, 4 per event | 0    | T0    | $b$, $1-b$ rel $2^{-49}$; $R,N$ abs $2^{-49}(H+8)^2$; $f$ abs $2^{-49}(H+8)$         |

In the `pool` row $A=\sum_G n_{\mathrm{eff}}(G)\bigl(1+\overline{\lvert\ell\rvert}_G\bigr)$ is a sum
the pool already has the terms of, and $\overline{\lvert\ell\rvert}_G$ is the group's mean
$\lvert\ell\rvert$ (section 3.5, 5.3). The cap on `fracStep`'s $R,N$ is the conditioning of $N$ in
$b$ ($\mathrm{d}N/\mathrm{d}b\approx(H/\ln2)^2$) times the few ulp of $b$, against the ideal
$2^{-1/H}$; it is not a cross-host difference, because $b$ is the same bits everywhere (3.9).

**What a cap is.** Revision 1 stated each cap as the largest bound its vectors happened to produce,
rounded up to a power of two, and did not assert it. Two of those (`logit`, `pool`) were then wrong
for inputs the vectors had not drawn (3.4, 3.5). A cap is now a formula in the case's own inputs and
outputs, `caps()` in the reference, asserted for every output of every case; the generator fails
when a bound exceeds it. The largest bound-to-cap ratio per routine in the committed file is
`betaPair` 0.50, `betaVar` 0.50, `shrink` 0.50, `decay` 0.54, `clamp_logit` 0.50, `sigmoid` 0.50,
`pool` 0.75, `fusion` 0.56, `innovation` 0.73, `fracStep` 0.29. The formulas are first-order sums of
per-operation terms (about $14uA$ for the pool, against the written $16uA$), so a new vector class
cannot exceed them silently.

### 3.2 Beta mean, variance and the pair

`betaPair(alpha, beta)`: refuse unless both are finite and positive and `s = alpha + beta` is finite
(a sum that rounds to infinity is `InvalidEpistemicState`; the committed refusal vectors are three
such pairs). Then `p = alpha / s`, `q = beta / s`. The divisor `s` is positive and finite, and `p`
and `q` are in $[0,1]$ (they may underflow to a subnormal or to zero, which is relative accuracy
lost and absolute accuracy kept: section 2).

`betaVar` is `(p * q) / (s + 1)`, from the pair. **Revision 1 computed
`(alpha*beta) / ((s*s)*(s+1))`, which is wrong on the domain it admitted** (NUM M1). Its divisor
`(s*s)*(s+1)` is zero when `s*s` underflows, and the VM's `Div` then returns `0.0` and succeeds, so
the variance read as 0 where it is 0.25, and `lcb` silently became the mean. It overflows to
infinity when $s>1.3\times10^{154}$ and the quotient reads 0. Python raises `ZeroDivisionError` on
the first, which is why the prototype never saw it. Reproduced (`m1.py`; binary64 Python floats are
the VM's arithmetic):

| $\alpha=\beta$     | true $v$              | revision 1 formula                                               | pair form $pq/(s+1)$  |
| ------------------ | --------------------- | ---------------------------------------------------------------- | --------------------- |
| $10^{-200}$        | 0.25                  | divisor 0: `Div` returns 0.0 (Python raises)                     | 0.25                  |
| $5\times10^{-324}$ | 0.25                  | divisor 0: `Div` returns 0.0                                     | 0.25                  |
| $10^{-160}$        | 0.25                  | divisor subnormal ($4\times10^{-320}$), 0.25 by luck of rounding | 0.25                  |
| $10^{150}$         | $1.25\times10^{-151}$ | $s^2(s+1)$ overflows to infinity: 0.0                            | $1.25\times10^{-151}$ |
| $10^{160}$         | $1.25\times10^{-161}$ | NaN                                                              | $1.25\times10^{-161}$ |

In the pair form $p,q\le1$ and $s+1\ge1$, so no divisor can be zero and no product can leave the
range: the former domain bound `alpha + beta <= 1e100`, which existed only to stop $s^2(s+1)$
overflowing, is gone. The refusal is `InvalidEpistemicState` (new, for values read from a record;
section 3.10). The committed vectors hold $\alpha,\beta$ in $\{10^{-300},10^{-160},2^{-1022},
5\times10^{-324},10^{150},10^{160},10^{300},8\times10^{307}\}$ and mixtures of them (28 `kind=range`
cases, 45 cases in all marked `range=subnormal`), and the checker's `var_old` mutant, which is the
revision-1 formula, is rejected by 31 of them.

### 3.3 Decay toward 1/2 (spec 3.2)

With `tau` the policy half-life and `a` the age, both in milliseconds as Floats (integers below
$2^{53}$, so exact):

```text
hl = a / tau                      # half-lives; tau > 0 checked at policy load; may overflow to +inf
if hl > 1100:  return (1/2, 1/2)  # exp(-x) is zero; decided before any product can overflow
x  = hl * LN2                     # LN2 is the double nearest ln 2 (3.4e-17 relative off, in the bound)
if x < 1/2:  em = expm1_series(-x); E = 1 + em;  h = 1/2 * (-em)
else:        E = Exp(-x);           h = 1/2 * (1 - E)
p' = E*p + h;   q' = E*q + h
```

`hl` may overflow to $+\infty$ (age $10^{300}$ ms over a half-life of $10^{-10}$ ms); the next
operation is the comparison with 1100, which is true for infinity, so no arithmetic ever touches it.
The generator records that case (`hl_inf`) rather than treating it as an error.

`p' + q' = 1` to within $u$: $E=\mathrm{fl}(1+em)$ while $h$ is formed from $em$, so the sum is
$E-em$, which differs from 1 by at most the rounding of $1+em$ (NUM m6; revision 1 said "exactly").
Both entries are sums of non-negatives, so neither loses digits. The textbook form
`1/2 + E*(p - 1/2)` loses the small side: for $w_0=1-10^{-6}$ and a short age its complement carries
an error near $5\times10^{-11}$ (the `decay_naive` mutant fails 14 vector cases by factors up to
$4\times10^{7}$). `expm1_series(y)` is a Horner polynomial on $|y|\le\tfrac12$ with coefficients
$1/k!$ as doubles: 18 terms, $y(1+y/2!+\dots+y^{17}/18!)$, 17 multiply-adds and one multiply. **The
first omitted term is $y^{19}/19!\le1.6\times10^{-23}$** (revision 1 said "17 terms, under
$10^{-24}$", which is false: the first omitted term after 17 would be $6\times10^{-22}$; ML 8). The
generator adds twice that, $3.1\times10^{-23}$, to the bound. The series needs no libm, so for ages
below about 0.72 half-lives the whole routine is T0. The branch is decided by a T0 comparison, so
every host takes the same branch. At age 0 the result is bit-identical to the input pair (`E = 1`,
`h = 0`).

The expensive part is the series, 35 opcodes per observation. At 128 sources that is about 4,500
opcodes per query, against about 1,700 for the pool itself. If the measured cost matters, I4-2 may
drop the series and accept the cap-widened bound for $w$ near 0 and 1 (section 4.3 shows how the
bound moves); the design recommends keeping it, and the same series now serves the health fraction's
$b$ (3.9), so it is written once.

### 3.4 Clamp, logit, sigmoid

`clampLogit(p, q)`: if `p < eps` use `(eps, 1 - eps)`; else if `q < eps` use `(1 - eps, eps)`. Then
`ell = Ln(p / q)`. One `Ln` of the ratio, not `Ln(p) - Ln(q)`: the difference cancels near
$w=\tfrac12$ (by the bound analysis, two libm logarithms near $-0.69$ leave up to $6\times10^{-16}$
absolute error; the ratio form leaves $1\times10^{-16}$ plus the libm term in $\lvert\ell\rvert$),
and the ratio is in $[\epsilon/(1-\epsilon),(1-\epsilon)/\epsilon]$, so `Div` and `Ln` are safe.
$\epsilon_w$ is the policy value (default $10^{-6}$), validated in $[10^{-300},\tfrac12)$ (A16): the
lower end keeps the ratio below the largest double, so `Ln` can never see an infinity (it refuses on
one, `vortex_vm.cpp:931`, which would surface as an untyped `Precondition` failure); it is no
practical floor, since $\ln(1/\epsilon)=690$.

**The logit error is proportional to $\lvert\ell\rvert$, so it depends on $\epsilon_w$** (NUM M2).
The libm term is $4u\lvert\ell\rvert$ and $\lvert\ell\rvert$ reaches $\ln(1/\epsilon_w)$. Revision 1
used one constant, $2^{-46}=1.4\times10^{-14}$, tuned at $\epsilon_w=10^{-6}$. Computed (`pool()` in
the reference, a clamped source against the constant):

| $\epsilon_w$ | $\lvert\ell\rvert$ at the clamp | running bound of one logit | against the revision-1 cap $2^{-46}$ | pool of 128 sources at the clamp: running bound of $s$ | against the revision-1 guard $128\cdot2^{-46}=1.8\times10^{-12}$ |
| ------------ | ------------------------------- | -------------------------- | ------------------------------------ | ------------------------------------------------------ | ---------------------------------------------------------------- |
| $10^{-6}$    | 13.8                            | $6.4\times10^{-15}$        | 0.45 of it                           | $2.4\times10^{-12}$                                    | **1.3x over**                                                    |
| $10^{-9}$    | 20.7                            | $9.4\times10^{-15}$        | 0.66                                 | $3.6\times10^{-12}$                                    | **2.0x over**                                                    |
| $10^{-15}$   | 34.5                            | $1.6\times10^{-14}$        | **1.09x over**                       | $5.9\times10^{-12}$                                    | **3.3x over**                                                    |
| $10^{-300}$  | 690.8                           | $3.1\times10^{-13}$        | **21.6x over**                       | $1.2\times10^{-10}$                                    | **65x over**                                                     |

So the revision-1 pool cap was exceeded by a valid input at the **default** epsilon (128 sources all
below the clamp), which its vectors never drew (the largest it measured, $8\times10^{-13}$, came
from the alternating 128-source vector, whose logits cancel; revision 1's section 11 called that 44%
of the cap). The numerics review offered two repairs: floor $\epsilon_w$ at $10^{-9}$, or derive the
guard per query. **This design takes the second, for three reasons.** A floor leaves the constant
wrong (at $10^{-9}$ the all-clamped case is still 2.0x over the revision-1 guard, so the "66%" in
the review was the logit alone, not the pool); a floor is a ceiling on what a user may configure
that buys nothing a per-query bound does not; and the bound the guard needs is already in hand,
because it is built from the terms the pool computes. The cap and guard are therefore the formulas
of 3.1 and 5.3 in the actual $\lvert\ell_i\rvert$ and $n_{\mathrm{eff}}$ of the query, and a
sub-1e-300 epsilon is a policy-load refusal, not a numeric one.

`sigmoid(s)`: `E = Exp(-Abs(s))`; `den = 1 + E`; `big = 1/den`; `small = E/den`; return
`(big, small)` if `s >= 0` else `(small, big)` as $(W,1-W)$. The exponent is never positive, so
`Exp` cannot overflow; for $|s|>745$ it underflows to zero, which the VM allows (spec 8.1). $W$
rounds to exactly 1 once $s>36.7$; ordering is therefore done on $s$, never on $W$ (section 3.6).

### 3.5 Compensated sum, pooling and $n_{\mathrm{eff}}$ (spec 3.1)

`sumC` is Neumaier's variant of Kahan summation: for each term `t = s + x`; if `Abs(s) >= Abs(x)`
`c += (s - t) + x` else `c += (x - t) + s`; `s = t`; result `s + c`. Seven opcodes per term. Bound:
$2u\lvert S\rvert+2n^2u^2\sum\lvert x_i\rvert$ plus the input bounds. A plain `Add` loop is bounded
by $n u\sum\lvert x_i\rvert$.

**The vectors now tell the two apart (ML-2).** Revision 1 justified `sumC` by a 128-logit pool
vector that alternates $+13.8$ and $-13.8$: every plain partial sum of that is $13.8$ or about 0, so
the plain sum is exact on it, and the plain-sum mutant passed (`pool` and `fusion` reported 0
failures). The rounding only accumulates when the positives come first, and since the canonical
order is by stable ID, an adversarial ID order is reachable in production. The `sum` routine is
therefore a vector class of its own, with inputs that are exact doubles (so no input bound hides the
difference): 8, 32 and 64 positives of magnitude $2^6$ to $2^{11}$ followed by their negations in
another order. The exact sum is 0 or a few $10^{-3}$ (the second variant scales the negatives by
$1-2^{-20}$); the compensated bound is $3\times10^{-26}$ to $6\times10^{-18}$; the plain loop is off
by $2.3\times10^{-13}$ to $6.4\times10^{-12}$ (every partial sum rounds at the ulp of a value in the
thousands) and the compensated routine is exact on all six. The plain loop fails the bound, and
`sum_plain` is rejected by 23 of the 27 `sum` cases. `pool` and `fusion` carry the same shape (one
group of 64 positive logits then their mirror images; 64 large positive $x$ then 64 negative) and
the plain sum is rejected there only by `fusion` (8 cases): the pool's bound ($1.9\times10^{-13}$,
mostly the libm terms of 128 logits) is larger than the plain-sum error on that vector
($3.5\times10^{-14}$), so the pool vector cannot discriminate and is kept for coverage, not as
evidence. Section 4.5 lists the survivors per mutant, and a surviving mutant fails the run.

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

A query that **decides** on $s$ or $W$ (a tie, `min_confidence`) also forms, from the same loop,
$A=\sum_G n_{\mathrm{eff}}(G)\,(1+\sum_{i\in G}\lvert\ell_i\rvert/g)$ and
$N_{\mathrm{eff}}=\sum_G n_{\mathrm{eff}}(G)$: an `Abs` and an `Add` per source, an `Add`, `Div`,
`Add`, `Mul` and `Add` per group, i.e. $+2n+5g$ opcodes ($14n+18g+8$ in all, 17% more than the pool
alone), and the guard of 5.3 is six more. The query that does not decide does not pay it.

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
cross-host noise it was meant to absorb is of the order of the per-query guard of 5.3, for ordinary
queries $10^{-12}$ in $s$, constant in the score.

**Recommendation (A14): `score_tolerance` is an absolute difference of $s$, default $10^{-9}$.** It
is over $100\times$ the guard of an ordinary 128-source query ($2.2\times10^{-12}$, 5.3), it never
saturates, and at $W\approx\tfrac12$ it is $2.5\times10^{-10}$ in $W$, tighter than the spec's.
`min_confidence` stays in $W$-space (it is a warrant threshold in $[0,1]$), tested as
`at_least(W, W', theta)`: `W >= theta` when `theta <= 1/2`, else `W' <= 1 - theta` (`1 - theta` is
exact for $\theta\in[\tfrac12,1]$), so a threshold near 1 keeps full resolution through the pair,
**and so does its margin**: the guard of 5.3 is relative to the smaller side (NUM m1), because
$W'\approx e^{-s}$ carries an error proportional to itself. Revision 1 used an absolute guard on
$W'$, which made every decision at $\theta=1-10^{-12}$ and $s>25$ `marginal` however clear it was.

Alternative: keep $W$-space and test the saturation (no amendment). Cost: the user guide must say
that confident candidates tie, and the policy hash is unchanged. The recommendation changes the
meaning of an existing key, so it belongs in `vagent-policy-3`, which I4-1a writes once.

### 3.7 Scalar fusion and the innovation gate (spec 5.2, 5.3)

`fusion`: for each input `inv_i = Max(w_i, floor) / r_i`, which is $1/\tilde r_i$ in one `Div`
(`r_i > 0` checked; `finite(inv_i)`). `sum = sumC(inv)`; `r_f = 1 / sum` (positive, so safe);
`x_f = r_f * sumC(inv_i * x_i)`. Mixed-sign $x_i$ are why the second sum is compensated. The refusal
on non-finite is `EpistemicNumericFault`.

**The cap of $\hat x_f$ is relative to $r_f\sum\lvert\mathrm{inv}_ix_i\rvert$, not to
$\lvert\hat x_f\rvert$** (NUM m2). `sumC` compensates the additions but each product
$\mathrm{inv}_ix_i$ carries $u\lvert\mathrm{inv}_ix_i\rvert$ of its own, so a cancelling sum
($x_i=\pm10^8$ plus an offset of 1) has a relative error near $10^{-8}$. Revision 1 stated
$\hat x_f$ rel $2^{-47}$ and the generator happened to draw $x_i$ in $[-100,100]$. The cap is now
$2^{-49}\,r_f\sum\lvert\mathrm{inv}_ix_i\rvert$ (`quant["L1"]` in the generator), asserted, with
ratio 0.56. The review's alternative, an input-conditioning refusal, is declined: the result is
still the best double for those inputs, and a refusal would turn a precise datum into an outage.

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

$b=2^{-1/H}$ and $1-b$ come from the series of 3.3: $x=\ln2/H$ is at most $0.347$ for $H\ge2$, so
`em = expm1_series(-x)`, `b = 1 + em` and `omb = -em` (the cap $1/(1-b)$ is `1 / omb`); $H=1$ is
$b=\tfrac12$ exactly. This is **T0**: no libm call, the same bits on every host, and $1-b$ is not a
subtraction of nearly equal numbers (NUM m3: at $H=10^6$ the subtraction `1 - b` had relative error
$1.6\times10^{-10}$, which moved the domain bound $\lfloor0.9/(1-b)\rfloor$ by one across hosts and
the rescale cap by $10^{-3}$). I3-2 as built computes `b = Exp(-(LN2 / H))`, which is T1; moving it
to the series changes the last bit of $b$ and nothing else, and is the first item of section 12.

The cap `1/(1-b)` and the rescale-then-update order are exactly I3-2's (R3-9). One event:
`R <- b*R + hit; N <- b*N + 1` after the rescale. I3-2a emits it parameterised by its two state
cells, `b` and the evidence floor (plan, ML minor 5; **the built routine is not**, section 12); I4-8
calls the same routine with `gate_rejected` and `gate_events`. The fraction `f = R/N` divides only
when `N >= min_events`. Because $b$ is T0 and the update is `+ *` only, the whole estimator is T0:
$R$, $N$ and $f$ are bit-identical on every host for the same event sequence, and the comparison
`N >= min_events` has no margin. (Revision 1 classified `N >= min_events` as an unguarded T1
decision, NUM m3; the repair is to make it T0, not to guard it. For the as-built T1 $b$ the guard is
$4\,\delta b\,(H/\ln2)^2\approx1.2\times10^{-10}$ at $H=64$, `marginal` as 5.3 says.)

Against the ideal $2^{-1/H}$ the bounds are: $b$ and $1-b$ relative $2^{-49}$; after $k$ events $R$
and $N$ differ by the conditioning of $N$ in $b$ times the ulps of $b$, within $2^{-49}(H+8)^2$
absolute ($2.4\times10^{-12}$ for $N$ at $H=64$; ratio to the cap 0.29), and $f$ within
$2^{-49}(H+8)$. That is a distance from the ideal and not a difference between hosts. The vectors
hold 10 values of $H$ for $b$ and five event sequences (every tenth a hit, a 13-burst, all hits, one
at $H=4096$, a random one), all bit-exact.

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

Domain list (spec 8.1, made executable; changes from revision 1 are amendment A16): $\alpha,\beta$
finite and positive with a finite sum; **$0\le c\le10^6$**; **$10^{-300}\le\epsilon_w<\tfrac12$**;
$0<\epsilon_{\mathrm{floor}}\le1$; $\rho$ finite in $[0,1]$; half-life $\tau>0$; age $a\ge0$ finite;
variance finite and positive; thresholds $\theta\in[0,1]$; $\tau_{\mathrm{score}}\ge0$; cluster
tolerance positive and $D\ge\tau_{\mathrm{cluster}}$; $\Delta t>0$ for slew; $H\in[1,10^6]$.

Why $c\le10^6$: it is not overflow. $v<\tfrac14$, so $t=c\sqrt v\le c/2$ and $t+t\le c$ never leaves
the range (revision 1 and NUM m6 said it could; it cannot). It is conditioning. When $v$ underflows
the computed $t=c\sqrt v$ loses the absolute error of $\sqrt v$, at most $2^{-537}$, multiplied by
$c$: at $c=1.7\times10^{308}$ and $\alpha=5\times10^{-324}$, $\beta=1$ the generator found the
neutral warrant $(\tfrac12,\tfrac12)$ computed as the unshrunk mean pair $(5\times10^{-324},1)$, a
wrong branch; at $c\le10^6$ the error is below $10^{-155}$ and no branch can move. A million
standard deviations is not a configuration.

## 4. Reference vectors

### 4.1 The reference program

`tools/vagent-epistemic/vagent_epistemic_reference.py` (a developer tool, **not** a build
dependency; `requirements.txt` beside it pins `mpmath==1.4.1` and `numpy==2.5.4`, and `python3 -I`
runs it) is written from the formulas of section 3 before any IR exists, and is reviewed as a design
artefact by ML and NUM. It uses `mpmath` at 60 digits for `exp`, `log` and `sqrt`. It imports no
`math`, `numpy`, `cmath` or `scipy`; `check_no_libm.sh` greps for them, and for the random methods
whose streams changed across Python releases (`choice`, `shuffle`, `randrange`, `randint`, `sample`,
`gauss`: NUM m4), so the generator draws only `random()` and `uniform()`. The real test is a gtest
reading the file path from a compile-time macro. `random.Random(19)` supplies the seeded inputs and
no input passes through a floating `log` (the generator draws exponents and raises 2 to them in
mpmath). The generator refuses to run under another `mpmath` unless `VAGENT_MPMATH_BUMP=1` is set,
because the 50-digit outputs are byte-reproducible only under the pinned version.

The program contains two versions of each routine. `exact_*` is the spec formula in mpmath and has
no rounding model. The *sequence* version replays the opcode sequence of section 3 and carries three
things per value: the ideal value, an absolute error bound, and (when no libm call is involved) the
same sequence in binary64. The binary64 path is Python float arithmetic, which is IEEE
round-to-nearest with gradual underflow, and it models the VM's `Div`: a zero divisor yields `0.0`
and the generator rejects the case (NUM M1; revision 1 rounded in an unbounded-exponent mpmath, so
no vector could underflow or overflow). The generator asserts the sequence agrees with the exact
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

A division whose divisor the bound cannot separate from zero aborts generation. A result in the
subnormal range adds $2^{-1075}$ if the operation rounds (`* / Sqrt Exp Ln`; `+ -` are exact there).
A branch whose test is continuous across its edge (the `shrink` three-way choice) adds $8u$ when the
result is within $64u$ of $\tfrac12$, which covers a host taking the other side at the edge. The
compensated sum uses the Higham-style bound of section 3.5. This is a first-order running bound with
the second-order terms kept where the operation is `+ - *`; it is not an interval proof, and a test
of the bound is the perturbation run of section 4.5.

### 4.3 Tolerance, per routine, from the operation count

The tolerance of a vector is its own written bound, so it is derived from that case's operation
sequence and operands. Three more rules fix what the bound may be:

1. **Caps.** The table in 3.1 gives a cap formula per routine. The generator fails the run if any
   output's bound exceeds its cap, plus $2^{-1070}$ for a case marked `range=subnormal`. (Revision 1
   said the generator did this; it did not.)
1. **The comparison.** A test accepts `got` when
   $\lvert\mathrm{got}-\mathrm{ref}\rvert\le B+u\lvert\mathrm{ref}\rvert+2^{-1073}$, where `ref` is
   the 50-digit decimal rounded to the nearest double by `std::strtod` (the
   $u\lvert\mathrm{ref}\rvert$ term pays for that rounding; the last term is two denormal steps,
   because relative accuracy has no meaning below $2^{-1022}$).
1. **The pool cap is per query**: $16\,u\,A$, where $A$ is the weighted sum of
   $1+\lvert\ell_i\rvert$ of 3.5, so it follows the data and not a constant, and a raised
   `max_sources` or a smaller $\epsilon_w$ raises it in step (section 3.4, 5.3).

For orientation, the relative bound of one correctly rounded operation is $1.1\times10^{-16}$, a
libm call $4.4\times10^{-16}$, `betaVar` (six operations) $8.9\times10^{-16}$ and `decay` about
$9.5\times10^{-16}$ in the series branch. A routine with $k$ rounded operations and $m$ libm calls
has a bound near $(k+4m)u$ times its condition number; the generator computes the exact per-case
number.

### 4.4 The file, and where everything lives

The artefacts are in the tree, uncommitted (the lead commits; the slice that wires the C++ test is
I4-2):

| Path                                                   | What it is                                                                                                                                                                          |
| ------------------------------------------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `tools/vagent-epistemic/vagent_epistemic_reference.py` | the reference and generator; `--check FILE` regenerates and compares                                                                                                                |
| `tools/vagent-epistemic/check_vectors.py`              | the Python stand-in for the C++ test: `host`, `perturb`, `mutant`, `all`                                                                                                            |
| `tools/vagent-epistemic/check_no_libm.sh`              | the import and random-method grep (shellcheck and shfmt clean)                                                                                                                      |
| `tools/vagent-epistemic/ar1_hits.py`                   | the pinned autocorrelated gate sequences (7.6); `--check`                                                                                                                           |
| `tools/vagent-epistemic/gate_bound.py` and its callers | the weighted Chernoff bound (log-space, any block length); `gate_bound2.py`, `gate_bound3.py`, `slow_bound.py`, `gate_startup_slow.py`, `i32a_burst.py`, `gate_flap.py`, `smoke.py` |
| `tools/vagent-epistemic/ar.c`                          | the AR(1) simulator of 7.1 and 7.6 (`cc -O2 ar.c -lm`)                                                                                                                              |
| `tools/vagent-epistemic/lcb_table.py`, `nums.py`       | the O9 option table (6) and the streak renewal table (7.1)                                                                                                                          |
| `tools/vagent-epistemic/requirements.txt`              | `mpmath==1.4.1`, `numpy==2.5.4`                                                                                                                                                     |
| `tests/samples/vagent/epistemic_vectors.tsv`           | the 704 vectors                                                                                                                                                                     |
| `tests/samples/vagent/gate_ar1_hits.tsv`               | five AR(1) rejection sequences of 300,000 events and the rules' latches                                                                                                             |

(The plan's earlier name `tools/vagent-epistemic-vectors.py` appears in older review text only. The
reference is a module and sits beside its checker, hence a directory under `tools/` rather than the
single file revision 1 proposed.) One command reproduces the vectors, and two runs are byte-equal:

```sh
python3 -m venv /tmp/p19venv && /tmp/p19venv/bin/pip install -r tools/vagent-epistemic/requirements.txt
/tmp/p19venv/bin/python -I tools/vagent-epistemic/vagent_epistemic_reference.py tests/samples/vagent/epistemic_vectors.tsv
/tmp/p19venv/bin/python -I tools/vagent-epistemic/check_vectors.py tests/samples/vagent/epistemic_vectors.tsv all
```

`tests/samples/vagent/epistemic_vectors.tsv` has one case per line, TAB-separated `key=value`
fields, so the C++ test needs nothing beyond `std::strtod` and string splitting. A header line
starts with `#vagent-epistemic-vectors` and records `version=2`, `seed`, `dps`, the unit roundoff,
the libm assumption `libm_relative=0x1p-51`, the `mpmath` version, `pool_kappa`, the case count, and
the **host definition** of 5.1: `host=libm_build+cpu_dispatch` and
`host_report=libc_version,exp_log_ifunc_variant`. The header cannot carry the values (it is
byte-reproducible on any machine); the checker and the C++ test print them first, as
`libc=2.44 cpu_dispatch_flags=fma,avx2`, so a failing line in a bug report names its host.

```text
beta_mean/001  beta_mean  random  range=ok
  in.alpha=0x1.0000000000000p+3   in.beta=0x1.0000000000000p+1
  out.mu=0.8   out.one_minus_mu=0.2                      (50 significant digits)
  bound.mu=1.776e-16   bound.one_minus_mu=4.441e-17
  bits.mu=0x1.999999999999ap-1  bits.one_minus_mu=0x1.999999999999ap-3
pool/001  pool  closed  range=ok
  in.w=0x1.999999999999ap-1;0x1.999999999999ap-3   in.rho=0x1p+0;0x1p+0   in.eps=0x1.0c6f7a0b5ed8dp-20
  out.s=0.00000000000000027755575615628915821...   out.W=0.50000000000000006938893903907228955...
  bound.s=3.3e-15   bound.W=1.047e-15
  decision.name=min_confidence  decision.threshold=0x1.0000000000000p-1  decision.distance=1.388e-16
  decision.guard=4.102e-14  decision.class=marginal
```

(Each case is one physical line; it is wrapped here. The second is the spec's "restores 0.5" vector:
the distance and the guard are relative to the smaller side, 5.3.) Inputs are hex-float bit patterns
so `std::strtod` reads them back exactly. Outputs are decimals so no float format limits the
reference. Lists are `;`-separated, groups `,`-separated inside a list. A `kind` of `random`,
`edge`, `closed`, `range`, `eps`, `adversarial`, `refuse` or `libm` says why the case exists.
`bits.*` exists only for T0 outputs (section 5.1) and holds the exact binary64 result. `refuse`
cases carry `refuse.name=InvalidEpistemicState` and no outputs: the test asserts the routine refuses
them.

The file holds 704 cases: seeded random cases per routine; the edges (alpha near $10^{12}$, a logit
at the clamp, an exponential that underflows, ages of 0 and $10^{300}$); 28 range cases and 3
refusals for `betaPair`/`betaVar` (3.2); 132 `clamp_logit` and 59 `pool` cases, 78 and 40 of them
`kind=eps`, across $\epsilon_w\in\{10^{-3},10^{-9},10^{-15},10^{-30},10^{-100},10^{-300}\}$, the
pool ones including 128 sources at the clamp (3.4); 27 `sum` cases, six adversarial, and one
adversarial `fusion` (3.5); the spec's closed forms; and 40 libm probes for each of `Exp` and `Ln`.

### 4.5 How a test reads it, and the evidence

The C++ test `VagentEpistemicVectors` parses each line, runs the routine through the VM graph on a
bare `WorkerContext` (the I4-2 path), and asserts: the tolerance rule of 4.3 on every `out.*`; exact
bit equality on every `bits.*`; the cap on every `bound.*`; `range=ok` outputs finite; each `refuse`
case refused; for a `decision.*`, either the outcome (class `firm`) or the reported `marginal` flag
(class `marginal`), never the outcome of a marginal case; and, for the libm probes, at most 2 ulp
from the reference on this host.

The stand-in (`check_vectors.py`) runs the 704 cases in three modes (`all` runs them in turn and the
exit status is the conjunction):

| Mode      | What it runs                                                             | Result                                                                                                    |
| --------- | ------------------------------------------------------------------------ | --------------------------------------------------------------------------------------------------------- |
| `host`    | the sequences in IEEE doubles with this host's libm                      | 0 failures; largest error 0.66 of its bound (`betaPair`)                                                  |
| `perturb` | every `Exp`/`Ln` result moved 0, 1 or 2 ulp at random (another libm)     | 0 failures; T0 bits unchanged; largest 0.77 (libm probe), `clamp_logit` 0.74, `sigmoid` 0.72, `pool` 0.67 |
| `mutant`  | six naive forms in turn; **each must be rejected by its target routine** | all six rejected, none survives (below)                                                                   |

| Mutant        | Naive form                           | Rejected by (target routine, then others) |
| ------------- | ------------------------------------ | ----------------------------------------- |
| `lcb`         | the spec's $\max(0,\mu-c\sqrt v)$    | `shrink` 243 (fails by design: section 6) |
| `decay_naive` | $\tfrac12+e(p-\tfrac12)$             | `decay` 14                                |
| `logit_diff`  | complement recomputed from $p$       | `clamp_logit` 33, `pool` 21               |
| `sum_plain`   | a plain `Add` loop instead of `sumC` | `sum` 23, `fusion` 8; **`pool` 0** (3.5)  |
| `var_old`     | revision-1 `betaVar`                 | `beta_var` 31, `shrink` 9                 |
| `eps_const`   | $\epsilon_w$ fixed at $10^{-6}$      | `clamp_logit` and `pool` 142              |

The assertion "every mutant is rejected by at least one case of the routine it targets" is part of
the checker and of the C++ test's plan; a surviving mutant fails the run. Revision 1 listed three
mutants and said "the vectors can fail", while the plain-sum mutant passed (ML-2). The `perturb`
mode is the evidence that a bound holds when libm differs. **It is not zero-slack** (ML 7): revision
1's `sigmoid` ratio of 1.000 came from a vector that sat on its bound; the maxima above are the
current file's. The assumed 2 ulp is what the *probe* enforces: with `VAGENT_PERTURB_ULP=3` the run
fails `libm_exp` (1 case), `libm_ln` (2) and `clamp_logit` (7), and nothing else, so a 3 ulp libm
host is rejected by name at the probe before any T1 vector bound is in question. The `bits` values
of `betaPair`, `betaVar`, `shrink`, `decay` (series branch), `fusion`, `innovation`, `fracStep` and
`sum` are matched exactly by Python doubles, which also confirms that 53-bit round-to-nearest
reproduces IEEE `+ - * / sqrt` for these sequences.

## 5. The cross-host contract (D1)

### 5.1 Statements

1. **A host is a (libm build, CPU feature set) pair** (NUM M3). glibc selects `exp`, `log`, `pow`
   and `sin` variants by `ifunc` at load: FMA, FMA4, AVX2 or plain SSE2, which contract differently
   and can differ in the last ulp. A different libm build (glibc before 2.28 had other `exp`/`log`;
   musl and vendor libms differ again), a different CPU of one fleet, or a VM live-migrated to an
   older CPU is therefore a different host even for the same binary. The vector file names the
   definition (`host=libm_build+cpu_dispatch`) and the checker and the C++ test print `libc=` and
   the dispatch flags first (4.4).
1. **Within one process: bit-equal.** A recomputation from the same stored inputs returns the same
   bits as the incremental result. The sum order is canonical, no reduction is parallel, a query
   reads the committed state, and the `ifunc` is resolved once at load. I5-4's replay test compares
   doubles by `memcmp` **inside the process that made the incremental result**; across processes (a
   restart, another machine, the same machine after a libc upgrade) it compares within the cap and
   reports `marginal`, and it is a T1 test only for values that reach `Exp` or `Ln` (T0 values
   `memcmp` everywhere). On one unchanged (libm build, CPU feature set) the result is bit-equal
   across processes too, but that is an observation about deployments, not a guarantee.
1. **Stored state is authoritative, and the replay claim rests on it.** Alpha, beta, weights, the
   normaliser and the fractions $R,N$ are stored as bits and are never recomputed on resume, upgrade
   or migration; a translator copies bits (T1's digest compares doubles by bit pattern, so
   $-0.0\ne0.0$). Hosts therefore diverge only on **new** computation.
1. **T0 results are bit-identical on every IEEE-754 host** that uses binary64 with
   round-to-nearest-even, no extended precision and no contraction across VM opcodes. T0 routines:
   `betaPair`, `betaVar`, `shrink`, `decay` below 0.72 half-lives, `sumC` and `nEff` of bit-equal
   inputs, `fusion`, `innovation`, `slew`, `cluster` of bit-equal inputs, and (new, 3.9) the whole
   health and gate fraction. The vector file's `bits.*` assert this exactly. I4-2 adds
   `static_assert(FLT_EVAL_METHOD == 0)` beside the math opcodes and compiles the VM translation
   unit with `-ffp-contract=off` (a contraction can only happen inside one C++ expression and each
   opcode is one operation through `std::variant`, so this is insurance, not a known defect). **The
   evidence is x86_64 only**: the Makefile has no `-march`, so the baseline emits no FMA and the
   `bits.*` match says nothing about FMA. The claim is recorded as "by construction, verified on
   x86_64"; it becomes "verified" when the same `bits.*` assertions pass once on aarch64 (or
   qemu-user) (NUM m5).
1. **T1 results agree within the cap.** `decay` beyond 0.72 half-lives, `clampLogit`, `sigmoid` and
   `pool` depend on `Exp` or `Ln`; two hosts differ by at most twice the per-query cap of 5.3. The
   libm probes check the 2 ulp assumption on each host that runs the suite; a host that fails them
   is unsupported for the cross-host claim, and the test names the function and the ulp.
1. **Decisions** use section 5.3.
1. **Rejected alternative.** A VM-owned `Exp` and `Ln` would make T1 bit-identical and costs a
   rewrite of accepted I1 work (the plan records it in 7.2). Not proposed.

### 5.2 Which values are T1, concretely

A source with no `decay` key and a Beta mean or `lcb` is T0 from the counts to the pooled logit's
inputs; the `Ln` in `clampLogit` is the first libm call on its path. The fused point and the
innovation gate are T0 for such a source, and T1 only through $w$ when `decay` is set and the
reading is older than 0.72 half-lives. The cluster, slew and diameter decisions, and every health
and gate fraction, are T0 always.

### 5.3 Decision margins

A decision compares a computed value $x$ with a threshold $\theta$. Two hosts' values of $x$ differ
by at most $2B$, where $B$ is the bound of $x$ **for this query**. A decision is **firm** when
$\lvert x-\theta\rvert>G$ with $G=4B$ (twice the largest possible disagreement, with a factor 2
spare), and **marginal** otherwise. A marginal decision is still made, deterministically, on this
host; it is reported with `marginal: true`, and another host may decide it differently.

The pool's bound is built from the query, not from a constant (3.4): with $A$ and $N_{\mathrm{eff}}$
of 3.5,

```math
B_s=16\,u\,A+2^{-48}N_{\mathrm{eff}}
```

where the second term is the relative error $2^{-49}$ of each of $p,q$ in a `decay` that reached
`Exp` (zero when no source has a `decay` key).

| Decision                                     | Value compared                                    | Guard $G$                                              | Class when T0       |
| -------------------------------------------- | ------------------------------------------------- | ------------------------------------------------------ | ------------------- |
| `min_confidence`: $W$ against $\theta$       | $v=\min(W,1-W)$ against $t=\min(\theta,1-\theta)$ | $\lvert v-t\rvert/t>4(B_s+2^{-49})$ (relative)         | n/a                 |
| tie: $\lvert s_a-s_b\rvert$ against $\tau_s$ | log-odds difference                               | $4(B_{s,a}+B_{s,b})$ absolute                          | n/a                 |
| health or gate fraction against a threshold  | $f=R/N$                                           | none: T0 (3.9); as built, with T1 $b$: $H\cdot2^{-48}$ | firm                |
| `N >= min_events`                            | $N$                                               | none: T0; as built: $4\,\delta b\,(H/\ln2)^2$          | firm                |
| innovation $d^2$ against $g_k$               | $d^2/g_k-1$                                       | $2^{-46}$ relative                                     | firm when $w$ is T0 |
| slew, cluster adjacency, diameter            | exact comparison                                  | 0                                                      | always firm         |

The relative form on the smaller side (NUM m1) is what lets a threshold near 1 keep its resolution:
$W'\approx e^{-s}$ has an error proportional to itself. The guard is an upper bound on the
disagreement ($\sigma'\le\tfrac14$ is not used, the running bound's $\tfrac32$ is), so it is
conservative. Computed: 128 independent sources at 0.8 ($A=305$) have $B_s=5.4\times10^{-13}$ and
$G_s=2.2\times10^{-12}$ ($3.6\times10^{-12}$ with decay); 128 sources all at the default clamp
($A=1,896$) $G_s=1.3\times10^{-11}$; at $\epsilon_w=10^{-300}$ ($A=88,547$) $G_s=6.3\times10^{-10}$.
Revision 1's constant $128\cdot2^{-44}=7.3\times10^{-12}$ was 1.8x too small at the second and 86x
at the third, and 3x too large at the first: it was not a bound, it was a tuning. The innovation
guard is $4\times$ the sum of the `innovation` cap ($2^{-50}$) and the `decay` cap ($2^{-49}$),
$1.1\times10^{-14}$, rounded up to $2^{-46}$.

Rules for the vectors and tests (ML M5 parts 4 to 6):

- Every vector that feeds a decision carries `distance`, `guard` and `class`. The generator keeps a
  case only if `distance > 4*guard` (firm, with its outcome) or `distance <= guard` (marginal, with
  no outcome). A case between the two is dropped. The committed file has 59 pool decisions: 57 firm,
  2 marginal.
- A test never pins the outcome of a marginal vector. It pins the `marginal` flag.
- An exact tie built from bit-identical inputs in the same order is exact on every host, because the
  same operations run on the same bits. Such a vector is class `firm`, `distance` 0, `kind=tie`.
- The "about 64 events" wording of I3-2a is correct and stays; no test pins a count on a boundary.
- The user guide (F1) says: a decision within about $10^{-11}$ in score of its threshold (more for a
  pool of many clamped sources: the result carries the guard it used) may differ between hosts, and
  the result says so.

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
negative logit. The vectors hold all of these (117 `shrink` cases, $c$ up to $10^6$).

## 7. O10: the gate-rejection rule

### 7.1 Why the streak fails, and why revision 1's window failed too

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

**Revision 1's fraction (H = 64, FAULTED at 0.70) is proved for independent events and fails under
autocorrelation** (ML-1). Gate innovations from a real filter are correlated (coloured noise,
unmodelled dynamics, a reference that lags a manoeuvre). Reproduced with a stationary Gaussian AR(1)
residual of lag-1 correlation $\rho$ and a two-sided 95% gate (`ar.c`; the marginal rejection rate
is 5% whatever $\rho$ is), restarting the counters at each latch:

| $\rho$ | events simulated | revision-1 latches | one per                 | per device-year at 100 Hz |
| ------ | ---------------- | ------------------ | ----------------------- | ------------------------- |
| 0.5    | $2\times10^7$    | 0                  | -                       | none seen                 |
| 0.9    | $2\times10^9$    | 0                  | -                       | none seen (0.63 yr)       |
| 0.99   | $8\times10^9$    | 503,379            | 15,900 events (2.6 min) | $2.0\times10^5$           |
| 0.995  | $8\times10^9$    | 1,532,819          | 5,200 events            | $6.0\times10^5$           |
| 0.999  | $4\times10^9$    | 1,948,629          | 2,050 events            | $1.5\times10^6$           |

against a claim of $10^{-6}$. The reviewer's first latch at event 8,261 for $\rho=0.99$ is one draw
from that geometric distribution. The proof was correct for what it assumed (7.4); a latch for life
rested on an assumption the document itself lists as unverified. No event-count rule can survive
arbitrary correlation, because a source that is wrong and a reference that has drifted look alike to
a gate (spec 5.2: a rejection "establishes inconsistency, not which sensor is wrong"). So the
guarantee is stated *conditional on a maximum residual autocorrelation*, and the rule is changed so
that the condition is a generous one.

### 7.2 The rule

Rejections drive **event-indexed exponentially weighted fractions of the same routine as I3-2's
health** (3.9, one function), on separate state:

- State per source and predicate calibration record: Doubles `gate_rejected` ($R_g$) and
  `gate_events` ($N_g$), absent until first written. This is the **fast** pair (half-life $H=64$),
  from which SUSPECT is read.
- State **per profile** (new in revision 2): a fast pair and a **slow** pair (half-life $H_s=4096$)
  over every tested event of the source on any record, `gate_rejected_fast`, `gate_events_fast`,
  `gate_rejected_slow`, `gate_events_slow`. FAULTED is read from these (ML-5a: with per-record
  counters alone, a source that spreads bad readings over 50 predicates never reaches the minimum
  evidence on any of them and never reads SUSPECT; the profile counts every tested event).
- An **event** is one reading that at least one enabled gate **tested** (verdict `ACCEPTED` or
  `REJECTED`). `UNTESTED` (cold start, no reference), duplicates, and readings refused before gating
  are not events. Gates run once, in the ingestion turn (spec 5.1), so no mark or dedup log is
  needed: a duplicate event ID never reaches the gate.
- A **hit** is a rejection by any enabled gate (innovation or slew). **The gate tests the nominal
  (uninflated) variance** (ML-4): if it tested the SUSPECT-inflated one, a source with a 3 sigma
  bias would be rejected 85% of the time at the nominal gate ($P(\lvert Z\rvert>1.96-3)$), enter
  SUSPECT, be gated at twice the standard deviation, be rejected only 18% of the time
  ($P(Z>3.92-3)$), fall below 0.30, return to NOMINAL, and repeat, and a moderately biased source
  could never reach 0.70. The inflation of SUSPECT applies to the source's weight in fusion only.
- Update: the I3-2 rescale, then $R\leftarrow bR+\mathrm{hit}$, $N\leftarrow bN+1$ with $b=2^{-1/H}$
  of the pair's own half-life. A note-style batch update is not needed (one verdict per event).
- $f=R/N$ once $N\ge$ the pair's minimum evidence weight.
- **SUSPECT** when the record's fast $N_g\ge N_S$ and $f_g\ge\theta_S$: displayed, discrete
  inflation 4 (spec 5.1).
- **FAULTED** when the profile's fast pair has $N\ge N_F$ and $f\ge\theta_F$ **and** the slow pair
  has $N_s\ge N_{Fs}$ and $f_s\ge\theta_{Fs}$: latches (reason code `gate_fraction`, origin
  `evidence`, A6). Only an authorised `reset` unlatches it. A reading accepted afterwards lowers the
  fractions and clears nothing, which is the spec's "accepted measurements clear only the streak"
  replaced by "the fractions decay".
- The displayed state is the worst of the health-fraction state, the gate state and the reasons; the
  inflation is the maximum of their factors, applied once.

Settings (actor-wide, as A2 says of health thresholds), with defaults and domains:

| Setting (`vagent.` prefix)          | Default | Domain                                        |
| ----------------------------------- | ------- | --------------------------------------------- |
| `epistemic_gate_half_life`          | 64      | Int in [1, 10^6] events                       |
| `epistemic_gate_suspect_fraction`   | 0.30    | finite, in (0, 1\]                            |
| `epistemic_gate_suspect_min_events` | 16      | [1, floor(0.9/(1-b))] = 83 at H = 64          |
| `epistemic_gate_fault_fraction`     | 0.70    | finite, in (`suspect_fraction`, 1\]           |
| `epistemic_gate_fault_min_events`   | 48      | \[`suspect_min_events`, floor(0.9/(1-b))\]    |
| `epistemic_gate_slow_half_life`     | 4096    | Int in \[8 x `half_life`, 10^6\] events       |
| `epistemic_gate_slow_fraction`      | 0.70    | finite, in (`suspect_fraction`, 1\]           |
| `epistemic_gate_slow_min_events`    | 2048    | [1, floor(0.9/(1-b_s))] = 5,318 at H_s = 4096 |

`min_events` is the weight $N$ at which $f$ starts to decide (I3-2's meaning): at $H=64$ the first
reading that can be SUSPECT is the 18th and the first that can latch on the fast pair is the 68th;
the slow pair first decides at the **2,515th** event ($N_s\ge2048$). The half-life is **twice**
I3-2's health default of 32, and the FAULTED threshold is 0.70: both are what the proof below needs
(a window twice as long halves the effect of correlated runs, section 7.4).

**What the slow pair costs, computed.** A source whose every reading is rejected reaches SUSPECT at
the 18th event, the fast FAULTED condition at the 68th, and the latch at the 2,515th (25 s at 100
Hz, 42 min at 1 Hz) from a clean start; from a long-clean source at $f_s=0.05$ the slow fraction
needs 6,812 events to reach 0.70 (68 s at 100 Hz, 1.9 h at 1 Hz). Meanwhile the readings are
rejected, so they enter no fusion: the latch is for life and does not need to be fast.
`slow_half_life` is a setting because the guarantee is a function of it (7.4.2).

### 7.3 Reuse and what differs from the health fraction

Same: the update, the rescale, the `min_events` meaning, the read-time rescale. Different: the
nominal rate (a content refusal rate is about 0; a gate rejection rate is the gate's false-alarm
rate), the minimum evidence for FAULTED, `N` counting only tested events, a latch (I3-2's health
fraction never latches), and three instantiations of one routine (a record's fast pair, a profile's
fast and slow pairs). The `reset` zeroes all of them with the rest of the epoch (I3-2 6.4); a new
incarnation inherits them as it inherits the health fraction. The routine must be free of the
hard-wired profile field names (plan, ML minor 5); **the built routine is not** (section 12).

### 7.4 The proof

**Claim.** If, for a stationary healthy source, the per-event rejection probability is at most $p$
and rejections are independent across events, then the probability that $f\ge\theta$ at any one
event is at most

```math
\inf_{t>0}\ \exp(-t\theta W)\prod_i\bigl(1-p+p\,e^{t w_i}\bigr),\qquad
w_i=b^i,\quad W=\sum_i w_i=\frac{1}{1-b}.
```

*Proof.* $f\ge\theta$ is $\sum_i w_i(X_i-\theta)\ge0$ with $X_i$ independent Bernoulli of mean at
most $p$ (a larger mean only raises the probability, so take $p$). For $t>0$, Markov's inequality on
$e^{t\sum w_iX_i}$ gives $P\le e^{-t\theta W}\prod_i E\,e^{t w_iX_i}$, and each factor is
$1-p+pe^{tw_i}$. The infimum over $t$ is attained by a convex one-dimensional minimisation
(`gate_bound.py`: a log-spaced grid then golden-section, in log space so that bounds below
$10^{-308}$ and weights of any size are representable; revision 1's search range stopped at $t=200$
and returned 1 for blocks of 16 or more; the tail beyond $60H$ terms has weight $2^{-60}$).
$\square$

The **union bound** over events then gives the false-latch probability per device-year as the
per-event bound times the events per year: 3.16e9 at 100 Hz, 3.16e8 at 10 Hz, 3.16e7 at 1 Hz. The
bound holds for any dependence between events *only* in the weak form $P\le p/\theta$ (Markov on the
mean of $f$), which is 0.14 at $p=0.10$; so the strong figures need the independence assumption
stated in 7.5, and the stress model of 7.4.2 shows how much correlation they tolerate. FAULTED needs
both pairs, so its probability is at most the smaller of the two bounds.

**7.4.1 Independent events, fast pair $H=64$, $\theta_F=0.70$, per device-year at 100 Hz.**

| $p$ (true rejection rate) | per event | per device-year |
| ------------------------- | --------- | --------------- |
| 0.05                      | 3.5e-123  | 1.1e-113        |
| 0.10                      | 4.7e-86   | 1.5e-76         |
| 0.15                      | 2.0e-64   | 6.3e-55         |

**7.4.2 Correlated runs, and what autocorrelation maps to.** Model: events fall into independent
blocks of $m$ consecutive events that reject together, each block with probability $p$ (a stress
model, not a worst case over all dependences); the weights become $w_j=b^{mj}(1-b^m)/(1-b)$ and the
same bound applies. A stationary AR(1) process of lag-1 correlation $\rho$ has the variance
inflation of its sample mean $(1+\rho)/(1-\rho)$, so **$m=(1+\rho)/(1-\rho)$** is the block length
of the same effective sample size: $\rho=0.9,0.95,0.99,0.995$ are $m=19,39,199,399$. Fast pair alone
(the revision-1 rule), per device-year at 100 Hz:

| $p$  | $m=1$   | $m=2$   | $m=4$   | $m=6$   | $m=8$  | $m\ge16$ ($\rho\ge0.88$) |
| ---- | ------- | ------- | ------- | ------- | ------ | ------------------------ |
| 0.05 | 1e-113  | 1.9e-52 | 7.7e-22 | 1.2e-11 | 1.5e-6 | $>1$: nothing proved     |
| 0.10 | 1.5e-76 | 6.9e-34 | 1.5e-12 | 1.9e-5  | 6.8e-2 | $>1$                     |
| 0.15 | 6e-55   | 4.5e-23 | 3.8e-7  | 7.6e-2  | 34     | $>1$                     |

Revision 1 stopped the table at $m=8$ and so never printed the region ($m\ge16$) its guarantee
covers nothing of. Slow pair ($H_s=4096$, $\theta_{Fs}=0.70$), the same units, from `slow_table.py`:

| $\rho$ | $m$ | $p=0.05$ | $p=0.10$ | $p=0.15$ |
| ------ | --- | -------- | -------- | -------- |
| 0.9    | 19  | 1e-403   | 1e-278   | 1e-206   |
| 0.95   | 39  | 1e-192   | 1e-131   | 1e-96    |
| 0.99   | 199 | 1e-30    | 1e-18    | 1e-11    |
| 0.995  | 399 | 1e-11    | 1e-5     | 0.19     |
| 0.998  | 999 | $>1$     | $>1$     | $>1$     |

Entries are the bound (the minimum of the two pairs, which here is the slow one) as a probability
per device-year at 100 Hz; values above 1 say nothing. At the shorter health half-life of 32,
$p=0.10$, $m=4$ gives 6.8e-2 per year, which is why the fast window is 64. Simulation agrees and is
not a proof: with the slow pair the rule latched **0 times** in $8\times10^9$ events at $\rho=0.99$
(2.5 device-years), $8\times10^9$ at 0.995, $4\times10^9$ at 0.999 and $2\times10^9$ at 0.9, where
revision 1 latched 503,379, 1,532,819, 1,948,629 and 0 times. At $\rho=0.9999$ ($m=19,999$) it fails
(1,153 latches in $4\times10^8$ events), and the table says so by printing nothing for it.

**7.4.3 Start-up, and how many starts there are.** A fresh profile or a reset has fewer terms. The
union over the first 1,500 events with $N_F=48$ is $9.2\times10^{-30}$ per start at $p=0.10$, $m=1$
and $2.3\times10^{-7}$ at $p=0.10$, $m=4$ for the fast pair alone. With the slow pair, whose first
decision is the 2,515th event, the union over the first 24,576 events is, per start: $m=50$
($\rho\approx0.96$) $10^{-30}$ ($p=0.05$) and $10^{-20}$ ($p=0.10$); $m=200$ ($\rho\approx0.99$)
$10^{-6}$ and $10^{-3}$; $m=400$ $10^{-2}$ and $1$ (`gate_startup_slow.py`). The count of starts is
not one: a start is a new profile, a `reset`, or a new epoch (counters are zeroed by reset, 7.3), so
a device with $K$ profiles and $R$ resets a year has $K+R$ starts (ML-3; with profile-level counters
$K$ is the number of sources, not of source-predicate records). **The figure is therefore the steady
state plus $(K+R)$ times the per-start bound**, and Q1 asks the lead to confirm it in that form.

**The accepted rate.** **At most $10^{-6}$ false FAULTED latches per device-year at 100 Hz** (a
fleet of $10^4$ such devices sees one per hundred years), proved in steady state for the region
**$p\le0.15$ and residual autocorrelation $\rho\le0.99$ ($m\le199$; bound $10^{-11}$) or $p\le0.05$
and $\rho\le0.995$ ($10^{-11}$)**, which covers a 95% gate with a factor of 3 of slack in the
rejection rate, plus $(K+R)$ starts at at most $10^{-30}$ each for $\rho\le0.96$ (and $10^{-6}$ each
at $p=0.05$, $\rho=0.99$, where the start-up is the binding term). At 10 Hz and 1 Hz the steady
figures are 10 and 100 times smaller. Outside the region the rate is not claimed, and a source
rejected that often is not obviously healthy. The condition goes into the user guide ("the gate
latch assumes residual correlation times under about 200 events, i.e. 2 s at 100 Hz") and into Q1.
The figure is a fleet-policy number the lead confirms; the bound does not depend on it.

**What the rule detects.** A source whose every reading is rejected: 7.2. A source that rejects half
its readings stays SUSPECT (inflated, still used; its rejected readings are excluded). The
stationary spread of the fast $f$ at a true rate $r=0.8$ is 0.029, so a source rejecting 80%
satisfies the fast condition within a few dozen events and the slow one by the 2,515th. A source
sitting exactly at the SUSPECT threshold flaps: at a true rate of 0.30 the state changes 537 times
per $10^4$ events (once per 19), at 0.25 156 times, at 0.35 191 times, and is quiet at 0.20 (3.6)
and 0.40 (9.5) (`gate_flap.py`). **Exit hysteresis is declined** (ML-4): the state is derived, a
hysteresis needs a stored bit per record and a cross-turn dependency, and the flap is bounded to
sources whose true rate is within about 0.05 of the threshold, where "SUSPECT" is a fair description
of either state; the version bump of I4-1b per flip is the cost, and I4-8 measures it before a bit
is added.

### 7.5 What the figure assumes, and the common-cause guard

The bound needs three things, which the test names: independent events, or correlation no longer
than the region of 7.4.3 ($\rho\le0.99$, or runs of $m\le199$); a per-event false-alarm rate at most
the stated $p$ (gate quantile at most 0.05, so $p\le0.15$ allows three times the nominal rate); and
a reference that is not itself wrong. The last fails by construction when the **reference** drifts:
every source then rejects together, each reaches $f\ge\theta_F$, and the rule would latch the whole
predicate. **Recommended guard (Q2):** if at least $\max(2,\lceil n_t/2\rceil)$ of the $n_t$ sources
tested on one predicate key have $f_g\ge\theta_S$, write no latch this turn and place a degrade-kind
reason `reference_suspect` on those sources instead. Spec 5.2 says a rejection "establishes
inconsistency, not which sensor is wrong"; the guard is that sentence made executable. It costs one
count per ingestion turn.

The guard is a **degrade, not an exoneration** (ML-5b): the sources stay inflated, their rejected
readings stay excluded, and `reference_suspect` is displayed on all of them. Colluding or Sybil bad
sources at half of the tested set can suppress the latch (including their own) while it lasts. For
$n_t=1$ there is no guard and cannot be one: a lone source and its reference are indistinguishable,
which is the reason the latch needs the slow pair. A **time bound on the suppression is declined**:
a permanent `reference_suspect` is the correct permanent report when the reference is permanently
wrong, and a bound would reopen exactly the latch-the-whole-predicate failure the guard exists for.

### 7.6 Tests of the implementation

Zero latches in $n$ events bounds a rate only by about $3/n$, and a one-sided Chernoff cap is passed
by a halved half-life or a doubled hit weight (ML-6). The tests are therefore:

- **Differential.** The same seeded rejection sequences go to the IR routine and to `fraction_run`
  of the reference; $R$, $N$ and $f$ must match **bit for bit** (the estimator is T0, 3.9). The five
  `fraction_run` vectors are the minimal form; I4-8 adds sequences of 10^5 events at $p=0.05$ and
  0.60.
- **Monte Carlo, two-sided, with a defined estimator.** The statistic is the *per-event occupancy*:
  over independent chains, the fraction of (chain, event) pairs after a 400-event warm-up with
  $f\ge\theta$ (chains do not stop at a latch, and events inside a chain are dependent, so the
  standard error is taken across the per-chain occupancies). At $p=0.60$, $\theta_F=0.70$, $H=64$
  the Chernoff bound is $1.73\times10^{-2}$ and the prototype's 4,000 chains of 3,600 events give
  $2.35\times10^{-3}$ with a standard error of $8\times10^{-5}$ (`smoke.py`, seed 19). The test
  asserts the observed occupancy lies within four standard errors of the pinned value ($2.03$ to
  $2.67\times10^{-3}$) **and** below the bound, so a halved half-life or a doubled hit weight (which
  move the occupancy by a factor of several) fails. At $p=0.55$: bound $1.3\times10^{-4}$, occupancy
  $1.6\times10^{-5}$ with standard error $5.5\times10^{-6}$, so the interval is wide and the test is
  the bound only.
- **AR(1), pinned** (`gate_ar1_hits.tsv`, `ar1_hits.py`). Five sequences of 300,000 events at
  $\rho=0.5,0.9,0.99,0.995,0.999$ and 5% marginal rejection. The test feeds each to the IR routine
  and asserts the recorded latches of both rules: at $\rho=0.5$ and 0.9 neither rule latches; at
  0.99, 0.995 and 0.999 the revision-1 rule latches 7, 51 and 68 times (first at events 74,766,
  3,735 and 13,082) and the revision-2 rule **0 times**. The 0.9 case is the boundary measured, and
  0.99 is the region's edge asserted; the pinned file documents that the old rule fails.
- Runs of rejections arriving as blocks (7.4.2); the common-mode drift of 7.5; a rejection trains
  nothing and blames no sensor (spec 5.2-15); the profile counter latches a source that spreads
  rejections over 50 predicates (ML-5a).

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

I3-2's accepted `suspect_fraction` decision has a T1 dependence through `b` **as built**. With `b`
from the series (3.9, section 12) the fraction is T0 and the decision has no margin. Until that
slice lands, the guard of 5.3 (the fraction guard $H\cdot2^{-48}$, and $4\,\delta b\,(H/\ln2)^2$ for
`N >= min_events`) supplies the `marginal` flag when I4 reads the state. This design leaves I3-2's
text alone.

## 9. Consequences for the slices

| Slice   | What this design gives it                                                                                                                                                                                                                            |
| ------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| I4-1a   | `score_tolerance` in log-odds (A14), the gate settings of 7.2 (now eight, with the slow window) and the domains of A16 enter `vagent-policy-3` and the settings table once; `decay` absent means $\lambda=0$ (8.1).                                  |
| I4-2    | The sequences of 3.2 to 3.4 and 6, the caps as formulas, the vectors and the test of 4.5 (mutants included), the `FLT_EVAL_METHOD` assertion and `-ffp-contract=off`, `finite()`, the typed refusals of 3.10. Commits the files of 4.4 after review. |
| I3-2a-r | (proposed, section 12) the retrofit of the built health routine: parameterised state cells, `b` by series.                                                                                                                                           |
| I4-3    | `sumC`, `nEff`, `pool`, the canonical order, `at_least`, the per-query guard $B_s$, the tie rule on $s$ (3.5, 3.6, 5.3); the spec's closed forms asserted as marginal, not as equality.                                                              |
| I4-8    | The rule of 7.2 and the proof 7.4; settings; the tests of 7.6 (differential, Monte Carlo, pinned AR(1)); the common-cause guard if the lead accepts Q2; profile-level counters; `innovation` and `slew` of 3.7.                                      |
| I4-9    | `cluster` without `Sqrt`, margin 0 (3.8); `fusion` (3.7); the $t_\star$ rule (8.2).                                                                                                                                                                  |
| I5-4    | The SGD vectors reuse the file format; `Exp` in $\sigma$ is T1; learned weights stay bits (5.1 item 3). Not designed here (I5-D).                                                                                                                    |

## 10. Proposed amendments and questions

| #   | Spec section | Proposed amendment                                                                                                                                                                                                                                                                                                                                           |
| --- | ------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| A12 | 5.1          | Gate rejections drive event-indexed fractions: SUSPECT at 0.30 (fast window 64) and FAULTED only when a fast (64, 0.70) and a slow (4,096, 0.70) fraction both hold, in place of "three consecutive rejections for suspicion, five for fault". The gate tests the nominal variance, not the SUSPECT-inflated one. Accepted measurements decay the fractions. |
| A13 | 3.4          | `risk: "lcb"` is the warrant shrunk toward 1/2 by $c\sqrt v$ and never past it: $\tfrac12+\operatorname{sgn}(\mu-\tfrac12)\max(0,\lvert\mu-\tfrac12\rvert-c\sqrt v)$.                                                                                                                                                                                        |
| A14 | 5.3          | `score_tolerance` is an absolute difference of the log-odds $s$ (default $10^{-9}$), not of $W$. `min_confidence` stays in $W$-space.                                                                                                                                                                                                                        |
| A15 | 5.2          | Drop the profile-variance sentence for v1; every consensus input carries `with variance:`.                                                                                                                                                                                                                                                                   |
| A16 | 8.1          | Policy domains made executable: `epsilon_w` in $[10^{-300},\tfrac12)$ (was $(0,\tfrac12)$) and `c` in $[0,10^6]$ (was $[0,\infty)$). A narrowing nobody configures against ($\ln(10^{300})=690$; a million standard deviations); it keeps the ratio of 3.4 and the product of 3.10 in range so that no numeric refusal is reachable from policy.             |

Questions for the lead:

1. **Q1.** Confirm the accepted false-latch rate ($10^{-6}$ per device-year at 100 Hz) **as a
   steady-state figure under a stated residual autocorrelation** ($\rho\le0.99$ at $p\le0.15$,
   7.4.3) **plus $(K+R)$ start-ups** at the per-start bounds there, or give another figure; the
   settings in 7.2 follow from it. Also confirm the latency price of the slow pair: a fully faulty
   source latches at the 2,515th tested event (25 s at 100 Hz, 42 min at 1 Hz) and not the 68th.
1. **Q2.** Accept the common-cause guard of 7.5 (recommended), or leave it to I4-8's review.
1. **Q3.** The `marginal` Bool on decision records (5.3) is a result-ABI field; I4-D2 or I4-5 owns
   it. Confirm the name and that it is a Bool and not a code per decision.
1. **Q4.** Gate counters: this design puts the SUSPECT pair per calibration record and the latch
   pair (fast and slow) on the profile. Per record alone keeps one predicate's frame error from
   latching a source for others but lets a source spread bad readings across predicates unseen
   (ML-5a); the profile latch needs rejection across the whole profile.
1. **Q5.** `InvalidEpistemicState` and `EpistemicNumericFault` are proposed additions to T1's
   refusal vocabulary.
1. **Q6.** Where the vectors live: now in the tree as 4.4 lists (a directory under `tools/` plus
   `tests/samples/vagent/`), uncommitted. Confirm, or name another home, and which slice commits
   them (proposed: the lead commits the review fixes now; I4-2 wires the C++ test).
1. **Q7.** The retrofit slice of section 12 (I3-2a-r) before I4-8: approve or fold into I4-8.

## 11. Not done, and limits

- The vectors cover every routine of 3.1 except `cluster` and `slew` (their sequences are
  `Sub Mul Add` and need only the `bits.*` form, which the generator produces the same way). The
  real generator adds them.
- The bound of 4.2 is a first-order running bound, not an interval proof; the perturbation run is
  its check, and a case that fails it is a finding about the bound.
- The false-latch bound assumes the gate's rejections are independent or in blocks no longer than
  199 events (7.4.3). The distribution-free bound is 0.14 per event. The block model is a stress
  model, not a bound for all dependences; simulation at $\rho=0.99$ to 0.999 agrees but reaches only
  2.5 device-years, nowhere near $10^{-6}$ per year.
- Per-start figures at $\rho=0.99$ are weak ($10^{-6}$ at $p=0.05$, $10^{-3}$ at 0.10): the slow
  pair has about a dozen effective blocks at its first decision. A larger `slow_min_events` buys it
  back at latency.
- The T0 claim is verified on x86_64 only (5.1).
- No C++ was written or run. The Python stand-in replays the sequences in IEEE doubles; it checks
  the format, the tolerance and the mutants, not the VM.

## 12. Consequences for built code

ML-1 asks whether the same weakness is in the already-built I3-2a health estimator
(`vagent/runtime/epistemic_emit_health.cpp`, I3-2 section 3.2). **It is, in kind, with a smaller
consequence.** The estimator is the same event-indexed exponentially weighted fraction, and its
declared reading ("a source refusing under 10% stays NOMINAL", I3-2 3.3) and its tests (5% bad, `f`
stays under `suspect_fraction`, I3-2 tests R3-12 and F4) assume independent outcomes. Refusals that
arrive in bursts break that: a failure inside a long note, a clock mapping misconfigured for a
minute, a firmware update. Computed (`i32a_burst.py`, $H=32$, $\theta=0.25$, `min_events` 8):

| Marginal refusal rate 5%, bursts of | SUSPECT onsets per $10^6$ events | time in SUSPECT |
| ----------------------------------- | -------------------------------- | --------------- |
| 1 (independent)                     | 0                                | 0               |
| 4                                   | 126                              | 0.07%           |
| 8                                   | 970                              | 0.96%           |
| 16                                  | 2,868                            | 4.9%            |

The per-event Chernoff bound for $f\ge0.25$ at $p=0.05$ is $10^{-8.1}$ for independent events,
$10^{-2.0}$ for blocks of 4 and $10^{-0.5}$ for blocks of 16; 14 consecutive refusals from steady
state suffice. So "stays NOMINAL under 10%" is true of independent refusals and false of bursts.

**What it costs today.** Nothing irreversible. The built code latches only through an authorised
`isolate` action (FAULTED, `kProfileFaulted` set at `:261`), never from `f`; SUSPECT from the
fraction clears by itself as the burst decays, "displayed and, in I4, inflated; its readings are
still stored and still calibrate" (I3-2 3.4). The harm is a transient display state and a transient
inflation of 4, and a wording that overclaims. **The latch risk appears only when I4-8 reuses the
routine for the gate**, which is what sections 7.1 and 7.2 repair (the slow pair).

Two further findings about the built routine, both met while checking this:

1. **It is not parameterised.** Revision 1 of this design, and I3-2 section 3.2 ("I3-2a emits it
   parameterised by its two state cells"), assumed the routine takes its state cells as arguments.
   As built it reads and writes the fixed profile fields `health_refused_weight` and `health_weight`
   (`epistemic_emit_health.cpp:624, 633, 642, 670, 671`). I4-8 needs three instantiations (7.3).
1. **`b` is T1.** It is `Exp(Div(-ln2, H))` (`emitHealthRescaled`, `:597-600`), and the note
   mixture's $b^n$ is a second `Exp` (`:702`), so the health fraction, its `N >= min_events` test
   and the rescale cap `1/(1-b)` differ across hosts in the last bit, and `1-b` loses digits at
   large $H$ (NUM m3). 3.9 moves it to the series of `decay`, which is T0 and equal to `Exp` to
   within its own ulp (the mixture's $b^n$ takes the series when $n\ln2/H<\tfrac12$ and `Exp`
   otherwise, so only a long note stays T1); stored $R$ and $N$ are untouched (stored bits are
   authoritative, 5.1 item 3).

**Proposed fix slice, I3-2a-r (no code edited here):**

1. Parameterise `emitHealthRescaled`/`emitHealthStore` and the update over the two state cell names,
   `b` and the evidence floor (a mechanical change; the existing call passes today's names).
1. Compute `b` and `1-b` by the series, H = 1 special-cased (3.9), and add the `fraction_run`
   vectors of 4.4 to the test; the T1 guard rows of 5.3 then retire.
1. Tests: a burst test (bursts of 4 and 8 at 5% marginal, onsets in the table above as an
   expectation, not a failure), and the same sequences through both the routine and `fraction_run`.
1. Reword I3-2 3.3's claim to "independent refusals", with the table.

It precedes I4-8 and nothing else waits on it (Q7).

## 13. Review dispositions (round 1)

Personas: NUM = numerics, ML = machine learning/statistics. "Declined" lines give the reason.

| ID     | Persona | Severity | Disposition                                                                                                                                                                                                                                      | Section           |
| ------ | ------- | -------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | ----------------- |
| NUM-M1 | NUM     | MAJOR    | Adopted. `betaVar = pq/(s+1)`; revision-1 failure reproduced (divisor 0 at 1e-200, overflow at 1e150); range tracker, 28 range vectors, 3 refusals; `var_old` mutant rejected by 31                                                              | 3.2, 2, 4.1-4.5   |
| NUM-M2 | NUM     | MAJOR    | Adopted by the second option, per-query cap and guard $16uA$, $B_s$; reproduced (revision-1 guard 1.3x over at the default epsilon, 65x at 1e-300); floor declined (leaves a wrong constant, is a ceiling); epsilon domain \[1e-300, 1/2) is A16 | 3.4, 3.1, 5.3, 10 |
| NUM-M3 | NUM     | MAJOR    | Adopted. Host = (libm build, CPU feature set); header names it, checker prints values (values stay out of the header to keep it byte-reproducible: partial); replay compares inside one process                                                  | 5.1, 4.4          |
| NUM-m1 | NUM     | minor    | Adopted. Relative guard on the smaller side                                                                                                                                                                                                      | 3.6, 5.3          |
| NUM-m2 | NUM     | minor    | Adopted. $\hat x_f$ cap relative to $r_f\sum\lvert inv_ix_i\rvert$; conditioning refusal declined (result still the best double)                                                                                                                 | 3.7, 3.1          |
| NUM-m3 | NUM     | minor    | Adopted and widened: $b$ and $1-b$ by series make the whole fraction T0; as-built guard retained until I3-2a-r                                                                                                                                   | 3.9, 5.3, 12      |
| NUM-m4 | NUM     | minor    | Adopted. Pinned `requirements.txt`, version assertion, random-method grep, explicit import list; files in the tree (uncommitted; commit is the lead's)                                                                                           | 4.1, 4.4          |
| NUM-m5 | NUM     | minor    | Partly: wording "by construction, verified on x86_64", `-ffp-contract=off`. Declined to claim verified: no aarch64 host here                                                                                                                     | 5.1               |
| NUM-m6 | NUM     | minor    | Adopted: "to within $u$"; $c\le10^6$ (A16) with the corrected reason (conditioning, not overflow: $t+t\le c$); analytic caps                                                                                                                     | 3.3, 3.10, 3.1    |
| ML-1   | ML      | MAJOR    | Adopted. Reproduced (503,379 latches in 2.5 device-years at rho 0.99). Guarantee conditional on rho \<= 0.99; slow window AND; AR(1) vectors and tests; built I3-2a affected in kind                                                             | 7.1-7.6, 12       |
| ML-2   | ML      | MAJOR    | Adopted. `sum` vectors (plain loop off by 2e-13 to 6e-12, compensated exact); every mutant must be rejected, asserted; pool/fusion shape added, pool shown not to discriminate                                                                   | 3.5, 4.5          |
| ML-3   | ML      | minor    | Adopted. Steady state plus $(K+R)$ per-start bounds; start-up with the slow pair computed                                                                                                                                                        | 7.4.3, Q1         |
| ML-4   | ML      | minor    | Partly. Gate on the nominal variance (feedback removed); flap rate quantified. Exit hysteresis declined: needs a stored bit per record, bounded to sources within 0.05 of the threshold                                                          | 7.2, 7.4.3        |
| ML-5   | ML      | minor    | (a) adopted: profile-level counters. (b) adopted: guard is a degrade, n_t = 1 stated. Time bound on suppression declined: it reopens the reference-drift latch                                                                                   | 7.2, 7.5, Q4      |
| ML-6   | ML      | minor    | Adopted. Differential bit-for-bit test, defined per-event occupancy estimator, two-sided interval, pinned AR(1) sequences                                                                                                                        | 7.6               |
| ML-7   | ML      | minor    | Adopted. Perturb maxima reported (0.77 at 2 ulp); a 3 ulp libm fails the probes by name; bound unchanged                                                                                                                                         | 4.5               |
| ML-8   | ML      | minor    | Adopted. Series is 18 terms, first omitted $1.6\times10^{-23}$; claim restated                                                                                                                                                                   | 3.3               |
