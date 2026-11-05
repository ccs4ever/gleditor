#!/usr/bin/env python3
"""Pinned autocorrelated gate-rejection sequences for the I4-8 test (design I4-D1 7.5, ML-1).

A healthy source behind a two-sided 95% gate has residual z_t, a stationary Gaussian AR(1) with lag-1
correlation rho; an event is a rejection when |z_t| > 1.959963984540054, so the marginal rejection rate
is 5% whatever rho is. For each rho the file lists the rejection positions (delta coded) and the first
event at which each rule latches, replaying the rules in binary64 exactly as section 7.2 writes them:
  old : fast fraction only (H = 64, f >= 0.70, N >= 48)               -- revision 1
  new : fast AND slow (H_s = 4096, f_s >= 0.70, N_s >= 2048)          -- revision 2
Counters restart at a latch, so `*_latches` counts latches in the sequence.

usage: ar1_hits.py OUT.tsv     writes the file;   ar1_hits.py --check FILE   regenerates and compares
The expected counts are exact for numpy's PCG64 stream (requirements.txt pins numpy); a different
numpy may move a rejection that sits within 1e-12 of the gate, which changes no count in the file.
"""
import sys
import numpy as np

GATE = 1.959963984540054
CASES = [(0.5, 11), (0.9, 12), (0.99, 13), (0.995, 14), (0.999, 15)]
N_EVENTS = 300_000
H_FAST, TH_FAST, N_FAST = 64.0, 0.70, 48.0
H_SLOW, TH_SLOW, N_SLOW = 4096.0, 0.70, 2048.0


def hits_for(rho, seed):
    rng = np.random.Generator(np.random.PCG64(seed))
    e = rng.standard_normal(N_EVENTS + 1)
    z = np.empty(N_EVENTS + 1)
    z[0] = e[0]
    s = (1.0 - rho * rho) ** 0.5
    for t in range(1, N_EVENTS + 1):
        z[t] = rho * z[t - 1] + s * e[t]
    return np.abs(z[1:]) > GATE


def rules(hits):
    bf, bs = 2.0 ** (-1.0 / H_FAST), 2.0 ** (-1.0 / H_SLOW)
    old_first = new_first = -1
    old_n = new_n = 0
    r1 = n1 = r2 = n2 = rs = ns = 0.0
    for t, h in enumerate(hits):
        h = 1.0 if h else 0.0
        r1 = bf * r1 + h
        n1 = bf * n1 + 1.0
        r2 = bf * r2 + h
        n2 = bf * n2 + 1.0
        rs = bs * rs + h
        ns = bs * ns + 1.0
        if n1 >= N_FAST and r1 >= TH_FAST * n1:
            old_n += 1
            old_first = t if old_first < 0 else old_first
            r1 = n1 = 0.0
        if n2 >= N_FAST and r2 >= TH_FAST * n2 and ns >= N_SLOW and rs >= TH_SLOW * ns:
            new_n += 1
            new_first = t if new_first < 0 else new_first
            r2 = n2 = rs = ns = 0.0
    return old_first, old_n, new_first, new_n


def render():
    lines = ["#vagent-ar1-hits\tversion=1\tevents=%d\tgate=%r\tnumpy=%s" % (N_EVENTS, GATE, np.__version__)]
    for rho, seed in CASES:
        h = hits_for(rho, seed)
        pos = np.flatnonzero(h)
        gaps = np.diff(pos, prepend=-1)
        of, on, nf, nn = rules(h)
        lines.append("\t".join([
            "ar1/rho=%g" % rho, "seed=%d" % seed, "rate=%.4f" % h.mean(),
            "old_first=%d" % of, "old_latches=%d" % on, "new_first=%d" % nf, "new_latches=%d" % nn,
            "gaps=" + ",".join(str(int(g)) for g in gaps)]))
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--check":
        ok = open(sys.argv[2]).read() == render()
        print("ar1 hits: " + ("reproduced" if ok else "DIFFERENT"))
        sys.exit(0 if ok else 1)
    open(sys.argv[1], "w").write(render())
    print(open(sys.argv[1]).read().count("\n") - 1, "cases ->", sys.argv[1])
