import sys, os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from gate_bound import log_mgf_bound
H=32; th=0.25
def blk(p,m,span=40):
    b=2.0**(-1.0/H); nb=int(H*span/m)+1
    j=np.arange(nb); w=(b**(m*j))*(1-b**m)/(1-b)
    return log_mgf_bound(p,w,th)[0]/np.log(10)
print("I3-2a: per-event bound on P(f>=0.25) (steady), H=32, log10; blocks of m")
for p in (0.02,0.05,0.10):
    print(" p=%.2f"%p, "  ".join("m=%d 1e%.1f"%(m,blk(p,m)) for m in (1,2,4,8,13,16)))
b=2.0**(-1/32)
R=lambda k:(1-b**k)/(1-b)
print("consecutive refusals from N=cap=%.1f, f=0 to reach 0.25:"%(1/(1-b)), next(k for k in range(1,200) if R(k)/(1/(1-b))>=0.25))
# stationary with p=0.05 and bursts: simulate Markov burst process: bursts of length m start with prob p/m per event
rng=np.random.default_rng(3)
for m in (1,4,8,16):
    n=3_000_000; start=rng.random(n)<0.05/m; hits=np.zeros(n,bool)
    idx=np.flatnonzero(start)
    for i in idx: hits[i:i+m]=True
    Rr=N=0.0; sus=0; flips=0; prev=False; first=None; rate=hits.mean()
    for t,h in enumerate(hits):
        Rr=b*Rr+h; N=b*N+1; s=(N>=8 and Rr>=0.25*N); flips+= (s and not prev); prev=s; sus+=s
    print("burst m=%d marginal rate %.3f: SUSPECT onsets per 1e6 events %.0f, time in SUSPECT %.4f"%(m,rate,flips/n*1e6,sus/n))
