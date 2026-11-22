#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <espresso/protocol/osc.h>

namespace protocol {

namespace {

// An overload set for std::visit: every alternative of `argument` needs a
// case, or the call does not compile.
template <typename... F>
struct overloaded : F... {
    using F::operator()...;
};
template <typename... F>
overloaded(F...) -> overloaded<F...>;

// Appends @p v in big-endian byte order, most significant byte first.
template <typename U>
    requires std::is_unsigned_v<U>
void putBigEndian(std::vector<std::uint8_t>& out, U v) {
    for (std::size_t byte = sizeof(U); byte-- > 0;)
        out.push_back(static_cast<std::uint8_t>(v >> (8u * byte)));
}

void putU32(std::vector<std::uint8_t>& out, std::uint32_t v) { putBigEndian(out, v); }

void putU64(std::vector<std::uint8_t>& out, std::uint64_t v) { putBigEndian(out, v); }

void pad(std::vector<std::uint8_t>& out) {
    while (out.size() % 4 != 0) out.push_back(0);
}

void putString(std::vector<std::uint8_t>& out, std::string_view s) {
    out.insert(out.end(), s.begin(), s.end());
    out.push_back(0);
    pad(out);
}

// Reads a packet's items in order. By the lenient rules, padding is
// skipped without reading it, so the position may pass the end; every
// read checks that its bytes are there.
class reader {
public:
    reader(std::span<const std::uint8_t> data, oscRules rules) : data_(data), rules_(rules) {}

    bool u32(std::uint32_t& v) {
        if (!has(4)) return false;
        v = 0;
        for (int k = 0; k < 4; ++k) v = (v << 8u) | std::uint32_t{data_[pos_++]};
        return true;
    }

    bool u64(std::uint64_t& v) {
        if (!has(8)) return false;
        v = 0;
        for (int k = 0; k < 8; ++k) v = (v << 8u) | std::uint64_t{data_[pos_++]};
        return true;
    }

    bool string(std::string& s) {
        const std::size_t start = pos_;
        while (pos_ < data_.size() && data_[pos_] != 0) ++pos_;
        if (pos_ >= data_.size()) return false;  // no terminator
        s.assign(data_.begin() + static_cast<std::ptrdiff_t>(start), data_.begin() + static_cast<std::ptrdiff_t>(pos_));
        ++pos_;
        return skipPadding();
    }

    bool bytes(std::size_t n, std::vector<std::uint8_t>& out) {
        if (!has(n)) return false;
        out.assign(data_.begin() + static_cast<std::ptrdiff_t>(pos_),
                   data_.begin() + static_cast<std::ptrdiff_t>(pos_ + n));
        pos_ += n;
        return skipPadding();
    }

    bool atEnd() const noexcept { return pos_ == data_.size(); }

private:
    // True when @p n bytes are left from the position.
    bool has(std::size_t n) const noexcept { return pos_ <= data_.size() && data_.size() - pos_ >= n; }

    bool skipPadding() {
        while (pos_ % 4 != 0) {
            if (rules_ == oscRules::control && (pos_ >= data_.size() || data_[pos_] != 0)) return false;
            ++pos_;
        }
        return true;
    }

