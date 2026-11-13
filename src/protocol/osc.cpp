#include <proj/protocol/osc.h>

#include <bit>
#include <cstring>
#include <sstream>

namespace protocol {

namespace {

void putU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(v >> shift));
}

void putU64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int shift = 56; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(v >> shift));
}

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
        for (int k = 0; k < 4; ++k) v = (v << 8) | data_[pos_++];
        return true;
    }

    bool u64(std::uint64_t& v) {
        if (data_.size() - pos_ < 8) return false;
        v = 0;
        for (int k = 0; k < 8; ++k) v = (v << 8) | data_[pos_++];
        return true;
    }

    bool string(std::string& s) {
        const std::size_t start = pos_;
        while (pos_ < data_.size() && data_[pos_] != 0) ++pos_;
        if (pos_ >= data_.size()) return false;  // no terminator
        s.assign(reinterpret_cast<const char*>(data_.data() + start), pos_ - start);
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
    static constexpr char tags[] = {'i', 'h', 'f', 'd', 's', 'b'};
    return tags[arg.index()];
}

std::string message::tags() const {
    std::string out;
    for (const auto& a : args) out.push_back(typeTag(a));
    return out;
}

std::optional<std::int32_t> message::int32(std::size_t i) const {
    if (i >= args.size()) return std::nullopt;
    if (const auto* v = std::get_if<std::int32_t>(&args[i])) return *v;
    return std::nullopt;
}

std::optional<std::int64_t> message::int64(std::size_t i) const {
    if (i >= args.size()) return std::nullopt;
    if (const auto* v = std::get_if<std::int64_t>(&args[i])) return *v;
    if (const auto* v = std::get_if<std::int32_t>(&args[i])) return *v;
    return std::nullopt;
}

std::optional<float> message::float32(std::size_t i) const {
    if (i >= args.size()) return std::nullopt;
    if (const auto* v = std::get_if<float>(&args[i])) return *v;
    if (const auto* v = std::get_if<double>(&args[i])) return static_cast<float>(*v);
    return std::nullopt;
}

std::optional<std::string_view> message::string(std::size_t i) const {
    if (i >= args.size()) return std::nullopt;
    if (const auto* v = std::get_if<std::string>(&args[i])) return std::string_view(*v);
    return std::nullopt;
}

const blob* message::bytes(std::size_t i) const {
    return i < args.size() ? std::get_if<blob>(&args[i]) : nullptr;
}

std::vector<std::uint8_t> encode(const message& m) {
    std::vector<std::uint8_t> out;
    putString(out, m.address);
    putString(out, "," + m.tags());
    for (const auto& a : m.args) {
        switch (a.index()) {
        case 0: putU32(out, static_cast<std::uint32_t>(std::get<std::int32_t>(a))); break;
        case 1: putU64(out, static_cast<std::uint64_t>(std::get<std::int64_t>(a))); break;
        case 2: putU32(out, std::bit_cast<std::uint32_t>(std::get<float>(a))); break;
        case 3: putU64(out, std::bit_cast<std::uint64_t>(std::get<double>(a))); break;
        case 4: putString(out, std::get<std::string>(a)); break;
        case 5: {
            const auto& b = std::get<blob>(a).bytes;
            putU32(out, static_cast<std::uint32_t>(b.size()));
            out.insert(out.end(), b.begin(), b.end());
            pad(out);
            break;
        }
        default: break;
        }
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
        switch (a.index()) {
        case 0: out << std::get<std::int32_t>(a); break;
        case 1: out << std::get<std::int64_t>(a); break;
        case 2: out << std::get<float>(a); break;
        case 3: out << std::get<double>(a); break;
        case 4: out << '"' << std::get<std::string>(a) << '"'; break;
        case 5: out << "<blob " << std::get<blob>(a).bytes.size() << " bytes>"; break;
        default: break;
        }
    }
    return out.str();
}

} // namespace protocol
