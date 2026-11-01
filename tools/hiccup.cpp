#include <cstdlib>
// Platform jitter probe: spin on one pinned CPU and record gaps between clock reads.
#include <chrono>
#include <cstdio>
#include <vector>
#include <algorithm>
#include <pthread.h>
int main(int argc,char**argv){
  int cpu = argc>1?atoi(argv[1]):1;
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu,&s); pthread_setaffinity_np(pthread_self(),sizeof s,&s);
  using C=std::chrono::steady_clock;
  auto end=C::now()+std::chrono::seconds(5);
  std::vector<long> gaps; long over10us=0, over100us=0, over1ms=0;
  auto prev=C::now(); long n=0;
  while(true){ auto t=C::now(); long g=std::chrono::duration_cast<std::chrono::nanoseconds>(t-prev).count(); prev=t; ++n;
    if(g>10000){++over10us; gaps.push_back(g);} if(g>100000)++over100us; if(g>1000000)++over1ms; if(t>end)break;}
  std::sort(gaps.begin(),gaps.end());
  printf("cpu%d: %ld reads in 5s, stalls >10us=%ld >100us=%ld >1ms=%ld, worst=%.2f ms\n",cpu,n,over10us,over100us,over1ms,gaps.empty()?0:gaps.back()/1e6);
}
