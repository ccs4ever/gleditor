#!/usr/bin/env python3
"""Reference vectors for P19 numerics (I4-D1, revision 2).

Rules (checked by check_no_libm.sh):
  * no `math`, `numpy`, `cmath` import; exp, log and sqrt come from mpmath at 60 digits only;
  * inputs are written as hex-float bit patterns, outputs as 50-significant-digit decimals;
  * for every output a rigorous running absolute error bound is written, derived by replaying the
    routine's *operation sequence* with these rounding rules:
      +,-,*,/,sqrt : correctly rounded, relative error <= U = 2^-53
      exp, ln      : host libm, relative error <= LIBM = 4*U (2 ulp), assumption tested by libm probe
"""
import random, sys, hashlib
from mpmath import mp, mpf
import mpmath

mp.dps = 60
U = mpf(2) ** -53
LIBM = 4 * U
SEED = 19


def fx(d):  # exact value of a double (given as python float or hex string)
    return mpf(float.fromhex(d)) if isinstance(d, str) else mpf(d)


def r53(fn):
    with mp.workprec(53):
        return fn()


# Range tracker. The ideal value lives in mpmath (unbounded exponent); the binary64 path `f` is
# Python float arithmetic, which is IEEE binary64 round-to-nearest with gradual underflow. Every
# result is checked against the binary64 range so a case can never silently leave it:
#   subnormal : a result in (0, 2^-1022): relative accuracy is lost, 2^-1074 is added to its bound;
#   divzero   : the binary64 path divided by zero, where the VM's Div returns 0.0 and succeeds;
#   overflow  : an ideal result above the largest double: raised, the case is a refusal or an error.
MINNORM = mpf(2) ** -1022
MAXD = (2 - mpf(2) ** -52) * mpf(2) ** 1023
TINY = mpf(2) ** -1075  # half the spacing of the subnormals: the rounding error of one op
RANGE = set()


class RangeOverflow(Exception):
    pass


class R:
    """x: ideal result; b: bound on |computed - x|; f: the same sequence in binary64 at every
    step (exact IEEE result for + - * / sqrt), None once a libm call is involved."""
    __slots__ = ("x", "b", "f")

    def __init__(s, x, b=0, f="same"):
        s.x = mpf(x)
        s.b = mpf(b)
        s.f = float(s.x) if f == "same" else f


def _mk(z, b, f, rounds=True):
    # + and - are exact in the subnormal range (no rounding), * / sqrt exp ln round to a multiple of 2^-1074
    if z != 0 and abs(z) < MINNORM:
        RANGE.add("subnormal")
        if rounds:
            b = b + TINY
    if abs(z) > MAXD:
        raise RangeOverflow("intermediate above the largest double")
    return R(z, b, f)


def lit(d):
    """A double literal: exact itself, no error."""
    return R(mpf(d), 0, float(d))


def const(exact, dbl):
    """A constant whose double differs from the real number it stands for."""
    return R(exact, abs(mpf(dbl) - exact), float(dbl))


def _f(a, c, op):
    if a.f is None or c.f is None:
        return None
    return op(a.f, c.f)


def _fdiv(p, q):
    if q == 0.0:
        RANGE.add("divzero")
        return 0.0  # vortex_vm.cpp:518: Div by zero is 0.0 and succeeds
    return p / q


def lt(a, c):
    return (a.f < c.f) if (a.f is not None and c.f is not None) else (a.x < c.x)


def add(a, c):
    z = a.x + c.x
    b = a.b + c.b
    return _mk(z, b + U * (abs(z) + b), _f(a, c, lambda p, q: p + q), rounds=False)


def sub(a, c):
    z = a.x - c.x
    b = a.b + c.b
    return _mk(z, b + U * (abs(z) + b), _f(a, c, lambda p, q: p - q), rounds=False)


def mul(a, c):
    z = a.x * c.x
    b = abs(a.x) * c.b + abs(c.x) * a.b + a.b * c.b
    return _mk(z, b + U * (abs(z) + b), _f(a, c, lambda p, q: p * q))


def div(a, c):
    assert abs(c.x) > c.b, "division by a value the bound cannot separate from zero"
    z = a.x / c.x
    b = (abs(a.x) * c.b + abs(c.x) * a.b) / (abs(c.x) * (abs(c.x) - c.b))
    return _mk(z, b + U * (abs(z) + b), _f(a, c, _fdiv))


def sqrt(a):
    assert a.x >= 0
    z = mp.sqrt(a.x)
    if a.x > a.b:
        b = a.b / (mp.sqrt(a.x) + mp.sqrt(a.x - a.b))
    else:
        b = mp.sqrt(a.x + a.b)
    return _mk(z, b + U * (z + b), (None if a.f is None else float(r53(lambda: mp.sqrt(mpf(a.f))))))


def exp(a):
    z = mp.exp(a.x)
    b = z * (mp.exp(a.b) - 1)
    return _mk(z, b + LIBM * (z + b), None)


def ln(a):
    assert a.x > a.b
    z = mp.log(a.x)
    b = -mp.log(1 - a.b / a.x)
    return _mk(z, b + LIBM * (abs(z) + b), None)


