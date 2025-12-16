#include <atomic>

std::atomic<double> market_price{100.0}; 
struct Order {
    int volume;      
    uint32_t thread_id;
};
struct Portfolio {
    double cash = 10000.0;
    int holdings = 0;
};

std::unordered_map<uint32_t, Portfolio> portfolios;
