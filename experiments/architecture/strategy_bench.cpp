#include "common.h"
#include "queues.h"
#include <array>
#include <deque>
#include <map>
#include <numeric>
#include <random>
#include <string>

constexpr unsigned LEVELS=4096,WINDOW=128;
struct Wire {uint32_t side,level,qty,price;};
struct Tick {unsigned side,level,qty,price;};
Tick decode(const Wire& w) {return {__builtin_bswap32(w.side),__builtin_bswap32(w.level),__builtin_bswap32(w.qty),__builtin_bswap32(w.price)};}
struct DenseBook {
    std::array<std::array<unsigned,LEVELS>,2> levels{};
    std::array<unsigned,2> best{LEVELS,LEVELS};
    void update(unsigned side,unsigned level,unsigned qty) {
        levels[side][level]=qty;
        if(qty&&level<best[side])best[side]=level;
        if(!qty&&level==best[side])while(best[side]<LEVELS&&!levels[side][best[side]])++best[side];
    }
    unsigned top(unsigned side)const{return best[side];}
    unsigned quantity(unsigned side)const{return best[side]==LEVELS?0:levels[side][best[side]];}
};
struct TreeBook {
    std::array<std::map<unsigned,unsigned>,2> levels;
    void update(unsigned side,unsigned level,unsigned qty){if(qty)levels[side][level]=qty;else levels[side].erase(level);}
    unsigned top(unsigned side)const{return levels[side].empty()?LEVELS:levels[side].begin()->first;}
    unsigned quantity(unsigned side)const{return levels[side].empty()?0:levels[side].begin()->second;}
};
template<bool INCREMENTAL>struct Signal {
    std::array<unsigned,WINDOW> prices{};unsigned next=0,count=0;int64_t sum=0;
    void push(unsigned price){if constexpr(INCREMENTAL)sum+=int64_t(price)-prices[next];prices[next]=price;next=(next+1)%WINDOW;if(count<WINDOW)++count;}
    int64_t total()const{if constexpr(INCREMENTAL)return sum;else return std::accumulate(prices.begin(),prices.end(),int64_t(0));}
};
struct Intent {uint64_t seq;unsigned side,price,qty;};
// No-inline allocation/release keeps this experiment from becoming an elided
// new/delete pair. Both paths use the same no-inline order-consumption function.
__attribute__((noinline)) Intent* allocate_intent(){return new Intent;}
__attribute__((noinline)) void release_intent(Intent* p){delete p;}
__attribute__((noinline)) uint64_t consume_intent(const Intent* p){return p->seq^(uint64_t(p->side)<<61)^(uint64_t(p->price)<<20)^p->qty;}
template<class Book,bool INCREMENTAL,bool HEAP>struct Processor {
    Book book;Signal<INCREMENTAL> signal;uint64_t checksum=1469598103934665603ULL,orders=0;
    void step(const Tick& t,uint64_t seq){
        book.update(t.side,t.level,t.qty);signal.push(t.price);
        const auto sum=signal.total();const int64_t delta=int64_t(t.price)*WINDOW-sum;
        uint64_t value=uint64_t(sum)^(uint64_t(book.top(0))<<32)^(uint64_t(book.top(1))<<48);
        const bool buy=delta>2*WINDOW,sell=delta<-int64_t(2*WINDOW);
        if(signal.count==WINDOW&&(buy||sell)){
            const unsigned side=buy?1:0;
            if(book.quantity(side)){
                Intent local;Intent* order;
                if constexpr(HEAP)order=allocate_intent();else order=&local;
                *order={seq,buy?0u:1u,buy?10001+book.top(1):10000-book.top(0),std::min(4u,book.quantity(side))};
                value^=consume_intent(order);++orders;
                if constexpr(HEAP)release_intent(order);
            }
        }
        checksum=(checksum^value)*1099511628211ULL;
    }
};
std::pair<uint64_t,uint64_t> reference(const std::vector<Wire>& trace){
    // Independent signal implementation and explicit map-based book model.
    std::array<std::map<unsigned,unsigned>,2> book;std::deque<unsigned> history;
    uint64_t hash=1469598103934665603ULL,orders=0;
    for(size_t i=0;i<trace.size();++i){
        auto t=decode(trace[i]);if(t.qty)book[t.side][t.level]=t.qty;else book[t.side].erase(t.level);
        history.push_back(t.price);if(history.size()>WINDOW)history.pop_front();
        int64_t sum=std::accumulate(history.begin(),history.end(),int64_t(0));
        unsigned bid=book[0].empty()?LEVELS:book[0].begin()->first,ask=book[1].empty()?LEVELS:book[1].begin()->first;
        uint64_t value=uint64_t(sum)^(uint64_t(bid)<<32)^(uint64_t(ask)<<48);
        int64_t delta=int64_t(t.price)*WINDOW-sum;
        if(history.size()==WINDOW&&(delta>256||delta<-256)){
            bool buy=delta>256;unsigned side=buy?1:0;
            if(!book[side].empty()){
                unsigned price=buy?10001+ask:10000-bid,qty=std::min(4u,book[side].begin()->second);
                value^=i^(uint64_t(buy?0:1)<<61)^(uint64_t(price)<<20)^qty;++orders;
            }
        }
        hash=(hash^value)*1099511628211ULL;
    }
    return {hash,orders};
}
struct Packet {Tick tick;uint64_t seq=0;int64_t start=0,scheduled=0;};
template<class Book,bool INC,bool HEAP>
int execute(const std::vector<Wire>& trace,const std::string& topology,int rate,int pcpu,int ccpu,const std::pair<uint64_t,uint64_t>& expected){
    Processor<Book,INC,HEAP> processor;
    const bool pipeline=topology!="inline";
    auto queue=make_queue<Packet>(topology=="cached"?128:0,topology=="cached");
    std::vector<int64_t> latency,scheduled;latency.reserve(trace.size()/257+1);scheduled.reserve(trace.size()/257+1);
    std::atomic<unsigned> ready{0};std::atomic<bool> go{false};int64_t epoch=0,producer_cpu=0,consumer_cpu=0,finished=0;
    uint64_t full=0,empty=0,sequence_errors=0;
    auto gate=[&]{ready.fetch_add(1);while(!go.load(std::memory_order_acquire))std::this_thread::yield();pace(epoch);};
    auto receive=[&](const Packet& p){processor.step(p.tick,p.seq);if(p.start){auto end=clock_ns();latency.push_back(end-p.start);scheduled.push_back(end-p.scheduled);}};
    auto produce=[&]{
        configure_thread(pcpu);gate();auto start=thread_cpu_ns();
        for(size_t i=0;i<trace.size();++i){
            int64_t target=rate?epoch+int64_t(i)*1000000000/rate:0;if(rate)pace(target);
            Packet packet;packet.seq=i;
            if(i%257==0){packet.start=clock_ns();packet.scheduled=rate?target:packet.start;}
            packet.tick=decode(trace[i]);
            if(pipeline){while(!queue->push(packet))++full;}else receive(packet);
        }
        producer_cpu=thread_cpu_ns()-start;
        if(!pipeline)finished=clock_ns();
    };
    std::thread consumer;
    if(pipeline)consumer=std::thread([&]{configure_thread(ccpu);gate();auto start=thread_cpu_ns();
        for(size_t i=0;i<trace.size();){Packet p;if(!queue->pop(p)){++empty;continue;}if(p.seq!=i)++sequence_errors;receive(p);++i;}
        consumer_cpu=thread_cpu_ns()-start;
        finished=clock_ns();
    });
    std::thread producer(produce);
    while(ready.load()!=(pipeline?2u:1u))std::this_thread::yield();epoch=clock_ns()+20000000;go.store(true,std::memory_order_release);
    producer.join();if(pipeline)consumer.join();const auto elapsed=finished-epoch;
    bool correct=processor.checksum==expected.first&&processor.orders==expected.second&&!sequence_errors;
    std::cout<<"{\"correct\":"<<(correct?"true":"false")<<",\"events\":"<<trace.size()<<",\"orders\":"<<processor.orders
             <<",\"checksum\":"<<processor.checksum<<",\"elapsed_ns\":"<<elapsed<<",\"producer_cpu_ns\":"<<producer_cpu
             <<",\"consumer_cpu_ns\":"<<consumer_cpu<<",\"full_retries\":"<<full<<",\"empty_polls\":"<<empty<<",\"latency_ns\":";
    print_distribution(latency);std::cout<<",\"scheduled_latency_ns\":";print_distribution(scheduled);std::cout<<"}\n";
    return correct?0:2;
}
int main(int argc,char**argv){
    try{
        std::string book="dense",topology="inline",profile="sparse";int count=200000,rate=0,inc=1,heap=0,seed=1,pcpu=-1,ccpu=-1;
        for(int i=1;i<argc;i+=2){
            if(i+1==argc)throw std::runtime_error("Missing option value");std::string k=argv[i],v=argv[i+1];
            if(k=="--book")book=v;else if(k=="--topology")topology=v;else if(k=="--profile")profile=v;
            else if(k=="--count")count=std::stoi(v);else if(k=="--rate")rate=std::stoi(v);else if(k=="--incremental")inc=std::stoi(v);
            else if(k=="--heap")heap=std::stoi(v);else if(k=="--seed")seed=std::stoi(v);else if(k=="--producer-cpu")pcpu=std::stoi(v);
            else if(k=="--consumer-cpu")ccpu=std::stoi(v);else throw std::runtime_error("Unknown option");
        }
        if(count<1||count>2000000||rate<0||(inc!=0&&inc!=1)||(heap!=0&&heap!=1)||
           (book!="dense"&&book!="tree")||(topology!="inline"&&topology!="pipeline"&&topology!="cached")||
           (profile!="dense"&&profile!="sparse"))throw std::runtime_error("Invalid configuration");
#ifndef __linux__
        if(pcpu>=0||ccpu>=0)throw std::runtime_error("Exact affinity requires Linux in this benchmark");
#endif
        std::vector<Wire> trace;trace.reserve(count);std::mt19937 rng(seed);int price=10000;
        for(int i=0;i<count;++i){
            price=std::clamp(price+int(rng()%7)-3,9900,10100);
            unsigned side=rng()%2,level=rng()%(profile=="dense"?256:LEVELS),qty=rng()%5==0?0:1+rng()%20;
            trace.push_back({__builtin_bswap32(side),__builtin_bswap32(level),__builtin_bswap32(qty),__builtin_bswap32(unsigned(price))});
        }
        const auto expected=reference(trace);
#define RUN(B,I,H) return execute<B,I,H>(trace,topology,rate,pcpu,ccpu,expected)
        if(book=="dense") {if(inc){if(heap){RUN(DenseBook,true,true);}RUN(DenseBook,true,false);}if(heap){RUN(DenseBook,false,true);}RUN(DenseBook,false,false);}
        if(inc){if(heap){RUN(TreeBook,true,true);}RUN(TreeBook,true,false);}if(heap){RUN(TreeBook,false,true);}RUN(TreeBook,false,false);
#undef RUN
    }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
