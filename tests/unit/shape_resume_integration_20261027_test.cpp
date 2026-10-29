// CPU-only integration boundaries. Does not link the scanner, CUDA runtime,
// configuration/persistence, transport, or any live search state.
#include "interval_progress.h"
#include "shape_checkpoint_restore.h"
#include "shape_checkpoint_commit.h"
#include <cstdio>
#include <stdexcept>

namespace {
uint64_t checks=0;
struct Completion {ChunkIdx id;uint64_t keys;bool weighted;};
std::vector<Completion> calls;
unsigned reset_calls=0;
void require(bool value,const char* why) {++checks;if(!value)throw std::runtime_error(why);}
ChunkIdx id(uint64_t value) {ChunkIdx result;result.from_u64(value);return result;}
bool equal(const u256 a,const u256 b) {return u256_cmp(a,b)==0;}
bool zero(const u256 value) {return !(value[0]|value[1]|value[2]|value[3]);}
void exact_volatile_state() {
    VolatileIntervalProgress progress;ChunkIdx got=id(91);uint64_t seed=7;u256 count{1,2,3,4};
    const u256 sentinel{1,2,3,4};
    require(!progress.recall_exact(3,got,seed,count),"missing digit is absent");
    require(got==id(91)&&seed==7&&equal(count,sentinel),"missing digit leaves outputs untouched");
    require(!progress.recall_exact(-1,got,seed,count)&&!progress.recall_exact(16,got,seed,count),"invalid digits absent");
    require(got==id(91)&&seed==7&&equal(count,sentinel),"invalid digit leaves outputs untouched");
    u256 exact{UINT64_MAX-3,1,0,0};ChunkIdx wide=id(11);wide.b[5]=0xa7;
    progress.remember_exact(3,wide,0,exact);
    require(progress.recall_exact(3,got,seed,count)&&got==wide&&seed==0&&equal(count,exact),"wide ordinal, counter and zero seed roundtrip");
    uint64_t narrow=0;
    require(progress.recall(3,got,seed,&narrow)&&narrow==UINT64_MAX,"legacy count saturates rather than truncates");
    u256 newer{17,19,23,29};progress.remember_exact(3,id(73),81,newer);
    require(progress.recall_exact(3,got,seed,count)&&got==id(73)&&seed==81&&equal(count,newer),"newer memory replaces stale digit pair exactly");
    progress.remember(15,id(5),90,123);
    require(progress.recall_exact(15,got,seed,count)&&got==id(5)&&seed==90&&count[0]==123&&!count[1]&&!count[2]&&!count[3],"legacy remember widens exactly");
    require(progress.recall(15,got,seed,&narrow)&&narrow==123,"legacy recall unchanged");
    progress.remember(0,id(0),0);
    require(progress.recall_exact(0,got,seed,count)&&got==id(0)&&seed==0&&zero(count),"zero state is present and valid");
    progress.remember_exact(-1,id(9),4,newer);progress.remember_exact(16,id(9),4,newer);
    require(progress.recall_exact(3,got,seed,count)&&got==id(73)&&seed==81&&equal(count,newer),"invalid writes do not alter valid digit");
    VolatileIntervalProgress fresh;
    require(!fresh.recall_exact(3,got,seed,count),"new application has no volatile state");
}
void weighted_batch_boundary() {
    SearchContext context;WaveGpuBatch batch;
    batch.completed={id(4),id(1),id(2)};batch.completed_counts={0,65537,UINT64_C(1)<<60};
    calls.clear();commit_completed_batch(context,batch,true);
    require(calls.size()==3,"every metadata completion forwarded");
    for(size_t i=0;i<calls.size();++i)
        require(calls[i].weighted&&calls[i].id==batch.completed[i]&&calls[i].keys==batch.completed_counts[i],"producer order and original weights retained");
    batch.completed_counts.pop_back();calls.clear();bool threw=false;
    try {commit_completed_batch(context,batch,true);}catch(const std::logic_error&){threw=true;}
    require(threw&&calls.empty(),"short weight vector rejects before any watermark mutation");
    batch.completed_counts={1,2,3,4};threw=false;
    try {commit_completed_batch(context,batch,true);}catch(const std::logic_error&){threw=true;}
    require(threw&&calls.empty(),"extra weight vector rejects before any watermark mutation");
    // SEQ/KXE retain the old two-argument accounting, regardless of metadata.
    batch.completed_counts.clear();commit_completed_batch(context,batch,false);
    require(calls.size()==3,"legacy batch still forwards all IDs");
    for(size_t i=0;i<calls.size();++i)
        require(!calls[i].weighted&&calls[i].id==batch.completed[i],"legacy completion overload and order unchanged");
    calls.clear();batch.completed.clear();commit_completed_batch(context,batch,true);
    require(calls.empty(),"empty shape batch does not mutate completion");
}
void accepted_resume_pair() {
    SearchContext context;const ChunkIdx position=id(17);u256 confirmed{UINT64_MAX,3,0,0};
    context.chunk_seq.set(id(999));context.resumed_items=-1;
    const unsigned before=reset_calls;
    apply_shape_checkpoint_state(context,position,confirmed);
    require(reset_calls==before+1,"acceptance performs one coherent weighted reset");
    require(context.chunk_seq.get()==position&&context.done_below==position,"claim and committed positions agree");
    require(equal(context.confirmed_items,confirmed)&&context.confirmed_items_valid,"exact saved confirmed counter restored");
    require(context.resumed_items==u256_to_ld(confirmed),"display starts from confirmed count");
    u256 empty{};apply_shape_checkpoint_state(context,id(0),empty);
    require(context.chunk_seq.get()==id(0)&&context.done_below==id(0)&&zero(context.confirmed_items)&&context.resumed_items==0,"fresh zero state applied without stale count");
}
}

// Capture calls at the boundary; weighted-watermark arithmetic and persistence
// are independently tested with the actual traverse.cpp by the core fixture.
void chunk_done(SearchContext&,const ChunkIdx& value) {calls.push_back({value,0,false});}
void chunk_done(SearchContext&,const ChunkIdx& value,uint64_t keys) {calls.push_back({value,keys,true});}
void checkpoint_reset(SearchContext& context,const ChunkIdx& value,const u256 confirmed) {
    ++reset_calls;std::lock_guard<std::mutex> lock(context.mtx_done);
    context.done_below=value;context.done_ahead.clear();u256_set(context.confirmed_items,confirmed);
    context.confirmed_items_valid=true;
}
long double u256_to_ld(const u256 value) {
    long double result=0;for(int i=3;i>=0;--i)result=result*18446744073709551616.0L+value[i];return result;
}
int main() {
    try {exact_volatile_state();weighted_batch_boundary();accepted_resume_pair();
        std::printf("PASS: %llu checks; exact digit cache, weighted commit validation, accepted resume pair. CPU helpers only.\n",(unsigned long long)checks);return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"FAIL: %s\n",error.what());return 1;}
}
