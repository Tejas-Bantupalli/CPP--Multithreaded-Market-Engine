#include "engine_state.h"

// ===================== Market State =====================
std::atomic<double> market_price{100.0};
std::deque<double> price_history;
std::mutex price_mtx;

std::atomic<bool> running{true};

// ===================== Book (engine owns it) =====================
std::priority_queue<Order, std::vector<Order>, BuyComp> bids;
std::priority_queue<Order, std::vector<Order>, SellComp> asks;

std::atomic<uint64_t> next_seq{0};

// ===================== Portfolios =====================
std::unordered_map<uint32_t, Portfolio> portfolios;
std::mutex portfolios_mtx;

// ===================== Latency Stats =====================
LatAgg lat_recv_to_submit[NUM_TRADERS];
LatAgg lat_event_to_submit[NUM_TRADERS];
LatAgg lat_submit_to_pop[NUM_TRADERS];

// ===================== Two Queue Networks =====================
// 1) Orders: trader -> engine  (per-trader SPSC)
// 2) Events: engine -> trader  (per-trader SPSC)

// ----- Orders (Trader -> Engine)
SPSCQueue<Command, QCAP> oq0;
SPSCQueue<Command, QCAP> oq1;
SPSCQueue<Command, QCAP> oq2;
SPSCQueue<Command, QCAP> oq99;

// Engine sleep/wake when no orders
std::atomic<int> pending_orders{0};
std::mutex pending_mtx;
std::condition_variable pending_cv;

// ----- Events (Engine -> Trader)
SPSCQueue<MarketEvent, QCAP> md0;
SPSCQueue<MarketEvent, QCAP> md1;
SPSCQueue<MarketEvent, QCAP> md2;
SPSCQueue<MarketEvent, QCAP> md99;
