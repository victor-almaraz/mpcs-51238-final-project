// Benchmarks that alternate their configurations round by round, and the
// median and range of each configuration's figures (threads-review G5,
// PF-F15; lessons.md "A data-type change is benchmarked before and after").

#pragma once

#include <algorithm>
#include <cstddef>
#include <ostream>
#include <vector>

namespace support {

/// Runs @p count configurations for @p rounds rounds, each in a new order:
/// the list rotated by a share of its length each round, so that every
/// configuration runs at a different point of each round and no
/// configuration always runs first. Calls @p run(round, configuration).
template <typename Run>
void inRotation(std::size_t count, std::size_t rounds, Run run) {
    for (std::size_t r = 0; r < rounds; ++r) {
        const std::size_t offset = rounds > 0 ? r * count / rounds : 0;
        for (std::size_t i = 0; i < count; ++i) run(r, (i + offset) % count);
    }
}

/// The median, least, and greatest of a set of figures.
struct spread {
    double median = 0.0;
    double least = 0.0;
    double greatest = 0.0;
};

/// The spread of @p values; zeros when there are none. An even count's
/// median is the mean of the middle two.
inline spread spreadOf(std::vector<double> values) {
    if (values.empty()) return {};
    std::ranges::sort(values);
    const std::size_t n = values.size();
    const double median = n % 2 == 1 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2.0;
    return {.median = median, .least = values.front(), .greatest = values.back()};
}

/// Writes @p s as "median (least to greatest)".
inline std::ostream& operator<<(std::ostream& out, const spread& s) {
    return out << s.median << " (" << s.least << " to " << s.greatest << ")";
}

}  // namespace support
