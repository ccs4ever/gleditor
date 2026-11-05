#!/usr/bin/env python3
"""Stand-in for the C++ test (VagentEpistemicVectors): replays each routine's operation sequence in
IEEE binary64 -- this is the implementation under test, so it may call the host libm -- and compares
with the committed vector file.

  host     : libm as is. Reports the host identity (libc version, exp/log ifunc variant) first.
  perturb  : every exp/ln result moved by a random 0, +-1 or +-2 ulp (models another libm).
  mutant   : each naive form of a routine in turn. EVERY mutant must be rejected by at least one case
             of the routine it targets; a mutant that survives makes the run fail (ML-2).
  all      : host, perturb and mutant; exit status is the conjunction.

usage: check_vectors.py VECTORS [host|perturb|mutant|all]
"""
import math
import os
import platform
import random
import sys
from collections import defaultdict

U = 2.0 ** -53
LN2 = 0.6931471805599453
SERIES_N = 17
INVF = [1.0 / math.factorial(k) for k in range(SERIES_N + 2)]
# mutant name -> routines whose cases MUST reject it
MUTANTS = {
    "lcb": {"shrink"},                 # the spec's max(0, mu - c sqrt v)
    "decay_naive": {"decay"},          # 1/2 + e (p - 1/2)
    "logit_diff": {"clamp_logit"},     # complement recomputed from p: cancels near 1
    "sum_plain": {"sum"},              # plain loop instead of Neumaier
    "var_old": {"beta_var"},           # alpha beta / ((s s)(s + 1)): underflows and overflows
    "eps_const": {"clamp_logit", "pool"},  # epsilon fixed at 1e-6 whatever the policy says
}
MUT = None
MODE = "host"
RNG = random.Random(7)
PERTURB_ULP = int(os.environ.get("VAGENT_PERTURB_ULP", "2"))  # 3 shows what an out-of-spec libm trips


def host_identity():
    libc = "unknown"
    try:
        import ctypes
        f = ctypes.CDLL(None).gnu_get_libc_version
        f.restype = ctypes.c_char_p
        libc = f().decode()
    except Exception:
        libc = platform.libc_ver()[0] + " " + platform.libc_ver()[1]
    flags = ""
    try:
        with open("/proc/cpuinfo") as fh:
            for line in fh:
                if line.startswith("flags"):
                    have = set(line.split(":", 1)[1].split())
                    flags = ",".join(x for x in ("fma", "fma4", "avx2", "avx512f") if x in have)
                    break
    except OSError:
        pass
    return "libc=%s cpu_dispatch_flags=%s machine=%s" % (libc, flags or "none", platform.machine())


def jitter(v):
    if MODE != "perturb":
        return v
    k = RNG.choice(list(range(-PERTURB_ULP, PERTURB_ULP + 1)) + [0])
    for _ in range(abs(k)):
        v = math.nextafter(v, math.inf if k > 0 else -math.inf)
    return v


def Exp(x):
    return jitter(math.exp(x))


def Ln(x):
    return jitter(math.log(x))


def Div(p, q):  # the VM's Div: a zero divisor yields 0.0 and succeeds (vortex_vm.cpp:518)
    return 0.0 if q == 0.0 else p / q


class Refused(Exception):
    pass


def beta_mean(a, b):
    s = a + b
    if not math.isfinite(s):
        raise Refused("InvalidEpistemicState")
    return a / s, b / s


def beta_var(a, b):
    if MUT == "var_old":
        s = a + b
        return Div(a * b, (s * s) * (s + 1.0))
    p, q = beta_mean(a, b)
    return (p * q) / ((a + b) + 1.0)


def shrink(a, b, c):
    pm, qm = beta_mean(a, b)
    if MUT == "lcb":
        lcb = max(0.0, pm - c * math.sqrt(beta_var(a, b)))
        return lcb, 1.0 - lcb
    t = c * math.sqrt(beta_var(a, b))
    dd = pm - qm
    if dd > t + t:
        return pm - t, qm + t
    if -dd > t + t:
        return pm + t, qm - t
    return 0.5, 0.5


def expm1_series(y):
    acc = INVF[SERIES_N + 1]
    for k in range(SERIES_N, 0, -1):
        acc = INVF[k] + y * acc
    return y * acc


