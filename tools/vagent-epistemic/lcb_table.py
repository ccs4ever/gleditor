from mpmath import mp, mpf
mp.dps=40
def mv(a,b):
    a=mpf(a);b=mpf(b);mu=a/(a+b);v=a*b/((a+b)**2*(a+b+1));return mu,mp.sqrt(v)
def W(w,eps=mpf('1e-6')):
    w=min(max(w,eps),1-eps);return w
def ell(w): w=W(w);return mp.log(w/(1-w))
def sig(s): return 1/(1+mp.exp(-s))
rows=[]
for (a,b,name) in [(1,1,'fresh Beta(1,1)'),(21,3,'mature mu=.875 n=22'),(3,21,'mature mu=.125 n=22')]:
    mu,sd=mv(a,b)
    for c in (0.5,1.0,1.7,2.0):
        c=mpf(c)
        spec=max(mpf(0),mu-c*sd)
        d=mu-mpf(1)/2; m=max(mpf(0),abs(d)-c*sd)
        optA = max(mpf(1)/2,mu-c*sd) if mu>=mpf(1)/2 else mu   # floor at 1/2 only the penalty, never raise
        optA2= max(mpf(1)/2,mu-c*sd)                            # literal "floor at 1/2"
        shr = mpf(1)/2+(m if d>=0 else -m)
        print("%-22s c=%.1f mu=%.4f sd=%.4f | spec %.4f (3 srcs W=%.3g) | floor1/2 %.4f | floor-penalty %.4f | shrink %.4f (3 srcs W=%.4g)"%(name,c,mu,sd,spec,sig(3*ell(spec)),optA2,optA,shr,sig(3*ell(shr))))
# tie saturation
import math
for n in (5,10,15,30):
    pass
# logit-space tolerance versus W-space at various s
for s in (0,5,10,15,20.7,25,36.7,40):
    s=mpf(s); Wv=sig(s); print("s=%5.1f W=%.17g  1-W=%.3g  dW/ds=%.3g  W-tol 1e-9 equals logit-tol %.3g"%(s,Wv,1-Wv,Wv*(1-Wv),1e-9/(Wv*(1-Wv)) if Wv*(1-Wv)>0 else float('inf')))
