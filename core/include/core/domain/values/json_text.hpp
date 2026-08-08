#pragma once

#include <string>
#include <utility>

namespace core::domain {

// Raw JSON string only — the domain never parses it; validity is checked by the IJsonValidator port.
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
