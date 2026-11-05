import sys, os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from gate_bound import log_mgf_bound
LEV = np.log(100*365.25*86400)
def blk(p,H,th,m,span=40):
    b=2.0**(-1.0/H); nb=int(H*span/m)+1
    j=np.arange(nb); w=(b**(m*j))*(1-b**m)/(1-b)
    lb,_=log_mgf_bound(p,w,th); return lb
def fmt(lb):  # per device-year at 100 Hz, as 10^x capped at "none"
    y=(lb+LEV)/np.log(10)
    return ">1" if y>=0 else "1e%.0f"%np.floor(y) if y<-1 else "%.2g"%10**y
if __name__=="__main__":
    ms=(1,4,16,50,100,200,400)
    for H,th in ((64,.7),(2048,.7),(4096,.7),(4096,.6),(8192,.7)):
        print("H=%d th=%.1f  m: %s"%(H,th,ms))
        for p in (0.05,0.10):
            print("  p=%.2f"%p, "  ".join("%s"%fmt(blk(p,H,th,m)) for m in ms))
