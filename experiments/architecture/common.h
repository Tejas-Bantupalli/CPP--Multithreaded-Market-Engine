#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <time.h>
#include <vector>
#include <pthread/qos.h>
inline int64_t clock_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline int64_t thread_cpu_ns() {
    timespec t{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t)) throw std::runtime_error("CPU clock unavailable");
    return int64_t(t.tv_sec)*1000000000+t.tv_nsec;
}
inline void configure_thread(int cpu=-1) {
    if (cpu>=0) throw std::runtime_error("Exact CPU pinning is unavailable on macOS");
    if (pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED,0)) throw std::runtime_error("QoS request failed");
}
inline void pace(int64_t target) {
    if(clock_ns()+20000<target)
        std::this_thread::sleep_until(std::chrono::steady_clock::time_point(std::chrono::nanoseconds(target-20000)));
    while(clock_ns()<target) {}
}
inline void print_distribution(std::vector<int64_t>& v) {
    std::sort(v.begin(),v.end());
    auto at=[&](size_t n,size_t d){return v.empty()?0:v[std::min(v.size()-1,(v.size()*n+d-1)/d-1)];};
    std::cout<<"{\"count\":"<<v.size()<<",\"p50\":"<<at(50,100)<<",\"p99\":"<<at(99,100)
             <<",\"p999\":"<<at(999,1000)<<",\"max\":"<<at(1,1)<<"}";
}