def neg(a):
    return R(-a.x, a.b, f=(None if a.f is None else -a.f))


def absr(a):
    return R(abs(a.x), a.b, f=(None if a.f is None else abs(a.f)))


# ---------------------------------------------------------------- routines (operation sequences)
LN2 = const(mp.log(2), float(mp.log(2)))
HALF = lit(0.5)
ONE = lit(1.0)

# 1/k! as doubles for the expm1 series
SERIES_N = 17
INVFACT = [const(mpf(1) / mp.factorial(k), float(mpf(1) / mp.factorial(k))) for k in range(SERIES_N + 2)]


def series_truncation(y):
    """Bound on the first omitted series terms for |y| <= 1/2."""
    return abs(y) ** (SERIES_N + 2) / mp.factorial(SERIES_N + 2) * 2


def expm1_series(y):
    """expm1(y) = y (1 + y/2! + y^2/3! + ...) by Horner: 2 ops per term, no libm call."""
    acc = INVFACT[SERIES_N + 1]
    for k in range(SERIES_N, 0, -1):
        acc = add(INVFACT[k], mul(y, acc))
    out = mul(y, acc)
    out.b += series_truncation(y.x)
    out.x = mp.expm1(y.x)  # exact target of the sequence (truncation is inside the bound)
    return out


def beta_mean_pair(alpha, beta):
    s = add(alpha, beta)
    return div(alpha, s), div(beta, s)  # (mu, 1-mu) each to ~2 roundings


def beta_var(alpha, beta):
    """v = (alpha/s)(beta/s)/(s+1) = p q / (s+1): p, q <= 1 and s+1 >= 1, so no intermediate can
    underflow because of s*s or overflow because of s*s*(s+1), and no divisor can be zero."""
    pm, qm = beta_mean_pair(alpha, beta)
    return div(mul(pm, qm), add(add(alpha, beta), ONE))


def beta_var_old(alpha, beta):
    """Revision 1 (rejected): alpha*beta / ((s*s)*(s+1)). Kept only to prove the edge vectors fail it."""
    s = add(alpha, beta)
    num = mul(alpha, beta)
    den = mul(mul(s, s), add(s, ONE))
    return div(num, den)


def shrink_pair(alpha, beta, c):
    """O9 recommendation: uncertainty moves the weight toward 1/2, never past it, never away."""
    pm, qm = beta_mean_pair(alpha, beta)
    t = mul(c, sqrt(beta_var(alpha, beta)))
    dd = sub(pm, qm)
    two_t = add(t, t)
    if lt(two_t, dd):
        p, q = sub(pm, t), add(qm, t)
    elif lt(two_t, neg(dd)):
        p, q = add(pm, t), sub(qm, t)
    else:
        p, q = HALF, HALF
    # branch test is continuous across its edge; a wrong branch moves the result by the test's error
    pad = 8 * U if abs(p.x - HALF.x) <= 64 * U else 0
    return R(p.x, p.b + pad, f=p.f), R(q.x, q.b + pad, f=q.f)


def decay_pair(p, q, age, tau):
    """w(a) = 1/2 + exp(-lambda a)(w0 - 1/2), carried as the pair (w, 1-w), both sums of nonnegatives.
    h = a/tau is the number of half-lives; beyond 1100 of them exp(-x) is zero and the pair is (1/2, 1/2),
    decided before any product that could overflow."""
    try:
        hl = div(age, tau)
    except RangeOverflow:
        # a/tau above the largest double is +inf in binary64; the next operation is the comparison
        # with 1100, which is true, so no arithmetic ever touches it (recorded, not an error)
        RANGE.add("hl_inf")
        return HALF, HALF
    if lt(lit(1100.0), hl):
        return HALF, HALF
    x = mul(hl, LN2)
    if lt(x, HALF):
        em = expm1_series(neg(x))
        e = add(ONE, em)
        h = mul(HALF, neg(em))
    else:
        e = exp(neg(x))
        h = mul(HALF, sub(ONE, e))
    return add(mul(e, p), h), add(mul(e, q), h)


def clamp_pair(p, q, eps):
    one_m = sub(ONE, eps)
    if lt(p, eps):
        return eps, one_m
    if lt(q, eps):
        return one_m, eps
    return p, q


def logit_pair(p, q):
    return ln(div(p, q))  # one Ln of the ratio: no cancellation near 1/2 or near 0 and 1


def neumaier(terms):
    """Compensated sum in the given order; bound 2u|S| + 2 n^2 u^2 sum|x| plus the inputs' bounds."""
    n = len(terms)
    s = sum((t.x for t in terms), mpf(0))
    sabs = sum((abs(t.x) for t in terms), mpf(0))
    tb = sum((t.b for t in terms), mpf(0))
    f = None
    if all(t.f is not None for t in terms):
        acc, comp = 0.0, 0.0
        for t in terms:
            x = t.f
            tt = acc + x
            comp = comp + (((acc - tt) + x) if abs(acc) >= abs(x) else ((x - tt) + acc))
            acc = tt
        f = acc + comp
    return R(s, tb + 2 * U * abs(s) + 2 * n * n * U * U * sabs, f=f)


