// Production-side factor host: dlopen()s factor plugins and computes every
// factor for every security in a bar file.
//   factor_runner <bars.csv> <out_prefix> <plugin.so>...
// bars.csv: sym,t,open,high,low,close,volume   (grouped by sym, oldest first)
// Outputs <out_prefix>.bin  float64 [num_factors][num_rows], row order = file
//         <out_prefix>.names  "family,factor" per line
#include <dlfcn.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "factor/factor_api.h"

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: factor_runner bars.csv out_prefix plugin.so...\n"); return 1; }

    // ---- load bars
    FILE* fp = std::fopen(argv[1], "r");
    if (!fp) { std::perror(argv[1]); return 1; }
    char line[512];
    if (!std::fgets(line, sizeof line, fp)) return 1;
    std::vector<double> o, h, l, c, v;
    std::vector<std::pair<int64_t, int64_t>> groups;  // [begin, end) per security
    std::string prev;
    char sym[32];
    long t;
    double a, b, d, e, f;
    while (std::fgets(line, sizeof line, fp)) {
        if (std::sscanf(line, "%31[^,],%ld,%lf,%lf,%lf,%lf,%lf", sym, &t, &a, &b, &d, &e, &f) != 7) continue;
        if (prev != sym) {
            if (!groups.empty()) groups.back().second = static_cast<int64_t>(c.size());
            groups.push_back({static_cast<int64_t>(c.size()), 0});
            prev = sym;
        }
        o.push_back(a); h.push_back(b); l.push_back(d); c.push_back(e); v.push_back(f);
    }
    std::fclose(fp);
    if (groups.empty()) { std::fprintf(stderr, "no bars\n"); return 1; }
    groups.back().second = static_cast<int64_t>(c.size());
    const int64_t rows = static_cast<int64_t>(c.size());

    // ---- load plugins
    struct Loaded { void* handle; const FactorPlugin* p; };
    std::vector<Loaded> plugins;
    int total = 0;
    for (int i = 3; i < argc; ++i) {
        void* hd = dlopen(argv[i], RTLD_NOW | RTLD_LOCAL);
        if (!hd) { std::fprintf(stderr, "dlopen %s: %s\n", argv[i], dlerror()); return 1; }
        auto entry = reinterpret_cast<factor_plugin_entry_fn>(dlsym(hd, "factor_plugin_entry"));
        if (!entry) { std::fprintf(stderr, "%s: no factor_plugin_entry\n", argv[i]); return 1; }
        const FactorPlugin* p = entry();
        if (p->abi_version != FACTOR_ABI_VERSION) { std::fprintf(stderr, "%s: ABI mismatch\n", argv[i]); return 1; }
        std::printf("  loaded %-28s family=%-10s factors=%d\n", argv[i], p->family, p->num_factors);
        plugins.push_back({hd, p});
        total += p->num_factors;
    }

    // ---- compute
    std::vector<double> out(static_cast<size_t>(total) * rows);
    std::string pre = argv[2];
    FILE* nf = std::fopen((pre + ".names").c_str(), "w");
    auto t0 = std::chrono::steady_clock::now();
    int k = 0;
    for (auto& lp : plugins) {
        for (int i = 0; i < lp.p->num_factors; ++i, ++k) {
            std::fprintf(nf, "%s,%s\n", lp.p->family, lp.p->factor_name(i));
            for (auto [beg, end] : groups) {
                FactorInput in{&o[beg], &h[beg], &l[beg], &c[beg], &v[beg], end - beg};
                if (lp.p->compute(i, &in, &out[static_cast<size_t>(k) * rows + beg]) != 0) {
                    std::fprintf(stderr, "factor %s failed\n", lp.p->factor_name(i));
                    return 1;
                }
            }
        }
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::fclose(nf);
    FILE* bf = std::fopen((pre + ".bin").c_str(), "wb");
    std::fwrite(out.data(), sizeof(double), out.size(), bf);
    std::fclose(bf);
    std::printf("computed %d factors x %ld rows (%zu securities) in %.3fs\n", total, (long)rows, groups.size(), secs);
    for (auto& lp : plugins) dlclose(lp.handle);
    return 0;
}
