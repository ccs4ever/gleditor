import numpy as np
rng=np.random.default_rng(5)
H=64; b=2.0**(-1/H)
for r in (0.20,0.25,0.30,0.35,0.40):
    n=2_000_000; hits=rng.random(n)<r
    R=N=0.0; prev=False; flips=0; sus=0
    for h in hits:
        R=b*R+h; N=b*N+1
        s = N>=16 and R>=0.30*N
        flips += s!=prev; prev=s; sus+=s
    print("r=%.2f flips per 1e4 events %.1f ; time in SUSPECT %.3f"%(r,flips/n*1e4, sus/n))
