import sys, os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I drops the script directory
import numpy as np, warnings
warnings.filterwarnings("ignore")
from gate_bound import log_mgf_bound
YR=365.25*86400
def blk(p,H,th,m,span=60):
    b=2.0**(-1.0/H); nb=int(H*span/m)+1
    j=np.arange(nb); w=(b**(m*j))*(1-b**m)/(1-b)
    lb,_=log_mgf_bound(p,w,th); return float(np.exp(lb))
print("FAULTED per-event bound -> per device-year at 100 Hz (3.156e9 events)")
print("H  th    p     m=1        m=2        m=4        m=8")
for H in (32,64):
  for th in (0.5,0.6,0.7,0.8):
    for p in (0.05,0.10):
      row=[blk(p,H,th,m)*100*YR for m in (1,2,4,8)]
      print(H,th,p,"  ".join("%.2e"%x for x in row))
b=lambda H:2.0**(-1.0/H)
print()
for H in (32,64):
  for th in (0.5,0.6,0.7,0.8):
    n=np.log(0.95/(1-th))/np.log(1/b(H)); 
    print("H=%d th=%.1f: events to FAULTED for q=1 from f0=.05: %.0f ; steady f at rejection rate r reaches th iff r>=th"%(H,th,n))
