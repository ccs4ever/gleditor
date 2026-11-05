// AR(1) false-latch simulation. usage: ar rho events seed Hs thS NsMin
// Rules on one stream, each restarts its own counters at its latch:
//  old: fast only (H=64, f>=.70, N>=48);  new: fast AND slow (Hs, thS, NsMin)
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>
static uint64_t s[2];
static inline uint64_t rotl(uint64_t x,int k){return (x<<k)|(x>>(64-k));}
static inline uint64_t nxt(void){uint64_t s0=s[0],s1=s[1],r=s0+s1;s1^=s0;s[0]=rotl(s0,55)^s1^(s1<<14);s[1]=rotl(s1,36);return r;}
static inline double u01(void){return ((nxt()>>11)+0.5)*(1.0/9007199254740992.0);}
static double spare;static int have=0;
static double gauss(void){ if(have){have=0;return spare;} double u=u01(),v=u01(),r=sqrt(-2*log(u));spare=r*sin(6.283185307179586*v);have=1;return r*cos(6.283185307179586*v);}
int main(int c,char**v){
  double rho=atof(v[1]); double ev=atof(v[2]); s[0]=(uint64_t)atoll(v[3])*0x9E3779B97F4A7C15ULL+1; s[1]=0xD1B54A32D192ED03ULL^s[0]; for(int i=0;i<20;i++)nxt();
  double Hs=atof(v[4]), thS=atof(v[5]), NsMin=atof(v[6]);
  double H=64, thF=0.70, NF=48;
  double b=pow(2,-1.0/H), bs=pow(2,-1.0/Hs);
  double R=0,N=0, R2=0,N2=0, Rs=0,Ns=0; double sg=sqrt(1-rho*rho); double x=gauss();
  long lo=0, ln=0, fo=-1, fn=-1, lastn=0; long n=(long)ev;
  for(long t=0;t<n;t++){
    x=rho*x+sg*gauss(); int hit=fabs(x)>1.959963984540054;
    R=b*R+hit; N=b*N+1;           // old rule counters
    R2=b*R2+hit; N2=b*N2+1; Rs=bs*Rs+hit; Ns=bs*Ns+1; // new rule counters
    if(N>=NF && R>=thF*N){ lo++; if(fo<0)fo=t; R=N=0; }
    if(N2>=NF && R2>=thF*N2 && Ns>=NsMin && Rs>=thS*Ns){ ln++; if(fn<0)fn=t; R2=N2=Rs=Ns=0; }
  }
  printf("rho=%g events=%.3g Hs=%g thS=%g NsMin=%g | old: latches=%ld first=%ld | new: latches=%ld first=%ld\n",rho,ev,Hs,thS,NsMin,lo,fo,ln,fn);
  return 0;}
