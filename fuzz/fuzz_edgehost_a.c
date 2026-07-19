/**
 * Class-A libFuzzer harness for edgehost (P1.5).
 *
 * Build: cmake -B build-fuzz -S . -DBUILD_FUZZ=ON -DCMAKE_C_COMPILER=clang
 *        cmake --build build-fuzz --target fuzz_edgehost_a
 */
#include "edge_sim_main.h"

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    return edge_sim_drive(data, size);
}
