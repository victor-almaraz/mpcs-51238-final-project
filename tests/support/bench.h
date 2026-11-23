// The timer of the hidden benchmarks, run with "[.bench]": each prints its
// wall time for the A/B benchmarks of lessons.md "Data types".

#pragma once

#include <chrono>
#include <iostream>
#include <string_view>

namespace support {

/// Times @p work and prints it as `bench NAME SECONDS`.
template <typename Work>
void bench(std::string_view name, Work&& work) {
    const auto start = std::chrono::steady_clock::now();
    work();
    const std::chrono::duration<double> took = std::chrono::steady_clock::now() - start;
    std::cout << "bench " << name << " " << took.count() << "\n";
}

}  // namespace support
