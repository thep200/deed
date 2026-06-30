// core/domain/values/json_text.hpp — JsonText value object (REFACTOR_SPEC §5.1).
// Holds a raw JSON string ONLY. The domain never parses JSON; structural validity is checked by the
// IJsonValidator port at the use-case layer. This keeps the golden rule (§0.4) intact.
#pragma once

#include <string>
#include <utility>

namespace core::domain {

class JsonText {
public:
  JsonText() = default;
  static JsonText of(std::string text) { return JsonText(std::move(text)); }
  static JsonText emptyObject() { return JsonText("{}"); }

  const std::string &text() const noexcept { return text_; }
  bool empty() const noexcept { return text_.empty(); }

  bool operator==(const JsonText &o) const { return text_ == o.text_; }
  bool operator!=(const JsonText &o) const { return text_ != o.text_; }

private:
  explicit JsonText(std::string text) : text_(std::move(text)) {}
  std::string text_;
};

} // namespace core::domain
