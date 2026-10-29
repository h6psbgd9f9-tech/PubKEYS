#include "cuda_batch.cuh"
#include "secp256k1.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <random>
#include <stdexcept>
#include <vector>
struct Capture {
    std::vector<std::pair<int,std::array<uint64_t,4>>> rows;
    void operator()(int offset,const cfe x) {
        std::array<uint64_t,4> v; memcpy(v.data(),x,32); rows.emplace_back(offset,v);
    }
};
static cpt packed(const Point &p) {
    cpt r{}; memcpy(r.x,p.x,32); memcpy(r.y,p.y,32); r.inf=p.inf; return r;
}
static void require(bool ok,const char* text) {if(!ok) throw std::runtime_error(text);}
static void verify(const Point &p,const Point &step,const std::vector<cpt>& gt,size_t stride) {
    const cpt P=packed(p),S=packed(step); cpt next{}; bool advanced=true;
    std::vector<std::array<uint64_t,4>> a(gt.size()*stride), b(a.size());
    Capture old_rows,new_rows;
    int old_ok=cpt_batch_visit(P,gt.data(),(int)gt.size(),reinterpret_cast<cfe*>(a.data()),stride,old_rows);
    int new_ok=cpt_batch_visit_step(P,gt.data(),(int)gt.size(),reinterpret_cast<cfe*>(b.data()),stride,new_rows,S,next,advanced);
    require(old_ok==new_ok && old_rows.rows==new_rows.rows,"batch coverage/order/X mismatch");
    if(!new_ok) require(!advanced && new_rows.rows.empty(),"degenerate batch advanced or emitted");
    if(!advanced) cpt_add(next,P,S);
    const Point expected=ec_add(p,step);
    require(bool(next.inf)==bool(expected.inf),"next-center infinity mismatch");
    if(!expected.inf) require(!memcmp(next.x,expected.x,32) && !memcmp(next.y,expected.y,32),"next-center coordinate mismatch");
}
int main() {
    try {
        std::mt19937_64 rng(0x748abd923);
        size_t cases=0;
        for(int half : {1,16,512}) {
            std::vector<cpt> gt(half);
            for(int i=0;i<half;++i) {u256 k{};k[0]=i+1;gt[i]=packed(ec_mul_G(k));}
            for(int i=0;i<20;++i) {
                u256 k{rng(),rng(),rng(),rng()>>4},s{rng(),rng(),rng(),rng()>>4};
                const Point P=ec_mul_G(k),S=ec_mul_G(s);
                verify(P,S,gt,1); verify(P,S,gt,5); cases+=2;
                if(i==0) {
                    verify(P,P,gt,1); ++cases;
                    Point negative=P; fe_neg(negative.y,P.y);verify(P,negative,gt,1);++cases;
                    Point infinity{};infinity.inf=true;
                    verify(P,infinity,gt,1);verify(infinity,S,gt,1);cases+=2;
                    u256 small{};small[0]=1;verify(ec_mul_G(small),S,gt,1);++cases;
                }
            }
        }
        printf("PASS shared-inverse step: %zu cases; batch output/order, independent CPU point addition, strided memory, infinity, doubling, inverse points, degenerate batches\n",cases);
        return 0;
    } catch(const std::exception& e) {fprintf(stderr,"FAIL %s\n",e.what());return 1;}
}
