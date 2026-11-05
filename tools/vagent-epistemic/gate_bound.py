"""Weighted Chernoff bounds for the event-indexed rejection fraction (O10).
Numpy only; no math/numpy exp-of-reference issue: this is a bound calculation, not a reference vector."""
import numpy as np, sys
LN2 = np.log(2.0)

def log_mgf_bound(p, w, theta, W=None):
    """min over t>0 of  -t*theta*W + sum log(1-p+p*exp(t*w_i)); returns (log bound, t*).
    Overflow-safe (logaddexp) and the search range scales with max(w), so blocks of any length work.
    The log bound may be far below the smallest double; callers exponentiate only to print."""
    if W is None: W = w.sum()
    wmax = float(w.max())
    lq, lp = np.log1p(-p), np.log(p)
    def g(t):
        return -t*theta*W + np.logaddexp(lq, lp + t*w).sum()
    # g is convex in t: coarse log-grid, then golden-section around the best grid point
    grid = wmax**-1 * np.geomspace(1e-4, 1e3, 700)
    vals = np.array([g(t) for t in grid])
    i = int(vals.argmin())
    a, b = grid[max(i-1, 0)], grid[min(i+1, len(grid)-1)]
    gr = (np.sqrt(5)-1)/2
    c = b - gr*(b-a); d = a + gr*(b-a)
    for _ in range(120):
        if g(c) < g(d): b = d
        else: a = c
        c = b - gr*(b-a); d = a + gr*(b-a)
    t = (a+b)/2
    return min(g(t), 0.0), t

def steady_bound(p, H, theta):
    b = 2.0**(-1.0/H)
    n = int(H*60)
    w = b**np.arange(n)
    lb, t = log_mgf_bound(p, w, theta)
    return float(np.exp(lb))

def startup_bounds(p, H, theta, kmax=None):
    """bound_k for window of k events (fresh profile): weights b^0..b^(k-1), compared with N_k."""
    b = 2.0**(-1.0/H)
    kmax = kmax or int(H*8)
    out = []
    for k in range(1, kmax+1):
        w = b**np.arange(k)
        lb, t = log_mgf_bound(p, w, theta)
        out.append((k, w.sum(), float(np.exp(lb))))
    return out

if __name__ == "__main__":
    print("steady-state per-event bound  P(f >= theta), iid Bernoulli(p)")
    print("H  p     theta  bound")
    for H in (32, 64):
        for p in (0.01, 0.05, 0.10):
            for th in (0.25, 0.30, 0.35, 0.40, 0.50):
                print(H, p, th, "%.3g" % steady_bound(p, H, th))
