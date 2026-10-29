#include "interval_progress.h"
#include <cstdio>
int main() {
    VolatileIntervalProgress progress;
    ChunkIdx first,later,got;first.from_u64(12);later.b[1]=7;later.b[31]=91;
    uint64_t seed=9;
    if(progress.recall(5,got,seed)||seed!=9)return 1;
    progress.remember(5,first,0);progress.remember(15,later,UINT64_MAX);
    if(!progress.recall(5,got,seed)||!(got==first)||seed!=0)return 2;
    if(!progress.recall(15,got,seed)||!(got==later)||seed!=UINT64_MAX)return 3;
    progress.remember(5,later,173);
    if(!progress.recall(5,got,seed)||!(got==later)||seed!=173)return 4;
    if(progress.recall(-1,got,seed)||progress.recall(16,got,seed))return 5;
    VolatileIntervalProgress new_run;
    if(new_run.recall(5,got,seed))return 6;
    std::puts("PASS: timed interval recall, independent digits, wide ordinals, zero seed, fresh process has no saved progress");
    return 0;
}
