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

#include <proj/protocol/osc.h>

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

class reader {
public:
    explicit reader(std::span<const std::uint8_t> data) : data_(data) {}

    bool u32(std::uint32_t& v) {
        if (data_.size() - pos_ < 4) return false;
        v = 0;
        for (int k = 0; k < 4; ++k) v = (v << 8u) | std::uint32_t{data_[pos_++]};
        return true;
    }

    bool u64(std::uint64_t& v) {
        if (data_.size() - pos_ < 8) return false;
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
        if (data_.size() - pos_ < n) return false;
        out.assign(data_.begin() + static_cast<std::ptrdiff_t>(pos_),
                   data_.begin() + static_cast<std::ptrdiff_t>(pos_ + n));
        pos_ += n;
        return skipPadding();
    }

    bool atEnd() const noexcept { return pos_ == data_.size(); }

private:
    bool skipPadding() {
        while (pos_ % 4 != 0) {
            if (pos_ >= data_.size() || data_[pos_] != 0) return false;
            ++pos_;
        }
        return true;
    }

    std::span<const std::uint8_t> data_;
    std::size_t pos_ = 0;
};

} // namespace

char typeTag(const argument& arg) noexcept {
    return std::visit(overloaded{
                          [](std::int32_t) { return 'i'; },
                          [](std::int64_t) { return 'h'; },
                          [](float) { return 'f'; },
                          [](double) { return 'd'; },
                          [](const std::string&) { return 's'; },
                          [](const blob&) { return 'b'; },
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
                   },
                   a);
    }
    return out;
}

std::optional<message> decode(std::span<const std::uint8_t> packet) {
    if (packet.empty() || packet.size() % 4 != 0) return std::nullopt;
    reader r(packet);
    message m;
    std::string tags;
    if (!r.string(m.address) || m.address.empty() || m.address[0] != '/') return std::nullopt;
    if (!r.string(tags) || tags.empty() || tags[0] != ',') return std::nullopt;
    for (std::size_t i = 1; i < tags.size(); ++i) {
        switch (tags[i]) {
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
        default:
            return std::nullopt;
        }
    }
    if (!r.atEnd()) return std::nullopt;
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
                   },
                   a);
    }
    return out.str();
}

} // namespace protocol
