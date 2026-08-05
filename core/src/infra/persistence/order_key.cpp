#include "core/infra/persistence/order_key.hpp"

#include <stdexcept>

namespace core::orderkey {
namespace {

const std::string kDigits = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
constexpr char kZero = '0';
constexpr char kLast = 'z';
// Smallest representable integer part ("A" + 26 zeros): nothing can be decremented past it.
const std::string kSmallestInt = std::string("A") + std::string(26, kZero);

std::size_t digitIndex(char c) {
    auto p = kDigits.find(c);
    if (p == std::string::npos) throw std::invalid_argument("order key: bad digit");
    return p;
}

// The head char encodes both the sign and the length of the integer part:
// 'a'..'z' -> positive, len 2..27;  'A'..'Z' -> negative, len 2..27.
std::size_t integerLen(char head) {
    if (head >= 'a' && head <= 'z') return static_cast<std::size_t>(head - 'a') + 2;
    if (head >= 'A' && head <= 'Z') return static_cast<std::size_t>('Z' - head) + 2;
    throw std::invalid_argument("order key: bad head");
}

std::string integerPart(const std::string& key) {
    if (key.empty()) throw std::invalid_argument("order key: empty");
    std::size_t n = integerLen(key[0]);
    if (n > key.size()) throw std::invalid_argument("order key: truncated integer part");
    return key.substr(0, n);
}

void validateInteger(const std::string& i) {
    if (i.empty() || i.size() != integerLen(i[0])) throw std::invalid_argument("order key: bad integer");
}

// Shortest string strictly between a and b (b empty = +inf). Neither may end in '0'.
std::string midpoint(const std::string& a, const std::string& b) {
    if (!b.empty() && a >= b) throw std::invalid_argument("order key: a >= b");
    if ((!a.empty() && a.back() == kZero) || (!b.empty() && b.back() == kZero))
        throw std::invalid_argument("order key: trailing zero");
    if (!b.empty()) {   // share the common prefix, recurse on the rest
        std::size_t n = 0;
        while (n < b.size() && (n < a.size() ? a[n] : kZero) == b[n]) ++n;
        if (n > 0)
            return b.substr(0, n) + midpoint(n < a.size() ? a.substr(n) : std::string(), b.substr(n));
    }
    std::size_t da = a.empty() ? 0 : digitIndex(a[0]);
    std::size_t db = b.empty() ? kDigits.size() : digitIndex(b[0]);
    if (db - da > 1) return std::string(1, kDigits[(da + db + 1) / 2]);   // room -> one digit is enough
    if (b.size() > 1) return b.substr(0, 1);
    // consecutive digits, b has none left -> keep a's digit and go one level deeper
    return std::string(1, kDigits[da]) + midpoint(a.empty() ? std::string() : a.substr(1), std::string());
}

// "" = cannot go further (caller falls back to extending the fractional part).
std::string incrementInteger(const std::string& x) {
    validateInteger(x);
    char head = x[0];
    std::string digs = x.substr(1);
    bool carry = true;
    for (std::size_t i = digs.size(); carry && i > 0; --i) {
        std::size_t d = digitIndex(digs[i - 1]) + 1;
        if (d == kDigits.size()) digs[i - 1] = kZero;
        else { digs[i - 1] = kDigits[d]; carry = false; }
    }
    if (!carry) return std::string(1, head) + digs;
    if (head == 'Z') return std::string("a") + kZero;   // negative -> positive
    if (head == 'z') return "";                          // exhausted
    char h = static_cast<char>(head + 1);
    if (h > 'a') digs.push_back(kZero);                  // positive: magnitude grows
    else digs.pop_back();                                // negative: magnitude shrinks
    return std::string(1, h) + digs;
}

std::string decrementInteger(const std::string& x) {
    validateInteger(x);
    char head = x[0];
    std::string digs = x.substr(1);
    bool borrow = true;
    for (std::size_t i = digs.size(); borrow && i > 0; --i) {
        std::size_t d = digitIndex(digs[i - 1]);
        if (d == 0) digs[i - 1] = kLast;
        else { digs[i - 1] = kDigits[d - 1]; borrow = false; }
    }
    if (!borrow) return std::string(1, head) + digs;
    if (head == 'a') return std::string("Z") + kLast;    // positive -> negative
    if (head == 'A') return "";                          // exhausted
    char h = static_cast<char>(head - 1);
    if (h < 'Z') digs.push_back(kLast);
    else digs.pop_back();
    return std::string(1, h) + digs;
}

} // namespace

bool isValid(const std::string& key) {
    if (key.empty() || key == kSmallestInt) return false;
    for (char c : key)
        if (kDigits.find(c) == std::string::npos) return false;
    try {
        std::string i = integerPart(key);
        std::string f = key.substr(i.size());
        return f.empty() || f.back() != kZero;   // integer part MAY end in '0'; the fraction may not
    } catch (...) {
        return false;
    }
}

std::string between(const std::string& a, const std::string& b) {
    if (!a.empty() && !isValid(a)) throw std::invalid_argument("order key: invalid lower bound");
    if (!b.empty() && !isValid(b)) throw std::invalid_argument("order key: invalid upper bound");
    if (!a.empty() && !b.empty() && a >= b) throw std::invalid_argument("order key: a >= b");

    if (a.empty()) {
        if (b.empty()) return std::string("a") + kZero;   // first key ever
        std::string ib = integerPart(b);
        std::string fb = b.substr(ib.size());
        if (ib == kSmallestInt) return ib + midpoint(std::string(), fb);
        if (ib < b) return ib;                            // b has a fraction -> its integer part fits
        std::string res = decrementInteger(ib);
        if (res.empty()) throw std::runtime_error("order key: cannot decrement any further");
        return res;
    }
    if (b.empty()) {
        std::string ia = integerPart(a);
        std::string fa = a.substr(ia.size());
        std::string i = incrementInteger(ia);
        return i.empty() ? ia + midpoint(fa, std::string()) : i;
    }
    std::string ia = integerPart(a), fa = a.substr(ia.size());
    std::string ib = integerPart(b), fb = b.substr(ib.size());
    if (ia == ib) return ia + midpoint(fa, fb);
    std::string i = incrementInteger(ia);
    if (i.empty()) throw std::runtime_error("order key: cannot increment any further");
    if (i < b) return i;
    return ia + midpoint(fa, std::string());
}

} // namespace core::orderkey
