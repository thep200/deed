#include "core/infra/serialization/field_json.hpp"

#include <nlohmann/json.hpp>

#include "infra/serialization/json_codec.hpp" // core::codec::parseGuarded — JSON nesting-depth guard (H5)

namespace core::serial {
namespace d = core::domain;
using nlohmann::json;

namespace {
std::string gs(const json &j, const char *k, const std::string &dflt = "") {
  auto it = j.find(k);
  return (it != j.end() && it->is_string()) ? it->get<std::string>() : dflt;
}
bool gb(const json &j, const char *k, bool dflt) {
  auto it = j.find(k);
  if (it == j.end()) return dflt;
  if (it->is_boolean()) return it->get<bool>();
  if (it->is_number()) return it->get<double>() != 0; // accept 0/1
  return dflt;
}
json parseGuarded(const std::string &text, const char *fallback) {
  return core::codec::parseGuarded(text.empty() ? fallback : text); // depth-guarded (H5)
}
template <class T> d::Result<T> parseErr(const std::string &what) {
  return d::Result<T>::fail({d::ErrorCode::Parse, what, ""});
}
} // namespace

// ---- Headers ----
std::string headersToJson(const d::HeaderList &list) {
  json a = json::array();
  for (const auto &h : list.items())
    a.push_back({{"key", h.name()}, {"value", h.value()}, {"enabled", h.enabled() ? 1 : 0}});
  return a.dump(2);
}
d::Result<d::HeaderList> jsonToHeaders(const std::string &text) {
  try {
    auto j = parseGuarded(text, "[]");
    if (!j.is_array()) return parseErr<d::HeaderList>("headers must be a JSON array");
    std::vector<d::Header> items;
    for (const auto &e : j) {
      if (!e.is_object()) continue;
      auto h = d::Header::create(gs(e, "key"), gs(e, "value"), gb(e, "enabled", true));
      if (!h) return d::Result<d::HeaderList>::fail(h.error());
      items.push_back(h.take());
    }
    return d::Result<d::HeaderList>::ok(d::HeaderList(std::move(items)));
  } catch (const std::exception &e) {
    return parseErr<d::HeaderList>(e.what());
  }
}

// ---- Query params ----
std::string paramsToJson(const d::QueryParamList &list) {
  json a = json::array();
  for (const auto &p : list.items())
    a.push_back({{"key", p.key()}, {"value", p.value()}, {"enabled", p.enabled() ? 1 : 0}});
  return a.dump(2);
}
d::Result<d::QueryParamList> jsonToParams(const std::string &text) {
  try {
    auto j = parseGuarded(text, "[]");
    if (!j.is_array()) return parseErr<d::QueryParamList>("params must be a JSON array");
    std::vector<d::QueryParam> items;
    for (const auto &e : j) {
      if (!e.is_object()) continue;
      auto p = d::QueryParam::create(gs(e, "key"), gs(e, "value"), gb(e, "enabled", true));
      if (!p) return d::Result<d::QueryParamList>::fail(p.error());
      items.push_back(p.take());
    }
    return d::Result<d::QueryParamList>::ok(d::QueryParamList(std::move(items)));
  } catch (const std::exception &e) {
    return parseErr<d::QueryParamList>(e.what());
  }
}

// ---- Auth ----
std::string authToJson(const d::Auth &auth) {
  json j;
  auth.match([&](auto &&a) {
    using T = std::decay_t<decltype(a)>;
    if constexpr (std::is_same_v<T, d::AuthNone>) {
      j["type"] = "none";
    } else if constexpr (std::is_same_v<T, d::AuthBasic>) {
      j["type"] = "basic";
      j["basic"] = {{"username", a.username}, {"password", a.password}};
    } else if constexpr (std::is_same_v<T, d::AuthBearer>) {
      j["type"] = "bearer";
      j["bearer"] = {{"token", a.token}};
    } else if constexpr (std::is_same_v<T, d::AuthApiKey>) {
      j["type"] = "apikey";
      j["apikey"] = {{"key", a.key}, {"value", a.value},
                     {"in", a.in == d::ApiKeyIn::Query ? "query" : "header"}};
    }
  });
  return j.dump(2);
}
d::Result<d::Auth> jsonToAuth(const std::string &text) {
  try {
    auto j = parseGuarded(text, "{\"type\":\"none\"}");
    if (!j.is_object()) return parseErr<d::Auth>("auth must be a JSON object");
    std::string type = gs(j, "type", "none");
    if (type == "none") return d::Result<d::Auth>::ok(d::Auth::none());
    if (type == "basic") {
      auto b = j.contains("basic") ? j["basic"] : json::object();
      return d::Auth::basic(gs(b, "username"), gs(b, "password"));
    }
    if (type == "bearer") {
      auto b = j.contains("bearer") ? j["bearer"] : json::object();
      return d::Auth::bearer(gs(b, "token"));
    }
    if (type == "apikey") {
      auto b = j.contains("apikey") ? j["apikey"] : json::object();
      auto in = gs(b, "in", "header") == "query" ? d::ApiKeyIn::Query : d::ApiKeyIn::Header;
      return d::Auth::apiKey(gs(b, "key"), gs(b, "value"), in);
    }
    return parseErr<d::Auth>("unknown auth.type: " + type);
  } catch (const std::exception &e) {
    return parseErr<d::Auth>(e.what());
  }
}

// ---- Body ----
std::string bodyToJson(const d::Body &body) {
  json j;
  body.match([&](auto &&b) {
    using T = std::decay_t<decltype(b)>;
    if constexpr (std::is_same_v<T, d::BodyNone>) {
      j["mode"] = "none";
    } else if constexpr (std::is_same_v<T, d::BodyRaw>) {
      const char *mode = b.subtype == d::RawSubtype::Text ? "text"
                         : b.subtype == d::RawSubtype::Xml ? "xml"
                                                           : "json";
      j["mode"] = mode;
      j[mode] = b.text;
    } else if constexpr (std::is_same_v<T, d::BodyFormUrlEncoded>) {
      j["mode"] = "form-urlencoded";
      json a = json::array();
      for (const auto &f : b.fields)
        a.push_back({{"key", f.key}, {"value", f.value}, {"enabled", f.enabled ? 1 : 0}});
      j["formUrlEncoded"] = a;
    } else if constexpr (std::is_same_v<T, d::BodyMultipart>) {
      j["mode"] = "multipart";
      json a = json::array();
      for (const auto &p : b.parts)
        a.push_back({{"key", p.key},
                     {"value", p.value},
                     {"type", p.kind == d::PartKind::File ? "file" : "text"},
                     {"filePath", p.filePath},
                     {"enabled", p.enabled ? 1 : 0}});
      j["multipart"] = a;
    } else if constexpr (std::is_same_v<T, d::BodyBinary>) {
      j["mode"] = "binary";
      j["binary"] = {{"filePath", b.filePath}};
    }
  });
  return j.dump(2);
}
d::Result<d::Body> jsonToBody(const std::string &text) {
  try {
    auto j = parseGuarded(text, "{\"mode\":\"none\"}");
    if (!j.is_object()) return parseErr<d::Body>("body must be a JSON object");
    std::string mode = gs(j, "mode", "none");
    if (mode == "json") return d::Result<d::Body>::ok(d::Body::raw(d::RawSubtype::Json, gs(j, "json")));
    if (mode == "text") return d::Result<d::Body>::ok(d::Body::raw(d::RawSubtype::Text, gs(j, "text")));
    if (mode == "xml") return d::Result<d::Body>::ok(d::Body::raw(d::RawSubtype::Xml, gs(j, "xml")));
    if (mode == "form-urlencoded") {
      std::vector<d::FormField> fields;
      if (auto it = j.find("formUrlEncoded"); it != j.end() && it->is_array())
        for (const auto &e : *it)
          fields.push_back({gs(e, "key"), gs(e, "value"), gb(e, "enabled", true)});
      return d::Result<d::Body>::ok(d::Body::formUrlEncoded(std::move(fields)));
    }
    if (mode == "multipart" || mode == "form-data") {
      std::vector<d::MultipartPart> parts;
      if (auto it = j.find("multipart"); it != j.end() && it->is_array())
        for (const auto &e : *it) {
          d::MultipartPart p;
          p.key = gs(e, "key");
          p.value = gs(e, "value");
          p.kind = gs(e, "type", "text") == "file" ? d::PartKind::File : d::PartKind::Text;
          p.filePath = gs(e, "filePath");
          p.enabled = gb(e, "enabled", true);
          parts.push_back(std::move(p));
        }
      return d::Body::multipart(std::move(parts));
    }
    if (mode == "binary") {
      std::string fp;
      if (auto it = j.find("binary"); it != j.end() && it->is_object()) fp = gs(*it, "filePath");
      if (fp.empty()) return d::Result<d::Body>::ok(d::Body::none()); // binary chosen, no file yet
      return d::Body::binary(fp);
    }
    return d::Result<d::Body>::ok(d::Body::none());
  } catch (const std::exception &e) {
    return parseErr<d::Body>(e.what());
  }
}

// ---- Editor body view (unwrapped per-mode) ----
EditorBody bodyToEditor(const d::Body &body) {
  EditorBody eb{"json", ""};
  body.match([&](auto &&b) {
    using T = std::decay_t<decltype(b)>;
    if constexpr (std::is_same_v<T, d::BodyRaw>) {
      eb.mode = b.subtype == d::RawSubtype::Text ? "text" : b.subtype == d::RawSubtype::Xml ? "xml" : "json";
      eb.content = b.text;
    } else if constexpr (std::is_same_v<T, d::BodyFormUrlEncoded>) {
      eb.mode = "form-urlencoded";
      json a = json::array();
      for (const auto &f : b.fields)
        a.push_back({{"key", f.key}, {"value", f.value}, {"enabled", f.enabled ? 1 : 0}});
      eb.content = a.dump(2);
    } else if constexpr (std::is_same_v<T, d::BodyBinary>) {
      eb.mode = "binary";
      eb.content = json({{"filePath", b.filePath}}).dump(2);
    }
    // BodyNone / BodyMultipart -> default {"json",""} (the editor has no multipart mode).
  });
  return eb;
}
d::Result<d::Body> bodyFromEditor(const std::string &mode, const std::string &content) {
  try {
    if (mode == "json") return d::Result<d::Body>::ok(d::Body::raw(d::RawSubtype::Json, content));
    if (mode == "text") return d::Result<d::Body>::ok(d::Body::raw(d::RawSubtype::Text, content));
    if (mode == "xml") return d::Result<d::Body>::ok(d::Body::raw(d::RawSubtype::Xml, content));
    if (mode == "form-urlencoded") {
      std::vector<d::FormField> fields;
      auto j = parseGuarded(content, "[]");
      if (j.is_array())
        for (const auto &e : j)
          if (e.is_object()) fields.push_back({gs(e, "key"), gs(e, "value"), gb(e, "enabled", true)});
      return d::Result<d::Body>::ok(d::Body::formUrlEncoded(std::move(fields)));
    }
    if (mode == "binary") {
      std::string fp;
      auto t = content;
      try {
        auto j = json::parse(content);
        if (j.is_object()) fp = j.contains("filePath") ? gs(j, "filePath") : gs(j, "path");
        else if (j.is_string()) fp = j.get<std::string>();
      } catch (...) {
        // not JSON -> treat the whole text as a path (trimmed)
        auto a = t.find_first_not_of(" \t\r\n");
        auto b = t.find_last_not_of(" \t\r\n");
        fp = (a == std::string::npos) ? "" : t.substr(a, b - a + 1);
      }
      if (fp.empty()) return d::Result<d::Body>::ok(d::Body::none());
      return d::Body::binary(fp);
    }
    return d::Result<d::Body>::ok(d::Body::none());
  } catch (const std::exception &e) {
    return parseErr<d::Body>(e.what());
  }
}

// ---- gRPC metadata ----
std::string metadataToJson(const d::GrpcMetadata &md) {
  json a = json::array();
  for (const auto &e : md.entries())
    a.push_back({{"key", e.key}, {"value", e.value}, {"enabled", e.enabled ? 1 : 0}});
  return a.dump(2);
}
d::Result<d::GrpcMetadata> jsonToMetadata(const std::string &text) {
  try {
    auto j = parseGuarded(text, "[]");
    if (!j.is_array()) return parseErr<d::GrpcMetadata>("metadata must be a JSON array");
    std::vector<d::MetadataEntry> entries;
    for (const auto &e : j) {
      if (!e.is_object()) continue;
      entries.push_back({gs(e, "key"), gs(e, "value"), gb(e, "enabled", true)});
    }
    return d::GrpcMetadata::create(std::move(entries));
  } catch (const std::exception &e) {
    return parseErr<d::GrpcMetadata>(e.what());
  }
}

// ---- Response headers (display) ----
std::string responseHeadersToJson(const std::vector<d::ResponseHeader> &headers) {
  json a = json::array();
  for (const auto &h : headers) a.push_back({{"key", h.name}, {"value", h.value}});
  return a.dump(2);
}

// ---- Generic JSON text helpers ----
std::string formatJson(const std::string &text, bool pretty) {
  try {
    auto j = json::parse(text);
    return pretty ? j.dump(2) : j.dump();
  } catch (...) {
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

// ---- Config ----
std::string configToJson(const d::RequestConfig &c) {
  json j;
  j["timeout_ms"] = c.timeout.millis();
  j["tls"] = c.tlsEnabledDefault;
  return j.dump(2);
}
d::Result<d::RequestConfig> jsonToConfig(const std::string &text) {
  try {
    auto j = parseGuarded(text, "{}");
    if (!j.is_object()) return parseErr<d::RequestConfig>("config must be a JSON object");
    long long ms = 30000;
    if (auto it = j.find("timeout_ms"); it != j.end() && it->is_number()) ms = it->get<long long>();
    auto t = d::Timeout::fromMillis(ms);
    if (!t) return d::Result<d::RequestConfig>::fail(t.error());
    return d::Result<d::RequestConfig>::ok(d::RequestConfig{t.take(), gb(j, "tls", true)});
  } catch (const std::exception &e) {
    return parseErr<d::RequestConfig>(e.what());
  }
}

} // namespace core::serial