def decay(p, q, age, tau):
    hl = age / tau
    if hl > 1100.0:
        return 0.5, 0.5
    x = hl * LN2
    if MUT == "decay_naive":
        e = math.exp(-x)
        return 0.5 + e * (p - 0.5), 0.5 + e * (q - 0.5)
    if x < 0.5:
        em = expm1_series(-x)
        e = 1.0 + em
        h = 0.5 * (-em)
    else:
        e = Exp(-x)
        h = 0.5 * (1.0 - e)
    return e * p + h, e * q + h


def clamp_logit(p, q, eps):
    if MUT == "eps_const":
        eps = 1e-6
    one_m = 1.0 - eps
    if p < eps:
        p, q = eps, one_m
    elif q < eps:
        p, q = one_m, eps
    if MUT == "logit_diff":
        return math.log(p / (1.0 - p))
    return Ln(p / q)


def sigmoid(s):
    e = Exp(-abs(s))
    den = 1.0 + e
    big, small = 1.0 / den, e / den
    return (big, small) if s >= 0 else (small, big)


def neumaier(xs):
    if MUT == "sum_plain":
        t = 0.0
        for x in xs:
            t += x
        return t
    s = 0.0
    c = 0.0
    for x in xs:
        t = s + x
        if abs(s) >= abs(x):
            c += (s - t) + x
        else:
            c += (x - t) + s
        s = t
    return s + c


def pool(groups, rhos, eps):
    terms = []
    for g, rho in zip(groups, rhos):
        ells = [clamp_logit(w, 1.0 - w, eps) for w in g]
        n = float(len(g))
        terms.append((n / (1.0 + (n - 1.0) * rho)) * (neumaier(ells) / n))
    s = neumaier(terms)
    return s, sigmoid(s)


def fusion(rs, ws, xs, fl):
    invs = [(w if w > fl else fl) / r for r, w in zip(rs, ws)]
    si = neumaier(invs)
    rf = 1.0 / si
    return rf, rf * neumaier([i * x for i, x in zip(invs, xs)])


def innovation(dx, r, w, p0, fl):
    num = dx[0] * dx[0]
    for t in dx[1:]:
        num = num + t * t
    return num / (r / (w if w > fl else fl) + p0)


def H(s):
    return float.fromhex(s)


def health_b(h):  # T0 for h >= 2: the series of `decay`, no libm; h = 1 is exactly 1/2
    if h == 1.0:
        return 0.5, 0.5
    em = expm1_series(-(LN2 / h))
    return 1.0 + em, -em


def fraction_run(h, hits):
    b, omb = health_b(h)
    cap = 1.0 / omb
    r = n = 0.0
    for hit in hits:
        if n > cap:
            r, n = (r * cap) / n, cap
        r = b * r + hit
        n = b * n + 1.0
    return r, n, r / n


def lst(v):
    return [H(t) for t in v.split(";")]


def run(rec):
    i, r = rec["in"], rec["routine"]
    if r == "beta_mean":
        mu, q = beta_mean(H(i["alpha"]), H(i["beta"]))
        return {"mu": mu, "one_minus_mu": q}
    if r == "beta_var":
        return {"v": beta_var(H(i["alpha"]), H(i["beta"]))}
    if r == "shrink":
        p, q = shrink(H(i["alpha"]), H(i["beta"]), H(i["c"]))
        return {"w": p, "one_minus_w": q}
    if r == "decay":
        p, q = decay(H(i["w0"]), H(i["one_minus_w0"]), H(i["age"]), H(i["tau"]))
        return {"w": p, "one_minus_w": q}
    if r == "clamp_logit":
        return {"ell": clamp_logit(H(i["w"]), H(i["one_minus_w"]), H(i["eps"]))}
    if r == "sigmoid":
        W, Wc = sigmoid(H(i["s"]))
        return {"W": W, "one_minus_W": Wc}
    if r == "sum":
        return {"sum": neumaier(lst(i["x"]))}
    if r == "pool":
        s, (W, Wc) = pool([[H(w) for w in g.split(",")] for g in i["w"].split(";")], lst(i["rho"]), H(i["eps"]))
        return {"s": s, "W": W, "one_minus_W": Wc}
    if r == "fusion":
        rf, xf = fusion(lst(i["r"]), lst(i["w"]), lst(i["x"]), H(i["eps_floor"]))
        return {"r_f": rf, "x_f": xf}
    if r == "innovation":
        return {"d2": innovation(lst(i["dx"]), H(i["r"]), H(i["w"]), H(i["p0"]), H(i["eps_floor"]))}
    if r == "fraction_b":
        b, omb = health_b(H(i["H"]))
        return {"b": b, "one_minus_b": omb}
    if r == "fraction_run":
        Rr, Nn, f = fraction_run(H(i["H"]), lst(i["hits"]))
        return {"R": Rr, "N": Nn, "f": f}
    if r == "libm_exp":
        return {"y": Exp(H(i["x"]))}
    if r == "libm_ln":
        return {"y": Ln(H(i["x"]))}
    raise SystemExit("unknown routine " + r)


