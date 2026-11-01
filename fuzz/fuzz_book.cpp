// libFuzzer target. Build with clang and -DME_FUZZ=ON, then e.g.
//   ./build-fuzz/fuzz_book -max_total_time=60 -max_len=4096
#include <cstdio>
#include <cstdlib>

#include "fuzz_common.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string err;
    if (!fuzz::run_case(data, size, &err)) {
        std::fprintf(stderr, "MISMATCH: %s\n", err.c_str());
        std::abort();
    }
    return 0;
}
