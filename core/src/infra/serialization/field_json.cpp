#include "core/infra/serialization/field_json.hpp"

#include <nlohmann/json.hpp>

#include "infra/serialization/field_json_common.hpp"
#include "infra/serialization/wire_format.hpp"

namespace core::serial {
namespace d = core::domain;
namespace w = core::wire;

namespace {
// Raw-body subtype -> its wire "mode" token (and the key its text is stored under).
const char *rawSubtypeKey(d::RawSubtype s) {
  switch (s) {
  case d::RawSubtype::Text: return w::kBodyText;
  case d::RawSubtype::Xml: return w::kBodyXml;
  case d::RawSubtype::Json: return w::kBodyJson;
  }
  return w::kBodyJson;
}
std::vector<d::FormField> formFieldsFrom(const json &j) {
  std::vector<d::FormField> fields;
  if (auto it = j.find("formUrlEncoded"); it != j.end() && it->is_array())
    for (const auto &e : *it) fields.push_back({gs(e, "key"), gs(e, "value"), gb(e, "enabled", true)});
  return fields;
}
std::vector<d::MultipartPart> multipartPartsFrom(const json &j) {
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
  return parts;
}
} // namespace

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

// One flat shape discriminated by "type": fields sit next to the discriminator, no per-type sub-object.
std::string authToJson(const d::Auth &auth) {
  json j;
  auth.match([&](auto &&a) {
    using T = std::decay_t<decltype(a)>;
    if constexpr (std::is_same_v<T, d::AuthNone>) {
      j["type"] = "none";
    } else if constexpr (std::is_same_v<T, d::AuthBasic>) {
      j["type"] = "basic";
      j["username"] = a.username;
      j["password"] = a.password;
    } else if constexpr (std::is_same_v<T, d::AuthBearer>) {
      j["type"] = "bearer";
      j["token"] = a.token;
    } else if constexpr (std::is_same_v<T, d::AuthOAuth2>) {
      j["type"] = "oauth2";
      j["grant"] = a.grant == d::OAuth2Grant::Password ? "password" : "client_credentials";
      j["tokenUrl"] = a.tokenUrl;
      j["clientId"] = a.clientId;
      j["clientSecret"] = a.clientSecret;
      j["scope"] = a.scope;
      j["clientAuth"] = a.clientAuth == d::OAuth2ClientAuth::Body ? "body" : "header";
      if (a.grant == d::OAuth2Grant::Password) {
        j["username"] = a.username;
        j["password"] = a.password;
      }
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
    if (type == "basic") return d::Auth::basic(gs(j, "username"), gs(j, "password"));
    if (type == "bearer") return d::Auth::bearer(gs(j, "token"));
    if (type == "oauth2") {
      d::AuthOAuth2 o;
      std::string grant = gs(j, "grant", "client_credentials");
      if (grant == "password") o.grant = d::OAuth2Grant::Password;
      else if (grant != "client_credentials")
        return parseErr<d::Auth>("unknown auth.grant: " + grant);
      std::string ca = gs(j, "clientAuth", "header");
      if (ca == "body") o.clientAuth = d::OAuth2ClientAuth::Body;
      else if (ca != "header")
        return parseErr<d::Auth>("unknown auth.clientAuth: " + ca);
      o.tokenUrl = gs(j, "tokenUrl");
      o.clientId = gs(j, "clientId");
      o.clientSecret = gs(j, "clientSecret");
      o.scope = gs(j, "scope");
      o.username = gs(j, "username");
      o.password = gs(j, "password");
      return d::Auth::oauth2(std::move(o));
    }
    return parseErr<d::Auth>("unknown auth.type: " + type);
  } catch (const std::exception &e) {
    return parseErr<d::Auth>(e.what());
  }
}

std::string bodyToJson(const d::Body &body) {
  json j;
  body.match([&](auto &&b) {
    using T = std::decay_t<decltype(b)>;
    if constexpr (std::is_same_v<T, d::BodyNone>) {
      j["mode"] = w::kBodyNone;
    } else if constexpr (std::is_same_v<T, d::BodyRaw>) {
      const char *mode = rawSubtypeKey(b.subtype);
      j["mode"] = mode;
      j[mode] = b.text;
    } else if constexpr (std::is_same_v<T, d::BodyFormUrlEncoded>) {
      j["mode"] = w::kBodyForm;
      json a = json::array();
      for (const auto &f : b.fields)
        a.push_back({{"key", f.key}, {"value", f.value}, {"enabled", f.enabled ? 1 : 0}});
      j["formUrlEncoded"] = a;
    } else if constexpr (std::is_same_v<T, d::BodyMultipart>) {
      j["mode"] = w::kBodyMultipart;
      json a = json::array();
      for (const auto &p : b.parts)
        a.push_back({{"key", p.key},
                     {"value", p.value},
                     {"type", p.kind == d::PartKind::File ? "file" : "text"},
                     {"filePath", p.filePath},
                     {"enabled", p.enabled ? 1 : 0}});
      j["multipart"] = a;
    } else if constexpr (std::is_same_v<T, d::BodyBinary>) {
      j["mode"] = w::kBodyBinary;
      j["binary"] = {{"filePath", b.filePath}};
    }
  });
  return j.dump(2);
}
d::Result<d::Body> jsonToBody(const std::string &text) {
  try {
    auto j = parseGuarded(text, "{\"mode\":\"none\"}");
    if (!j.is_object()) return parseErr<d::Body>("body must be a JSON object");
    std::string mode = gs(j, "mode", w::kBodyNone);
    if (mode == w::kBodyJson) return d::Result<d::Body>::ok(d::Body::raw(d::RawSubtype::Json, gs(j, w::kBodyJson)));
    if (mode == w::kBodyText) return d::Result<d::Body>::ok(d::Body::raw(d::RawSubtype::Text, gs(j, w::kBodyText)));
    if (mode == w::kBodyXml) return d::Result<d::Body>::ok(d::Body::raw(d::RawSubtype::Xml, gs(j, w::kBodyXml)));
    if (mode == w::kBodyForm)
      return d::Result<d::Body>::ok(d::Body::formUrlEncoded(formFieldsFrom(j)));
    if (mode == w::kBodyMultipart)
      return d::Body::multipart(multipartPartsFrom(j));
    if (mode == w::kBodyBinary) {
      std::string fp;
      if (auto it = j.find(w::kBodyBinary); it != j.end() && it->is_object()) fp = gs(*it, "filePath");
      if (fp.empty()) return d::Result<d::Body>::ok(d::Body::none()); // binary chosen, no file yet
      return d::Body::binary(fp);
    }
    return d::Result<d::Body>::ok(d::Body::none());
  } catch (const std::exception &e) {
    return parseErr<d::Body>(e.what());
  }
}

EditorBody bodyToEditor(const d::Body &body) {
  EditorBody eb{w::kBodyJson, ""};
  body.match([&](auto &&b) {
    using T = std::decay_t<decltype(b)>;
    if constexpr (std::is_same_v<T, d::BodyRaw>) {
      eb.mode = rawSubtypeKey(b.subtype);
      eb.content = b.text;
    } else if constexpr (std::is_same_v<T, d::BodyFormUrlEncoded>) {
      eb.mode = w::kBodyForm;
      json a = json::array();
      for (const auto &f : b.fields)
        a.push_back({{"key", f.key}, {"value", f.value}, {"enabled", f.enabled ? 1 : 0}});
      eb.content = a.dump(2);
    } else if constexpr (std::is_same_v<T, d::BodyBinary>) {
      eb.mode = w::kBodyBinary;
      eb.content = json({{"filePath", b.filePath}}).dump(2);
    }
    // BodyNone / BodyMultipart -> default {"json",""} (the editor has no multipart mode).
  });
  return eb;
}

// Accepts {"filePath"|"path": ...}, a bare JSON string, or a plain path typed as-is.
static std::string binaryFilePathFromEditor(const std::string &content) {
  try {
    auto j = json::parse(content);
    if (j.is_object()) return j.contains("filePath") ? gs(j, "filePath") : gs(j, "path");
    if (j.is_string()) return j.get<std::string>();
    return {};
  } catch (...) {
    // not JSON -> treat the whole text as a path (trimmed)
    auto a = content.find_first_not_of(" \t\r\n");
    auto b = content.find_last_not_of(" \t\r\n");
    return (a == std::string::npos) ? std::string() : content.substr(a, b - a + 1);
  }
}

d::Result<d::Body> bodyFromEditor(const std::string &mode, const std::string &content) {
  try {
    if (mode == w::kBodyJson) return d::Result<d::Body>::ok(d::Body::raw(d::RawSubtype::Json, content));
    if (mode == w::kBodyText) return d::Result<d::Body>::ok(d::Body::raw(d::RawSubtype::Text, content));
    if (mode == w::kBodyXml) return d::Result<d::Body>::ok(d::Body::raw(d::RawSubtype::Xml, content));
    if (mode == w::kBodyForm) {
      std::vector<d::FormField> fields;
      auto j = parseGuarded(content, "[]");
      if (j.is_array())
        for (const auto &e : j)
          if (e.is_object()) fields.push_back({gs(e, "key"), gs(e, "value"), gb(e, "enabled", true)});
      return d::Result<d::Body>::ok(d::Body::formUrlEncoded(std::move(fields)));
    }
    if (mode == w::kBodyBinary) {
      std::string fp = binaryFilePathFromEditor(content);
      if (fp.empty()) return d::Result<d::Body>::ok(d::Body::none());
      return d::Body::binary(fp);
    }
    return d::Result<d::Body>::ok(d::Body::none());
  } catch (const std::exception &e) {
    return parseErr<d::Body>(e.what());
  }
}

std::string responseHeadersToJson(const std::vector<d::ResponseHeader> &headers) {
  json a = json::array();
  for (const auto &h : headers) a.push_back({{"key", h.name}, {"value", h.value}});
  return a.dump(2);
}

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
