// MIT License
//
// Copyright (c) 2025 canplane
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

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

int hex_val(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int b64url_val(char c) noexcept {
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
    if (in.size() % 2 != 0) return make_error(Errno::ebadmsg);
    ensure(out.size() >= in.size() / 2, Errno::enospc, out.size(), in.size());
    for (std::size_t i = 0; i < in.size(); i += 2) {
        int hi = hex_val(in[i]);
        int lo = hex_val(in[i + 1]);
        if (hi < 0 || lo < 0) return make_error(Errno::ebadmsg);
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
    // A final group of one character carries only six bits, less than a
    // byte, so no encoding ends that way.
    if (in.size() % 4 == 1) return make_error(Errno::ebadmsg);
    std::size_t bytes = in.size() / 4 * 3 + (in.size() % 4 == 0 ? 0 : in.size() % 4 - 1);
    ensure(out.size() >= bytes, Errno::enospc, out.size(), bytes);
    std::size_t at = 0;
    for (std::size_t i = 0; i < in.size(); i += 4) {
        std::size_t n = in.size() - i < 4 ? in.size() - i : 4;
        std::uint32_t w = 0;
        for (std::size_t k = 0; k < 4; k++) {
            int v = k < n ? b64url_val(in[i + k]) : 0;
            if (v < 0) return make_error(Errno::ebadmsg);
            w = w << 6 | static_cast<std::uint32_t>(v);
        }
        // Bits past the last byte are zero in any canonical encoding.
        if ((n == 2 && (w & 0xffff) != 0) || (n == 3 && (w & 0xff) != 0)) return make_error(Errno::ebadmsg);
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

// The grammar check recurses once for each level of nesting, and a module's
// stack has a fixed size, so a deeper text is refused.
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
        // Every byte of a multi-byte UTF-8 sequence is above 0x7F and so is
        // never a quote or a backslash; a string can be walked a byte at a
        // time.
        at++;
    }
}

// Follows JSON's number grammar: `+1`, `.5`, `1.` and `01` are refused, as the
// runtime's `JSON.parse` refuses them.
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

static std::optional<std::size_t> skip_val(std::string_view s, std::size_t at, std::uint32_t depth) noexcept {
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
            std::optional<std::size_t> end = skip_val(s, i, depth + 1);
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
            std::optional<std::size_t> end = skip_val(s, skip_ws(s, i + 1), depth + 1);
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
    std::optional<std::size_t> end = skip_val(text, start, 0);
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

// The value of a number beyond the range of doubles, which `from_chars`
// refuses. `JSON.parse` and Rust's `parse` read a magnitude above the largest
// double as an infinity and one below the least as zero, and so does this; it
// tells the two apart by the decimal place of the leading digit plus the
// exponent.
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
        // The exponent is clamped: beyond this size it decides the result
        // alone, whatever the mantissa.
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

JsonItemIter JsonValue::items() const noexcept {
    return JsonItemIter(s_, type_of() == JsonType::array ? 1 : s_.size());
}

JsonFieldIter JsonValue::fields() const noexcept {
    return JsonFieldIter(s_, type_of() == JsonType::object ? 1 : s_.size());
}

std::optional<JsonValue> JsonValue::get(std::string_view key) const noexcept {
    std::optional<JsonValue> last;
    JsonFieldIter f = fields();
    while (auto field = f.next()) {
        if (field->first.eq_str(key)) last = field->second;
    }
    return last;
}

std::optional<JsonValue> JsonValue::at(std::size_t idx) const noexcept {
    JsonItemIter it = items();
    for (std::size_t i = 0;; i++) {
        std::optional<JsonValue> v = it.next();
        if (!v || i == idx) return v;
    }
}

std::optional<JsonValue> JsonItemIter::next() noexcept {
    std::size_t start = skip_ws(s_, at_);
    if (start >= s_.size() || s_[start] == ']') {
        at_ = s_.size();
        return std::nullopt;
    }
    // `read_json` checked the whole text, so a value starts here and the
    // skip cannot fail.
    std::size_t end = *skip_val(s_, start, 0);
    at_ = skip_ws(s_, end) + 1;
    return JsonValue(s_.substr(start, end - start));
}

std::optional<std::pair<JsonStr, JsonValue>> JsonFieldIter::next() noexcept {
    std::size_t key_at = skip_ws(s_, at_);
    if (key_at >= s_.size() || s_[key_at] != '"') {
        at_ = s_.size();
        return std::nullopt;
    }
    std::size_t key_end = *skip_string(s_, key_at);
    std::size_t val_at = skip_ws(s_, skip_ws(s_, key_end) + 1);
    std::size_t val_end = *skip_val(s_, val_at, 0);
    at_ = skip_ws(s_, val_end) + 1;
    return std::pair{JsonStr(s_.substr(key_at + 1, key_end - key_at - 2)), JsonValue(s_.substr(val_at, val_end - val_at))};
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

std::optional<char32_t> JsonCharIter::next() noexcept {
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
    JsonCharIter c = chars();
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
#include <cstdarg>
#include <cstdlib>

namespace xpute {

namespace {
std::atomic<Report> installed_report{nullptr};
}

void set_report(Report report) noexcept {
    Report none = nullptr;
    installed_report.compare_exchange_strong(none, report);
}

#ifndef __wasm__
sigjmp_buf *&bug_detail::catcher() noexcept {
    thread_local sigjmp_buf *innermost = nullptr;
    return innermost;
}
#endif

void bug(Errno errnum, std::uint64_t a, std::uint64_t b, const char *msg, std::source_location at) noexcept {
    if (Report report = installed_report.load()) report(errnum, at, a, b, msg);
#ifndef __wasm__
    if (sigjmp_buf *c = bug_detail::catcher()) siglongjmp(*c, 1);
#endif
    std::abort();
}

} // namespace xpute

namespace {
// The bytes of a libc++ check's message: its file, its line, the expression
// and what it says of it, a longer one cut.
constexpr std::size_t ABORT_MSG_CAP = 512;

// The bug of a failed check of libc++'s own, with the check's message as
// `fmt` and `args` write it. The message is written into one static buffer,
// so that it is storage nothing allocates, as a bug's message is, one a
// thread, as a native bug is caught on its own thread. It is
// written here and not by `vsnprintf`, which links WASI's file calls into a
// module: a string, a character and an integer of any length are written, and
// from a conversion that is none of them on, `fmt` goes as it is, since
// nothing says how wide its argument is.
[[noreturn]] void abort_bug(const char *fmt, std::va_list args) noexcept {
    static thread_local char msg[ABORT_MSG_CAP];
    std::size_t n = 0;
    auto put = [&](char c) {
        if (n + 1 < ABORT_MSG_CAP) msg[n++] = c;
    };
    auto number = [&](unsigned long long v, unsigned base) {
        char digits[24];
        std::size_t k = 0;
        do digits[k++] = "0123456789abcdef"[v % base];
        while ((v /= base) != 0);
        while (k != 0) put(digits[--k]);
    };
    const char *f = fmt;
    while (*f != '\0') {
        if (*f != '%') {
            put(*f++);
            continue;
        }
        const char *spec = f + 1;
        int length = 0; // 0 an int, 1 a long, 2 a long long, 3 a size_t
        for (; *spec == 'l' || *spec == 'z'; spec++) length = *spec == 'z' ? 3 : length + 1;
        auto is_signed = *spec == 'd' || *spec == 'i';
        if (*spec == '%') {
            put('%');
        } else if (*spec == 's' && length == 0) {
            const char *str = va_arg(args, const char *);
            for (; str != nullptr && *str != '\0'; str++) put(*str);
        } else if (*spec == 'c' && length == 0) {
            put(static_cast<char>(va_arg(args, int)));
        } else if ((is_signed || *spec == 'u' || *spec == 'x') && length <= 3) {
            unsigned long long v;
            if (length == 0) {
                v = is_signed ? static_cast<unsigned long long>(va_arg(args, int)) : va_arg(args, unsigned);
            } else if (length == 1) {
                v = is_signed ? static_cast<unsigned long long>(va_arg(args, long)) : va_arg(args, unsigned long);
            } else if (length == 2) {
                v = va_arg(args, unsigned long long);
            } else {
                v = va_arg(args, std::size_t);
            }
            if (is_signed && static_cast<long long>(v) < 0) {
                put('-');
                v = 0 - v;
            }
            number(v, *spec == 'x' ? 16 : 10);
        } else {
            for (; *f != '\0'; f++) put(*f);
            break;
        }
        f = spec + 1;
    }
    msg[n] = '\0';
    xpute::bug(xpute::Errno::enotrecoverable, 0, 0, msg);
}
} // namespace

void xpute_verbose_abort(const char *fmt, ...) noexcept {
    std::va_list args;
    va_start(args, fmt);
    abort_bug(fmt, args);
}

#ifdef __wasm__
// libc++'s compiled library calls its own handler, not the macro, and its
// definition prints to stderr. On WebAssembly that imports WASI's file calls,
// which would give the module a second way to reach the host beside the
// contract, so a module's handler is the bug as well.
void std::__libcpp_verbose_abort(char const *fmt, ...) noexcept {
    std::va_list args;
    va_start(args, fmt);
    abort_bug(fmt, args);
}

// libc++abi's `__cxa_pure_virtual`, which an abstract class's vtable names,
// prints to stderr in the same way, so it is replaced as well.
extern "C" void __cxa_pure_virtual() {
    xpute::bug(xpute::Errno::enotrecoverable);
}

// So is libc++abi's `__abort_message`, which its type information's vtables
// reach, and which a class over one of libc++'s, as the kit's `HeapMalloc`
// memory resource is, links in.
extern "C" void __abort_message(const char *fmt, ...) {
    std::va_list args;
    va_start(args, fmt);
    abort_bug(fmt, args);
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
        if (!has(sizeof(T))) return make_error(Errno::ebadmsg);
        return std::optional<Value>(le<T>());
    };
    auto run = [this]() -> Result<std::span<const std::uint8_t>> {
        if (!has(4)) return make_error(Errno::ebadmsg);
        std::uint32_t len = le<std::uint32_t>();
        if (!has(len)) return make_error(Errno::ebadmsg);
        std::span<const std::uint8_t> bytes = buf_.subspan(off_, len);
        off_ += len;
        return bytes;
    };
    switch (tag) {
    case Tag::end:
        end_ = End::closed;
        return std::optional<Value>();
    case Tag::boolean: {
        if (!has(1)) return make_error(Errno::ebadmsg);
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
        return std::optional<Value>(Blob{*r});
    }
    }
    return make_error(Errno::ebadmsg);
}

} // namespace xpute::tlv

// cpp/xpute/kit/wire/xtp/cursor.cpp

namespace xpute::xtp {

template <class T> Result<T> Cursor::get_as() const noexcept {
    Result<Value> v = get();
    if (!v) return v.error();
    if (const T *x = std::get_if<T>(&*v)) return *x;
    return make_error(Errno::ebadmsg);
}

template <class T> Result<Value> Cursor::scalar() const noexcept {
    if (std::uint64_t{base_} + sizeof(T) > pkt_.size()) return make_error(Errno::ebadmsg);
    return Value(load<T>(pkt_.data() + base_));
}

Result<Cursor> Cursor::at(std::span<const std::uint8_t> pkt, std::uint32_t base, NodeType type) noexcept {
    Result<std::uint32_t> unit = align_sz(type);
    if (!unit) return unit.error();
    if (base % *unit != 0) return make_error(Errno::ebadmsg);
    if (type != type_of(SpecialType::branch)) return Cursor(pkt, base, type);
    if (std::uint64_t{base} + WORD_SZ > pkt.size()) return make_error(Errno::ebadmsg);
    Result<std::uint32_t> len = get_len(pkt, base);
    if (!len) return len.error();
    if (*len > (pkt.size() - base - WORD_SZ) / WORD_SZ) return make_error(Errno::ebadmsg);
    return Cursor(pkt, base, type, *len);
}

Result<BranchCursor> Cursor::as_branch() const noexcept {
    if (is_null()) return make_error(Errno::efault);
    if (!is_branch()) return make_error(Errno::ebadmsg);
    return BranchCursor(*this);
}

Result<Array> Cursor::array(std::uint32_t elem) const noexcept {
    std::uint64_t payload = std::uint64_t{base_} + WORD_SZ;
    if (payload > pkt_.size()) return make_error(Errno::ebadmsg);
    Result<std::uint32_t> len = get_len(pkt_, base_);
    if (!len) return len.error();
    std::uint64_t bytes = std::uint64_t{*len} * elem;
    if (bytes > pkt_.size() - payload) return make_error(Errno::ebadmsg);
    return Array{static_cast<SequenceType>(type_), pkt_.subspan(payload, bytes)};
}

Result<Bitset> Cursor::bitset() const noexcept {
    std::uint64_t payload = std::uint64_t{base_} + WORD_SZ;
    if (payload > pkt_.size()) return make_error(Errno::ebadmsg);
    Result<std::uint32_t> len = get_len(pkt_, base_);
    if (!len) return len.error();
    std::uint64_t bytes = (std::uint64_t{*len} + 7) >> 3;
    if (bytes > pkt_.size() - payload) return make_error(Errno::ebadmsg);
    return Bitset{pkt_.subspan(payload, bytes), *len};
}

// A str must end with the NUL that `len` does not count, and its bytes must
// be UTF-8.
Result<std::string_view> Cursor::str() const noexcept {
    std::uint64_t payload = std::uint64_t{base_} + WORD_SZ;
    if (payload > pkt_.size()) return make_error(Errno::ebadmsg);
    Result<std::uint32_t> nbyte = get_len(pkt_, base_);
    if (!nbyte) return nbyte.error();
    if (std::uint64_t{*nbyte} + 1 > pkt_.size() - payload) return make_error(Errno::ebadmsg);
    if (pkt_[payload + *nbyte] != 0) return make_error(Errno::ebadmsg);
    std::string_view s(reinterpret_cast<const char *>(pkt_.data() + payload), *nbyte);
    if (!is_utf8(s)) return make_error(Errno::ebadmsg);
    return s;
}

Result<std::string_view> Cursor::get_str_ref() const noexcept {
    if (is_null() || type_ != type_of(SequenceType::str)) return make_error(Errno::ebadmsg);
    return str();
}

Result<StrList> Cursor::strs() const noexcept {
    std::uint64_t payload = std::uint64_t{base_} + WORD_SZ;
    if (payload > pkt_.size()) return make_error(Errno::ebadmsg);
    Result<std::uint32_t> len = get_len(pkt_, base_);
    if (!len) return len.error();
    std::uint32_t n = *len;
    std::uint64_t blob = payload + 4 * (std::uint64_t{n} + 1);
    if (blob > pkt_.size()) return make_error(Errno::ebadmsg);
    auto off = [&](std::uint32_t i) { return load<std::uint32_t>(pkt_.data() + payload + 4 * i); };
    std::uint32_t total = off(n);
    if (total > pkt_.size() - blob) return make_error(Errno::ebadmsg);
    for (std::uint32_t i = 0; i < n; i++) {
        if (off(i) > off(i + 1) || off(i + 1) > total) return make_error(Errno::ebadmsg);
        if (!is_utf8(std::string_view(reinterpret_cast<const char *>(pkt_.data() + blob + off(i)), off(i + 1) - off(i)))) return make_error(Errno::ebadmsg);
    }
    return StrList{pkt_.subspan(payload, 4 * (std::uint64_t{n} + 1)), pkt_.subspan(blob, total), n};
}

Result<StrList> Cursor::get_strs() const noexcept {
    if (is_null() || type_ != type_of(SequenceType::strs)) return make_error(Errno::ebadmsg);
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
    return make_error(Errno::ebadmsg);
}

Result<std::uint64_t> Cursor::get_u64() const noexcept {
    return get_as<std::uint64_t>();
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
            else return make_error(Errno::ebadmsg);
        },
        *v);
}

Result<Cursor> BranchCursor::at(std::uint32_t idx) const noexcept {
    if (idx >= len()) return make_error(Errno::efault);
    Result<Desc> d = get_desc(node_.pkt_, base() + WORD_SZ + idx * WORD_SZ);
    if (!d) return d.error();
    auto [rel_off, type] = *d;
    if (rel_off == 0) return Cursor(node_.pkt_, 0, type);
    if (rel_off >= node_.pkt_.size() - base()) return make_error(Errno::ebadmsg);
    std::uint32_t child = base() + rel_off;
    std::uint32_t end = end_of(idx);
    if (child < base() + WORD_SZ + len() * WORD_SZ || child >= end) return make_error(Errno::ebadmsg);
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
    if (pkt.size() < HDR_SZ) return make_error(Errno::ebadmsg);
    Result<std::uint32_t> magic = get_len(pkt, 0);
    if (!magic || *magic != MAGIC) return make_error(Errno::ebadmsg);
    Result<Desc> d = get_desc(pkt, WORD_SZ);
    if (!d) return d.error();
    auto [payload_sz, type] = *d;
    if (payload_sz > pkt.size() - HDR_SZ) return make_error(Errno::ebadmsg);
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
    if (buf_.size() < HDR_SZ) err_ = make_error(Errno::einval);
    else std::memset(buf_.data(), 0, HDR_SZ);
}

std::optional<std::uint32_t> PacketWriter::spoil(Errno errnum) noexcept {
    if (!err_) err_ = make_error(errnum);
    return std::nullopt;
}

std::optional<std::uint32_t> PacketWriter::reserve(std::uint32_t unit, std::uint64_t bytes) noexcept {
    if (err_) return std::nullopt;
    std::uint64_t base = (std::uint64_t{lim_} + unit - 1) & ~std::uint64_t{unit - 1};
    std::uint64_t end = base + bytes;
    if (end > buf_.size()) return spoil(Errno::eoverflow);
    std::memset(buf_.data() + lim_, 0, end - lim_);
    lim_ = static_cast<std::uint32_t>(end);
    return static_cast<std::uint32_t>(base);
}

PacketWriter &PacketWriter::entry(std::optional<std::uint32_t> at, NodeType type) noexcept {
    ensure(table_.written < table_.cnt, Errno::enospc, table_.cnt);
    if (table_.root) root_type_ = type;
    else if (!err_) set_word(buf_, table_.base + WORD_SZ + table_.written * WORD_SZ, at ? *at - table_.base : 0, type);
    table_.written++;
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
    if (err_) return std::nullopt;
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
    // Copied through `str_with`, as a string written in place is, so that the
    // two fit and lay out alike.
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

std::optional<std::uint32_t> PacketWriter::branch_open(std::uint32_t cnt) noexcept {
    std::optional<std::uint32_t> at = reserve(WORD_SZ, WORD_SZ + std::uint64_t{cnt} * WORD_SZ);
    if (at) set_word(buf_, *at, cnt, RESERVED);
    return at;
}

// A branch's extent ends on a word boundary, so a node after it starts on the
// next word even when it needs less alignment. Every language pads the same
// way, which keeps their bytes equal.
std::optional<std::uint32_t> PacketWriter::branch_close(std::optional<std::uint32_t> at) noexcept {
    if (!at || err_) return at;
    std::uint64_t end = word_up(lim_);
    if (end > buf_.size()) return spoil(Errno::eoverflow);
    std::memset(buf_.data() + lim_, 0, end - lim_);
    lim_ = static_cast<std::uint32_t>(end);
    return at;
}

PacketWriter &PacketWriter::graft(std::span<const std::uint8_t> pkt) noexcept {
    std::optional<std::uint32_t> at;
    NodeType type = type_of(SpecialType::nil);
    Result<std::uint32_t> magic = pkt.size() < HDR_SZ ? make_error(Errno::ebadmsg) : get_len(pkt, 0);
    Result<Desc> d = magic && *magic == MAGIC ? get_desc(pkt, WORD_SZ) : make_error(Errno::ebadmsg);
    if (!d) {
        spoil(Errno::ebadmsg);
    } else {
        auto [payload_sz, desc_type] = *d;
        type = desc_type;
        if (payload_sz > pkt.size() - HDR_SZ) {
            spoil(Errno::ebadmsg);
        } else if (payload_sz != 0) {
            Result<std::uint32_t> unit = align_sz(type);
            if (!unit || payload_sz % *unit != 0) {
                spoil(Errno::ebadmsg);
            } else if ((at = reserve(*unit, payload_sz))) {
                std::memcpy(buf_.data() + *at, pkt.data() + HDR_SZ, payload_sz);
            }
        }
    }
    return entry(at, type);
}

Result<std::uint32_t> PacketWriter::finish() noexcept {
    ensure(table_.root && table_.written == table_.cnt, Errno::enotrecoverable, table_.cnt, table_.written);
    if (err_) return *err_;
    std::uint64_t end = word_up(lim_);
    if (end > buf_.size()) return make_error(Errno::eoverflow);
    std::memset(buf_.data() + lim_, 0, end - lim_);
    set_word(buf_, 0, MAGIC, RESERVED);
    set_word(buf_, WORD_SZ, lim_ - HDR_SZ, root_type_);
    return static_cast<std::uint32_t>(end);
}

} // namespace xpute::xtp

// cpp/xpute/link/link.cpp

namespace xpute::link {

namespace {

constexpr std::int32_t status(Errno e) noexcept {
    return static_cast<std::int32_t>(e);
}

// Whether `a` and `b` are one token.
bool same(const Token &a, const Token &b) noexcept {
    if (a.variant.index() != b.variant.index()) return false;
    if (a.variant.index() == 0) return std::get<0>(a.variant).val == std::get<0>(b.variant).val;
    return std::get<1>(a.variant).val == std::get<1>(b.variant).val;
}

// Whether `uri` starts with `prefix`.
bool starts_with(std::span<const Token> uri, std::span<const Token> prefix) noexcept {
    if (prefix.size() > uri.size()) return false;
    for (std::size_t k = 0; k < prefix.size(); k++)
        if (!same(uri[k], prefix[k])) return false;
    return true;
}

// A copy of a packet, which its answer owns.
std::optional<std::vector<std::uint8_t>> copy(std::optional<std::span<const std::uint8_t>> pkt) {
    if (!pkt) return std::nullopt;
    return std::vector<std::uint8_t>(pkt->begin(), pkt->end());
}

} // namespace

void Side::mount(Handle h, std::vector<Token> prefix, Opener open) {
    mounts_.emplace_back(h, std::move(prefix), std::move(open));
}

void Side::mount_named(Handle h, std::unique_ptr<Object> o) {
    bool fresh = named_.emplace(h, std::move(o)).second;
    ensure(fresh, Errno::eexist, h);
}

Result<Handle> Side::open(std::span<const Token> uri) {
    Args args = Packet{8, [uri](Lower &w) { store_uri(w, 0, uri); }};
    auto [result, data] = call(ROOT, OPEN, args);
    return answer(result, data, [](Lift &r) { return r.get<std::uint32_t>(0); });
}

void Side::drop(Handle h) {
    send(h, DROP, std::nullopt, nullptr);
}

std::pair<std::int32_t, std::optional<std::span<const std::uint8_t>>> Side::call(Handle h, Method m, const Args &args) {
    auto reply = out(tag(), h, m, false, 0, args);
    ensure(reply.first != status(Errno::einprogress), Errno::einprogress, h, m);
    return reply;
}

Tag Side::send(Handle h, Method m, const Args &args, Answer answer) {
    Tag t = tag();
    if (answer) pending_.emplace(t, std::move(answer));
    auto [result, data] = out(t, h, m, false, 0, args);
    if (result != status(Errno::einprogress)) {
        if (auto it = pending_.find(t); it != pending_.end()) {
            Answer a = std::move(it->second);
            pending_.erase(it);
            due_.push_back(Due{std::move(a), result, copy(data)});
        }
    }
    return t;
}

void Side::cancel(Tag t) {
    if (pending_.contains(t)) send(ROOT, CANCEL, Packet{4, [t](Lower &w) { w.put<std::uint32_t>(0, t); }}, nullptr);
}

void Side::on_cancel(Tag t, std::function<void()> stop) {
    cancels_.insert_or_assign(t, std::move(stop));
}

void Side::task_return(Tag t, std::int32_t result, const Data &data) {
    cancels_.erase(t);
    if (!crossing_) {
        out(t, ROOT, 0, true, result, data);
        return;
    }
    if (!data) {
        completions_.push_back(Completion{t, result, std::nullopt});
        return;
    }
    std::vector<std::uint8_t> buf(layout().cap - HDR_SZ);
    Lower w(buf, data->fixed_sz);
    data->write(w);
    Result<std::size_t> n = w.finish();
    if (!n) {
        completions_.push_back(Completion{t, status(n.error().errnum), std::nullopt});
        return;
    }
    buf.resize(*n);
    completions_.push_back(Completion{t, result, std::move(buf)});
}

std::int32_t Side::recv(std::uint64_t at) {
    std::uint32_t cap = layout().cap;
    Frame f = Frame::at(at);
    Result<std::optional<std::span<const std::uint8_t>>> pkt = f.pkt(at, cap);
    if (f.res) {
        if (auto it = pending_.find(f.tag); it != pending_.end()) {
            Answer a = std::move(it->second);
            pending_.erase(it);
            due_.push_back(Due{std::move(a), f.result, pkt ? copy(*pkt) : std::nullopt});
        }
        return 0;
    }
    // The args and the answer lie in the frame's packet, the answer written
    // once the args are read.
    Lower w(mem::bytes(at + HDR_SZ, cap - HDR_SZ));
    Frame words = f;
    words.res = true;
    words.result = pkt ? run(f.handle, f.method, f.tag, Lift((*pkt).value_or(std::span<const std::uint8_t>{})), w)
                       : status(pkt.error().errnum);
    // An answer past the cap is refused as a later one is (`out`).
    Result<std::size_t> n = w.finish();
    words.pkt_len = n ? static_cast<std::uint32_t>(*n) : 0;
    if (!n) words.result = status(n.error().errnum);
    words.lay(at);
    return words.result;
}

void Side::run_due() {
    while (!completions_.empty()) {
        Completion c = std::move(completions_.front());
        completions_.pop_front();
        Args pkt;
        if (c.pkt) {
            const std::vector<std::uint8_t> &bytes = *c.pkt;
            pkt = Packet{bytes.size(), [&bytes](Lower &w) {
                             for (std::size_t k = 0; k < bytes.size(); k++) w.put<std::uint8_t>(k, bytes[k]);
                         }};
        }
        out(c.tag, ROOT, 0, true, c.result, pkt);
    }
    while (!due_.empty()) {
        Due d = std::move(due_.front());
        due_.pop_front();
        d.answer(d.result, std::move(d.data));
    }
}

std::int32_t Side::run(Handle h, Method m, Tag tag, Lift args, Lower &ret) {
    if (h == ROOT && m == OPEN) {
        Result<Handle> o = opened(args);
        if (!o) return status(o.error().errnum);
        ret.start(4);
        ret.put<std::uint32_t>(0, *o);
        return 0;
    }
    if (h == ROOT && m == CANCEL) {
        Tag t = args.get<std::uint32_t>(0);
        if (!args.finish()) return status(Errno::ebadmsg);
        if (auto it = cancels_.find(t); it != cancels_.end()) {
            std::function<void()> stop = std::move(it->second);
            cancels_.erase(it);
            stop();
        }
        return 0;
    }
    if (m == DROP) return objects_.remove(h) ? 0 : status(Errno::ebadf);
    // A named handle no interface is mounted under is no object there; an
    // issued one no longer held is a bad handle. Nothing drops an object
    // during its call: the host's drop comes only in an entry of its own.
    if ((h & NAMED) != 0) {
        auto it = named_.find(h);
        if (it == named_.end()) return status(Errno::enoent);
        return it->second->call(m, tag, args, ret);
    }
    std::unique_ptr<Object> *o = objects_.get(h);
    if (o == nullptr) return status(Errno::ebadf);
    return (*o)->call(m, tag, args, ret);
}

Result<Handle> Side::opened(Lift &args) {
    std::vector<Token> uri = load_uri(args, 0);
    if (Result<void> read = args.finish(); !read) return read.error();
    // The longest matching mount wins, the first mounted of those as long.
    const Opener *open = nullptr;
    std::size_t longest = 0;
    for (const auto &[i, prefix, o] : mounts_) {
        if (starts_with(uri, prefix) && (open == nullptr || prefix.size() > longest)) {
            open = &o;
            longest = prefix.size();
        }
    }
    if (open == nullptr) return make_error(Errno::enoent);
    // A copy, as an opener may mount another.
    Opener opener = *open;
    Handle h = objects_.issue();
    Result<std::unique_ptr<Object>> o = opener(uri, h);
    if (!o) {
        objects_.remove(h);
        return o.error();
    }
    objects_.put(h, std::move(*o));
    return h;
}

Tag Side::tag() noexcept {
    Tag t = next_;
    while (pending_.contains(t)) t++;
    next_ = t + 1;
    return t;
}

Result<void> Side::lay(std::uint64_t at, Frame words, const Args &pkt) {
    std::uint32_t cap = layout().cap;
    words.pkt_len = 0;
    if (pkt) {
        Lower w(mem::bytes(at + HDR_SZ, cap - HDR_SZ), pkt->fixed_sz);
        pkt->write(w);
        Result<std::size_t> n = w.finish();
        if (!n) return n.error();
        words.pkt_len = static_cast<std::uint32_t>(*n);
    }
    words.lay(at);
    return {};
}

// Lays a frame in this side's box and crosses with it, and answers what the
// callee laid there: its result, and its return data. A later answer past
// the cap is answered EMSGSIZE with no data, so its request still hears;
// args this side writes past it are a bug.
std::pair<std::int32_t, std::optional<std::span<const std::uint8_t>>> Side::out(Tag tag, Handle h, Method m, bool res, std::int32_t result, const Args &pkt) {
    Layout l = layout();
    std::uint64_t at = out_box_(l);
    Frame words{.tag = tag, .res = res, .handle = h, .method = m, .result = result, .pkt_len = 0};
    if (Result<void> laid = lay(at, words, pkt); !laid) {
        ensure(res, Errno::emsgsize, h, m);
        words.result = status(laid.error().errnum);
        ensure(lay(at, words, std::nullopt).ok(), Errno::emsgsize);
    }
    std::int32_t r = crossed([&] { return cross_(at); });
    Frame f = Frame::at(at);
    std::optional<std::span<const std::uint8_t>> data;
    if (f.res && !res) {
        if (auto p = f.pkt(at, l.cap); p) data = *p;
    }
    return {r, data};
}

std::vector<Token> uri(std::initializer_list<std::string_view> names) {
    std::vector<Token> out;
    for (std::string_view n : names) out.push_back(Token{Token::Str{std::string(n)}});
    return out;
}

void store_uri(Lower &w, std::size_t at, std::span<const Token> uri) {
    if (auto base = w.list(at, uri.size(), Token::SZ, Token::ALIGN))
        for (std::size_t k = 0; k < uri.size(); k++) uri[k].store(w, *base + k * Token::SZ);
}

std::vector<Token> load_uri(Lift &r, std::size_t at) {
    auto [base, len] = r.list(at, Token::SZ, Token::ALIGN);
    std::vector<Token> uri;
    uri.reserve(len);
    for (std::size_t k = 0; k < len; k++) uri.push_back(Token::load(r, base + k * Token::SZ));
    return uri;
}

Result<void> answer(std::int32_t result) noexcept {
    if (result == 0) return {};
    return make_error(or_bug(of_Errno(result), Errno::eproto));
}

} // namespace xpute::link

// cpp/xpute/link/mem.cpp

namespace xpute::link::mem {

namespace {

// The byte at `at`, as a pointer.
std::uint8_t *ptr(std::uint64_t at) noexcept {
    ensure(at <= UINTPTR_MAX, Errno::efault);
    return reinterpret_cast<std::uint8_t *>(static_cast<std::uintptr_t>(at));
}

// The aligned word at `at` as an atomic.
std::atomic_ref<std::uint32_t> atomic(std::uint64_t at) noexcept {
    std::uint8_t *p = ptr(at);
    ensure(reinterpret_cast<std::uintptr_t>(p) % std::atomic_ref<std::uint32_t>::required_alignment == 0, Errno::efault, at);
    return std::atomic_ref<std::uint32_t>(*reinterpret_cast<std::uint32_t *>(p));
}

} // namespace

std::span<std::uint8_t> bytes(std::uint64_t at, std::uint32_t len) noexcept {
    return {ptr(at), len};
}

std::uint64_t detail::addr(const void *p) noexcept {
    return reinterpret_cast<std::uintptr_t>(p);
}

std::uint32_t word(std::uint64_t at) noexcept {
    std::uint32_t v;
    std::memcpy(&v, ptr(at), 4);
    return v;
}

void set_word(std::uint64_t at, std::uint32_t v) noexcept {
    std::memcpy(ptr(at), &v, 4);
}

std::uint32_t load_acquire(std::uint64_t at) noexcept {
    return atomic(at).load(std::memory_order_acquire);
}

void store_release(std::uint64_t at, std::uint32_t v) noexcept {
    atomic(at).store(v, std::memory_order_release);
}

} // namespace xpute::link::mem

// cpp/xpute/guest/clock.cpp

namespace xpute::guest::clock {

namespace {
constexpr double NS_PER_MS = 1e6;
}

double now() noexcept {
    return ms(or_bug(abi::clocks::monotonic_clock::now(), Errno::eproto));
}

double ms(abi::clocks::monotonic_clock::Mark mark) noexcept {
    return static_cast<double>(mark) / NS_PER_MS;
}

std::uint64_t mark(double ms) noexcept {
    return static_cast<std::uint64_t>(std::floor((ms > 0 ? ms : 0) * NS_PER_MS));
}

std::uint64_t duration(double ms) noexcept {
    return static_cast<std::uint64_t>(std::ceil((ms > 0 ? ms : 0) * NS_PER_MS));
}

double system() noexcept {
    abi::clocks::system_clock::Instant t = or_bug(abi::clocks::system_clock::now(), Errno::eproto);
    return static_cast<double>(t.seconds) * 1e3 + static_cast<double>(t.nanoseconds) / NS_PER_MS;
}

} // namespace xpute::guest::clock

// cpp/xpute/guest/link.cpp

// A module takes the host's `link_out` under `env` (TS `link.ts` `imports`);
// a native guest, from the host linked into its process.
#ifdef __wasm__
extern "C" __attribute__((import_module("env"), import_name("link_out"))) std::int32_t link_out(std::uint64_t at);
#else
extern "C" std::int32_t link_out(std::uint64_t at);
#endif

namespace xpute::guest {

namespace {

// The guest's own side: it lays in its outbox, past the inbox, and crosses
// by `link_out`. It is made by the first `init` and lasts as long as the
// guest, so no constructor, guard or destructor of a static runs for it: a
// module's would link the libc's, which reach WASI.
link::Side *the_side = nullptr;

link::Side &side() noexcept {
    ensure(the_side != nullptr, Errno::enotrecoverable);
    return *the_side;
}

// Whether the guest is in an entry.
bool entered = false;
// The program's start and end of an entry (`init`).
void (*enter_fn)() = [] {};
bool (*end_fn)() = [] { return false; };

// Starts an entry by the program's `enter`. Every entry ends by `leave`.
void enter() {
    ensure(!std::exchange(entered, true), Errno::ebusy);
    enter_fn();
}

// Ends an entry: runs the answers due and the program's `end`, which answers
// whether it left work, again until neither has work left, and ends the
// entry.
void leave() {
    for (;;) {
        side().run_due();
        bool again = end_fn();
        if (!again && !side().has_due()) break;
    }
    entered = false;
}

} // namespace

std::uint64_t init(std::uint32_t cap, void (*enter)(), bool (*end)()) noexcept {
    ensure(cap % 8 == 0 && cap >= link::HDR_SZ, Errno::einval, cap);
    if (the_side == nullptr) the_side = new link::Side([](link::Layout l) { return l.inbox + l.cap; }, link_out);
    // The layout (frame.json5 `LAYOUT`), then the inbox and the outbox, for as
    // long as the guest runs.
    std::size_t n = (4 * link::frame::LAYOUT::WORDS + 2 * cap) / 8;
    auto *words = static_cast<std::uint64_t *>(std::pmr::get_default_resource()->allocate(n * 8, 8));
    std::fill_n(words, n, 0);
    std::uint64_t at = link::mem::addr(words[0]);
    link::Layout l{at + 4 * link::frame::LAYOUT::WORDS, cap};
    l.lay(at);
    side().set_layout(l);
    enter_fn = enter;
    end_fn = end;
    return at;
}

void boot(void (*boot)()) {
    ensure(!std::exchange(entered, true), Errno::ebusy);
    boot();
    enter_fn();
    leave();
}

std::int32_t recv() {
    enter();
    std::int32_t result = side().recv(side().layout().inbox);
    leave();
    return result;
}

void mount(link::Handle h, std::vector<link::Token> prefix, link::Opener open) {
    side().mount(h, std::move(prefix), std::move(open));
}

void mount_named(link::Handle h, std::unique_ptr<link::Object> o) {
    side().mount_named(h, std::move(o));
}

link::Handle add(std::unique_ptr<link::Object> o) {
    return side().add(std::move(o));
}

Result<link::Handle> open(std::span<const link::Token> uri) {
    return side().open(uri);
}

void drop(link::Handle h) {
    side().drop(h);
}

std::pair<std::int32_t, std::optional<std::span<const std::uint8_t>>> call(link::Handle h, link::Method m, const link::Args &args) {
    return side().call(h, m, args);
}

link::Tag send(link::Handle h, link::Method m, const link::Args &args, link::Answer answer) {
    return side().send(h, m, args, std::move(answer));
}

void cancel(link::Tag t) {
    side().cancel(t);
}

void on_cancel(link::Tag t, std::function<void()> stop) {
    side().on_cancel(t, std::move(stop));
}

void task_return(link::Tag t, std::int32_t result, const link::Data &data) {
    side().task_return(t, result, data);
}

bool in_guest() noexcept {
    return entered && !side().crossing();
}

} // namespace xpute::guest

// cpp/xpute/guest/log.cpp

namespace xpute::guest {

void log(abi::log::Level level, std::string_view context, std::string_view msg) noexcept {
    (void)abi::log::log(level, context, msg);
}

} // namespace xpute::guest

// cpp/xpute/guest/stop.cpp

namespace xpute::guest::stop {

void hook() noexcept {
    set_report([](Errno errnum, const std::source_location &at, std::uint64_t n1, std::uint64_t n2, const char *msg) {
        if (in_guest())
            (void)abi::stop::stopped(static_cast<std::int32_t>(errnum), at.file_name(), at.line(), n1, n2, msg ? std::optional<std::string_view>(msg) : std::nullopt);
    });
}

} // namespace xpute::guest::stop