def neff(g, rho):
    gm1 = sub(g, ONE)
    return div(g, add(ONE, mul(gm1, rho)))


def pool(groups, rho_list, eps):
    """groups: list of lists of (p, q) pairs in canonical order. Returns (s, (W, 1-W), A, Neff) where
    A = sum_G n_eff(G) (1 + mean|ell|) and Neff = sum_G n_eff(G): the quantities the per-query cap
    and guard are written in (design 5.3). Both are ideal values, not rounded."""
    terms = []
    a_acc, neff_acc = mpf(0), mpf(0)
    for members, rho in zip(groups, rho_list):
        ells = [clamp_logit(p, q, eps) for (p, q) in members]
        g = lit(len(members))
        mean = div(neumaier(ells), g)
        ne = neff(g, rho)
        terms.append(mul(ne, mean))
        a_acc += ne.x * (1 + sum((abs(e.x) for e in ells), mpf(0)) / len(members))
        neff_acc += ne.x
    s = neumaier(terms)
    return s, sigmoid_pair(s), a_acc, neff_acc


def clamp_logit(p, q, eps):
    p, q = clamp_pair(p, q, eps)
    return logit_pair(p, q)


def sigmoid_pair(s):
    e = exp(neg(absr(s)))
    den = add(ONE, e)
    big, small = div(ONE, den), div(e, den)
    return (big, small) if s.x >= 0 else (small, big)


def health_b(H):
    """b = 2^(-1/H) and 1 - b, both T0 for H >= 2: x = ln2/H <= 0.35 < 1/2, so b = 1 + expm1(-x) comes
    from the series of `decay` (no libm) and 1 - b = -expm1(-x) is exact in the sense that it is not a
    subtraction of nearly equal numbers (NUM-m3). H = 1 is b = 1/2 exactly."""
    if H == 1:
        return HALF, HALF
    em = expm1_series(neg(div(LN2, lit(H))))
    return add(ONE, em), neg(em)


def fraction_run(H, hits):
    """The rescale-then-update of I3-2 3.2 over a hit sequence from R = N = 0. Returns (R, N, f)."""
    b, omb = health_b(H)
    cap = div(ONE, omb)
    Rr, Nn = lit(0.0), lit(0.0)
    for h in hits:
        if lt(cap, Nn):  # N > cap: R <- R cap / N, N <- cap (after a shrunken H)
            Rr, Nn = div(mul(Rr, cap), Nn), cap
        Rr = add(mul(b, Rr), lit(float(h)))
        Nn = add(mul(b, Nn), ONE)
    return Rr, Nn, div(Rr, Nn)


def inv_variance_fusion(rs, ws, xs, eps_floor):
    """r_f = 1 / sum(max(w, floor) / r_i);  x_f = r_f * sum(max(w, floor) / r_i * x_i).
    Also returns L1 = r_f sum|inv_i x_i|, the conditioning the x_f cap is written in (NUM-m2)."""
    invs = []
    for r, w in zip(rs, ws):
        wf = w if lt(eps_floor, w) else eps_floor
        invs.append(div(wf, r))
    sum_inv = neumaier(invs)
    rf = div(ONE, sum_inv)
    prods = [mul(i, x) for i, x in zip(invs, xs)]
    num = neumaier(prods)
    l1 = rf.x * sum((abs(p.x) for p in prods), mpf(0))
    return rf, mul(rf, num), l1


def innovation_d2(dx, r, w, p0, eps_floor):
    """d^2 = sum dx_k^2 / (r/max(w, floor) + p0); squares and sum in index order, no sqrt."""
    wf = w if lt(eps_floor, w) else eps_floor
    num = mul(dx[0], dx[0])
    for t in dx[1:]:
        num = add(num, mul(t, t))
    return div(num, add(div(r, wf), p0))


# ---------------------------------------------------------------- exact (spec-formula) versions
def exact_decay(w0, age, tau):
    lam = mp.log(2) / tau
    return mpf(1) / 2 + mp.exp(-lam * age) * (w0 - mpf(1) / 2)


def exact_shrink(alpha, beta, c):
    mu = alpha / (alpha + beta)
    v = alpha * beta / ((alpha + beta) ** 2 * (alpha + beta + 1))
    d = mu - mpf(1) / 2
    m = max(mpf(0), abs(d) - c * mp.sqrt(v))
    return mpf(1) / 2 + (m if d >= 0 else -m)


def exact_pool(groups, rhos, eps):
    s = mpf(0)
    for members, rho in zip(groups, rhos):
        ells = []
        for w in members:
            w = min(max(w, eps), 1 - eps)
            ells.append(mp.log(w / (1 - w)))
        g = len(members)
        s += g / (1 + (g - 1) * rho) * sum(ells) / g
    return s


# ---------------------------------------------------------------- caps (analytic, asserted per case)
# A cap is a formula in the case's own inputs and outputs, so a new vector class cannot exceed it
# silently: gen() asserts bound <= cap for every output of every case and fails the run otherwise.
U2 = lambda k: mpf(2) ** -k
KAPPA_POOL = 16          # pool: bound.s <= KAPPA_POOL * u * A  (A below); measured maximum in MEASURED
KAPPA_FUSION_X = 2 ** -49


