/// @file
/// Parameter and port tables for every primitive type
/// (`primitives.md` §7.4.3). The tables are data: the interpreter checks
/// arguments against them, the IR validator checks nodes against them,
/// and the engine reads defaults from them.

#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include <proj/metadata/graph.h>

namespace metadata {

/// What a parameter's value means (`primitives.md` §7.4.1).
enum class dimension : std::uint8_t {
    number,     ///< A plain number without a unit
    time,       ///< Seconds
    frequency,  ///< Hertz
    level,      ///< Linear amplitude
    interval,   ///< Semitones
    beats,      ///< Musical time, in beats
    choice,     ///< One name from a fixed list
    text,       ///< Free text, such as a device name or an OSC address
    list,       ///< A list of numbers
};

/// Lowercase name of @p d, for messages.
std::string_view toString(dimension d) noexcept;

/// A set of pipe kinds, as bit flags.
using kindSet = std::uint8_t;

/// The flag for @p kind.
constexpr kindSet bit(pipeKind kind) noexcept {
    return static_cast<kindSet>(1u << static_cast<unsigned>(kind));
}

/// No pipe kind: the parameter is static only.
inline constexpr kindSet staticOnly = 0;
/// Audio pipes.
inline constexpr kindSet audioPipes = bit(pipeKind::audio);
/// Control pipes.
inline constexpr kindSet controlPipes = bit(pipeKind::control);
/// Event pipes.
inline constexpr kindSet eventPipes = bit(pipeKind::event);
/// Audio or control pipes: a signal.
inline constexpr kindSet signalPipes = audioPipes | controlPipes;

/// True when @p set contains @p kind.
constexpr bool contains(kindSet set, pipeKind kind) noexcept { return (set & bit(kind)) != 0; }

/// One parameter of a primitive type.
struct paramSpec {
    /// Name, in kebab-case: `freq`, `max-time`.
    std::string_view name;
    /// Dimension of the value.
    dimension dim = dimension::number;
    /// Default, in canonical units, for numeric parameters.
    double defaultValue = 0.0;
    /// Smallest accepted static value.
    double min = -1e9;
    /// Largest accepted static value.
    double max = 1e9;
    /// Pipe kinds the parameter may be bound to.
    kindSet bindable = staticOnly;
    /// Accepted names, for `dimension::choice`.
    std::span<const std::string_view> choices = {};
    /// Default, for choice and text parameters.
    std::string_view defaultText = {};
};

/// One input or output port of a primitive type.
struct portSpec {
    /// Name: `in`, `out`, `gate`.
    std::string_view name;
    /// Pipe kinds the port accepts.
    kindSet kinds = audioPipes;
    /// True when the port must be bound.
    bool required = false;
    /// True when the port accepts more than one pipe.
    bool multiple = false;
};

/// What a primitive does with external data (`primitives.md` §7).
enum class role : std::uint8_t {
    source,     ///< Moves external data into pipes
    sink,       ///< Moves pipe data out of the graph
    generator,  ///< Produces signals from parameters and pipes
};

/// The table entry for one primitive type.
struct primitiveSpec {
    /// IR type name: `sineOsc`.
    std::string_view type;
    /// Source, sink, or generator.
    role kind = role::generator;
    /// One-line description.
    std::string_view summary;
    /// Parameters.
    std::span<const paramSpec> params;
    /// Input ports.
    std::span<const portSpec> inputs;
    /// Output ports.
    std::span<const portSpec> outputs;

    /// The parameter named @p name, or null.
    const paramSpec* param(std::string_view name) const noexcept;
    /// The input port named @p name, or null.
    const portSpec* input(std::string_view name) const noexcept;
    /// The output port named @p name, or null.
    const portSpec* output(std::string_view name) const noexcept;
};

/// Every primitive type, grouped as in `primitives.md` §7.
std::span<const primitiveSpec> primitiveSpecs() noexcept;

/// The entry for @p type, or null.
const primitiveSpec* findPrimitive(std::string_view type) noexcept;

} // namespace metadata
