// Backtest fill-simulation tool.
//
// Validation mode (default): generates a trading day of market flow, then
//   1. truth : runs the strategy with its orders really inside the matching
//              engine, competing with the original flow;
//   2. sim   : runs the same strategy against the recorded tick feed of the
//              market-only flow, once per fill model (queue / touch / through);
// and reports how close each model gets to the truth, order by order.
//   backtest [--symbols 50] [--orders 1000000] [--seed 42] [--venue sse|szse]
//            [--interval-ms 5000] [--latency-ms 20] [--ttl-ms 30000] [--take 0.1]
//            [--base N] [--commission-bps x] [--stamp-bps x] [--fills out.csv]
//
// Feed mode: backtest the strategy on an existing tick feed (CSV written by
// exchange_sim / any feed converted to that format), queue model only.
//   backtest --ticks feed.csv --venue sse|szse [same strategy options]
#include <cstdio>
#include <cstring>
#include <string>

#include "me/backtest.h"

using namespace me;
using namespace me::bt;

namespace {

void print_header() {
    std::printf("  %-8s %8s %10s %9s %8s %9s %9s %8s %10s %10s %12s\n", "model", "orders", "filled", "fill%",
                "same_dec", "exact%", "agree%", "MAE", "over", "under", "PnL");
}

void print_row(const char* name, const Result& r, const Comparison& c, bool is_truth) {
    const double fill = c.placed_qty ? 100.0 * c.filled_qty / c.placed_qty : 0;
    if (is_truth) {
        std::printf("  %-8s %8lu %10ld %8.1f%% %8s %9s %9s %8s %10s %10s %12.0f\n", name, (unsigned long)c.orders,
                    (long)c.filled_qty, fill, "-", "-", "-", "-", "-", "-", r.pnl);
        return;
    }
    const double same = c.orders ? 100.0 * c.common / c.orders : 0;
    std::printf("  %-8s %8lu %10ld %8.1f%% %7.1f%% %8.1f%% %8.1f%% %8.1f %10ld %10ld %12.0f\n", name,
                (unsigned long)c.orders, (long)c.filled_qty, fill, same, c.common ? 100.0 * c.exact / c.common : 0,
                c.common ? 100.0 * c.agree / c.common : 0, c.mae, (long)c.over, (long)c.under, r.pnl);
}

void write_fills(const Result& r, const std::string& path) {
    FILE* fp = std::fopen(path.c_str(), "w");
    if (!fp) { std::perror(path.c_str()); return; }
    std::fputs("time,sym,order_id,side,price,qty\n", fp);
    for (const FillRec& f : r.fills)
        std::fprintf(fp, "%09u,%u,%lu,%c,%s,%d\n", from_ms(f.t_ms), f.sym, (unsigned long)(f.id - kOwnBase),
                     f.side == Side::Buy ? 'B' : 'S', ticks_to_str(f.price).c_str(), f.qty);
    std::fclose(fp);
}

}  // namespace

int main(int argc, char** argv) {
    FlowParams fp;
    fp.symbols = 50;
    fp.orders = 1'000'000;
    Params p;
    BookConfig cfg;
    std::string ticks, fills;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string k = argv[i], v = argv[i + 1];
        if (k == "--symbols") fp.symbols = std::stoi(v);
        else if (k == "--orders") fp.orders = std::stoll(v);
        else if (k == "--seed") fp.seed = std::stoull(v);
        else if (k == "--venue") cfg.venue = v == "szse" ? Venue::SZSE : Venue::SSE;
        else if (k == "--interval-ms") p.interval_ms = std::stoll(v);
        else if (k == "--latency-ms") p.latency_ms = std::stoll(v);
        else if (k == "--ttl-ms") p.ttl_ms = std::stoll(v);
        else if (k == "--take") p.take_ratio = std::stod(v);
        else if (k == "--base") p.base_position = std::stoi(v);
        else if (k == "--commission-bps") p.commission_bps = std::stod(v);
        else if (k == "--stamp-bps") p.stamp_tax_bps = std::stod(v);
        else if (k == "--ticks") ticks = v;
        else if (k == "--fills") fills = v;
    }
    JoinBestStrategy st{p.take_ratio};
    const char* venue = cfg.venue == Venue::SSE ? "SSE" : "SZSE";

    if (!ticks.empty()) {  // ---------------------------------- feed mode
        FILE* in = std::fopen(ticks.c_str(), "r");
        if (!in) { std::perror(ticks.c_str()); return 1; }
        std::vector<TickRecord> feed;
        char line[256];
        TickRecord r;
        SymbolId nsym = 0;
        if (!std::fgets(line, sizeof line, in)) return 1;
        while (std::fgets(line, sizeof line, in))
            if (read_tick_line(line, r)) { feed.push_back(r); nsym = std::max(nsym, r.sym + 1); }
        std::fclose(in);
        Result res = run_sim(feed, nsym, cfg.venue, FillModel::Queue, st, p);
        std::printf("[%s feed] %zu records, %u securities, simulated in %.2fs (%.2f M rec/s)\n", venue, feed.size(),
                    nsym, res.seconds, feed.size() / res.seconds / 1e6);
        std::printf("  fills=%zu bought=%ld sold=%ld turnover=%.0f fees=%.0f PnL=%.0f\n", res.fills.size(),
                    (long)res.bought, (long)res.sold, res.turnover, res.fees, res.pnl);
        if (!fills.empty()) write_fills(res, fills);
        return 0;
    }

    // ------------------------------------------------------- validation mode
    Flow f = generate_flow(fp);
    anonymize(f);
    std::printf("[%s] market flow: %zu msgs, %zu securities | strategy: join best bid/ask every %ld ms, "
                "take %.0f%%, latency %ld ms, TTL %ld ms\n",
                venue, f.msgs.size(), f.symbols.size(), (long)p.interval_ms, p.take_ratio * 100,
                (long)p.latency_ms, (long)p.ttl_ms);

    Result truth = run_truth(f, cfg, st, p);
    std::vector<TickRecord> feed = make_feed(f, cfg);
    std::printf("truth (orders inside the engine): %.2fs | recorded feed: %zu records\n", truth.seconds, feed.size());

    Result models[3];
    const FillModel ms[3] = {FillModel::Queue, FillModel::Touch, FillModel::Through};
    for (int k = 0; k < 3; ++k) models[k] = run_sim(feed, f.symbols.size(), cfg.venue, ms[k], st, p);
    std::printf("simulation speed (queue model): %.2f M feed records/s\n", feed.size() / models[0].seconds / 1e6);

    const char* sections[3] = {"all orders", "passive orders (join the queue)", "aggressive orders (cross the spread)"};
    for (int which = 0; which < 3; ++which) {
        std::printf("\n%s\n", sections[which]);
        print_header();
        print_row("truth", truth, compare(truth, truth, which), true);
        for (int k = 0; k < 3; ++k) print_row(model_name(ms[k]), models[k], compare(truth, models[k], which), false);
    }
    std::printf("\nsame_dec: orders whose decision (side/price/qty) is identical in both worlds; exact%%/agree%%/MAE/over/under\n"
                "are computed over those: exact filled qty, both filled-or-not, mean abs error (shares), total shares\n"
                "over-/under-filled vs truth. PnL marks positions at each world's last price.\n");
    if (!fills.empty()) write_fills(models[0], fills);
    return 0;
}
