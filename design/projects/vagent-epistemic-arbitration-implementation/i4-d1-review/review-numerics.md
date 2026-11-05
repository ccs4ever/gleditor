# I4-D1 numerics review (NUM)

FAIL: 3 MAJOR, 0 BLOCKING, 6 MINOR.

Not reproduced: `mpmath` is not installed here, so I could not regenerate `vectors.tsv`. I did
re-derive by hand the figures I cite below, and the O10 bound at p=0.10, m=1 (4.77e-86, matches
7.4.1). Checked against `vortex_vm.cpp:516-523` and `:922-945`, and the Makefile: no `-ffast-math`,
no `-ffp-contract`, no `-march`.

## MAJOR

### M1. `betaVar` divisor underflows to zero and the VM's `Div` quirk returns 0.0 (3.2, 3.1, 1)

- 3.2 says "the divisor `s` is positive, so the Div quirk cannot arise". That covers `betaPair`
  only. `betaVar` divides by `(s*s)*(s+1)`, which is not `s`.
- The domain admits any finite positive alpha, beta with sum \<= 1e100. Take alpha = beta = 1e-200.
  Then `s*s` = 4e-400 underflows to 0, so the divisor is 0 and `Div` (`vortex_vm.cpp:518`) returns
  `0.0` and succeeds. The result is `v = 0`; the true value is 0.25. `shrink` then sees `t = 0`, and
  `lcb` silently equals the mean.
- Python raises `ZeroDivisionError` on the same inputs, which is why the prototype never noticed.
- Alpha = beta = 1e-160 gives `s*s` = 4e-320, a denormal, so the bound is wrong by a large factor.
- The reference uses mpmath at `workprec(53)`, which has an unbounded exponent (`r53`, line 25). The
  `bits.*` and the bound therefore never model underflow, overflow or denormals, and no committed
  vector goes below ~1e-6.
- Fix:
  - Compute `v = (p*q) / (s+1)` from the pair. Here p, q are in (0,1\] and `s+1 >= 1`, so no divisor
    can be zero.
  - This also removes the need for the `alpha+beta <= 1e100` bound that exists only to stop
    `s*s*(s+1)` overflowing.
  - Add edge vectors alpha, beta in {1e-300, 1e-160, 2^-1022}, and a generator check that every
    intermediate lies in [2^-1022, 2^1023]. Reject the case or flag it otherwise.

### M2. The logit and pool caps assume the default epsilon_w; the allowed domain breaks them

Sections: 3.4, 3.1, 3.10, 5.3.

- `epsilon_w` is validated in (0, 1/2) (3.4, 3.10). `clampLogit` has |ell| up to ln(1/eps), and its
  libm error is about 4u * |ell|.
- The cap is a constant 2^-46 = 1.42e-14, tuned on eps = 1e-6 (the prototype uses only
  `eps = lit(1e-6)`, line 327).
- At eps = 1e-15: |ell| = 34.5, error 1.53e-14, which exceeds the cap. At eps = 1e-300: |ell| = 690,
  error 3e-13, which is 20 times the cap. The `pool` cap n*2^-46 and the guard G = n*2^-44 are then
  too small by the same factor.
- A decision called `firm` can flip between hosts. That breaks the D1 contract the design exists to
  deliver.
- Below eps ~ 5.6e-309, `p/q` overflows to inf and `Ln` refuses with "input not finite"
  (`vortex_vm.cpp:931`). That surfaces as an unspecified Precondition failure, not a typed refusal.
- Fix, either:
  - Restrict `epsilon_w` to \[1e-9, 1/2). Then |ell| \<= 20.7 and the logit error is \<= 9.2e-15,
    which is below the cap. Say so in 3.10, and re-run the pool n = 128 worst case at eps = 1e-9.
    The measured 8.0e-13 was taken at 1e-6, and 44% of the cap there becomes about 66% here.
  - Or make the guard per query: G = 4\*(4u*sum_i w_i|ell_i| + 2u*|s|). That removes the dependence
    on a tuned constant.

### M3. "Same host, same binary: bit-equal" is false for `Exp`/`Ln` as stated (5.1 item 1, 2, 4)

- glibc x86_64 selects `exp`, `log` (and `pow`, `sin`) variants by ifunc at load time: FMA, FMA4,
  AVX2 or plain SSE2. The FMA variants contract differently, and results can differ in the last ulp
  on some inputs.
- So the same binary on two CPUs of one fleet, a VM live-migrated to an older CPU, or the same box
  after a glibc upgrade (`exp`/`log` were rewritten in 2.28) can return different bits.
- D1 is headlined as "replay is bit-equal on one host", and 5.1 item 4's cross-host cap is the only
  fallback. The design does not say that "host" means a given CPU feature set plus a given libm
  build.
- Concrete failure: I5-4 replays an Exp-bearing derived value after an OS update and `memcmp` fails.
- Fix:
  - Define the T1 same-host claim as "same process, or same libm build and CPU dispatch".
  - Add the libm identity to the vector header: `gnu_get_libc_version`, and the CPU features that
    select an `exp`/`log` variant.
  - Make the replay test compare within one process. Across processes, compare within the cap and
    report `marginal` accordingly.
  - Keep 5.1 item 2 (stored bits authoritative) as the real guarantee, and say the replay claim
    depends on it.

## MINOR

