#pragma once
#include <atomic>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include "spsc_queue.h"

template<class T> struct QueueInterface {
    virtual ~QueueInterface()=default;
    virtual bool push(const T&)=0;
    virtual bool pop(T&)=0;
    virtual size_t index_distance() const=0;
};
// Capacity and payload placement are held fixed across the controlled layouts.
// Cached indices are accessed only by their owning producer/consumer thread.
template<class T,size_t CAP,size_t ALIGN,bool CACHE>
class alignas(128) ExperimentalRing final : public QueueInterface<T> {
    struct State {std::atomic<size_t> index{0};size_t cached=0;};
    struct alignas(128) Storage {
        alignas(ALIGN) State head;
        alignas(ALIGN) State tail;
        alignas(128) T data[CAP];
    } storage_;
public:
    bool push(const T& value) override {
        auto& head_=storage_.head;auto& tail_=storage_.tail;
        const size_t h=head_.index.load(std::memory_order_relaxed),next=(h+1)&(CAP-1);
        size_t t;
        if constexpr(CACHE) {
            t=head_.cached;
            if(next==t) t=head_.cached=tail_.index.load(std::memory_order_acquire);
        } else t=tail_.index.load(std::memory_order_acquire);
        if(next==t) return false;
        storage_.data[h]=value;head_.index.store(next,std::memory_order_release);return true;
    }
    bool pop(T& value) override {
        auto& head_=storage_.head;auto& tail_=storage_.tail;
        const size_t t=tail_.index.load(std::memory_order_relaxed);
        size_t h;
        if constexpr(CACHE) {
            h=tail_.cached;
            if(t==h) h=tail_.cached=head_.index.load(std::memory_order_acquire);
        } else h=head_.index.load(std::memory_order_acquire);
        if(t==h) return false;
        value=storage_.data[t];tail_.index.store((t+1)&(CAP-1),std::memory_order_release);return true;
    }
    size_t index_distance() const override {
        return reinterpret_cast<uintptr_t>(&storage_.tail.index)-reinterpret_cast<uintptr_t>(&storage_.head.index);
    }
};
template<class T,size_t CAP> class ProductionRing final : public QueueInterface<T> {
    SPSCQueue<T,CAP> ring_;
public:
    bool push(const T& v) override{return ring_.try_push(v);}
    bool pop(T& v) override{return ring_.try_pop(v);}
    size_t index_distance() const override{return 0;} // private layout; not introspected
};
template<class T,size_t CAP=1024>
std::unique_ptr<QueueInterface<T>> make_queue(int alignment,bool cache) {
    static_assert((CAP&(CAP-1))==0);
    if(alignment==0) {
        if(cache) throw std::runtime_error("Production control has no remote-index cache");
        return std::make_unique<ProductionRing<T,CAP>>();
    }
#define CHOOSE(A) if(alignment==A) {if(cache) return std::make_unique<ExperimentalRing<T,CAP,A,true>>();else return std::make_unique<ExperimentalRing<T,CAP,A,false>>();}
    CHOOSE(8) CHOOSE(64) CHOOSE(128)
#undef CHOOSE
    throw std::runtime_error("Unsupported queue alignment");
}
