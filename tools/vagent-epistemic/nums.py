import numpy as np, warnings
warnings.filterwarnings("ignore")
from gate_bound import log_mgf_bound
YR=365.25*86400
H=64; b=2.0**(-1.0/H); cap=1/(1-b)
print("H=64 cap 1/(1-b) = %.2f ; 0.9cap=%.1f"%(cap,0.9*cap))
def N_after(k): return (1-b**k)/(1-b)
for Nmin in (32,40,48,56):
    k=next(k for k in range(1,1000) if N_after(k)>=Nmin); print("N_min",Nmin,"first deciding event",k)
def startup(p,th,Nmin,m=1,kmax=1500):
    tot=0.0
    k0=next(k for k in range(1,1000) if N_after(k)>=Nmin)
    for k in range(k0,kmax):
        # newest first weights b^0..b^(k-1); group into blocks of m from the oldest end
        w=b**np.arange(k)
        if m>1:
            pad=(-k)%m; ww=np.concatenate([w,np.zeros(pad)]).reshape(-1,m).sum(1); w=ww
        lb,_=log_mgf_bound(p,w,th,W=(b**np.arange(k)).sum()); tot+=float(np.exp(lb))
    return tot
for (p,m) in [(0.10,1),(0.10,4),(0.05,8)]:
    print("start-up union bound (per reset), FAULTED th=0.7 Nmin=48 p=%.2f m=%d: %.3g"%(p,m,startup(p,0.7,48,m)))
# steady union per device-year, H=64 th=0.7 table incl m=6
def blk(p,th,m,span=60):
    nb=int(H*span/m)+1; j=np.arange(nb); w=(b**(m*j))*(1-b**m)/(1-b); lb,_=log_mgf_bound(p,w,th); return float(np.exp(lb))
print("H=64 th=0.7 per device-year @100Hz (3.156e9 events):")
for p in (0.05,0.10,0.15):
    print(p,["m=%d %.2e"%(m,blk(p,0.7,m)*100*YR) for m in (1,2,4,6,8)])
print("SUSPECT th=0.30 H=64 per-event p=.05/.10:", blk(.05,.30,1), blk(.10,.30,1), " per year@100Hz:", blk(.05,.30,1)*100*YR, blk(.10,.30,1)*100*YR)
print("SUSPECT th=0.30 m=4:", blk(.05,.30,4), blk(.10,.30,4))
# streak rule comparison (renewal): expected events to first run of r at prob p
def run_events(p,r): return (1-p**r)/((1-p)*p**r)
for p in (0.05,0.01):
    for r in (3,5): print("streak r=%d p=%.2f: %.3g events -> @100Hz %.3g h"%(r,p,run_events(p,r),run_events(p,r)/100/3600))
# MC smoke: marginal P(f>=th), p=0.45
rng=np.random.default_rng(19)
def mc(p,th,chains=3000,T=6000,warm=500):
    R=np.zeros(chains);N=np.zeros(chains);cnt=0;tot=0
    for t in range(T):
        hit=(rng.random(chains)<p)
        R=b*R+hit; N=b*N+1
        if t>=warm:
            cnt+=int(np.sum(R>=th*N)); tot+=chains
    return cnt/tot
for p,th in [(0.45,0.7),(0.40,0.7),(0.30,0.6)]:
    lb,_=log_mgf_bound(p,b**np.arange(int(H*60)),th); print("smoke p=%.2f th=%.1f bound %.3g  MC %.3g"%(p,th,np.exp(lb),mc(p,th)))
