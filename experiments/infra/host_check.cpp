#include "host.cpp"
#include <cassert>

int main() {
    auto orders = make_channel<Command>(TransportKind::Spsc);
    auto events = make_channel<Event>(TransportKind::Spsc);
    AgentContext ctx(0, *orders, *events, 10000, 1, 999);
    InfraTrader trader(Params{});
    Event e;
    e.kind = EventKind::Trade;
    e.px = 10000; e.bid_px = 9999; e.ask_px = 10001;
    e.bid_qty = e.ask_qty = 100;
    auto send = [&](Event ev) { ev.ts = now_ns(); ctx.dispatch(ev, trader); };
    Command c;
    for (unsigned i = 0; i < INFRA_WINDOW; ++i) send(e);
    assert(!orders->pop(c)); // flat price produces no momentum signal
    for (int i = 0; i < 20; ++i) {
        e.px += 10; e.ask_px = e.px + 1; e.bid_px = e.px - 1;
        send(e);
        assert(orders->pop(c) && c.side == Side::Buy && c.qty == 1 && c.tif == TimeInForce::IOC);
        const OrderId id = c.id;
        send(e);
        assert(!orders->pop(c)); // outstanding reservation
        Event ack;
        ack.kind = EventKind::Ack; ack.order_id = id; ack.qty = 1;
        send(ack); send(e);
        assert(!orders->pop(c)); // filled ack does not release before actual fill
        Event fill;
        fill.kind = EventKind::Fill; fill.order_id = id;
        fill.side = Side::Buy; fill.qty = 1; fill.px = e.ask_px;
        send(fill);
    }
    assert(ctx.position() == 20);
    send(e);
    assert(!orders->pop(c)); // cap is preserved
    e.px = 9000; e.bid_px = 8999; e.ask_px = 9001;
    send(e);
    assert(orders->pop(c) && c.side == Side::Sell);
    Event rejected;
    rejected.kind = EventKind::Rejected; rejected.order_id = c.id;
    send(rejected); send(e);
    assert(orders->pop(c) && c.side == Side::Sell);
    Event empty_ack;
    empty_ack.kind = EventKind::Ack; empty_ack.order_id = c.id; empty_ack.qty = 0;
    send(empty_ack); send(e);
    assert(orders->pop(c)); // unfilled IOC releases reservation
}
