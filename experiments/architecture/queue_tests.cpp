#include "queues.h"
#include <cassert>
#include <deque>
#include <random>
#include <thread>
#include <iostream>
template<size_t CAP> void check(int align,bool cached) {
    auto q=make_queue<uint64_t,CAP>(align,cached);
    if(align)assert(q->index_distance()==size_t(align==8?16:align));
    std::deque<uint64_t> reference;std::mt19937 rng(871);
    for(uint64_t i=0;i<100000;++i){
        if(rng()%2){bool expected=reference.size()<CAP-1;assert(q->push(i)==expected);if(expected)reference.push_back(i);}
        else {uint64_t v=0;bool expected=!reference.empty();assert(q->pop(v)==expected);if(expected){assert(v==reference.front());reference.pop_front();}}
    }
    uint64_t v;while(q->pop(v)){assert(v==reference.front());reference.pop_front();}assert(reference.empty());
    constexpr uint64_t count=100000;
    std::thread p([&]{for(uint64_t i=0;i<count;++i)while(!q->push(i))std::this_thread::yield();});
    std::thread c([&]{for(uint64_t i=0;i<count;++i){uint64_t x;while(!q->pop(x))std::this_thread::yield();assert(x==i);}});
    p.join();c.join();assert(!q->pop(v));
}
int main(){
    for(int a:{0,8,64,128})for(bool c:{false,true}){
        if(a==0&&c)continue;
        check<2>(a,c);check<8>(a,c);check<1024>(a,c);
    }
    std::cout<<"21 queue/capacity variants passed reference, full/empty, wraparound and concurrent FIFO checks\n";
}