def caps(routine, ins, res, extra):
    """Return {output: cap} for one case. `extra` carries the analytic quantities (A, Neff, L1)."""
    out = {}
    if routine == "beta_mean":
        out = {k: U2(51) * abs(r.x) for k, r in res.items()}
    elif routine == "beta_var":
        out = {"v": U2(49) * abs(res["v"].x)}
    elif routine == "shrink":
        out = {k: U2(49) for k in res}
    elif routine == "decay":
        out = {k: U2(49) * max(abs(r.x), mpf(0)) + U2(50) * 0 for k, r in res.items()}
    elif routine == "clamp_logit":
        out = {"ell": 8 * U * (1 + abs(res["ell"].x))}
    elif routine == "sigmoid":
        out = {k: U2(49) * abs(r.x) for k, r in res.items()}
    elif routine == "sum":
        out = {"sum": res["sum"].b}   # the bound is the formula 2u|S| + 2n^2u^2 sum|x| (+ inputs)
    elif routine == "pool":
        cap_s = KAPPA_POOL * U * extra["A"]
        W, Wc = res["W"].x, res["one_minus_W"].x
        # The running bound treats e in e/(1+e) as independent in numerator and denominator, so for the
        # small side m = e/(1+e) it is up to 3/2 m B_s ((1+2e)/(1+e) <= 3/2), and for the big side
        # 1/(1+e) it is m B_s; the true derivative is smaller (sigma' <= 1/4). Formed from the pair.
        m = min(W, Wc)
        k_small, k_big = mpf(3) / 2, mpf(1)
        cs = cap_s * (1 + cap_s)
        out = {"s": cap_s,
               "W": (k_small if W <= Wc else k_big) * m * cs + U2(49) * W,
               "one_minus_W": (k_small if Wc <= W else k_big) * m * cs + U2(49) * Wc}
    elif routine == "fusion":
        out = {"r_f": U2(50) * abs(res["r_f"].x), "x_f": KAPPA_FUSION_X * extra["L1"]}
    elif routine == "innovation":
        out = {"d2": U2(50) * abs(res["d2"].x)}
    elif routine == "fraction_b":
        out = {"b": U2(49) * abs(res["b"].x), "one_minus_b": U2(49) * abs(res["one_minus_b"].x)}
    elif routine == "fraction_run":
# dN/db = sum i b^(i-1) ~ 1/(1-b)^2 ~ (H/ln2)^2: the bound against the ideal b = 2^(-1/H) is the
        # conditioning of N in b times the 3u of b; the same b is computed on every host (T0), so this
        # is the distance from the ideal, not a cross-host difference
        hh = extra["H"] + 8
        out = {"R": U2(49) * hh * hh, "N": U2(49) * hh * hh, "f": U2(49) * hh}
    elif routine.startswith("libm_"):
        out = {k: mpf(0) for k in res}
    return out


MEASURED = {}  # routine -> largest bound/cap ratio seen in this run (printed, and written to the header)


# ---------------------------------------------------------------- vector generation
def dec(x, digits=50):
    return mp.nstr(x, digits, min_fixed=-60, max_fixed=60)


def hx(d):
    return float(d).hex()


def logu(rng, lo, hi):
    """Log-uniform double in [2^lo, 2^hi] without a floating log (the exponent is drawn, 2^e in mpmath)."""
    e = mpf(rng.uniform(float(lo), float(hi)))
    return float(mp.power(2, e))


def draw_int(rng, lo, hi):
    """Integer in [lo, hi] from random() alone: randint/randrange/choice changed across releases."""
    return lo + int(rng.random() * (hi - lo + 1))


