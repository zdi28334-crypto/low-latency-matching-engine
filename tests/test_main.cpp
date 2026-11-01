#include <chrono>

#include "test_util.h"

int main() {
    int run = 0;
    for (auto& t : registry()) {
        int before = failures();
        auto t0 = std::chrono::steady_clock::now();
        t.fn();
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("[%s] %-32s %8.1f ms\n", failures() == before ? " OK " : "FAIL", t.name, ms);
        ++run;
    }
    std::printf("\n%d tests, %d failed checks\n", run, failures());
    return failures() ? 1 : 0;
}
