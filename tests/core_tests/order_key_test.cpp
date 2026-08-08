// The ordering feature rests on: byte-compare == logical order, and "insert between" never touching the neighbours.
#include <algorithm>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "core/infra/persistence/order_key.hpp"

using core::orderkey::between;
using core::orderkey::isValid;

static int ok_pass = 0, ok_fail = 0;
#define OK_CHECK(cond, msg)                                                                        \
  do {                                                                                             \
    if (cond) { ++ok_pass; }                                                                       \
    else { ++ok_fail; std::printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); }             \
  } while (0)

namespace {

bool charsetOk(const std::string& k) {
  for (char c : k)
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) return false;
  return !k.empty();
}

void testInsertMiddle() {
  std::string lo = between("", "");
  std::string hi = between(lo, "");
  for (int i = 0; i < 1000; ++i) {
    std::string mid = between(lo, hi);
    if (!(lo < mid && mid < hi)) { OK_CHECK(false, "midpoint not between neighbours"); return; }
    if (!isValid(mid) || !charsetOk(mid)) { OK_CHECK(false, "midpoint invalid"); return; }
    (i % 2) ? (lo = mid) : (hi = mid);   // squeeze the gap from both sides
  }
  OK_CHECK(true, "1000 middle inserts stay ordered + valid");
}

// Appending at the end is the most common op (new request) — the key must NOT grow linearly.
void testAppendEnd() {
  std::string k = between("", "");
  std::string prev;
  std::size_t maxLen = k.size();
  for (int i = 0; i < 1000; ++i) {
    prev = k;
    k = between(prev, "");
    if (!(prev < k)) { OK_CHECK(false, "append not increasing"); return; }
    if (!isValid(k)) { OK_CHECK(false, "append produced invalid key"); return; }
    maxLen = std::max(maxLen, k.size());
  }
  OK_CHECK(true, "1000 appends stay increasing");
  OK_CHECK(maxLen <= 6, "append keeps keys short (integer part works)");
}

// Prepending walks the integer part down into the 'A'..'Z' (negative) range.
void testPrependStart() {
  std::string k = between("", "");
  std::string next;
  std::size_t maxLen = k.size();
  bool sawNegative = false;
  for (int i = 0; i < 1000; ++i) {
    next = k;
    k = between("", next);
    if (!(k < next)) { OK_CHECK(false, "prepend not decreasing"); return; }
    if (!isValid(k)) { OK_CHECK(false, "prepend produced invalid key"); return; }
    if (k[0] >= 'A' && k[0] <= 'Z') sawNegative = true;
    maxLen = std::max(maxLen, k.size());
  }
  OK_CHECK(true, "1000 prepends stay decreasing");
  OK_CHECK(sawNegative, "prepend reaches the negative integer range");
  OK_CHECK(maxLen <= 6, "prepend keeps keys short");
}

void testSortMatchesInsertOrder() {
  std::mt19937 rng(12345);
  std::vector<std::string> keys{between("", "")};
  for (int i = 0; i < 2000; ++i) {
    std::size_t at = rng() % (keys.size() + 1);
    std::string lo = at > 0 ? keys[at - 1] : std::string();
    std::string hi = at < keys.size() ? keys[at] : std::string();
    keys.insert(keys.begin() + static_cast<long>(at), between(lo, hi));
  }
  std::vector<std::string> shuffled = keys;
  std::shuffle(shuffled.begin(), shuffled.end(), rng);
  std::sort(shuffled.begin(), shuffled.end());           // plain byte compare
  OK_CHECK(shuffled == keys, "byte sort == insertion order (2000 random inserts)");
  bool uniq = std::adjacent_find(keys.begin(), keys.end()) == keys.end();
  OK_CHECK(uniq, "no duplicate keys");
}

void testValidation() {
  OK_CHECK(!isValid(""), "empty key invalid");
  OK_CHECK(!isValid("a"), "truncated integer part invalid");
  OK_CHECK(!isValid("!0"), "bad head invalid");
  OK_CHECK(!isValid("a0+"), "non-digit char invalid");
  OK_CHECK(!isValid("a0V0"), "fractional part ending in '0' invalid");
  OK_CHECK(isValid("a0"), "first key valid");
  OK_CHECK(isValid("a0V"), "key with fraction valid");
  OK_CHECK(isValid("Zz"), "negative-range key valid");
  OK_CHECK(between("", "") == "a0", "first key is a0");
}

} // namespace

int run_order_key_tests() {
  std::printf("[order_key]\n");
  testValidation();
  testInsertMiddle();
  testAppendEnd();
  testPrependStart();
  testSortMatchesInsertOrder();
  std::printf("  order_key: %d passed, %d failed\n", ok_pass, ok_fail);
  return ok_fail;
}
