/// @file
/// The modified Bessel function of the first kind of order 0, and the
/// Kaiser-windowed sinc it shapes: the band-limiting kernel of the reads
/// and writes of buffers (`samples.md` §6.5) and of the server's resampler
/// (§4.5.1).

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>

#include <espresso/primitives/realtime.h>

namespace primitives {

/// I0(@p x), the modified Bessel function of the first kind of order 0, by
/// its power series: the sum of (x / 2)^2k / (k!)^2 from k = 0, adding
/// terms while the last one added exceeds @p tolerance times the sum, and
/// at most 63 terms past the first. A Kaiser window of shape β is
/// I0(β √(1 − r²)) / I0(β) at r from −1 to 1.
constexpr double besselI0(double x, double tolerance) noexcept {
    double sum = 1.0;
    double term = 1.0;
    for (int k = 1; k < 64 && term > tolerance * sum; ++k) {
        const double half = x / (2.0 * k);
        term *= half * half;
        sum += term;
    }
    return sum;
}

/// A Kaiser-windowed sinc of @p Zeros zero crossings either side of its
/// centre, as a table of @p Resolution points a crossing of @p T values,
/// read by linear interpolation between points. Point i holds
/// sinc(u) × I0(β √(1 − (u / Zeros)²)) / I0(β) at u = i / Resolution
/// crossings, computed in double, and a 0 follows the last point. Made
/// where allocation is allowed; read on any thread.
template <typename T, int Zeros, int Resolution>
class kaiserSinc {
public:
    /// The last point: the kernel's reach, in points from its centre.
    static constexpr int reach = Zeros * Resolution;

    /// The kernel of window shape @p beta, whose series (besselI0) stops
    /// at terms @p tolerance times its sum.
    kaiserSinc(double beta, double tolerance) noexcept {
        const double norm = besselI0(beta, tolerance);
        for (int i = 0; i <= reach; ++i) {
            const double u = static_cast<double>(i) / Resolution;
            const double r = u / Zeros;
            const double sinc = u == 0.0 ? 1.0 : std::sin(std::numbers::pi * u) / (std::numbers::pi * u);
            table_[static_cast<std::size_t>(i)] =
                static_cast<T>(sinc * besselI0(beta * std::sqrt(1.0 - (r * r)), tolerance) / norm);
        }
    }

    /// The kernel at @p p points from its centre, from 0 to `reach` + 1.
    T at(T p) const noexcept ESPRESSO_NONBLOCKING {
        const int i = std::min(static_cast<int>(p), reach);  // p >= 0; int converts in one instruction
        const T frac = p - static_cast<T>(i);
        const auto k = static_cast<std::size_t>(i);
        return table_[k] + (frac * (table_[k + 1] - table_[k]));
    }

private:
    std::array<T, static_cast<std::size_t>(reach) + 2> table_{};
};

}  // namespace primitives
