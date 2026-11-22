/// @file
/// The modified Bessel function of the first kind of order 0, which shapes
/// the Kaiser windows of the band-limiting kernels: the reads and writes of
/// buffers (`samples.md` §6.5) and the server's resampler (§4.5.1).

#pragma once

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

}  // namespace primitives
