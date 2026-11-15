/// @file
/// Parameter and port tables for every primitive type
/// (`primitives.md` §7.4.3). The tables are data: the interpreter checks
/// arguments against them, the IR validator checks nodes against them,
/// and the engine reads defaults from them.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

/// No text: the default of an optional text field.
inline constexpr std::string_view noText{};
/// No names: the default of an optional list of names.
inline constexpr std::span<const std::string_view> noNames{};

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
    std::span<const std::string_view> choices = noNames;
    /// Default, for choice and text parameters.
    std::string_view defaultText = noText;
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

/// How a mapper operation (`primitives.md` §7.9) treats the dimensions of
/// its operands: the input, then the parameters in table order.
enum class operands : std::uint8_t {
    none,    ///< Not a mapper operation
    plain,   ///< Every operand is a plain number: exp, log, sin, cos, tanh, pow
    keep,    ///< The result has the input's dimension: floor, round
    sign,    ///< As `keep`, but a level is ambiguous: neg, abs
    same,    ///< Every operand has the input's dimension, as the result does: min, max, clamp, quantize
    ranges,  ///< The input shares a dimension with the first two parameters; the last two share the result's:
             ///< scale, scaleExp
};

/// What a constraint requires of a node (`constraint`).
enum class rule : std::uint8_t {
    anyInput,      ///< At least one of the input ports `ports` is bound
    anyOutput,     ///< At least one of the output ports `ports` is bound
    entryPerPipe,  ///< List parameter `param`, when it has entries, has one per pipe of input `ports[0]`
    notEmpty,      ///< List parameter `param` has an entry; an absent list is empty
    channels,      ///< Output `ports[0]`, when bound, has a pipe of at least `count` channels
    audioWith,     ///< When input `ports[0]` is bound, output `ports[1]` is an audio pipe
    textWith,      ///< When input `ports[0]` is bound, text parameter `param` is not empty
};

/// A requirement that spans parameters and ports, which the engine's
/// builder relies on.
struct constraint {
    /// What is required.
    rule kind = rule::anyInput;
    /// The parameter the rule names, if any.
    std::string_view param = noText;
    /// The ports the rule names.
    std::span<const std::string_view> ports = noNames;
    /// The channel count, for `rule::channels`.
    int count = 0;
};

/// No constraints: the default of `primitiveSpec::constraints`.
inline constexpr std::span<const constraint> noConstraints{};

/// The table entry for one primitive type.
struct primitiveSpec {
    /// IR type name: `sineOsc`.
    std::string_view type;
    /// Source, sink, or generator.
    role kind = role::generator;
    /// Parameters.
    std::span<const paramSpec> params;
    /// Input ports.
    std::span<const portSpec> inputs;
    /// Output ports.
    std::span<const portSpec> outputs;
    /// Requirements across parameters and ports.
    std::span<const constraint> constraints = noConstraints;
    /// Whether the type is a mapper operation, and how it treats dimensions.
    /// A mapper operation reads `in` and takes its operands as its
    /// parameters, in order.
    operands mapper = operands::none;

    /// The parameter named @p name, if there is one.
    optionalRef<paramSpec> param(std::string_view name) const noexcept;
    /// The input port named @p name, if there is one.
    optionalRef<portSpec> input(std::string_view name) const noexcept;
    /// The output port named @p name, if there is one.
    optionalRef<portSpec> output(std::string_view name) const noexcept;
};

/// Every primitive type, grouped as in `primitives.md` §7.
std::span<const primitiveSpec> primitiveSpecs() noexcept;

/// The entry for @p type, if there is one.
optionalRef<primitiveSpec> findPrimitive(std::string_view type) noexcept;

/// How constraint messages name parameters and ports.
enum class naming : std::uint8_t {
    ir,        ///< `parameter levels`, `input in`
    language,  ///< `:levels`, `:in`
};

/// A message for each constraint of @p spec that @p node, a node of
/// @p graph, breaks. Channel counts come from the graph's pipe
/// declarations; an undeclared pipe is not checked.
std::vector<std::string> checkConstraints(const primitiveSpec& spec, const nodeSpec& node,
                                          const graphSpec& graph, naming names);

} // namespace metadata
