// End-to-end multi-threaded engine benchmark with stage-level latency.
//   bench_engine [--flow file] [--symbols N] [--orders N] [--shards K]
//                [--log none|sync|async] [--rate msgs_per_sec (0 = max)]
#include <cstdlib>
#include <cstring>
#include <string>

#include "me/engine.h"
#include "me/flow.h"
#include "me/latency.h"

using namespace me;

int main(int argc, char** argv) {
    std::string flow_path, log = "async";
    int nsym = 100, shards = 2;
    int64_t norders = 2'000'000;
    double rate = 0;
    bool pin = false;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string k = argv[i], v = argv[i + 1];
        if (k == "--flow") flow_path = v;
        else if (k == "--symbols") nsym = std::stoi(v);
        else if (k == "--orders") norders = std::stoll(v);
        else if (k == "--shards") shards = std::stoi(v);
        else if (k == "--log") log = v;
        else if (k == "--rate") rate = std::stod(v);
        else if (k == "--pin") pin = v == "1";
    }
    // --pin 1: gateway + logger on CPU 0, shard k on CPU 1+k (wrapping).
    const int ncpu = static_cast<int>(std::thread::hardware_concurrency());
    Flow f = flow_path.empty() ? generate_flow({nsym, norders, 7}) : load_flow(flow_path);

    EngineOptions opt;
    opt.shards = shards;
    opt.log_mode = log == "sync" ? LogMode::Sync : log == "none" ? LogMode::None : LogMode::Async;
    opt.log_path = "/tmp/me_bench_" + log + ".log";
    opt.expected_msgs = f.msgs.size();
    if (pin && ncpu > 1) {
        for (int k = 0; k < shards; ++k) opt.shard_cpus.push_back(1 + k % (ncpu - 1));
        opt.logger_cpu = 0;
        pin_current_thread(0);
    }
    Engine eng(f.symbols, opt);
    eng.start();

    const int64_t t0 = now_ns();
    if (rate > 0) {  // paced: measure latency at a fixed offered load
        const double gap = 1e9 / rate;
        for (size_t i = 0; i < f.msgs.size(); ++i) {
            int64_t due = t0 + static_cast<int64_t>(i * gap);
            for (int64_t now; (now = now_ns()) < due;) {
                if (due - now > 20000) std::this_thread::yield();  // let the logger run
                else ME_CPU_RELAX();
            }
            eng.submit(f.msgs[i]);
        }
    } else {
        for (const OrderMsg& m : f.msgs) eng.submit(m);
    }
    eng.finish();
    const double secs = (now_ns() - t0) / 1e9;

    std::vector<int64_t> qw, mc, pub, e2e;
    uint64_t trades = 0;
    for (int k = 0; k < eng.shards(); ++k) {
        const ShardStats& s = eng.stats(k);
        qw.insert(qw.end(), s.queue_wait.begin(), s.queue_wait.end());
        mc.insert(mc.end(), s.match_core.begin(), s.match_core.end());
        pub.insert(pub.end(), s.publish.begin(), s.publish.end());
        e2e.insert(e2e.end(), s.e2e.begin(), s.e2e.end());
        trades += s.trades;
        std::printf("  shard %d: %lu msgs, %lu trades\n", k, (unsigned long)s.msgs, (unsigned long)s.trades);
    }
    std::printf("shards=%d log=%s pin=%d rate=%s msgs=%zu trades=%lu  wall=%.3fs  throughput=%.0f msg/s\n", shards,
                log.c_str(), int(pin), rate > 0 ? std::to_string((long)rate).c_str() : "max", f.msgs.size(),
                (unsigned long)trades, secs, f.msgs.size() / secs);
    print_percentiles("queue_wait", summarize(std::move(qw)));
    print_percentiles("match_core", summarize(std::move(mc)));
    print_percentiles("publish", summarize(std::move(pub)));
    print_percentiles("e2e", summarize(std::move(e2e)));
    std::printf("  log records written=%lu dropped=%lu\n", (unsigned long)eng.log_written(),
                (unsigned long)eng.log_dropped());
    return 0;
}