def gen(path, n_random=40):
    rng = random.Random(SEED)  # only random() and uniform(): stable across Python versions
    out = []

    def emit(routine, kind, ins, res, **extra):
        rng_flags = sorted(RANGE)
        assert "divzero" not in rng_flags, "case %s reaches the Div-by-zero quirk" % routine
        RANGE.clear()
        rec = {"id": "%s/%03d" % (routine, sum(1 for r in out if r["routine"] == routine)),
               "routine": routine, "kind": kind,
               "range": "+".join(rng_flags) if rng_flags else "ok",
               "in": {k: (hx(v) if not isinstance(v, (list, tuple)) else None) for k, v in ins.items()},
               "out": {k: dec(r.x) for k, r in res.items()},
               "bound": {k: mp.nstr(r.b, 4) for k, r in res.items()}}
        if all(r.f is not None for r in res.values()):
            rec["bits"] = {k: float(r.f).hex() for k, r in res.items()}
        for k, v in ins.items():
            if isinstance(v, (list, tuple)):
                rec["in"][k] = [hx(t) if not isinstance(t, (list, tuple)) else [hx(u) for u in t] for t in v]
        # analytic cap per output: asserted here, so the doc's caps are not a measurement of the vectors
        quant = extra.pop("quant", {})
        floor = mpf(2) ** -1070 if "subnormal" in rng_flags else mpf(0)  # <= 32 rounding steps of 2^-1075
        for k, cap in caps(routine, ins, res, quant).items():
            cap = cap + floor
            if cap > 0 or res[k].b > 0:
                ratio = res[k].b / cap if cap > 0 else mpf("inf")
                assert ratio <= 1, "%s %s: bound %s exceeds cap %s" % (rec["id"], k, mp.nstr(res[k].b, 4), mp.nstr(cap, 4))
                MEASURED[routine] = max(MEASURED.get(routine, mpf(0)), ratio)
        rec.update(extra)
        out.append(rec)

    def refuse(routine, ins, why):
        RANGE.clear()
        out.append({"id": "%s/%03d" % (routine, sum(1 for r in out if r["routine"] == routine)),
                    "routine": routine, "kind": "refuse", "range": "n/a",
                    "in": {k: hx(v) for k, v in ins.items()}, "refuse": {"name": why}})

    # beta_mean / beta_var
    cases = [(1.0, 1.0), (8.0, 2.0), (1e12, 1.0), (1.0, 1e12), (0.1, 0.1), (3.0, 97.0)]
    for _ in range(n_random):
        cases.append((logu(rng, -3, 40), logu(rng, -3, 40)))
    # NUM-M1: values whose revision-1 formula underflowed, overflowed or went denormal. alpha + beta is
    # at most the largest double (checked); every intermediate of the pair form is in [0, 1] or s + 1.
    range_cases = [(1e-300, 1e-300), (1e-160, 1e-160), (2.0 ** -1022, 2.0 ** -1022), (5e-324, 5e-324),
                   (5e-324, 1.0), (1.0, 5e-324), (5e-324, 1e300), (1e-200, 1e200), (1e150, 1e150), (1e160, 1e160),
                   (1e300, 1e300), (8e307, 8e307), (1.5e308, 1.0), (1.0, 1.5e308)]
    for a, b in cases + range_cases:
        pm, qm = beta_mean_pair(lit(a), lit(b))
        kind = "range" if (a, b) in range_cases else ("edge" if a in (1.0, 1e12) or b in (1.0, 1e12) else "random")
        emit("beta_mean", kind, {"alpha": a, "beta": b}, {"mu": pm, "one_minus_mu": qm})
        emit("beta_var", kind, {"alpha": a, "beta": b}, {"v": beta_var(lit(a), lit(b))})
    for a, b in [(1.7e308, 1.7e308), (1.5e308, 1.5e308), (1.7976931348623157e308, 1.0e300)]:
        # the sum is not a double: refused at the routine entry (InvalidEpistemicState), never evaluated
        try:
            add(lit(a), lit(b))
        except RangeOverflow:
            refuse("beta_mean", {"alpha": a, "beta": b}, "InvalidEpistemicState")
        else:
            raise AssertionError("expected an overflowing sum")
    # shrink (lcb at an uninformative prior, O9)
    for a, b in [(1.0, 1.0), (2.0, 1.0), (1.0, 2.0), (20.0, 2.0), (2.0, 20.0), (1e6, 1.0), (50.0, 50.0),
                 (1e-300, 1e-300), (5e-324, 5e-324), (1e300, 1e300), (5e-324, 1.0)]:
        for c in (0.0, 0.5, 1.0, 1.7, 2.0, 10.0, 1e6):
            p, q = shrink_pair(lit(a), lit(b), lit(c))
            ex = exact_shrink(mpf(a), mpf(b), mpf(c))
            assert abs(p.x - ex) < mpf(10) ** -45, (a, b, c, p.x, ex)
            emit("shrink", "closed" if (a, b) == (1.0, 1.0) else "edge", {"alpha": a, "beta": b, "c": c},
                 {"w": p, "one_minus_w": q})
    for _ in range(n_random):
        a, b, c = logu(rng, 0, 30), logu(rng, 0, 30), rng.uniform(0, 3)
        p, q = shrink_pair(lit(a), lit(b), lit(c))
        assert abs(p.x - exact_shrink(mpf(a), mpf(b), mpf(c))) < mpf(10) ** -45
        emit("shrink", "random", {"alpha": a, "beta": b, "c": c}, {"w": p, "one_minus_w": q})
    # decay
    ws = [0.8, 0.999999, 1e-6, 0.5, 0.3, 0.9999999999]
    for w0 in ws:
        for age, tau in [(0.0, 1.5), (1e-9, 1.5), (1.5, 1.5), (0.1, 1.5), (3.0, 1.5), (1e3, 1.5), (1e5, 1.0), (1e300, 1e-10), (1e4, 5.0)]:
            p0 = lit(w0)
            q0 = lit(float(1 - mpf(w0)))  # the pair as the pipeline carries it
            p, q = decay_pair(p0, q0, lit(age), lit(tau))
            ex = exact_decay(mpf(w0), mpf(age), mpf(tau))
            assert abs(p.x - ex) < mpf(10) ** -13, (w0, age, tau, p.x, ex)
            kind = "closed" if (age == tau and w0 == 0.8) else "edge"
            emit("decay", kind, {"w0": w0, "one_minus_w0": float(1 - mpf(w0)), "age": age, "tau": tau},
                 {"w": p, "one_minus_w": q})
    # clamp_logit: the ratio form, at the default epsilon and across the whole epsilon domain (NUM-M2)
    for _ in range(n_random):
        pr = rng.uniform(1e-6, 1 - 1e-6)
        qr = 1 - mpf(pr)
        emit("clamp_logit", "random", {"w": pr, "one_minus_w": float(qr), "eps": 1e-6},
             {"ell": clamp_logit(lit(pr), lit(float(qr)), lit(1e-6))})
    for pr in [1 - 1e-6, 1 - 2e-6, 0.999, 1e-6, 2e-6, 0.5, 0.5 + 2.0 ** -40, 0.5 - 2.0 ** -53, 0.75]:
        qr = 1 - mpf(pr)
        emit("clamp_logit", "edge", {"w": pr, "one_minus_w": float(qr), "eps": 1e-6},
             {"ell": clamp_logit(lit(pr), lit(float(qr)), lit(1e-6))})
    for qv in [1e-6, 2e-6, 3.3e-6, 1e-5, 1.234567e-4]:  # q carried accurately; p = fl(1 - q) is not 1 - q
        pr = 1.0 - qv
        emit("clamp_logit", "edge", {"w": pr, "one_minus_w": qv, "eps": 1e-6}, {"ell": clamp_logit(lit(pr), lit(qv), lit(1e-6))})
    for e in (1e-3, 1e-9, 1e-15, 1e-30, 1e-100, 1e-300):
        for pr in (0.0, 1e-320, 1e-300, e * 0.5, e, e * 2, 0.5, 1 - e * 2, 1.0):
            pr = float(pr)
            qr = float(1 - mpf(pr))
            emit("clamp_logit", "eps", {"w": pr, "one_minus_w": qr, "eps": e}, {"ell": clamp_logit(lit(pr), lit(qr), lit(e))})
        for qv in (1e-320, e * 0.5, e, e * 2):  # the near-1 side, q accurate
            emit("clamp_logit", "eps", {"w": float(1 - mpf(qv)), "one_minus_w": qv, "eps": e},
                 {"ell": clamp_logit(lit(float(1 - mpf(qv))), lit(qv), lit(e))})
    # sigmoid
    for sv in [0.0, 1e-300, 1e-12, 0.5, -0.5, 5.0, -5.0, 20.7, -20.7, 36.7, 700.0, -700.0, 745.0, -745.0, 1768.0, -1768.0]:
        W, Wc = sigmoid_pair(lit(sv))
        emit("sigmoid", "edge", {"s": sv}, {"W": W, "one_minus_W": Wc})
    for _ in range(n_random):
        sv = rng.uniform(-40, 40)
        W, Wc = sigmoid_pair(lit(sv))
        emit("sigmoid", "random", {"s": sv}, {"W": W, "one_minus_W": Wc})
    # sum (Neumaier): ML-2. The vectors must be able to tell a compensated sum from a plain one.
    for _ in range(n_random // 2):
        k = draw_int(rng, 2, 20)
        xs = [rng.uniform(-100, 100) for _ in range(k)]
        emit("sum", "random", {"x": xs}, {"sum": neumaier([lit(v) for v in xs])})
    for n_half in (8, 32, 64):
        # all positives, then their negations in another order: the exact sum is 0 and every plain
        # partial sum rounds at ulp(partial) ~ 1e-13 .. 1e-11, so the plain sum is wrong by far more than
        # the compensated bound 2 n^2 u^2 sum|x| (~1e-23)
        pos = [float(mp.power(2, mpf(rng.uniform(6.0, 11.0)))) for _ in range(n_half)]
        perm = pos[:]
        rng_perm = [(rng.random(), i) for i in range(n_half)]
        perm = [pos[i] for _, i in sorted(rng_perm)]
        xs = pos + [-v for v in perm]
        emit("sum", "adversarial", {"x": xs}, {"sum": neumaier([lit(v) for v in xs])})
        # the same with a residual: negatives scaled by 1 - 2^-20 (not exact), S ~ 1e-6 sum
        xs2 = pos + [-v * (1 - 2.0 ** -20) for v in perm]
        emit("sum", "adversarial", {"x": xs2}, {"sum": neumaier([lit(v) for v in xs2])})
    big_then_small = [1e16, 1.0, -1e16, 1.0, 0.5, -0.5] * 4
    emit("sum", "edge", {"x": big_then_small}, {"sum": neumaier([lit(v) for v in big_then_small])})
    # pool
    def mk_groups(sizes, wlo, whi):
        return [[rng.uniform(wlo, whi) for _ in range(k)] for k in sizes]

    def pool_case(groups, rhos, e, kind):
        RANGE.clear()
        pg = [[(lit(w), lit(float(1 - mpf(w)))) for w in g] for g in groups]
        s, (W, Wc), A, NE = pool(pg, [lit(r) for r in rhos], lit(e))
        ex = exact_pool([[mpf(w) for w in g] for g in groups], [mpf(r) for r in rhos], mpf(e))
        assert abs(s.x - ex) < mpf(10) ** -9 * (1 + abs(ex) * mpf(10) ** -3)
        cap_s = KAPPA_POOL * U * A
        # decision: W >= min_confidence = 1/2, compared on the smaller side so a threshold near 1
        # keeps full resolution: v = min(W, 1-W), t = min(theta, 1-theta); guard relative to t
        v = min(W.x, Wc.x)
        t = mpf(1) / 2
        dist = abs(v - t) / t
        guard = 4 * (cap_s + U2(49))
        if guard < dist <= 4 * guard:
            RANGE.clear()
            return False  # neither firm nor marginal: not a vector (rule of 5.3)
        decision = {"name": "min_confidence", "threshold": hx(0.5), "distance": mp.nstr(dist, 4),
                    "guard": mp.nstr(guard, 4), "class": "firm" if dist > 4 * guard else "marginal",
                    "outcome": bool(W.x >= mpf(1) / 2) if dist > 4 * guard else None}
        emit("pool", kind, {"w": groups, "rho": rhos, "eps": e}, {"s": s, "W": W, "one_minus_W": Wc},
             decision=decision, quant={"A": A, "Neff": NE})
        return True

    pool_cases = [([[0.8]], [1.0]), ([[0.8], [0.2]], [1.0, 1.0]), ([[0.9, 0.55]], [1.0]),
                  ([[0.8] * 5], [0.0]), ([[0.8] * 5], [1.0])]
    for _ in range(12):
        sizes = [draw_int(rng, 1, 6) for _ in range(draw_int(rng, 1, 8))]
        pool_cases.append((mk_groups(sizes, 0.02, 0.98), [(0.0, 0.3, 1.0)[draw_int(rng, 0, 2)] for _ in sizes]))
    # 128 mixed-sign logits of near-cancelling size (the revision-1 vector: alternating, so a plain
    # partial sum is exact; kept for the record, it does not discriminate, see `sum`)
    pool_cases.append(([[0.999999 if i % 2 == 0 else 1e-6 for i in range(128)]], [0.0]))
    # ML-2: 64 positive logits then their 64 mirror images, in one group, distinct magnitudes
    posw = [rng.uniform(0.9, 0.999999) for _ in range(64)]
    pool_cases.append(([posw + [1 - w for w in posw]], [0.0]))
    for groups, rhos in pool_cases:
        pool_case(groups, rhos, 1e-6, "closed" if groups in ([[0.8]], [[0.8], [0.2]]) else "random")
    # NUM-M2: pools across the epsilon domain, with weights on both sides of the clamp
    for e in (1e-3, 1e-9, 1e-15, 1e-30, 1e-300):
        for n_src in (1, 16, 128):
            ws_ = []
            for i in range(n_src):
                pick = rng.random()
                ws_.append(float(1 - mpf(logu(rng, -1000, -2))) if pick < 0.4 else
                           (logu(rng, -1000, -2) if pick < 0.8 else rng.uniform(0.01, 0.99)))
            ws_ = [min(max(w, 5e-324), 1 - 2.0 ** -53) for w in ws_]
            half = n_src // 2
            ordered = sorted(ws_)[::-1][:half] + sorted(ws_)[:n_src - half]  # strong for first, weak then
            pool_case([ordered], [0.0], e, "eps")
            pool_case([[w] for w in ws_[:8]], [1.0] * min(8, n_src), e, "eps")
        # the worst case of the revision-1 cap: every source at the clamp, one sign, no cancellation
        pool_case([[e * 0.5] * 128], [0.0], e, "eps")
        pool_case([[e * 0.5] * 64, [e * 0.25] * 64], [1.0, 0.0], e, "eps")
    # scalar inverse-variance fusion and the innovation statistic
    for _ in range(n_random // 2):
        k = draw_int(rng, 2, 6)
        rs = [logu(rng, -10, 6) for _ in range(k)]
        wsl = [rng.uniform(1e-3, 0.999) for _ in range(k)]
        xs = [rng.uniform(-100, 100) for _ in range(k)]
        RANGE.clear()
        rf, xf, l1 = inv_variance_fusion([lit(v) for v in rs], [lit(v) for v in wsl], [lit(v) for v in xs], lit(1e-3))
        emit("fusion", "random", {"r": rs, "w": wsl, "x": xs, "eps_floor": 1e-3}, {"r_f": rf, "x_f": xf}, quant={"L1": l1})
        dx = [rng.uniform(-5, 5) for _ in range(draw_int(rng, 1, 3))]
        r0, w0, p0 = logu(rng, -6, 4), rng.uniform(0.01, 1.0), logu(rng, -6, 4)
        d2 = innovation_d2([lit(v) for v in dx], lit(r0), lit(w0), lit(p0), lit(1e-3))
        emit("innovation", "random", {"dx": dx, "r": r0, "w": w0, "p0": p0, "eps_floor": 1e-3}, {"d2": d2})
    # ML-2: fusion with 64 large positive-x inputs then 64 negative-x inputs (the plain-sum shape)
    rs = [logu(rng, -2, 2) for _ in range(64)]
    xs = [rng.uniform(1e3, 1e4) for _ in range(64)]
    rs, xs = rs + rs[::-1], xs + [-v for v in xs[::-1]]
    wsl = [rng.uniform(0.5, 0.999) for _ in rs]
    RANGE.clear()
    rf, xf, l1 = inv_variance_fusion([lit(v) for v in rs], [lit(v) for v in wsl], [lit(v) for v in xs], lit(1e-3))
    emit("fusion", "adversarial", {"r": rs, "w": wsl, "x": xs, "eps_floor": 1e-3}, {"r_f": rf, "x_f": xf}, quant={"L1": l1})
    # health / gate estimator: b and 1 - b are T0 (series), and the event updates are + * only
    for H in (1, 2, 3, 8, 16, 32, 64, 1000, 4096, 1000000):
        b, omb = health_b(H)
        emit("fraction_b", "edge", {"H": float(H)}, {"b": b, "one_minus_b": omb})
    for H, pattern, n_ev in ((64, "every10", 300), (32, "burst13", 200), (64, "all", 150), (4096, "every20", 600),
                             (64, "random", 400)):
        if pattern == "every10":
            hits = [1 if i % 10 == 9 else 0 for i in range(n_ev)]
        elif pattern == "burst13":
            hits = [1 if 100 <= i < 113 else 0 for i in range(n_ev)]
        elif pattern == "all":
            hits = [1] * n_ev
        elif pattern == "every20":
            hits = [1 if i % 20 == 19 else 0 for i in range(n_ev)]
        else:
            hits = [1 if rng.random() < 0.3 else 0 for _ in range(n_ev)]
        Rr, Nn, f = fraction_run(H, hits)
        emit("fraction_run", pattern, {"H": float(H), "hits": [float(h) for h in hits]},
             {"R": Rr, "N": Nn, "f": f}, quant={"H": H, "k": n_ev})
    # libm probes: exp and ln at hex inputs
    for _ in range(n_random):
        xv = rng.uniform(-30, 30)
        emit("libm_exp", "libm", {"x": xv}, {"y": R(mp.exp(mpf(xv)), 0, f=None)})
        yv = float(mp.power(2, mpf(rng.uniform(-40, 40))))
        emit("libm_ln", "libm", {"x": yv}, {"y": R(mp.log(mpf(yv)), 0, f=None)})
    header = ["#vagent-epistemic-vectors", "version=2", "seed=%d" % SEED, "dps=%d" % int(mp.dps),
              "unit_roundoff=0x1p-53", "libm_relative=0x1p-51", "mpmath=" + mpmath.__version__,
              "host=libm_build+cpu_dispatch", "host_report=libc_version,exp_log_ifunc_variant",
              "pool_kappa=%d" % KAPPA_POOL, "digits_out=50", "cases=%d" % len(out)]
    with open(path, "w") as f:
        f.write("\t".join(header) + "\n")
        for r in out:
            f.write(to_line(r) + "\n")
    sys.stderr.write("largest bound/cap per routine: %s\n" % ", ".join(
        "%s %.2f" % (k, v) for k, v in sorted(MEASURED.items())))
    return len(out)


def flat(prefix, v, acc):
    if isinstance(v, dict):
        for k, x in v.items():
            flat(prefix + "." + k, x, acc)
    elif isinstance(v, list):
        # lists of floats join with ',' and lists of lists with ';'
        acc.append("%s=%s" % (prefix, ";".join(",".join(t) if isinstance(t, list) else t for t in v)))
    elif v is not None:
        acc.append("%s=%s" % (prefix, v))


def to_line(rec):
    """One case per line, TAB-separated key=value fields: no JSON parser needed on the C++ side."""
    acc = [rec["id"], rec["routine"], rec["kind"]]
    for key in ("range", "in", "out", "bound", "bits", "decision", "refuse"):
        if key in rec:
            flat(key, rec[key], acc)
    return "\t".join(acc)


PINNED_MPMATH = "1.4.1"  # requirements.txt; the 50-digit outputs are byte-reproducible under this version


def main(argv):
    import os
    import tempfile
    if mpmath.__version__ != PINNED_MPMATH and not os.environ.get("VAGENT_MPMATH_BUMP"):
        sys.stderr.write("mpmath %s != pinned %s: set VAGENT_MPMATH_BUMP=1 to bump the pin deliberately\n"
                         % (mpmath.__version__, PINNED_MPMATH))
        return 2
    if len(argv) == 3 and argv[1] == "--check":
        with tempfile.TemporaryDirectory() as d:
            tmp = os.path.join(d, "v.tsv")
            gen(tmp)
            same = open(tmp, "rb").read() == open(argv[2], "rb").read()
        print("vectors: " + ("reproduced byte for byte" if same else "DIFFERENT"))
        return 0 if same else 1
    path = argv[1] if len(argv) > 1 else "epistemic_vectors.tsv"
    n = gen(path)
    print(n, "cases ->", path, hashlib.sha256(open(path, "rb").read()).hexdigest()[:16])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