def parse(line):
    f = line.rstrip("\n").split("\t")
    rec = {"id": f[0], "routine": f[1], "kind": f[2], "in": {}, "out": {}, "bound": {}, "bits": {},
           "decision": {}, "refuse": {}, "range": "ok"}
    for t in f[3:]:
        k, v = t.split("=", 1)
        if k == "range":
            rec["range"] = v
            continue
        sec, name = k.split(".", 1)
        rec[sec][name] = v
    return rec


def evaluate(cases, hdr_ok=True):
    """Returns (fails per routine, worst err/allowed per routine, bit cases, bit fails, refusals)."""
    worst = defaultdict(float)
    fails = defaultdict(int)
    bitc, bitf = defaultdict(int), defaultdict(int)
    for rec in cases:
        r = rec["routine"]
        if rec["refuse"]:
            try:
                run(rec)
                fails[r] += 1  # the routine accepted a value the domain check must refuse
            except Refused:
                pass
            continue
        try:
            got = run(rec)
        except (Refused, ValueError, OverflowError, ZeroDivisionError):
            fails[r] += 1
            continue
        if rec["range"] == "ok" and any(not math.isfinite(v) for v in got.values()):
            fails[r] += 1
            continue
        if "bits" in rec and rec["bits"]:
            bitc[r] += 1
            if any(got[k] != H(v) for k, v in rec["bits"].items()):
                bitf[r] += 1
                fails[r] += 1
        for k, ref in rec["out"].items():
            refd = float(ref)  # decimal -> nearest double
            bound = float(rec["bound"][k])
            err = abs(got[k] - refd)
            allowed = bound + U * abs(refd)
            if r.startswith("libm_"):
                allowed = 2 * 2.0 ** -52 * abs(refd) + U * abs(refd)  # 2 ulp
            allowed += 2.0 ** -1073  # two denormal steps: relative accuracy has no meaning below 2^-1022
            ratio = err / allowed if allowed else (0.0 if err == 0 else math.inf)
            worst[r] = max(worst[r], ratio)
            if err > allowed:
                fails[r] += 1
    return fails, worst, bitc, bitf


def main():
    global MODE, MUT
    path = sys.argv[1]
    modes = ["host", "perturb", "mutant"] if (len(sys.argv) < 3 or sys.argv[2] == "all") else [sys.argv[2]]
    lines = open(path).read().splitlines()
    hdr = dict(t.split("=", 1) for t in lines[0].split("\t")[1:])
    cases = [parse(l) for l in lines[1:]]
    status = 0
    print("vectors: %s cases=%s mpmath=%s  host: %s" % (path, hdr.get("cases"), hdr.get("mpmath"), host_identity()))
    for mode in modes:
        MODE = mode if mode != "mutant" else "host"
        if mode in ("host", "perturb"):
            MUT = None
            fails, worst, bitc, bitf = evaluate(cases)
            print("mode=%s" % mode)
            for r in sorted(worst):
                print("  %-12s max err/allowed %7.3f  fails %3d  bit-exact cases %3d (fail %d)" % (r, worst[r], fails[r], bitc[r], bitf[r]))
            if any(fails.values()):
                status = 1
        else:
            print("mode=mutant (every mutant must fail at least one case of its target routine)")
            for name, targets in MUTANTS.items():
                MUT = name
                fails, worst, _, _ = evaluate(cases)
                killed = sum(fails[t] for t in targets)
                others = {k: v for k, v in fails.items() if k not in targets and v}
                print("  %-12s target %-22s rejected by %3d cases %s%s" % (
                    name, "+".join(sorted(targets)), killed, "" if killed else "SURVIVED  ",
                    ("also: " + ", ".join("%s %d" % kv for kv in sorted(others.items()))) if others else ""))
                if not killed:
                    status = 1
            MUT = None
    return status


if __name__ == "__main__":
    sys.exit(main())
