// C++20 atomic waiting on the same SPSC data queue. This is not a raw Linux
// futex benchmark; the standard library chooses its platform wait backend.
#include "common.h"
#include "queues.h"
#include "transport.h"
#include <string>
struct Message {uint64_t seq=0,value=0;int64_t started=0,scheduled=0;};
struct Link {
    std::unique_ptr<QueueInterface<Message>> queue;
    std::unique_ptr<Channel<Message>> cv;
    alignas(128) std::atomic<uint32_t> sequence{0};
    std::string mode;
    explicit Link(std::string m):mode(std::move(m)){
        if(mode=="cv")cv=make_channel<Message>(TransportKind::MutexCv);
        else queue=make_queue<Message,16384>(0,false);
    }
    bool push(const Message& m){
        if(cv)return cv->push(m);
        if(!queue->push(m))return false;
        if(mode=="atomic"||mode=="hybrid"){
            sequence.fetch_add(1,std::memory_order_release);sequence.notify_one();
        }
        return true;
    }
    bool pop(Message& m,uint64_t& waits){
        if(cv){++waits;return cv->wait_pop(m,50000);}
        if(mode=="spin"||mode=="yield")return queue->pop(m);
        // Observe notification generation BEFORE checking the data queue.
        // A publication racing with the empty check changes the generation,
        // so wait(expected) cannot park on the already-consumed notification.
        uint32_t expected=sequence.load(std::memory_order_acquire);
        unsigned attempts=mode=="hybrid"?65:1;
        for(unsigned i=0;i<attempts;++i)if(queue->pop(m))return true;
        ++waits;sequence.wait(expected,std::memory_order_acquire);return false;
    }
};
struct Stats {
    std::vector<int64_t> latency,scheduled;
    uint64_t errors=0,full=0,waits=0;int64_t producer_cpu=0,consumer_cpu=0,finish=0;
    explicit Stats(int n){latency.reserve(n);scheduled.reserve(n);}
};
int main(int argc,char**argv){
    try{
        int n=1,count=5000,rate=10000,burst=1;std::string mode="atomic";
        for(int i=1;i<argc;i+=2){if(i+1==argc)throw std::runtime_error("Missing option");std::string k=argv[i],v=argv[i+1];
            if(k=="--mode")mode=v;else if(k=="--n")n=std::stoi(v);else if(k=="--count")count=std::stoi(v);
            else if(k=="--rate")rate=std::stoi(v);else if(k=="--burst")burst=std::stoi(v);else throw std::runtime_error("Unknown option");}
        if(n<1||n>32||count<1||count>1000000||rate<0||burst<1||burst>count||
           (mode!="spin"&&mode!="yield"&&mode!="atomic"&&mode!="hybrid"&&mode!="cv"))throw std::runtime_error("Invalid configuration");
        std::vector<std::unique_ptr<Link>> links;std::vector<Stats> stats;
        for(int i=0;i<n;++i){links.push_back(std::make_unique<Link>(mode));stats.emplace_back(count);}
        std::atomic<int> ready{0};std::atomic<bool> go{false};int64_t epoch=0;
        auto gate=[&]{configure_thread();ready.fetch_add(1);while(!go.load(std::memory_order_acquire))std::this_thread::yield();pace(epoch);};
        std::vector<std::thread> threads;
        for(int id=0;id<n;++id){
            threads.emplace_back([&,id]{auto& s=stats[id];gate();auto start=thread_cpu_ns();
                for(int i=0;i<count;){Message m;if(!links[id]->pop(m,s.waits)){if(mode=="yield")std::this_thread::yield();continue;}
                    auto end=clock_ns();if(m.seq!=uint64_t(i)||m.value!=(m.seq^0x821abcULL))++s.errors;
                    s.latency.push_back(end-m.started);s.scheduled.push_back(end-m.scheduled);++i;
                }s.consumer_cpu=thread_cpu_ns()-start;s.finish=clock_ns();
            });
            threads.emplace_back([&,id]{auto& s=stats[id];gate();auto start=thread_cpu_ns();
                for(int i=0;i<count;++i){int64_t target=rate?epoch+int64_t(i/burst)*burst*1000000000/rate:0;if(rate)pace(target);
                    Message m{uint64_t(i),uint64_t(i)^0x821abcULL,clock_ns(),target};if(!rate)m.scheduled=m.started;
                    while(!links[id]->push(m))++s.full;
                }s.producer_cpu=thread_cpu_ns()-start;
            });
        }
        while(ready.load()!=2*n)std::this_thread::yield();epoch=clock_ns()+20000000;go.store(true,std::memory_order_release);
        for(auto& t:threads)t.join();
        uint64_t errors=0,full=0,waits=0;int64_t pcpu=0,ccpu=0,finished=0;
        std::vector<int64_t> latency,scheduled;
        for(auto& s:stats){errors+=s.errors;full+=s.full;waits+=s.waits;pcpu+=s.producer_cpu;ccpu+=s.consumer_cpu;finished=std::max(finished,s.finish);
            latency.insert(latency.end(),s.latency.begin(),s.latency.end());scheduled.insert(scheduled.end(),s.scheduled.begin(),s.scheduled.end());}
        std::cout<<"{\"mode\":\""<<mode<<"\",\"pairs\":"<<n<<",\"count_per_pair\":"<<count<<",\"rate\":"<<rate
                 <<",\"burst\":"<<burst<<",\"errors\":"<<errors<<",\"received\":"<<latency.size()<<",\"elapsed_ns\":"<<finished-epoch
                 <<",\"producer_cpu_ns\":"<<pcpu<<",\"consumer_cpu_ns\":"<<ccpu<<",\"full_retries\":"<<full<<",\"wait_calls\":"<<waits<<",\"latency_ns\":";
        print_distribution(latency);std::cout<<",\"scheduled_latency_ns\":";print_distribution(scheduled);std::cout<<"}\n";
        return errors?2:0;
    }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
