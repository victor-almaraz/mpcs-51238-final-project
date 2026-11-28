/// @file
/// Deterministic random numbers for primitives (`primitives.md` §7.6.4,
/// §7.8.2). Each node's generator is seeded from the project seed and
/// its node ID, so offline renders repeat exactly.

#pragma once

#include <cstdint>
#include <string_view>

namespace primitives {

/// SplitMix64: a 64-bit generator with one word of state, for audio noise
/// and random choices. Not for cryptographic use.
class rng {
public:
    /// Starts the sequence at @p seed.
    explicit rng(std::uint64_t seed = 0) noexcept : state_(seed) {}

    /// The seed for node @p nodeId in a project seeded with @p projectSeed.
    static std::uint64_t seedFor(std::uint64_t projectSeed, std::string_view nodeId) noexcept {
        std::uint64_t h = 0xcbf29ce484222325ull;  // FNV-1a
        for (char c : nodeId) h = (h ^ static_cast<unsigned char>(c)) * 0x100000001b3ull;
        return h ^ (projectSeed * 0x9e3779b97f4a7c15ull);
    }

    /// The next 64 random bits.
    std::uint64_t next() noexcept {
        std::uint64_t z = (state_ += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27U)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31U);
    }

    /// Uniform in [0, 1).
    float uniform() noexcept { return static_cast<float>(next() >> 40U) * (1.0f / 16777216.0f); }

    /// Uniform in [−1, 1).
    float bipolar() noexcept { return 2.0f * uniform() - 1.0f; }

private:
    std::uint64_t state_;
};

}  // namespace primitives
