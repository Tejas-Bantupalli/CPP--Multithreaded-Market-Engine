// Uncontended queue-operation cost. Deliberately one thread, not a handoff test.
#include <memory>
#include "transport.h"
#include <algorithm>
#include <iostream>
#include <thread>
#include <time.h>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif
int64_t cpu() {
    timespec t{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t)) std::abort();
    return int64_t(t.tv_sec)*1000000000+t.tv_nsec;
}
struct Payload { uint64_t seq=0, pad[4]{}; };
int main(int argc,char** argv) {
    TransportKind kind;
    if (argc!=2 || !parse_transport(argv[1],kind)) return 1;
#ifdef __APPLE__
    if (pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED,0)) return 2;
#endif
    auto q=make_channel<Payload>(kind);
    constexpr unsigned rounds=40, count=50000;
    std::cout << "{\"transport\":\"" << argv[1] << "\",\"message_bytes\":" << sizeof(Payload)
              << ",\"pairs_per_batch\":" << count << ",\"batches\":[";
    uint64_t checksum=0;
    for (unsigned r=0;r<rounds;++r) {
        const auto start=now_ns(), start_cpu=cpu();
        for (unsigned i=0;i<count;++i) {
            Payload in; in.seq=uint64_t(r)*count+i;
            Payload out;
            if (!q->push(in) || !q->pop(out) || out.seq!=in.seq) return 3;
            checksum ^= out.seq;
        }
        const auto end_cpu=cpu(), end=now_ns();
        if (r) std::cout << ',';
        std::cout << "{\"cpu_ns_per_pair\":" << double(end_cpu-start_cpu)/count
                  << ",\"wall_ns_per_pair\":" << double(end-start)/count << '}';
    }
    uint64_t expected=0;
    for (unsigned i=0;i<rounds*count;++i) expected ^= i;
    std::cout << "],\"checksum_ok\":" << (checksum==expected ? "true" : "false") << "}\n";
    return checksum==expected ? 0 : 4;
}