    std::span<const std::uint8_t> data_;
    oscRules rules_;
    std::size_t pos_ = 0;
};

// The four bytes of @p v, most significant first.
std::array<std::uint8_t, 4> bytesOf(std::uint32_t v) noexcept {
    return {static_cast<std::uint8_t>(v >> 24u), static_cast<std::uint8_t>(v >> 16u),
            static_cast<std::uint8_t>(v >> 8u), static_cast<std::uint8_t>(v)};
}

// True for the type tags of the client and server's own messages
// (protocol.md §2.2).
bool controlTag(char tag) noexcept { return std::string_view("ihfdsb").find(tag) != std::string_view::npos; }

} // namespace

char typeTag(const argument& arg) noexcept {
    return std::visit(overloaded{
                          [](std::int32_t) { return 'i'; },
                          [](std::int64_t) { return 'h'; },
                          [](float) { return 'f'; },
                          [](double) { return 'd'; },
                          [](const std::string&) { return 's'; },
                          [](const blob&) { return 'b'; },
                          [](bool v) { return v ? 'T' : 'F'; },
                          [](nil) { return 'N'; },
                          [](impulse) { return 'I'; },
                          [](character) { return 'c'; },
                          [](rgba) { return 'r'; },
                          [](const midiMessage&) { return 'm'; },
                          [](timeTag) { return 't'; },
                          [](const symbol&) { return 'S'; },
                      },
                      arg);
}

std::string message::tags() const {
    std::string out;
    for (const auto& a : args) out.push_back(typeTag(a));
    return out;
}

namespace {

// Argument @p i when it holds a T.
template <typename T>
bool holds(const std::vector<argument>& args, std::size_t i) {
    return i < args.size() && std::holds_alternative<T>(args[i]);
}

} // namespace

std::optional<std::int32_t> message::int32(std::size_t i) const {
    if (holds<std::int32_t>(args, i)) return std::get<std::int32_t>(args[i]);
    return std::nullopt;
}

std::optional<std::int64_t> message::int64(std::size_t i) const {
    if (holds<std::int64_t>(args, i)) return std::get<std::int64_t>(args[i]);
    if (holds<std::int32_t>(args, i)) return std::get<std::int32_t>(args[i]);
    return std::nullopt;
}

std::optional<float> message::float32(std::size_t i) const {
    if (holds<float>(args, i)) return std::get<float>(args[i]);
    if (holds<double>(args, i)) return static_cast<float>(std::get<double>(args[i]));
    return std::nullopt;
}

std::optional<double> message::float64(std::size_t i) const {
    if (holds<double>(args, i)) return std::get<double>(args[i]);
    if (holds<float>(args, i)) return std::get<float>(args[i]);
    return std::nullopt;
}

std::optional<std::string_view> message::string(std::size_t i) const {
    if (holds<std::string>(args, i)) return std::string_view(std::get<std::string>(args[i]));
    return std::nullopt;
}

std::optional<std::span<const std::uint8_t>> message::bytes(std::size_t i) const {
    if (holds<blob>(args, i)) return std::span<const std::uint8_t>(std::get<blob>(args[i]).bytes);
    return std::nullopt;
}

std::vector<std::uint8_t> encode(const message& m) {
    std::vector<std::uint8_t> out;
    putString(out, m.address);
    putString(out, "," + m.tags());
    for (const auto& a : m.args) {
        std::visit(overloaded{
                       [&](std::int32_t v) { putU32(out, static_cast<std::uint32_t>(v)); },
                       [&](std::int64_t v) { putU64(out, static_cast<std::uint64_t>(v)); },
                       [&](float v) { putU32(out, std::bit_cast<std::uint32_t>(v)); },
                       [&](double v) { putU64(out, std::bit_cast<std::uint64_t>(v)); },
                       [&](const std::string& v) { putString(out, v); },
                       [&](const blob& v) {
                           putU32(out, static_cast<std::uint32_t>(v.bytes.size()));
                           out.insert(out.end(), v.bytes.begin(), v.bytes.end());
                           pad(out);
                       },
                       [](bool) {},  // T and F carry no bytes, nor do N and I
                       [](nil) {},
                       [](impulse) {},
                       [&](character v) { putU32(out, v.code); },
                       [&](rgba v) { putU32(out, v.value); },
                       [&](const midiMessage& v) { out.insert(out.end(), v.bytes.begin(), v.bytes.end()); },
                       [&](timeTag v) { putU64(out, v.value); },
                       [&](const symbol& v) { putString(out, v.name); },
                   },
                   a);
    }
    return out;
}

std::optional<message> decode(std::span<const std::uint8_t> packet, oscRules rules) {
    const bool control = rules == oscRules::control;
    if (control && (packet.empty() || packet.size() % 4 != 0)) return std::nullopt;
    reader r(packet, rules);
    message m;
    std::string tags;
    if (!r.string(m.address) || !m.address.starts_with('/')) return std::nullopt;
    if (!r.string(tags)) {
        if (control) return std::nullopt;
        // OSC 1.0 asks receivers to accept a message whose sender left out
        // the type tag string: it has no arguments.
        return m;
    }
    if (!tags.starts_with(',')) return std::nullopt;
    for (const char tag : std::string_view(tags).substr(1)) {
        if (control && !controlTag(tag)) return std::nullopt;
        switch (tag) {
        case 'i': {
            std::uint32_t v = 0;
            if (!r.u32(v)) return std::nullopt;
            m.args.emplace_back(static_cast<std::int32_t>(v));
            break;
        }
        case 'h': {
            std::uint64_t v = 0;
            if (!r.u64(v)) return std::nullopt;
            m.args.emplace_back(static_cast<std::int64_t>(v));
            break;
        }
        case 'f': {
            std::uint32_t v = 0;
            if (!r.u32(v)) return std::nullopt;
            m.args.emplace_back(std::bit_cast<float>(v));
            break;
        }
        case 'd': {
            std::uint64_t v = 0;
            if (!r.u64(v)) return std::nullopt;
            m.args.emplace_back(std::bit_cast<double>(v));
            break;
        }
        case 's': {
            std::string s;
            if (!r.string(s)) return std::nullopt;
            m.args.emplace_back(std::move(s));
            break;
        }
        case 'b': {
            std::uint32_t n = 0;
            blob b;
            if (!r.u32(n) || !r.bytes(n, b.bytes)) return std::nullopt;
            m.args.emplace_back(std::move(b));
            break;
        }
        case 'T': m.args.emplace_back(true); break;
        case 'F': m.args.emplace_back(false); break;
        case 'N': m.args.emplace_back(nil{}); break;
        case 'I': m.args.emplace_back(impulse{}); break;
        case 'c': {
            std::uint32_t v = 0;
            if (!r.u32(v)) return std::nullopt;
            m.args.emplace_back(character{v});
            break;
        }
        case 'r': {
            std::uint32_t v = 0;
            if (!r.u32(v)) return std::nullopt;
            m.args.emplace_back(rgba{v});
            break;
        }
        case 'm': {
            std::uint32_t v = 0;
            if (!r.u32(v)) return std::nullopt;
            m.args.emplace_back(midiMessage{bytesOf(v)});
            break;
        }
        case 't': {
            std::uint64_t v = 0;
            if (!r.u64(v)) return std::nullopt;
            m.args.emplace_back(timeTag{v});
            break;
        }
        case 'S': {
            symbol v;
            if (!r.string(v.name)) return std::nullopt;
            m.args.emplace_back(std::move(v));
            break;
        }
        default: return std::nullopt;  // the size of an unknown type is unknown
        }
    }
    if (control && !r.atEnd()) return std::nullopt;
    return m;
}

std::string describe(const message& m) {
    std::ostringstream out;
    out << m.address;
    for (const auto& a : m.args) {
        out << ' ';
        std::visit(overloaded{
                       [&](std::int32_t v) { out << v; },
                       [&](std::int64_t v) { out << v; },
                       [&](float v) { out << v; },
                       [&](double v) { out << v; },
                       [&](const std::string& v) { out << '"' << v << '"'; },
                       [&](const blob& v) { out << "<blob " << v.bytes.size() << " bytes>"; },
                       [&](bool v) { out << (v ? "true" : "false"); },
                       [&](nil) { out << "<nil>"; },
                       [&](impulse) { out << "<impulse>"; },
                       [&](character v) { out << "<char " << v.code << '>'; },
                       [&](rgba v) { out << "<rgba " << v.value << '>'; },
                       [&](const midiMessage& v) {
                           out << "<midi";
                           for (const auto b : v.bytes) out << ' ' << unsigned{b};
                           out << '>';
                       },
                       [&](timeTag v) { out << "<time " << v.value << '>'; },
                       [&](const symbol& v) { out << "<symbol " << v.name << '>'; },
                   },
                   a);
    }
    return out.str();
}

} // namespace protocol
