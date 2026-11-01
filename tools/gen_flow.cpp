// gen_flow <out.flow> [symbols=100] [orders=2000000] [seed=42]
#include <cstdlib>

#include "me/flow.h"

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: gen_flow out.flow [symbols] [orders] [seed]\n"); return 1; }
    me::FlowParams p;
    if (argc > 2) p.symbols = std::atoi(argv[2]);
    if (argc > 3) p.orders = std::atoll(argv[3]);
    if (argc > 4) p.seed = std::strtoull(argv[4], nullptr, 10);
    me::Flow f = me::generate_flow(p);
    me::save_flow(f, argv[1]);
    size_t news = 0, cancels = 0, phases = 0;
    for (auto& m : f.msgs) (m.type == me::MsgType::New ? news : m.type == me::MsgType::Cancel ? cancels : phases)++;
    std::printf("wrote %s: %zu securities, %zu msgs (new=%zu cancel=%zu phase=%zu)\n", argv[1], f.symbols.size(),
                f.msgs.size(), news, cancels, phases);
}
