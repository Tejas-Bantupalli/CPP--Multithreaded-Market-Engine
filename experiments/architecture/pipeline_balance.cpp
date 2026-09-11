#include "common.h"
#include "queues.h"
#include <string>
// Synthetic stage work with a serial dependency chain. The result is consumed
// and checked, so the compiler cannot remove the computation.
uint64_t work(uint64_t value,int rounds){
    for(int i=0;i<rounds;++i)value=(value^(value>>13))*0x9e3779b97f4a7c15ULL+uint64_t(i);
    return value;
}
struct Message {uint64_t seq,value;int64_t started;};
int main(int argc,char**argv){
    try{
        int count=100000,first=64,second=64;std::string topology="inline";
        for(int i=1;i<argc;i+=2){if(i+1==argc)throw std::runtime_error("Missing value");std::string k=argv[i],v=argv[i+1];
            if(k=="--count")count=std::stoi(v);else if(k=="--first")first=std::stoi(v);else if(k=="--second")second=std::stoi(v);
            else if(k=="--topology")topology=v;else throw std::runtime_error("Unknown argument");}
        if(count<1||count>1000000||first<0||first>2048||second<0||second>2048||(topology!="inline"&&topology!="pipeline"))throw std::runtime_error("Invalid configuration");
        uint64_t expected=0;for(int i=0;i<count;++i)expected+=work(work(i,first),second);
        auto q=make_queue<Message>(128,false);
        std::atomic<int> ready{0};std::atomic<bool> go{false};int64_t epoch=0,finish=0,pcpu=0,ccpu=0;
        uint64_t result=0,errors=0;std::vector<int64_t> lat;lat.reserve(count/257+1);
        bool pipeline=topology=="pipeline";
        auto gate=[&]{configure_thread();ready.fetch_add(1);while(!go.load(std::memory_order_acquire))std::this_thread::yield();pace(epoch);};
        auto consume=[&](Message m){result+=work(m.value,second);if(m.started)lat.push_back(clock_ns()-m.started);};
        std::thread c;
        if(pipeline)c=std::thread([&]{gate();auto start=thread_cpu_ns();for(int i=0;i<count;){Message m;if(!q->pop(m))continue;if(m.seq!=uint64_t(i))++errors;consume(m);++i;}ccpu=thread_cpu_ns()-start;finish=clock_ns();});
        std::thread p([&]{gate();auto start=thread_cpu_ns();for(int i=0;i<count;++i){Message m;m.seq=i;m.started=i%257==0?clock_ns():0;m.value=work(i,first);if(pipeline){while(!q->push(m)){} }else consume(m);}pcpu=thread_cpu_ns()-start;if(!pipeline)finish=clock_ns();});
        while(ready.load()!=(pipeline?2:1))std::this_thread::yield();epoch=clock_ns()+20000000;go.store(true,std::memory_order_release);
        p.join();if(pipeline)c.join();bool correct=!errors&&result==expected;
        std::cout<<"{\"correct\":"<<(correct?"true":"false")<<",\"count\":"<<count<<",\"first\":"<<first<<",\"second\":"<<second
                 <<",\"topology\":\""<<topology<<"\",\"elapsed_ns\":"<<finish-epoch<<",\"producer_cpu_ns\":"<<pcpu<<",\"consumer_cpu_ns\":"<<ccpu<<",\"latency_ns\":";
        print_distribution(lat);std::cout<<"}\n";return correct?0:2;
    }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