### m1. The absolute W' guard defeats the "full resolution through the pair" claim

Sections: 3.6, 5.3.

- `at_least(W, W', theta)` for theta near 1 tests `W' <= 1-theta`.
- The 5.3 guard is the absolute n\*2^-44, which is 7.3e-12 at n = 128. But W' ~ e^-s has error W' \*
  delta_s, so its absolute error is tiny when W' is tiny.
- For theta = 1 - 1e-12 and any s > 25 (W' < 1.4e-11) the decision is always `marginal`.
- Use a relative guard for the W' side: G' = W' * n\*2^-44 (+ rel 2^-49). Fix in 5.3 and 3.6.

### m2. The `fusion` x_f "rel 2^-47" cap cannot hold for mixed signs (3.1, 3.7)

- sumC compensates the additions, but the products `inv_i*x_i` each carry u\*|inv_i x_i| absolute
  error. Cancellation of the sum therefore makes the error large relative to |x_f|.
- Example: x_i = ±1e8 plus an offset of 1 gives about 1e-8 relative.
- The generator filters such cases, so vectors pass by selection. It is T0, so no cross-host effect.
- State the cap relative to sum|inv_i x_i|, or add an input-conditioning refusal.

### m3. The N >= min_events threshold is an unguarded T1 decision (3.9, 5.3 table)

- N is a sum of powers of b (T1: b is `Exp`). The test `N >= min_events` is on N, not on f.
- The 5.3 table guards only f against theta. A host whose N lands within ~1e-13 of an integer
  threshold can start deciding one event earlier or later.
- Probability is negligible, but it is not classified. Add a row: N vs min_events, guard ~H\*2^-48
  relative. Or round the comparison with an explicit margin and set the `marginal` flag.
- Also: `1-b` computed from b loses digits at large H. At H = 1e6, `1-b` has relative error 1.6e-10.
  The domain bound floor(0.9/(1-b)) can therefore differ by 1 across hosts, and the rescale cap N
  differs by ~1e-3 absolute. Compute `1-b` as -expm1-style (series, T0) or state the domain bound in
  H directly.

### m4. The reference and vectors are not durable or pinned (4.1, 4.4, Q6)

- `/tmp/p19-work/i4d1/ref/` holds the only copy. There is no `tools/vagent-epistemic-reference.py`
  or `tests/samples/vagent/epistemic_vectors.tsv` in the tree. The paths are good (the
  `tests/samples/vagent/` directory exists and neither path is ignored), but the slice that commits
  them is not named. State in 9 that I4-2 commits both, with the review fixes, as its first commit.
- `uv run --with mpmath` is unpinned. Use `mpmath==1.4.1` (the version recorded in the header). The
  byte-compare fallback "45 digits" then only applies on a deliberate pin bump.
- The header does not record the Python version. `random.Random(19).random()/uniform()` are stable
  across versions. Assert in the generator that only those two are used (`choice`, `shuffle` and
  `randrange` have changed across releases).
- The `check_no_libm.sh` regex is not in the design. Specify the banned import list (`math`,
  `numpy`, `cmath`, `scipy`, `from math`, `import math as`) explicitly.

### m5. The T0 claim is evidenced on x86_64 only (5.1 item 3)

- The Makefile has no `-ffp-contract` and no `-march`, so x86_64 baseline has no FMA and every
  `bits.*` match is vacuous for the FMA question.
- Contraction across VM opcodes is structurally impossible: each opcode is a separate evaluation
  through `std::variant`. The `static_assert(FLT_EVAL_METHOD == 0)` guards x87 only, not FMA.
- Require one non-x86 run (aarch64 or qemu-user) of the `bits.*` assertions before the T0 claim is
  recorded as verified. Otherwise call it "by construction, verified on x86_64".

### m6. Smaller points

- 3.3: "`p'+q' = 1` exactly" is false in floating point. `E = fl(1+em)` and `h` is not rounded, so
  the sum is `E - em`, which differs from 1 by at most u. Say "to within u".
- 6: `t = c*Sqrt(v)` with a finite but huge c overflows to inf. The result is an
  `EpistemicNumericFault`, not neutral. Bound c \<= 1e6 in 3.10, or define the behaviour.
- 3.1 caps are "largest bound seen, rounded up to a power of two" (1.3 to 2.3 times). Cap and guard
  then inherit prototype case selection (`logit` 6.3e-15 vs 1.42e-14 at eps = 1e-6; see M2). Record
  the analytic cap formula, not only the measured one, so a new vector class cannot exceed it
  silently.

## Checked, no finding

- Decay branch at hl \<= 1100 / x < 1/2: the branch is T0, both sides agree within the bound, and
  `exp(-762)` underflows to a finite 0 as spec 8.1 allows. The series truncation after INVFACT[18]
  is below 1e-24 (true value about 3.9e-25).
- The `Ln(p/q)` form: the ratio is in [eps/(1-eps), (1-eps)/eps] for eps >= ~1e-300.
- `nEff` at rho = 1 and rho = 0 is exact for g < 2^53.
- Neumaier branch comparison (`Abs(s) >= Abs(x)`) is T0 and has 7 opcodes.
- Fraction cross-host error (4.4e-16 per host for b in \[0.5, 1)) is consistent with the 6.6e-16\*H
  figure.
- O10 bound reproduced for p = 0.10, m = 1.
