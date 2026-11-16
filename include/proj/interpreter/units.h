/// @file
/// Conversion of unit literals to canonical units (`interpreter.md` §8,
/// `grammar.md` §5.1).

#pragma once

#include <proj/interpreter/reader.h>
#include <proj/metadata/primitiveSpec.h>

namespace interpreter {

/// A value in the canonical unit of its dimension.
struct canonicalQuantity {
    /// Seconds, beats, hertz, linear amplitude, or semitones.
    double value = 0.0;
    /// time, beats, frequency, level, or interval.
    metadata::dimension dim = metadata::dimension::number;
};

/// Converts @p q. `smp` uses @p sampleRate; `bar` is 4 beats; `db` is
/// converted to linear amplitude.
canonicalQuantity canonical(const quantityLiteral& q, double sampleRate = 48000.0) noexcept;

/// Linear amplitude @p amplitude, above 0, in decibels: the inverse of
/// `canonical` for `db`.
double decibels(double amplitude) noexcept;

} // namespace interpreter
