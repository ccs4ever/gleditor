import sys, os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np, warnings
warnings.filterwarnings("ignore")
from gate_bound import log_mgf_bound
H=64; b=2.0**(-1.0/H)
rng=np.random.default_rng(19)
def mc(p,th,chains=4000,T=4000,warm=400):
    R=np.zeros(chains);N=np.zeros(chains);occ=np.zeros(chains)
    for t in range(T):
        hit=(rng.random(chains)<p); R=b*R+hit; N=b*N+1
        if t>=warm: occ+=(R>=th*N)
    per=occ/(T-warm); m=per.mean(); se=per.std(ddof=1)/np.sqrt(chains)
    return m,se
w=b**np.arange(H*60)
for p in (0.60,0.55):
    lb,_=log_mgf_bound(p,w,0.7); B=float(np.exp(lb)); m,se=mc(p,0.7)
    print("p=%.2f bound %.3g  per-event occupancy %.3g  se(over chains) %.2g  (4se interval %.3g..%.3g) ratio bound/obs %.1f"%(p,B,m,se,max(m-4*se,0),m+4*se,B/m))
