import sys, os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from gate_bound import log_mgf_bound
H=4096; th=0.7; Nmin=2048
b=2.0**(-1.0/H)
def N_after(k): return (1-b**k)/(1-b)
k0=next(k for k in range(1,20000) if N_after(k)>=Nmin)
print("slow: first deciding event k0 =",k0)
def start_log(p,m,stride=64,kmax=H*14):
    # union over events k0..kmax of the bound for a window of k terms; bound_k is non-increasing in k
    # beyond k0 for fixed p, th, so each stride is charged its left-end value (conservative)
    logs=[]
    for k in range(k0,kmax,stride):
        w=b**np.arange(k)
        if m>1:
            pad=(-k)%m; w=np.concatenate([w,np.zeros(pad)]).reshape(-1,m).sum(1)
        lb,_=log_mgf_bound(p,w,th,W=(b**np.arange(k)).sum()); logs.append(lb+np.log(stride))
    return np.logaddexp.reduce(logs)/np.log(10)
for p in (0.05,0.10):
    print("p=%.2f"%p, "  ".join("m=%d 1e%.0f"%(m,start_log(p,m)) for m in (1,4,50,200,400)))
# detection latency
f0=0.05
print("events to slow f>=0.7 from f0=.05 (all rejected):", np.log((1-f0)/(1-th))/np.log(1/b))
print("fast first deciding at N>=48: k=",next(k for k in range(1,999) if (1-2**(-k/64))/(1-2**(-1/64))>=48))
