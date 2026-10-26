// xpute.cpp
//
// GENERATED from cpp/xpute/ by `make -C cpp amalgamation` (cpp/amalgamation.awk) — do not edit.
//
// Every source of xpute's C++ library, as one translation unit over xpute.hpp.

#include "xpute.hpp"

// cpp/xpute/kit/codec/encoding.cpp

namespace xpute {

namespace {

constexpr char HEX[] = "0123456789abcdef";
constexpr char B64URL[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int b64url_value(char c) noexcept {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

} // namespace

std::size_t hex_encode(std::span<const std::uint8_t> in, std::span<char> out) noexcept {
    ensure(out.size() >= hex_len(in.size()), Errno::enospc, out.size(), in.size());
    std::size_t at = 0;
    for (std::uint8_t b : in) {
        out[at++] = HEX[b >> 4];
        out[at++] = HEX[b & 0xf];
    }
    return at;
}

Result<std::size_t> hex_decode(std::string_view in, std::span<std::uint8_t> out) noexcept {
    if (in.size() % 2 != 0) return marshal_error(Errno::ebadmsg);
    ensure(out.size() >= in.size() / 2, Errno::enospc, out.size(), in.size());
    for (std::size_t i = 0; i < in.size(); i += 2) {
        int hi = hex_value(in[i]);
        int lo = hex_value(in[i + 1]);
        if (hi < 0 || lo < 0) return marshal_error(Errno::ebadmsg);
        out[i / 2] = static_cast<std::uint8_t>(hi << 4 | lo);
    }
    return in.size() / 2;
}

std::size_t base64url_encode(std::span<const std::uint8_t> in, std::span<char> out) noexcept {
    ensure(out.size() >= base64url_len(in.size()), Errno::enospc, out.size(), in.size());
    std::size_t at = 0;
    std::size_t i = 0;
    for (; i + 3 <= in.size(); i += 3) {
        std::uint32_t w = std::uint32_t{in[i]} << 16 | std::uint32_t{in[i + 1]} << 8 | in[i + 2];
        out[at++] = B64URL[w >> 18 & 63];
        out[at++] = B64URL[w >> 12 & 63];
        out[at++] = B64URL[w >> 6 & 63];
        out[at++] = B64URL[w & 63];
    }
    std::size_t rest = in.size() - i;
    if (rest > 0) {
        std::uint32_t w = std::uint32_t{in[i]} << 16 | (rest == 2 ? std::uint32_t{in[i + 1]} << 8 : 0);
        out[at++] = B64URL[w >> 18 & 63];
        out[at++] = B64URL[w >> 12 & 63];
        if (rest == 2) out[at++] = B64URL[w >> 6 & 63];
    }
    return at;
}

Result<std::size_t> base64url_decode(std::string_view in, std::span<std::uint8_t> out) noexcept {
    // A final group of one character carries six bits, which is no whole
    // byte: no encoding ends that way.
    if (in.size() % 4 == 1) return marshal_error(Errno::ebadmsg);
    std::size_t bytes = in.size() / 4 * 3 + (in.size() % 4 == 0 ? 0 : in.size() % 4 - 1);
    ensure(out.size() >= bytes, Errno::enospc, out.size(), bytes);
    std::size_t at = 0;
    for (std::size_t i = 0; i < in.size(); i += 4) {
        std::size_t n = in.size() - i < 4 ? in.size() - i : 4;
        std::uint32_t w = 0;
        for (std::size_t k = 0; k < 4; k++) {
            int v = k < n ? b64url_value(in[i + k]) : 0;
            if (v < 0) return marshal_error(Errno::ebadmsg);
            w = w << 6 | static_cast<std::uint32_t>(v);
        }
        // Bits past the last byte are zero in any canonical encoding.
        if ((n == 2 && (w & 0xffff) != 0) || (n == 3 && (w & 0xff) != 0)) return marshal_error(Errno::ebadmsg);
        out[at++] = static_cast<std::uint8_t>(w >> 16);
        if (n > 2) out[at++] = static_cast<std::uint8_t>(w >> 8);
        if (n > 3) out[at++] = static_cast<std::uint8_t>(w);
    }
    return at;
}

} // namespace xpute

// cpp/xpute/kit/codec/json.cpp

#include <charconv>

namespace xpute {

namespace json_detail {

std::size_t skip_ws(std::string_view s, std::size_t at) noexcept {
    while (at < s.size() && (s[at] == ' ' || s[at] == '\t' || s[at] == '\n' || s[at] == '\r')) at++;
    return at;
}

void push_utf8(std::uint32_t c, char *out, std::size_t &n) noexcept {
    if (c < 0x80) {
        out[n++] = static_cast<char>(c);
    } else if (c < 0x800) {
        out[n++] = static_cast<char>(0xc0 | (c >> 6));
        out[n++] = static_cast<char>(0x80 | (c & 0x3f));
    } else if (c < 0x10000) {
        out[n++] = static_cast<char>(0xe0 | (c >> 12));
        out[n++] = static_cast<char>(0x80 | ((c >> 6) & 0x3f));
        out[n++] = static_cast<char>(0x80 | (c & 0x3f));
    } else {
        out[n++] = static_cast<char>(0xf0 | (c >> 18));
        out[n++] = static_cast<char>(0x80 | ((c >> 12) & 0x3f));
        out[n++] = static_cast<char>(0x80 | ((c >> 6) & 0x3f));
        out[n++] = static_cast<char>(0x80 | (c & 0x3f));
    }
}

static bool is_hex(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

std::optional<std::uint32_t> hex4(std::string_view s, std::size_t at) noexcept {
    if (at + 4 > s.size()) return std::nullopt;
    std::uint32_t v = 0;
    for (std::size_t i = at; i < at + 4; i++) {
        char c = s[i];
        if (!is_hex(c)) return std::nullopt;
        v = v * 16 + static_cast<std::uint32_t>(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
    }
    return v;
}

} // namespace json_detail

using json_detail::hex4;
using json_detail::skip_ws;

// The check recurses, and a module's stack is fixed.
static constexpr std::uint32_t MAX_DEPTH = 128;

static std::optional<std::size_t> skip_string(std::string_view s, std::size_t at) noexcept {
    at++;
    for (;;) {
        if (at >= s.size()) return std::nullopt;
        auto b = static_cast<unsigned char>(s[at]);
        if (b == '"') return at + 1;
        if (b == '\\') {
            if (at + 1 >= s.size()) return std::nullopt;
            switch (s[at + 1]) {
            case '"': case '\\': case '/': case 'b': case 'f': case 'n': case 'r': case 't': at += 2; break;
            case 'u':
                if (!hex4(s, at + 2)) return std::nullopt;
                at += 6;
                break;
            default: return std::nullopt;
            }
            continue;
        }
        if (b < 0x20) return std::nullopt;
        // A UTF-8 continuation byte is never a quote or a backslash.
        at++;
    }
}

// By JSON's grammar: `+1`, `.5`, `1.` and `01` are refused, as `JSON.parse`
// refuses them.
static std::optional<std::size_t> skip_number(std::string_view s, std::size_t at) noexcept {
    auto digits = [&](std::size_t from) {
        std::size_t i = from;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
        return i - from;
    };
    if (at < s.size() && s[at] == '-') at++;
    std::size_t n = digits(at);
    if (n == 0 || (n > 1 && s[at] == '0')) return std::nullopt;
    at += n;
    if (at < s.size() && s[at] == '.') {
        n = digits(at + 1);
        if (n == 0) return std::nullopt;
        at += 1 + n;
    }
    if (at < s.size() && (s[at] == 'e' || s[at] == 'E')) {
        at++;
        if (at < s.size() && (s[at] == '+' || s[at] == '-')) at++;
        n = digits(at);
        if (n == 0) return std::nullopt;
        at += n;
    }
    return at;
}

static std::optional<std::size_t> skip_value(std::string_view s, std::size_t at, std::uint32_t depth) noexcept {
    if (depth > MAX_DEPTH || at >= s.size()) return std::nullopt;
    auto literal = [&](std::string_view word) -> std::optional<std::size_t> {
        if (s.substr(at, word.size()) == word) return at + word.size();
        return std::nullopt;
    };
    switch (s[at]) {
    case 'n': return literal("null");
    case 't': return literal("true");
    case 'f': return literal("false");
    case '"': return skip_string(s, at);
    case '[': {
        std::size_t i = skip_ws(s, at + 1);
        if (i < s.size() && s[i] == ']') return i + 1;
        for (;;) {
            std::optional<std::size_t> end = skip_value(s, i, depth + 1);
            if (!end) return std::nullopt;
            i = skip_ws(s, *end);
            if (i >= s.size()) return std::nullopt;
            if (s[i] == ',') i = skip_ws(s, i + 1);
            else if (s[i] == ']') return i + 1;
            else return std::nullopt;
        }
    }
    case '{': {
        std::size_t i = skip_ws(s, at + 1);
        if (i < s.size() && s[i] == '}') return i + 1;
        for (;;) {
            if (i >= s.size() || s[i] != '"') return std::nullopt;
            std::optional<std::size_t> key = skip_string(s, i);
            if (!key) return std::nullopt;
            i = skip_ws(s, *key);
            if (i >= s.size() || s[i] != ':') return std::nullopt;
            std::optional<std::size_t> end = skip_value(s, skip_ws(s, i + 1), depth + 1);
            if (!end) return std::nullopt;
            i = skip_ws(s, *end);
            if (i >= s.size()) return std::nullopt;
            if (s[i] == ',') i = skip_ws(s, i + 1);
            else if (s[i] == '}') return i + 1;
            else return std::nullopt;
        }
    }
    default: return skip_number(s, at);
    }
}

std::optional<JsonValue> read_json(std::string_view text) noexcept {
    std::size_t start = skip_ws(text, 0);
    std::optional<std::size_t> end = skip_value(text, start, 0);
    if (!end || skip_ws(text, *end) != text.size()) return std::nullopt;
    return JsonValue(text.substr(start, *end - start));
}

JsonType JsonValue::type_of() const noexcept {
    switch (s_[0]) {
    case 'n': return JsonType::null;
    case 't': case 'f': return JsonType::boolean;
    case '"': return JsonType::string;
    case '[': return JsonType::array;
    case '{': return JsonType::object;
    default: return JsonType::number;
    }
}

std::optional<bool> JsonValue::as_bool() const noexcept {
    if (s_ == "true") return true;
    if (s_ == "false") return false;
    return std::nullopt;
}

// Past the doubles, JSON.parse and Rust's `parse` read a magnitude over the
// largest as an infinity and one under the least as a zero: which, by the
// place of the leading digit plus the exponent.
static double beyond_doubles(std::string_view t) noexcept {
    bool neg = t.front() == '-';
    std::size_t e = t.find_first_of("eE");
    std::string_view mant = t.substr(neg ? 1 : 0, e == std::string_view::npos ? std::string_view::npos : e - (neg ? 1 : 0));
    std::size_t point = std::min(mant.find('.'), mant.size());
    std::size_t lead = mant.find_first_not_of("0.");
    long place = static_cast<long>(point) - static_cast<long>(lead) - (lead < point ? 1 : 0);
    long exp = 0;
    if (e != std::string_view::npos) {
        std::size_t i = e + 1;
        bool exp_neg = t[i] == '-';
        if (t[i] == '-' || t[i] == '+') i++;
        // Clamped: past this, any exponent decides alone.
        for (; i < t.size(); i++) exp = std::min(exp * 10 + (t[i] - '0'), 100000L);
        if (exp_neg) exp = -exp;
    }
    double v = place + exp > 0 ? std::numeric_limits<double>::infinity() : 0.0;
    return neg ? -v : v;
}

std::optional<double> JsonValue::as_f64() const noexcept {
    if (type_of() != JsonType::number) return std::nullopt;
    std::string_view t = s_;
    double v = 0;
    auto [end, ec] = std::from_chars(t.data(), t.data() + t.size(), v);
    if (end != t.data() + t.size()) return std::nullopt;
    if (ec == std::errc::result_out_of_range) return beyond_doubles(t);
    if (ec != std::errc()) return std::nullopt;
    return v;
}

std::optional<JsonStr> JsonValue::as_str() const noexcept {
    if (type_of() != JsonType::string) return std::nullopt;
    return JsonStr(s_.substr(1, s_.size() - 2));
}

JsonItems JsonValue::items() const noexcept {
    return JsonItems(s_, type_of() == JsonType::array ? 1 : s_.size());
}

JsonFields JsonValue::fields() const noexcept {
    return JsonFields(s_, type_of() == JsonType::object ? 1 : s_.size());
}

std::optional<JsonValue> JsonValue::get(std::string_view key) const noexcept {
    std::optional<JsonValue> last;
    JsonFields f = fields();
    while (auto field = f.next()) {
        if (field->first.eq_str(key)) last = field->second;
    }
    return last;
}

std::optional<JsonValue> JsonValue::at(std::size_t index) const noexcept {
    JsonItems it = items();
    for (std::size_t i = 0;; i++) {
        std::optional<JsonValue> v = it.next();
        if (!v || i == index) return v;
    }
}

std::optional<JsonValue> JsonItems::next() noexcept {
    std::size_t start = skip_ws(s_, at_);
    if (start >= s_.size() || s_[start] == ']') {
        at_ = s_.size();
        return std::nullopt;
    }
    // The text was checked whole.
    std::size_t end = *skip_value(s_, start, 0);
    at_ = skip_ws(s_, end) + 1;
    return JsonValue(s_.substr(start, end - start));
}

std::optional<std::pair<JsonStr, JsonValue>> JsonFields::next() noexcept {
    std::size_t key_at = skip_ws(s_, at_);
    if (key_at >= s_.size() || s_[key_at] != '"') {
        at_ = s_.size();
        return std::nullopt;
    }
    std::size_t key_end = *skip_string(s_, key_at);
    std::size_t value_at = skip_ws(s_, skip_ws(s_, key_end) + 1);
    std::size_t value_end = *skip_value(s_, value_at, 0);
    at_ = skip_ws(s_, value_end) + 1;
    return std::pair{JsonStr(s_.substr(key_at + 1, key_end - key_at - 2)), JsonValue(s_.substr(value_at, value_end - value_at))};
}

std::optional<std::string_view> JsonStr::as_plain() const noexcept {
    if (raw_.find('\\') != std::string_view::npos) return std::nullopt;
    return raw_;
}

static char32_t utf8_next(std::string_view s, std::size_t &at) noexcept {
    auto b = static_cast<unsigned char>(s[at]);
    std::size_t len = b < 0x80 ? 1 : b < 0xe0 ? 2 : b < 0xf0 ? 3 : 4;
    char32_t c = len == 1 ? b : len == 2 ? (b & 0x1f) : len == 3 ? (b & 0x0f) : (b & 0x07);
    for (std::size_t i = 1; i < len && at + i < s.size(); i++) c = (c << 6) | (static_cast<unsigned char>(s[at + i]) & 0x3f);
    at += len;
    return c;
}

std::optional<char32_t> JsonChars::next() noexcept {
    if (rest_.empty()) return std::nullopt;
    std::size_t at = 0;
    char32_t c = utf8_next(rest_, at);
    if (c != U'\\') {
        rest_.remove_prefix(at);
        return c;
    }
    if (at >= rest_.size()) return std::nullopt;
    char32_t e = utf8_next(rest_, at);
    switch (e) {
    case U'b': c = U'\b'; break;
    case U'f': c = U'\f'; break;
    case U'n': c = U'\n'; break;
    case U'r': c = U'\r'; break;
    case U't': c = U'\t'; break;
    case U'u': {
        std::optional<std::uint32_t> hi = hex4(rest_, at);
        if (!hi) return std::nullopt;
        at += 4;
        std::uint32_t unit = *hi;
        if (*hi >= 0xd800 && *hi < 0xdc00 && rest_.substr(at, 2) == "\\u") {
            std::optional<std::uint32_t> lo = hex4(rest_, at + 2);
            if (lo && *lo >= 0xdc00 && *lo < 0xe000) {
                unit = 0x10000 + ((*hi - 0xd800) << 10) + (*lo - 0xdc00);
                at += 6;
            }
        }
        c = unit >= 0xd800 && unit < 0xe000 ? U'�' : static_cast<char32_t>(unit);
        break;
    }
    default: c = e;
    }
    rest_.remove_prefix(at);
    return c;
}

bool JsonStr::eq_str(std::string_view s) const noexcept {
    if (std::optional<std::string_view> plain = as_plain()) return *plain == s;
    JsonChars c = chars();
    std::size_t at = 0;
    for (;;) {
        std::optional<char32_t> a = c.next();
        if (!a) return at == s.size();
        if (at >= s.size() || utf8_next(s, at) != *a) return false;
    }
}

} // namespace xpute

// cpp/xpute/kit/status/bug.cpp

#include <atomic>

namespace xpute {

namespace {
std::atomic<Report> installed_report{nullptr};
}

void set_report(Report report) noexcept {
    Report none = nullptr;
    installed_report.compare_exchange_strong(none, report);
}

void bug(Errno errnum, std::uint64_t a, std::uint64_t b, std::source_location at) noexcept {
    if (Report report = installed_report.load()) report(errnum, at, a, b);
    std::abort();
}

} // namespace xpute

#ifdef __wasm__
// libc++'s own definition prints to stderr, which on wasm imports WASI's file
// calls, a second way to the host beside the contract.
void std::__libcpp_verbose_abort(char const *, ...) noexcept {
    xpute::bug(xpute::Errno::enotrecoverable);
}
#endif

// cpp/xpute/kit/wire/tlv.cpp

namespace xpute::tlv {

Result<std::optional<Value>> Reader::next() noexcept {
    if (end_) return std::optional<Value>();
    if (off_ >= buf_.size()) {
        end_ = End::unclosed;
        return std::optional<Value>();
    }
    auto tag = static_cast<Tag>(buf_[off_++]);
    auto scalar = [this]<class T>() -> Result<std::optional<Value>> {
        if (!has(sizeof(T))) return marshal_error(Errno::ebadmsg);
        return std::optional<Value>(le<T>());
    };
    auto run = [this]() -> Result<std::span<const std::uint8_t>> {
        if (!has(4)) return marshal_error(Errno::ebadmsg);
        std::uint32_t len = le<std::uint32_t>();
        if (!has(len)) return marshal_error(Errno::ebadmsg);
        std::span<const std::uint8_t> bytes = buf_.subspan(off_, len);
        off_ += len;
        return bytes;
    };
    switch (tag) {
    case Tag::end:
        end_ = End::closed;
        return std::optional<Value>();
    case Tag::boolean: {
        if (!has(1)) return marshal_error(Errno::ebadmsg);
        return std::optional<Value>(buf_[off_++] != 0);
    }
    case Tag::u8:
        return scalar.operator()<std::uint8_t>();
    case Tag::i8:
        return scalar.operator()<std::int8_t>();
    case Tag::u16:
        return scalar.operator()<std::uint16_t>();
    case Tag::i16:
        return scalar.operator()<std::int16_t>();
    case Tag::u32:
        return scalar.operator()<std::uint32_t>();
    case Tag::i32:
        return scalar.operator()<std::int32_t>();
    case Tag::u64:
        return scalar.operator()<std::uint64_t>();
    case Tag::i64:
        return scalar.operator()<std::int64_t>();
    case Tag::f32:
        return scalar.operator()<float>();
    case Tag::f64:
        return scalar.operator()<double>();
    case Tag::str: {
        Result<std::span<const std::uint8_t>> r = run();
        if (!r) return r.error();
        return std::optional<Value>(Str{std::string_view(reinterpret_cast<const char *>(r->data()), r->size())});
    }
    case Tag::bytes: {
        Result<std::span<const std::uint8_t>> r = run();
        if (!r) return r.error();
        return std::optional<Value>(Bytes{*r});
    }
    }
    return marshal_error(Errno::ebadmsg);
}

} // namespace xpute::tlv

// cpp/xpute/kit/wire/xtp/cursor.cpp

namespace xpute::xtp {

template <class T> Result<T> Cursor::get_as() const noexcept {
    Result<Value> v = get();
    if (!v) return v.error();
    if (const T *x = std::get_if<T>(&*v)) return *x;
    return marshal_error(Errno::ebadmsg);
}

template <class T> Result<Value> Cursor::scalar() const noexcept {
    if (std::uint64_t{base_} + sizeof(T) > pkt_.size()) return marshal_error(Errno::ebadmsg);
    return Value(load<T>(pkt_.data() + base_));
}

Result<Cursor> Cursor::at(std::span<const std::uint8_t> pkt, std::uint32_t base, NodeType type) noexcept {
    Result<std::uint32_t> unit = align_sz(type);
    if (!unit) return unit.error();
    if (base % *unit != 0) return marshal_error(Errno::ebadmsg);
    if (type != type_of(SpecialType::branch)) return Cursor(pkt, base, type);
    if (std::uint64_t{base} + WORD_SZ > pkt.size()) return marshal_error(Errno::ebadmsg);
    Result<std::uint32_t> len = get_len(pkt, base);
    if (!len) return len.error();
    if (*len > (pkt.size() - base - WORD_SZ) / WORD_SZ) return marshal_error(Errno::ebadmsg);
    return Cursor(pkt, base, type, *len);
}

Result<BranchCursor> Cursor::as_branch() const noexcept {
    if (is_null()) return marshal_error(Errno::efault);
    if (!is_branch()) return marshal_error(Errno::ebadmsg);
    return BranchCursor(*this);
}

Result<Array> Cursor::array(std::uint32_t elem) const noexcept {
    std::uint64_t payload = std::uint64_t{base_} + WORD_SZ;
    if (payload > pkt_.size()) return marshal_error(Errno::ebadmsg);
    Result<std::uint32_t> len = get_len(pkt_, base_);
    if (!len) return len.error();
    std::uint64_t bytes = std::uint64_t{*len} * elem;
    if (bytes > pkt_.size() - payload) return marshal_error(Errno::ebadmsg);
    return Array{static_cast<SequenceType>(type_), pkt_.subspan(payload, bytes)};
}

Result<Bitset> Cursor::bitset() const noexcept {
    std::uint64_t payload = std::uint64_t{base_} + WORD_SZ;
    if (payload > pkt_.size()) return marshal_error(Errno::ebadmsg);
    Result<std::uint32_t> len = get_len(pkt_, base_);
    if (!len) return len.error();
    std::uint64_t bytes = (std::uint64_t{*len} + 7) >> 3;
    if (bytes > pkt_.size() - payload) return marshal_error(Errno::ebadmsg);
    return Bitset{pkt_.subspan(payload, bytes), *len};
}

// The wire carries a NUL after the bytes, which the length leaves out and a
// string that reads must have; the bytes are UTF-8 or refused.
Result<std::string_view> Cursor::str() const noexcept {
    std::uint64_t payload = std::uint64_t{base_} + WORD_SZ;
    if (payload > pkt_.size()) return marshal_error(Errno::ebadmsg);
    Result<std::uint32_t> nbyte = get_len(pkt_, base_);
    if (!nbyte) return nbyte.error();
    if (std::uint64_t{*nbyte} + 1 > pkt_.size() - payload) return marshal_error(Errno::ebadmsg);
    if (pkt_[payload + *nbyte] != 0) return marshal_error(Errno::ebadmsg);
    std::string_view s(reinterpret_cast<const char *>(pkt_.data() + payload), *nbyte);
    if (!is_utf8(s)) return marshal_error(Errno::ebadmsg);
    return s;
}

Result<std::string_view> Cursor::get_str_ref() const noexcept {
    if (is_null() || type_ != type_of(SequenceType::str)) return marshal_error(Errno::ebadmsg);
    return str();
}

Result<Strs> Cursor::strs() const noexcept {
    std::uint64_t payload = std::uint64_t{base_} + WORD_SZ;
    if (payload > pkt_.size()) return marshal_error(Errno::ebadmsg);
    Result<std::uint32_t> len = get_len(pkt_, base_);
    if (!len) return len.error();
    std::uint32_t n = *len;
    std::uint64_t blob = payload + 4 * (std::uint64_t{n} + 1);
    if (blob > pkt_.size()) return marshal_error(Errno::ebadmsg);
    auto off = [&](std::uint32_t i) { return load<std::uint32_t>(pkt_.data() + payload + 4 * i); };
    std::uint32_t total = off(n);
    if (total > pkt_.size() - blob) return marshal_error(Errno::ebadmsg);
    for (std::uint32_t i = 0; i < n; i++) {
        if (off(i) > off(i + 1) || off(i + 1) > total) return marshal_error(Errno::ebadmsg);
        if (!is_utf8(std::string_view(reinterpret_cast<const char *>(pkt_.data() + blob + off(i)), off(i + 1) - off(i)))) return marshal_error(Errno::ebadmsg);
    }
    return Strs{pkt_.subspan(payload, 4 * (std::uint64_t{n} + 1)), pkt_.subspan(blob, total), n};
}

Result<Strs> Cursor::get_strs() const noexcept {
    if (is_null() || type_ != type_of(SequenceType::strs)) return marshal_error(Errno::ebadmsg);
    return strs();
}

Result<Value> Cursor::get() const noexcept {
    if (is_null()) return Value(Null{});
    if (is_branch()) return Value(BranchCursor(*this));
    auto seq = [](auto r) -> Result<Value> {
        if (!r) return r.error();
        return Value(*r);
    };
    switch (static_cast<ScalarType>(type_)) {
    case ScalarType::u8:
        return scalar<std::uint8_t>();
    case ScalarType::i8:
        return scalar<std::int8_t>();
    case ScalarType::u16:
        return scalar<std::uint16_t>();
    case ScalarType::i16:
        return scalar<std::int16_t>();
    case ScalarType::u32:
        return scalar<std::uint32_t>();
    case ScalarType::i32:
        return scalar<std::int32_t>();
    case ScalarType::u64:
        return scalar<std::uint64_t>();
    case ScalarType::i64:
        return scalar<std::int64_t>();
    case ScalarType::f32:
        return scalar<float>();
    case ScalarType::f64:
        return scalar<double>();
    case ScalarType::boolean: {
        Result<Value> b = scalar<std::uint8_t>();
        if (!b) return b;
        return Value(std::get<std::uint8_t>(*b) != 0);
    }
    }
    switch (static_cast<SequenceType>(type_)) {
    case SequenceType::u8_array:
    case SequenceType::i8_array:
    case SequenceType::u16_array:
    case SequenceType::i16_array:
    case SequenceType::u32_array:
    case SequenceType::i32_array:
    case SequenceType::u64_array:
    case SequenceType::i64_array:
    case SequenceType::f32_array:
    case SequenceType::f64_array:
        return seq(array(elem_sz(static_cast<SequenceType>(type_))));
    case SequenceType::bitset:
        return seq(bitset());
    case SequenceType::str:
        return seq(str());
    case SequenceType::strs:
        return seq(strs());
    }
    return marshal_error(Errno::ebadmsg);
}

Result<std::uint64_t> Cursor::get_u64() const noexcept {
    return get_as<std::uint64_t>();
}
Result<std::int64_t> Cursor::get_i64() const noexcept {
    return get_as<std::int64_t>();
}
Result<bool> Cursor::get_bool() const noexcept {
    return get_as<bool>();
}

Result<double> Cursor::get_number() const noexcept {
    Result<Value> v = get();
    if (!v) return v.error();
    return std::visit(
        [](const auto &x) -> Result<double> {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_arithmetic_v<T> && !std::is_same_v<T, bool> && sizeof(T) <= 4) return static_cast<double>(x);
            else if constexpr (std::is_same_v<T, double>) return x;
            else return marshal_error(Errno::ebadmsg);
        },
        *v);
}

Result<Cursor> BranchCursor::at(std::uint32_t idx) const noexcept {
    if (idx >= len()) return marshal_error(Errno::efault);
    Result<Desc> d = get_desc(node_.pkt_, base() + WORD_SZ + idx * WORD_SZ);
    if (!d) return d.error();
    auto [rel_off, type] = *d;
    if (rel_off == 0) return Cursor(node_.pkt_, 0, type);
    if (rel_off >= node_.pkt_.size() - base()) return marshal_error(Errno::ebadmsg);
    std::uint32_t child = base() + rel_off;
    std::uint32_t end = end_of(idx);
    if (child < base() + WORD_SZ + len() * WORD_SZ || child >= end) return marshal_error(Errno::ebadmsg);
    return Cursor::at(node_.pkt_.first(end), child, type);
}

std::uint32_t BranchCursor::end_of(std::uint32_t idx) const noexcept {
    std::uint64_t end = node_.pkt_.size();
    for (std::uint32_t j = idx + 1; j < len(); j++) {
        std::uint32_t rel_off = get_word(node_.pkt_, base() + WORD_SZ + j * WORD_SZ)[0];
        if (rel_off != 0) return static_cast<std::uint32_t>(std::min(std::uint64_t{base()} + rel_off, end));
    }
    return static_cast<std::uint32_t>(end);
}

Result<BranchCursor> BranchCursor::at_branch(std::uint32_t idx) const noexcept {
    Result<Cursor> c = at(idx);
    if (!c) return c.error();
    return c->as_branch();
}

Result<TreeReader> TreeReader::make(std::span<const std::uint8_t> pkt) noexcept {
    if (pkt.size() < HDR_SZ) return marshal_error(Errno::ebadmsg);
    Result<std::uint32_t> magic = get_len(pkt, 0);
    if (!magic || *magic != MAGIC) return marshal_error(Errno::ebadmsg);
    Result<Desc> d = get_desc(pkt, WORD_SZ);
    if (!d) return d.error();
    auto [payload_sz, type] = *d;
    if (payload_sz > pkt.size() - HDR_SZ) return marshal_error(Errno::ebadmsg);
    if (payload_sz == 0) return TreeReader(Cursor(pkt, 0, type));
    Result<Cursor> root = Cursor::at(pkt.first(HDR_SZ + payload_sz), HDR_SZ, type);
    if (!root) return root.error();
    return TreeReader(*root);
}

Result<Value> BranchCursor::get_at(std::uint32_t idx) const noexcept {
    Result<Cursor> c = at(idx);
    if (!c) return c.error();
    return c->get();
}

} // namespace xpute::xtp

// cpp/xpute/kit/wire/xtp/encode.cpp

namespace xpute::xtp {
namespace {

constexpr std::uint64_t word_up(std::uint64_t n) noexcept {
    return (n + WORD_SZ - 1) & ~std::uint64_t{WORD_SZ - 1};
}

} // namespace

PacketWriter::PacketWriter(std::span<std::uint8_t> buf) noexcept : buf_(buf.first(std::min<std::size_t>(buf.size(), MAX_PKT_SZ))) {
    if (buf_.size() < HDR_SZ) error_ = marshal_error(Errno::einval);
    else std::memset(buf_.data(), 0, HDR_SZ);
}

std::optional<std::uint32_t> PacketWriter::spoil(Errno errnum) noexcept {
    if (!error_) error_ = marshal_error(errnum);
    return std::nullopt;
}

std::optional<std::uint32_t> PacketWriter::reserve(std::uint32_t unit, std::uint64_t bytes) noexcept {
    if (error_) return std::nullopt;
    std::uint64_t base = (std::uint64_t{lim_} + unit - 1) & ~std::uint64_t{unit - 1};
    std::uint64_t end = base + bytes;
    if (end > buf_.size()) return spoil(Errno::eoverflow);
    std::memset(buf_.data() + lim_, 0, end - lim_);
    lim_ = static_cast<std::uint32_t>(end);
    return static_cast<std::uint32_t>(base);
}

PacketWriter &PacketWriter::entry(std::optional<std::uint32_t> at, NodeType type) noexcept {
    ensure(frame_.written < frame_.count, Errno::enospc, frame_.count);
    if (frame_.root) root_type_ = type;
    else if (!error_) set_word(buf_, frame_.base + WORD_SZ + frame_.written * WORD_SZ, at ? *at - frame_.base : 0, type);
    frame_.written++;
    return *this;
}

std::optional<std::uint32_t> PacketWriter::seq(std::uint32_t len, std::uint64_t bytes) noexcept {
    std::optional<std::uint32_t> at = reserve(WORD_SZ, WORD_SZ + word_up(bytes));
    if (at) set_word(buf_, *at, len, RESERVED);
    return at;
}

PacketWriter &PacketWriter::bitset(std::optional<std::span<const std::uint8_t>> bits) noexcept {
    std::optional<std::uint32_t> at;
    if (bits) {
        auto len = static_cast<std::uint32_t>(bits->size());
        at = seq(len, (std::uint64_t{len} + 7) >> 3);
        if (at) {
            std::uint8_t *payload = buf_.data() + *at + WORD_SZ;
            for (std::uint32_t i = 0; i < len; i++) {
                if ((*bits)[i] != 0) payload[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
            }
        }
    }
    return entry(at, type_of(SequenceType::bitset));
}

std::uint32_t PacketWriter::str_payload() const noexcept {
    return static_cast<std::uint32_t>(word_up(lim_) + WORD_SZ);
}

std::optional<std::uint32_t> PacketWriter::str_room() noexcept {
    if (error_) return std::nullopt;
    std::uint64_t payload = word_up(lim_) + WORD_SZ;
    if (payload > buf_.size()) return spoil(Errno::eoverflow);
    return static_cast<std::uint32_t>(buf_.size() - payload);
}

std::optional<std::uint32_t> PacketWriter::str_commit(std::uint32_t len) noexcept {
    std::uint32_t payload = str_payload();
    std::uint64_t end = word_up(std::uint64_t{payload} + len + 1);
    if (end > buf_.size()) return spoil(Errno::eoverflow);
    std::uint32_t base = payload - WORD_SZ;
    std::memset(buf_.data() + lim_, 0, base - lim_);
    std::memset(buf_.data() + payload + len, 0, end - (payload + len));
    set_word(buf_, base, len, RESERVED);
    lim_ = static_cast<std::uint32_t>(end);
    return base;
}

PacketWriter &PacketWriter::str(std::optional<std::string_view> s) noexcept {
    if (!s) return entry(std::nullopt, type_of(SequenceType::str));
    // Copied the way a string written in place goes in, so the two lay out alike.
    return str_with([&](std::span<char> room) -> std::optional<std::size_t> {
        if (s->size() > room.size()) return std::nullopt;
        std::memcpy(room.data(), s->data(), s->size());
        return s->size();
    });
}

PacketWriter &PacketWriter::strs(std::optional<std::span<const std::string_view>> items) noexcept {
    std::optional<std::uint32_t> at;
    if (items) {
        auto n = static_cast<std::uint32_t>(items->size());
        std::uint64_t lane = 4 * (std::uint64_t{n} + 1);
        std::uint64_t blob = 0;
        for (std::string_view s : *items) blob += s.size();
        at = seq(n, lane + blob);
        if (at) {
            std::uint8_t *payload = buf_.data() + *at + WORD_SZ;
            std::uint32_t off = 0;
            store<std::uint32_t>(payload, off);
            for (std::uint32_t i = 0; i < n; i++) {
                std::string_view s = (*items)[i];
                std::memcpy(payload + lane + off, s.data(), s.size());
                off += static_cast<std::uint32_t>(s.size());
                store<std::uint32_t>(payload + 4 * (i + 1), off);
            }
        }
    }
    return entry(at, type_of(SequenceType::strs));
}

std::optional<std::uint32_t> PacketWriter::branch_open(std::uint32_t count) noexcept {
    std::optional<std::uint32_t> at = reserve(WORD_SZ, WORD_SZ + std::uint64_t{count} * WORD_SZ);
    if (at) set_word(buf_, *at, count, RESERVED);
    return at;
}

// A branch ends on a word, so what follows it starts where the encoder
// starts it.
std::optional<std::uint32_t> PacketWriter::branch_close(std::optional<std::uint32_t> at) noexcept {
    if (!at || error_) return at;
    std::uint64_t end = word_up(lim_);
    if (end > buf_.size()) return spoil(Errno::eoverflow);
    std::memset(buf_.data() + lim_, 0, end - lim_);
    lim_ = static_cast<std::uint32_t>(end);
    return at;
}

PacketWriter &PacketWriter::graft(std::span<const std::uint8_t> packet) noexcept {
    std::optional<std::uint32_t> at;
    NodeType type = type_of(SpecialType::nil);
    Result<std::uint32_t> magic = packet.size() < HDR_SZ ? marshal_error(Errno::ebadmsg) : get_len(packet, 0);
    Result<Desc> d = magic && *magic == MAGIC ? get_desc(packet, WORD_SZ) : marshal_error(Errno::ebadmsg);
    if (!d) {
        spoil(Errno::ebadmsg);
    } else {
        auto [payload_sz, desc_type] = *d;
        type = desc_type;
        if (payload_sz > packet.size() - HDR_SZ) {
            spoil(Errno::ebadmsg);
        } else if (payload_sz != 0) {
            Result<std::uint32_t> unit = align_sz(type);
            if (!unit || payload_sz % *unit != 0) {
                spoil(Errno::ebadmsg);
            } else if ((at = reserve(*unit, payload_sz))) {
                std::memcpy(buf_.data() + *at, packet.data() + HDR_SZ, payload_sz);
            }
        }
    }
    return entry(at, type);
}

Result<std::uint32_t> PacketWriter::finish() noexcept {
    ensure(frame_.root && frame_.written == frame_.count, Errno::enotrecoverable, frame_.count, frame_.written);
    if (error_) return *error_;
    std::uint64_t end = word_up(lim_);
    if (end > buf_.size()) return marshal_error(Errno::eoverflow);
    std::memset(buf_.data() + lim_, 0, end - lim_);
    set_word(buf_, 0, MAGIC, RESERVED);
    set_word(buf_, WORD_SZ, lim_ - HDR_SZ, root_type_);
    return static_cast<std::uint32_t>(end);
}

} // namespace xpute::xtp

// cpp/xpute/guest/clock.cpp

namespace xpute {

namespace {
Clock installed_clock = nullptr;
}

void set_clock(Clock clock) noexcept {
    installed_clock = clock;
}

double now() noexcept {
    return installed_clock != nullptr ? installed_clock() : 0.0;
}

} // namespace xpute

// cpp/xpute/guest/ipc/directory.cpp

namespace xpute {

void write_directory(Memory mem, std::uint32_t at, std::uint32_t words, std::span<const Entry> entries) noexcept {
    ensure(directory_words(static_cast<std::uint32_t>(entries.size())) <= words, Errno::enospc, entries.size());
    for (std::size_t i = 0; i < entries.size(); i++) {
        for (std::size_t j = 0; j < i; j++) {
            ensure(entries[j].key != entries[i].key, Errno::einval, entries[i].key);
        }
    }
    mem.set_word(at, directory::MAGIC);
    mem.set_word(at + 4, directory::VERSION);
    mem.set_word(at + 8, static_cast<std::uint32_t>(entries.size()));
    mem.set_word(at + 12, 0);
    std::uint32_t off = at + 4 * directory::HEAD;
    for (const Entry &e : entries) {
        mem.set_word(off, e.key);
        mem.set_word(off + 4, e.value);
        off += 8;
    }
}

std::array<Entry, 3> quantum_entries(const QuantumPolicy &q) noexcept {
    auto scaled = [](double v) { return saturate_as<std::uint32_t>(round(v * directory::QUANTUM_SCALE)); };
    return {{
        {directory::key::MARGIN_SHARE, scaled(q.margin_share)},
        {directory::key::BATCH_FRAMES, scaled(q.batch_frames)},
        {directory::key::SETTLE_MS, scaled(q.settle_ms)},
    }};
}

} // namespace xpute

// cpp/xpute/guest/ipc/ring.cpp

namespace xpute {

static bool power_of_two(std::uint32_t n) noexcept {
    return n != 0 && (n & (n - 1)) == 0;
}

// Payloads are padded to 8 bytes.
static constexpr std::uint64_t padded(std::uint64_t n) noexcept {
    return (n + 7) / 8 * 8;
}

Ring::Ring(Memory mem, std::uint32_t at) noexcept
    : mem_(mem), at_(at), cap_(header(ring::CAP)), slot_bytes_(header(ring::SLOT_BYTES)), slot_base_(header(ring::SLOT_BASE)) {
    ensure(power_of_two(cap_), Errno::einval, cap_, at);
}

Ring Ring::init(Memory mem, std::uint32_t at, std::uint32_t cap, std::uint32_t slot_base, std::uint32_t slot_bytes) noexcept {
    ensure(power_of_two(cap), Errno::einval, cap);
    ensure(power_of_two(slot_bytes) && slot_bytes % 8 == 0, Errno::einval, slot_bytes);
    std::memset(mem.at(at), 0, ring_bytes(cap));
    mem.set_word(at + 4 * ring::CAP, cap);
    mem.set_word(at + 4 * ring::SLOT_BYTES, slot_bytes);
    mem.set_word(at + 4 * ring::SLOT_BASE, slot_base);
    return Ring(mem, at);
}

std::uint32_t Ring::slot_at(std::uint32_t position) const noexcept {
    return slot_base_ + (position & (cap_ - 1)) * slot_bytes_;
}

void Ring::pad(std::uint32_t slot, std::uint32_t n) const noexcept {
    std::memset(mem_.at(slot + n), 0, padded(n) - n);
}

void Ring::publish(std::uint32_t tag, std::uint32_t cmd, std::uint32_t flags, std::int32_t result, std::uint32_t packet_at) const noexcept {
    std::uint32_t t = tail();
    std::uint32_t f = at_ + 4 * entry_at(t);
    mem_.set_word(f + 4 * frame::TAG, tag);
    mem_.set_word(f + 4 * frame::CMD, frame::cmd_word(cmd, flags));
    mem_.set_word(f + 4 * frame::RESULT, static_cast<std::uint32_t>(result));
    mem_.set_word(f + 4 * frame::PACKET, packet_at);
    // The frame is whole before the tail says so.
    set_header(ring::TAIL, t + 1);
}

Errno Ring::push(std::uint32_t tag, std::uint32_t cmd, std::uint32_t flags, std::int32_t result, std::optional<std::span<const std::uint8_t>> packet) const noexcept {
    if (len() >= cap()) return Errno::eagain;
    std::uint32_t packet_at = 0;
    if (packet) {
        if (padded(packet->size()) > slot_bytes()) return Errno::emsgsize;
        packet_at = slot_at(tail());
        auto n = static_cast<std::uint32_t>(packet->size());
        std::memcpy(mem_.at(packet_at), packet->data(), n);
        pad(packet_at, n);
    }
    publish(tag, cmd, flags, result, packet_at);
    return Errno::ok;
}

std::optional<Frame> Ring::peek() const noexcept {
    if (empty()) return std::nullopt;
    std::uint32_t f = at_ + 4 * entry_at(head());
    std::uint32_t word = mem_.word(f + 4 * frame::CMD);
    return Frame{
        .tag = mem_.word(f + 4 * frame::TAG),
        .cmd = frame::cmd_of(word),
        .flags = frame::flags_of(word),
        .result = static_cast<std::int32_t>(mem_.word(f + 4 * frame::RESULT)),
        .packet_at = mem_.word(f + 4 * frame::PACKET),
    };
}

Result<std::span<const std::uint8_t>> Ring::packet(const Frame &f) const noexcept {
    if (f.packet_at == 0) return std::span<const std::uint8_t>();
    std::uint32_t base = slot_base_, slot = slot_bytes_;
    if (f.packet_at < base) return marshal_error(Errno::ebadmsg);
    std::uint64_t off = f.packet_at - base;
    if (off % slot != 0 || off + 16 > slots_bytes(cap(), slot)) return marshal_error(Errno::ebadmsg);
    // The header, then the payload its second word names, to the word.
    std::uint64_t n = padded(16 + std::uint64_t{mem_.word(f.packet_at + 8)});
    if (n > slot) return marshal_error(Errno::ebadmsg);
    return std::span<const std::uint8_t>(mem_.at(f.packet_at), static_cast<std::size_t>(n));
}

void Ring::advance() const noexcept {
    ensure(!empty(), Errno::enotrecoverable);
    set_header(ring::HEAD, head() + 1);
}

} // namespace xpute

// cpp/xpute/guest/ipc/stream.cpp

namespace xpute {

Stream::Stream(Memory mem, std::uint32_t at, std::uint32_t words, std::uint32_t head) noexcept : mem_(mem), at_(at), words_(words), head_(head) {
    ensure(head > END && head < words, Errno::einval, head, words);
    mem_.set_word(at_ + WORD * END, 0);
}

std::uint32_t Stream::open(std::uint32_t op, std::uint32_t n, std::source_location from) noexcept {
    std::uint32_t at = head_ + len_;
    ensure(std::uint64_t{at} + 2 + n <= words_, Errno::enospc, words_ * WORD, 0, from);
    len_ += 2 + n;
    std::uint32_t off = at_ + WORD * at;
    mem_.set_word(off, op);
    mem_.set_word(off + WORD, n);
    return off + 2 * WORD;
}

std::optional<std::uint32_t> Stream::reserve(std::uint32_t op, std::uint32_t n, std::source_location from) noexcept {
    if (mark_.state == Mark::spilled) return std::nullopt;
    if (mark_.state == Mark::open && std::uint64_t{left()} < (std::uint64_t{2} + n) * WORD) {
        mark_.state = Mark::spilled;
        return std::nullopt;
    }
    return open(op, n, from);
}

void Stream::record(std::uint32_t op, std::span<const std::uint32_t> args, std::span<const std::uint32_t> tail,
                    std::optional<std::span<const std::uint8_t>> bytes, std::source_location from) noexcept {
    auto a = static_cast<std::uint32_t>(args.size()), t = static_cast<std::uint32_t>(tail.size());
    std::uint32_t byte_words = static_cast<std::uint32_t>(record_words(0, bytes ? std::optional<std::uint64_t>{bytes->size()} : std::nullopt) - 2);
    std::optional<std::uint32_t> w = reserve(op, a + t + byte_words, from);
    if (!w) return;
    for (std::uint32_t i = 0; i < a; i++) mem_.set_word(*w + WORD * i, args[i]);
    for (std::uint32_t i = 0; i < t; i++) mem_.set_word(*w + WORD * (a + i), tail[i]);
    if (bytes) {
        std::uint32_t k = *w + WORD * (a + t);
        mem_.set_word(k, static_cast<std::uint32_t>(bytes->size()));
        if (byte_words > 1) mem_.set_word(k + WORD * (byte_words - 1), 0);
        std::memcpy(mem_.at(k + WORD), bytes->data(), bytes->size());
    }
}

void Stream::publish() noexcept {
    mem_.set_word(at_ + WORD * END, len_);
    len_ = 0;
}

} // namespace xpute

// cpp/xpute/guest/ipc/sys.cpp

namespace xpute {

static std::span<const std::uint8_t> bytes_of(std::string_view s) noexcept {
    return {reinterpret_cast<const std::uint8_t *>(s.data()), s.size()};
}

void Sys::openat(Fd fd, std::uint32_t root, std::uint32_t flags, std::string_view path) noexcept {
    const std::array<std::uint32_t, OPENAT_ARGS> args{fd, root, flags};
    stream_.record(sys::op::OPENAT, args, {}, bytes_of(path));
}

void Sys::read(Fd fd, std::span<std::uint8_t> dst) noexcept {
    const std::array<std::uint32_t, READ_ARGS> args{fd, mem_.off_of(dst), static_cast<std::uint32_t>(dst.size())};
    stream_.record(sys::op::READ, args);
}

void Sys::write(Fd fd, std::vector<std::uint8_t> bytes) noexcept {
    const std::array<std::uint32_t, WRITE_ARGS> args{fd, mem_.off_of(std::span<const std::uint8_t>(bytes)), static_cast<std::uint32_t>(bytes.size())};
    stream_.record(sys::op::WRITE, args);
    held_.push_back(std::move(bytes));
}

void Sys::close(Fd fd) noexcept {
    const std::array<std::uint32_t, CLOSE_ARGS> args{fd};
    stream_.record(sys::op::CLOSE, args);
}

void Sys::call(std::uint32_t op, std::span<const std::uint32_t> args, std::span<const std::uint8_t> bytes) noexcept {
    ensure((op & sys::op::PROGRAM) != 0, Errno::einval, op);
    stream_.record(op, args, {}, bytes);
}

bool Sys::try_call(std::uint32_t op, std::span<const std::uint32_t> args, std::span<const std::uint8_t> bytes, std::uint32_t reserve) noexcept {
    if (!stream_.fits(record_bytes(args.size(), bytes.size()), reserve)) return false;
    call(op, args, bytes);
    return true;
}

void Sys::abort(Errno errnum, const std::source_location &at, std::uint64_t a, std::uint64_t b) noexcept {
    stream_.publish();
    const std::array<std::uint32_t, 7> args{static_cast<std::uint32_t>(errnum), at.line(), at.column(), static_cast<std::uint32_t>(a),
                                            static_cast<std::uint32_t>(a >> 32), static_cast<std::uint32_t>(b), static_cast<std::uint32_t>(b >> 32)};
    stream_.record(sys::op::ABORT, args, {}, bytes_of(at.file_name()));
    stream_.publish();
}

} // namespace xpute

// cpp/xpute/guest/mem.cpp

namespace xpute::mem {

namespace {
// Two words rather than a `Memory`, so they are set before any constructor
// runs and a static that allocates finds them.
std::uintptr_t core_base = 0;
std::uintptr_t core_bytes = UINTPTR_MAX;
} // namespace

void set(std::uintptr_t base, std::uintptr_t bytes) noexcept {
    core_base = base;
    core_bytes = bytes;
}

Memory core() noexcept {
    return Memory(core_base, core_bytes);
}

} // namespace xpute::mem

// cpp/xpute/guest/mem/heap.cpp

namespace xpute {

namespace {
// The platform's allocator is never a guest's: on wasm it links the libc that
// prints its failures through WASI.
Allocator installed_heap{nullptr, nullptr};
} // namespace

void set_heap(Allocator heap) noexcept {
    installed_heap = heap;
}

Allocator heap() noexcept {
    return installed_heap;
}

} // namespace xpute

// cpp/xpute/guest/sched/task.cpp

namespace xpute {

namespace {
Executor *installed_executor = nullptr;
}

void set_executor(Executor &executor) noexcept {
    installed_executor = &executor;
}

Executor *executor() noexcept {
    return installed_executor;
}

bool spawn(Task<> task, Priority priority) noexcept {
    if (installed_executor == nullptr || !task) return false;
    Task<>::Handle h = task.release();
    h.promise().executor = installed_executor;
    if (installed_executor->spawn(h, priority)) return true;
    h.destroy();
    return false;
}

void poll() noexcept {
    if (installed_executor != nullptr) installed_executor->poll();
}

bool pending() noexcept {
    return installed_executor != nullptr && installed_executor->pending();
}

} // namespace xpute

// cpp/xpute/gpu/encode.cpp

namespace xpute::gpu::call {

using detail::emit;
using detail::f;
using detail::flag;
using detail::opt;
using detail::u;

static bool words(Stream &stream, std::uint32_t op, std::initializer_list<std::uint32_t> w) noexcept {
    return emit(stream, op, [&](auto &s) { detail::words(s, w); });
}

static std::uint32_t off(Stream &stream, std::span<const std::uint8_t> bytes) noexcept { return stream.memory().off_of(bytes); }

template <class S> static void buffer_write(S &s, Id buffer, std::uint32_t offset, std::span<const std::uint8_t> bytes) noexcept {
    detail::words(s, {buffer, offset});
    s.bytes(bytes);
}

bool write_buffer(Stream &stream, Id buffer, std::uint32_t offset, std::span<const std::uint8_t> bytes) noexcept {
    return emit(stream, op::WRITE_BUFFER, [&](auto &s) { buffer_write(s, buffer, offset, bytes); });
}

std::uint64_t write_buffer_bytes(std::span<const std::uint8_t> bytes) noexcept {
    return detail::measured([&](auto &s) { buffer_write(s, 0, 0, bytes); });
}

bool write_buffer_at(Stream &stream, Id buffer, std::uint32_t offset, std::span<const std::uint8_t> bytes) noexcept {
    return words(stream, op::WRITE_BUFFER_AT, {buffer, offset, off(stream, bytes), static_cast<std::uint32_t>(bytes.size())});
}

template <class S> static void texture_write(S &s, const TexelCopyTexture &place, std::array<std::uint32_t, 2> layout, std::array<std::uint32_t, 3> size) noexcept {
    detail::texture_place(s, place);
    detail::words(s, {layout[0], layout[1]});
    detail::extent(s, size);
}

bool write_texture(Stream &stream, const TexelCopyTexture &place, std::array<std::uint32_t, 2> layout, std::array<std::uint32_t, 3> size, std::span<const std::uint8_t> bytes) noexcept {
    return emit(stream, op::WRITE_TEXTURE, [&](auto &s) {
        texture_write(s, place, layout, size);
        s.bytes(bytes);
    });
}

std::uint64_t write_texture_bytes(std::span<const std::uint8_t> bytes) noexcept {
    const TexelCopyTexture place{.texture = 0, .mip = 0, .origin = {0, 0, 0}, .aspect = TextureAspect::all};
    return detail::measured([&](auto &s) {
        texture_write(s, place, {0, 0}, {0, 0, 0});
        s.bytes(bytes);
    });
}

bool write_texture_at(Stream &stream, const TexelCopyTexture &place, std::array<std::uint32_t, 2> layout, std::array<std::uint32_t, 3> size, std::span<const std::uint8_t> bytes) noexcept {
    std::uint32_t at = off(stream, bytes);
    return emit(stream, op::WRITE_TEXTURE_AT, [&](auto &s) {
        texture_write(s, place, layout, size);
        detail::words(s, {at, static_cast<std::uint32_t>(bytes.size())});
    });
}

bool copy_external_image(Stream &stream, const TexelCopyTexture &to, bool flip_y, bool premultiplied, std::span<const std::uint8_t> image, std::uint32_t &word, std::uint32_t ticket) noexcept {
    word = ticket;
    std::uint32_t image_at = off(stream, image), word_at = stream.memory().off_of(&word);
    return emit(stream, op::COPY_EXTERNAL_IMAGE, [&](auto &s) {
        detail::texture_place(s, to);
        detail::words(s, {flag(flip_y), flag(premultiplied), image_at, static_cast<std::uint32_t>(image.size()), word_at, ticket});
    });
}

bool copy_buffer_to_buffer(Stream &stream, Id encoder, std::array<std::uint32_t, 2> from, std::array<std::uint32_t, 2> to, std::uint32_t size) noexcept {
    return words(stream, op::COPY_BUFFER_TO_BUFFER, {encoder, from[0], from[1], to[0], to[1], size});
}

bool copy_buffer_to_texture(Stream &stream, Id encoder, const TexelCopyBuffer &from, const TexelCopyTexture &to, std::array<std::uint32_t, 3> size) noexcept {
    return emit(stream, op::COPY_BUFFER_TO_TEXTURE, [&](auto &s) {
        s.word(encoder);
        detail::buffer_layout(s, from);
        detail::texture_place(s, to);
        detail::extent(s, size);
    });
}

bool copy_texture_to_buffer(Stream &stream, Id encoder, const TexelCopyTexture &from, const TexelCopyBuffer &to, std::array<std::uint32_t, 3> size) noexcept {
    return emit(stream, op::COPY_TEXTURE_TO_BUFFER, [&](auto &s) {
        s.word(encoder);
        detail::texture_place(s, from);
        detail::buffer_layout(s, to);
        detail::extent(s, size);
    });
}

bool copy_texture_to_texture(Stream &stream, Id encoder, const TexelCopyTexture &from, const TexelCopyTexture &to, std::array<std::uint32_t, 3> size) noexcept {
    return emit(stream, op::COPY_TEXTURE_TO_TEXTURE, [&](auto &s) {
        s.word(encoder);
        detail::texture_place(s, from);
        detail::texture_place(s, to);
        detail::extent(s, size);
    });
}

bool clear_buffer(Stream &stream, Id encoder, Id buffer, std::uint32_t offset, std::optional<std::uint32_t> size) noexcept {
    return words(stream, op::CLEAR_BUFFER, {encoder, buffer, offset, opt(size)});
}

bool resolve_query_set(Stream &stream, Id encoder, Id query_set, std::uint32_t first, std::uint32_t count, Id buffer, std::uint32_t offset) noexcept {
    return words(stream, op::RESOLVE_QUERY_SET, {encoder, query_set, first, count, buffer, offset});
}

bool set_pipeline(Stream &stream, Id pass, Id pipeline) noexcept { return words(stream, op::SET_PIPELINE, {pass, pipeline}); }

bool set_bind_group(Stream &stream, Id pass, std::uint32_t index, Id group, std::span<const std::uint32_t> dynamic_offsets) noexcept {
    return emit(stream, op::SET_BIND_GROUP, [&](auto &s) {
        detail::words(s, {pass, index, group, static_cast<std::uint32_t>(dynamic_offsets.size())});
        for (std::uint32_t o : dynamic_offsets) s.word(o);
    });
}

bool set_vertex_buffer(Stream &stream, Id pass, std::uint32_t slot, Id buffer, std::uint32_t offset, std::optional<std::uint32_t> size) noexcept {
    return words(stream, op::SET_VERTEX_BUFFER, {pass, slot, buffer, offset, opt(size)});
}

bool set_index_buffer(Stream &stream, Id pass, Id buffer, IndexFormat format, std::uint32_t offset, std::optional<std::uint32_t> size) noexcept {
    return words(stream, op::SET_INDEX_BUFFER, {pass, buffer, u(format), offset, opt(size)});
}

bool set_viewport(Stream &stream, Id pass, std::array<float, 4> rect, std::array<float, 2> depth) noexcept {
    return words(stream, op::SET_VIEWPORT, {pass, f(rect[0]), f(rect[1]), f(rect[2]), f(rect[3]), f(depth[0]), f(depth[1])});
}

bool set_scissor(Stream &stream, Id pass, std::array<std::uint32_t, 4> rect) noexcept { return words(stream, op::SET_SCISSOR, {pass, rect[0], rect[1], rect[2], rect[3]}); }

bool set_blend_constant(Stream &stream, Id pass, std::array<float, 4> rgba) noexcept {
    return words(stream, op::SET_BLEND_CONSTANT, {pass, f(rgba[0]), f(rgba[1]), f(rgba[2]), f(rgba[3])});
}

bool set_stencil_reference(Stream &stream, Id pass, std::uint32_t reference) noexcept { return words(stream, op::SET_STENCIL_REFERENCE, {pass, reference}); }

bool draw(Stream &stream, Id pass, std::uint32_t vertices, std::uint32_t instances, std::uint32_t first_vertex, std::uint32_t first_instance) noexcept {
    return words(stream, op::DRAW, {pass, vertices, instances, first_vertex, first_instance});
}

bool draw_indexed(Stream &stream, Id pass, std::uint32_t indices, std::uint32_t instances, std::uint32_t first_index, std::int32_t base_vertex, std::uint32_t first_instance) noexcept {
    return words(stream, op::DRAW_INDEXED, {pass, indices, instances, first_index, static_cast<std::uint32_t>(base_vertex), first_instance});
}

bool draw_indirect(Stream &stream, Id pass, Id buffer, std::uint32_t offset) noexcept { return words(stream, op::DRAW_INDIRECT, {pass, buffer, offset}); }

bool draw_indexed_indirect(Stream &stream, Id pass, Id buffer, std::uint32_t offset) noexcept { return words(stream, op::DRAW_INDEXED_INDIRECT, {pass, buffer, offset}); }

bool dispatch(Stream &stream, Id pass, std::array<std::uint32_t, 3> workgroups) noexcept {
    return words(stream, op::DISPATCH, {pass, workgroups[0], workgroups[1], workgroups[2]});
}

bool dispatch_indirect(Stream &stream, Id pass, Id buffer, std::uint32_t offset) noexcept { return words(stream, op::DISPATCH_INDIRECT, {pass, buffer, offset}); }

bool map_async(Stream &stream, Id buffer, std::uint32_t offset, std::span<std::uint8_t> into, std::uint32_t &word, std::uint32_t ticket) noexcept {
    word = ticket;
    Memory mem = stream.memory();
    return words(stream, op::MAP_ASYNC, {buffer, offset, static_cast<std::uint32_t>(into.size()), mem.off_of(into), mem.off_of(&word), ticket});
}

bool queue_done(Stream &stream, std::uint32_t &word, std::uint32_t ticket) noexcept {
    word = ticket;
    return words(stream, op::QUEUE_DONE, {stream.memory().off_of(&word), ticket});
}

bool configure(Stream &stream, std::uint32_t surface, std::optional<TextureFormat> format, std::uint32_t usage, CanvasAlphaMode alpha) noexcept {
    return words(stream, op::CONFIGURE, {surface, format ? u(*format) : SURFACE_PREFERRED, usage, u(alpha)});
}

bool resize(Stream &stream, std::uint32_t surface, std::array<std::uint32_t, 2> size) noexcept { return words(stream, op::RESIZE, {surface, size[0], size[1]}); }

bool device(Stream &stream, std::span<std::uint32_t> into) noexcept {
    return words(stream, op::DEVICE, {stream.memory().off_of(into), static_cast<std::uint32_t>(into.size())});
}

} // namespace xpute::gpu::call
