#include "core/infra/serialization/field_json.hpp"

#include <nlohmann/json.hpp>

namespace core::serial {
using nlohmann::json;

// Tag-level tokenizer, no parser. Anything unusual (CDATA/comments/DOCTYPE/mismatched nesting) ->
// return the input verbatim — a display aid must never corrupt what the server actually sent.
std::string formatXml(const std::string &text) {
  auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos || text[first] != '<') return text;
  if (text.find("<![CDATA[") != std::string::npos || text.find("<!--") != std::string::npos ||
      text.find("<!DOCTYPE") != std::string::npos)
    return text;

  std::string out;
  out.reserve(text.size() + text.size() / 4);
  int depth = 0;
  std::size_t i = first;
  auto indent = [&](int d) {
    out += '\n';
    out.append(static_cast<std::size_t>(d) * 2, ' ');   // one append, not one += per level
  };
  bool firstTag = true;
  while (i < text.size()) {
    std::size_t lt = text.find('<', i);
    if (lt == std::string::npos) break;
    // Inter-tag text: keep non-whitespace content INLINE with its element (no reformat).
    std::string between = text.substr(i, lt - i);
    bool textContent = between.find_first_not_of(" \t\r\n") != std::string::npos;
    if (textContent) out += between;
    std::size_t gt = text.find('>', lt);
    if (gt == std::string::npos) return text; // malformed -> verbatim
    std::string tag = text.substr(lt, gt - lt + 1);
    bool closing = tag.size() > 1 && tag[1] == '/';
    bool selfClose = tag.size() > 1 && tag[tag.size() - 2] == '/';
    bool decl = tag.size() > 1 && (tag[1] == '?' || tag[1] == '!');
    if (closing) {
      --depth;
      if (depth < 0) return text; // mismatch -> verbatim
      if (!textContent) indent(depth);
      out += tag;
    } else {
      if (!firstTag && !textContent) indent(depth);
      out += tag;
      if (!decl && !selfClose) ++depth;
    }
    firstTag = false;
    i = gt + 1;
  }
  if (depth != 0) return text; // unbalanced -> verbatim
  return out.substr(out.size() > 0 && out[0] == '\n' ? 1 : 0);
}

std::string formatJson(const std::string &text, bool pretty) {
  try {
    auto j = json::parse(text);
    return pretty ? j.dump(2) : j.dump();
  } catch (...) {
    // Not JSON — an XML body (SOAP response, XML API) still deserves Pretty.
    if (pretty) return formatXml(text);
    return text;
  }
}

std::string jsonEncodeString(const std::string &text) {
  json j = text;
  return j.dump();
}

std::string jsonDecodeString(const std::string &text) {
  try {
    auto j = json::parse(text);
    if (j.is_string()) return j.get<std::string>();
    return text;
  } catch (...) {
    return text;
  }
}

} // namespace core::serial
