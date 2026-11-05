import sys, os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I drops the script directory
import numpy as np
from gate_bound import *
H=32; b=2.0**(-1.0/H)
YR = 365.25*86400
print("events/yr at 100 Hz: %.4g, 10 Hz %.4g, 1 Hz %.4g"%(100*YR,10*YR,YR))
# 1. per-year union bounds
for (p,th,name) in [(0.05,0.30,'SUSPECT'),(0.10,0.30,'SUSPECT'),(0.05,0.50,'FAULTED'),(0.10,0.50,'FAULTED'),(0.05,0.25,'SUSPECT .25'),(0.10,0.45,'FAULTED .45')]:
    pb = steady_bound(p,32,th)
    print("%-12s p=%.2f th=%.2f per-event %.3g  per-year@100Hz %.3g  @10Hz %.3g @1Hz %.3g"%(name,p,th,pb,pb*100*YR,pb*10*YR,pb*YR))
# 2. start-up exposure
print("\nstart-up union bound (first 400 events, FAULTED th=0.5) by N_min")
sb = startup_bounds(0.10,32,0.5,kmax=400)
for Nmin in (8,16,24,32,40):
    s = sum(v for k,N,v in sb if N>=Nmin)
    kmin = next(k for k,N,v in sb if N>=Nmin)
    print("N_min=%d first deciding event k=%d  sum bound %.3g"%(Nmin,kmin,s))
sb5 = startup_bounds(0.05,32,0.5,kmax=400)
print("p=0.05:",["N_min=%d: %.3g"%(Nmin,sum(v for k,N,v in sb5 if N>=Nmin)) for Nmin in (8,16,24,32)])
print("\nstart-up SUSPECT th=0.30 p=0.05")
sbs = startup_bounds(0.05,32,0.30,kmax=400)
for Nmin in (8,12,16,24):
    s = sum(v for k,N,v in sbs if N>=Nmin); kmin=next(k for k,N,v in sbs if N>=Nmin)
    print("N_min=%d k=%d sum %.3g"%(Nmin,kmin,s))
# 3. block-correlation: m consecutive events perfectly correlated
def block_bound(p,H,th,m):
    b=2.0**(-1.0/H); nb=int(H*60/m)
    j=np.arange(nb); w=(b**(m*j))*(1-b**m)/(1-b)
    lb,t=log_mgf_bound(p,w,th)  # W = sum w = 1/(1-b)
    return float(np.exp(lb))
print("\nblock-correlated (worst case within block of m):")
for m in (1,2,4,8):
    print("m=%d  FAULTED p=.10 th=.5: %.3g (per-yr@100Hz %.3g)   SUSPECT p=.05 th=.30: %.3g"%(m,block_bound(0.10,32,0.5,m),block_bound(0.10,32,0.5,m)*100*YR,block_bound(0.05,32,0.30,m)))
# 4. detection: q=1 closed form, from f0
f0=0.05
for th in (0.30,0.5):
    n = np.log((1-f0)/(1-th))/np.log(1/b); print("q=1 from f0=.05 to th=%.2f: %.1f events"%(th,n))
