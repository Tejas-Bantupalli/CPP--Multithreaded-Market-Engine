#pragma once

#include "market_types.h"
#include "spsc_queue.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <vector>

// ===================== Market State =====================
extern std::atomic<double> market_price;
extern std::deque<double> price_history;
extern std::mutex price_mtx;

extern std::atomic<bool> running;

// ===================== Book (engine owns it) =====================
extern std::priority_queue<Order, std::vector<Order>, BuyComp> bids;
extern std::priority_queue<Order, std::vector<Order>, SellComp> asks;

extern std::atomic<uint64_t> next_seq;

// ===================== Portfolios =====================
extern std::unordered_map<uint32_t, Portfolio> portfolios;
extern std::mutex portfolios_mtx;

// ===================== Latency Stats =====================
extern LatAgg lat_recv_to_submit[NUM_TRADERS];
extern LatAgg lat_event_to_submit[NUM_TRADERS];
extern LatAgg lat_submit_to_pop[NUM_TRADERS];

// ===================== Two Queue Networks =====================
// 1) Orders: trader -> engine  (per-trader SPSC)
// 2) Events: engine -> trader  (per-trader SPSC)

// ----- Orders (Trader -> Engine)
extern SPSCQueue<Command, QCAP> oq0;
extern SPSCQueue<Command, QCAP> oq1;
extern SPSCQueue<Command, QCAP> oq2;
extern SPSCQueue<Command, QCAP> oq99;

// Engine sleep/wake when no orders
extern std::atomic<int> pending_orders;
extern std::mutex pending_mtx;
extern std::condition_variable pending_cv;

// ----- Events (Engine -> Trader)
extern SPSCQueue<MarketEvent, QCAP> md0;
extern SPSCQueue<MarketEvent, QCAP> md1;
extern SPSCQueue<MarketEvent, QCAP> md2;
extern SPSCQueue<MarketEvent, QCAP> md99;
