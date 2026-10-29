#include "search_lifecycle.h"
#include "minute_speed.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

static std::atomic<bool> fail_allocation{false};
void* operator new(std::size_t bytes) {
    if(fail_allocation.exchange(false))throw std::bad_alloc();
    if(void* pointer=std::malloc(bytes?bytes:1))return pointer;
    throw std::bad_alloc();
}
void operator delete(void* pointer) noexcept {std::free(pointer);}
void operator delete(void* pointer,std::size_t) noexcept {std::free(pointer);}

int main() {
    unsigned checks=0;
    auto require=[&](bool ok){++checks;if(!ok)throw std::runtime_error("lifecycle regression");};
    try {
        // Internal interval stop is deliberately absent from this policy.
        require(search_keep_console_open(true,true,true,false,false));
        require(!search_keep_console_open(true,true,true,true,false));
        require(search_keep_console_open(true,true,true,true,true));
        require(!search_keep_console_open(false,true,true,false,false));
        require(!search_keep_console_open(true,false,true,false,true));
        require(!search_keep_console_open(true,true,false,false,true));
        require(!search_close_completion_wait(false,false));
        require(search_close_completion_wait(true,false));
        require(!search_close_completion_wait(true,true));
        SearchThreadFailure failures;
        std::atomic<unsigned> continued{0},stops{0};
        std::vector<std::thread> workers;
        for(unsigned i=0;i<16;++i)workers.emplace_back([&,i]{
            failures.run([&]{
                if(i==0)throw std::runtime_error("synthetic worker fault");
                if(i==1)throw 17;
                ++continued; // A successful finding is not a stop request.
                ++continued; // Work after the finding is reached.
            },[&]{++stops;});
        });
        for(auto& worker:workers)worker.join();
        require(failures.failed());require(stops==2);require(continued==28);
        bool caught=false;
        try{failures.rethrow_after_join();}catch(...){caught=true;}
        require(caught);
        SearchThreadFailure healthy;
        healthy.run([&]{++continued;},[&]{++stops;});
        healthy.rethrow_after_join();require(!healthy.failed());require(stops==2);
        static_assert(noexcept(std::declval<MinuteSpeed::LogScope&>().finish()));
        {
            MinuteSpeed status(MinuteSpeed::Clock::now());
            MinuteSpeed::LogScope scope;
            fail_allocation=true;
            scope.finish();
            require(!fail_allocation.load());
            scope.resume();
            fail_allocation=true; // destructor must contain optional redraw failure
        }
        require(!fail_allocation.load());
        std::printf("PASS lifecycle checks=%u GPU=0 notifications=0\n",checks);
        return 0;
    }catch(...){fail_allocation=false;std::fputs("FAIL lifecycle\n",stderr);return 1;}
}
