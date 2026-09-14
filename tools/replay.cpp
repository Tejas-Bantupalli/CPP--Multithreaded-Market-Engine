// Replays a --cmdlog CSV through OrderBook on one thread and prints what the
// session must have produced: trade count, volume, open orders, book checksum.
#include "order_book.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: replay CMDLOG.csv [min_px_ticks max_px_ticks]\n";
        return 2;
    }
    const Price min_px = argc > 2 ? std::stoll(argv[2]) : 1;
    const Price max_px = argc > 3 ? std::stoll(argv[3]) : 100000;

    std::ifstream in(argv[1]);
    if (!in) { std::cerr << "cannot open " << argv[1] << "\n"; return 1; }

    OrderBook book(min_px, max_px);
    std::vector<Fill> fills;
    std::vector<CancelInfo> stp;
    uint64_t commands = 0, trades = 0, rejected = 0, cancels = 0;
    long long volume = 0;

    std::string line;
    std::getline(in, line); // header
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string kind, agent, id, side, tif, px, qty, ts;
        std::getline(ss, kind, ','); std::getline(ss, agent, ','); std::getline(ss, id, ',');
        std::getline(ss, side, ','); std::getline(ss, tif, ','); std::getline(ss, px, ',');
        std::getline(ss, qty, ','); std::getline(ss, ts, ',');

        Command c;
        c.agent = static_cast<AgentId>(std::stoul(agent));
        c.id = std::stoull(id);
        ++commands;
        if (kind == "cancel") {
            CancelInfo ci; RejectReason why;
            if (book.cancel(c.id, c.agent, ci, why)) ++cancels; else ++rejected;
            continue;
        }
        c.kind = CmdKind::New;
        c.side = side == "buy" ? Side::Buy : Side::Sell;
        c.tif = tif == "ioc" ? TimeInForce::IOC : TimeInForce::GTC;
        c.px = std::stoll(px);
        c.qty = std::stoi(qty);
        fills.clear(); stp.clear();
        const AddResult r = book.add(c, fills, stp);
        if (!r.accepted) { ++rejected; continue; }
        trades += fills.size();
        for (const Fill& f : fills) volume += f.qty;
    }

    std::cout << "commands=" << commands << " trades=" << trades << " volume=" << volume
              << " cancels=" << cancels << " rejected=" << rejected
              << " open_orders=" << book.open_orders()
              << " bid=" << (book.has_bid() ? book.best_bid() : 0)
              << " ask=" << (book.has_ask() ? book.best_ask() : 0)
              << " checksum=" << std::hex << book.checksum() << std::dec << "\n";
    return 0;
}
