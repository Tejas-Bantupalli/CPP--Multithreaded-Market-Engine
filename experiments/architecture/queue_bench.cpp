#include "common.h"
#include "queues.h"
#include <string>
struct Message {uint64_t seq=0,value=0;int64_t started=0,scheduled=0;};
int main(int argc,char** argv) {
    try {
        int alignment=128,cached=0,count=500000,rate=0,pcpu=-1,ccpu=-1;
        for(int i=1;i<argc;i+=2) {
            if(i+1==argc) throw std::runtime_error("Missing value");
            std::string k=argv[i];int v=std::stoi(argv[i+1]);
            if(k=="--align")alignment=v;else if(k=="--cached")cached=v;
            else if(k=="--count")count=v;else if(k=="--rate")rate=v;
            else if(k=="--producer-cpu")pcpu=v;else if(k=="--consumer-cpu")ccpu=v;
            else throw std::runtime_error("Unknown option");
        }
        if(count<1 || count>10000000 || rate<0 || (cached!=0&&cached!=1)) throw std::runtime_error("Invalid options");
#ifndef __linux__
        if(pcpu>=0||ccpu>=0) throw std::runtime_error("Exact affinity requires Linux in this benchmark");
#endif
        auto q=make_queue<Message>(alignment,cached);
        std::atomic<int> ready{0};std::atomic<bool> go{false};int64_t epoch=0;
        uint64_t full=0,empty=0,checksum=0,errors=0;
        int64_t producer_cpu=0,consumer_cpu=0;
        std::vector<int64_t> latency,scheduled;latency.reserve(count/257+1);scheduled.reserve(count/257+1);
        auto gate=[&]{ready.fetch_add(1);while(!go.load(std::memory_order_acquire))std::this_thread::yield();pace(epoch);};
        std::thread consumer([&]{
            configure_thread(ccpu);gate();auto start=thread_cpu_ns();
            for(int i=0;i<count;) {
                Message m;
                if(!q->pop(m)){++empty;continue;}
                if(m.seq!=uint64_t(i)||m.value!=(m.seq^0xacf12983ULL))++errors;
                checksum+=m.seq;
                if(m.started){auto end=clock_ns();latency.push_back(end-m.started);scheduled.push_back(end-m.scheduled);}
                ++i;
            }
            consumer_cpu=thread_cpu_ns()-start;
        });
        std::thread producer([&]{
            configure_thread(pcpu);gate();auto start=thread_cpu_ns();
            for(int i=0;i<count;++i){
                int64_t target=rate?epoch+int64_t(i)*1000000000/rate:0;
                if(rate)pace(target);
                Message m;m.seq=i;m.value=m.seq^0xacf12983ULL;
                if(i%257==0){m.started=clock_ns();m.scheduled=rate?target:m.started;}
                while(!q->push(m))++full;
            }
            producer_cpu=thread_cpu_ns()-start;
        });
        while(ready.load()!=2)std::this_thread::yield();epoch=clock_ns()+20000000;go.store(true,std::memory_order_release);
        producer.join();consumer.join();auto elapsed=clock_ns()-epoch;
        if(checksum!=uint64_t(count)*(count-1)/2)++errors;
        std::cout<<"{\"alignment\":"<<alignment<<",\"cached\":"<<cached<<",\"count\":"<<count
                 <<",\"rate\":"<<rate<<",\"index_distance\":"<<q->index_distance()<<",\"capacity\":1023"
                 <<",\"errors\":"<<errors<<",\"elapsed_ns\":"<<elapsed<<",\"producer_cpu_ns\":"<<producer_cpu
                 <<",\"consumer_cpu_ns\":"<<consumer_cpu<<",\"full_retries\":"<<full<<",\"empty_polls\":"<<empty
                 <<",\"producer_cpu\":"<<pcpu<<",\"consumer_cpu\":"<<ccpu<<",\"latency_ns\":";
        print_distribution(latency);std::cout<<",\"scheduled_latency_ns\":";print_distribution(scheduled);std::cout<<"}\n";
        return errors?2:0;
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
