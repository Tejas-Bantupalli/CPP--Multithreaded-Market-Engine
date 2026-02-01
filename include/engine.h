#pragma once

#include "market_types.h"

#include <chrono>

double compute_ma_or_price();
void push_price(double px);

void submit_order(bool is_buy,
                  int qty,
                  double limit_px,
                  uint32_t trader_id,
                  std::chrono::steady_clock::time_point t_event,
                  std::chrono::steady_clock::time_point t_recv);

void publish_event(uint64_t ev_seq, double last_trade_px);
void engine_loop();

bool pop_latest_event(uint32_t id, MarketEvent& out);

void ma_strategy_baseline(uint32_t id);
void ma_strategy_fast(uint32_t id);
void chaos(uint32_t id);
