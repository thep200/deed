#include "infra/transport/kafka/schema_registry_client.hpp"

#include <vector>

#include <nlohmann/json.hpp>

#include "core/domain/http/http_request.hpp"
#include "core/domain/request/request_model.hpp"
#include "infra/transport/http/blocking_fetch.hpp"

namespace core::infra {
namespace d = core::domain;
using nlohmann::json;

namespace {
constexpr long long kSrTimeoutMs = 5000;                 // per REST call; keeps a tail responsive
constexpr std::chrono::seconds kNegativeCacheTtl{30};    // down registry -> one retry per id per 30s

template <class T> d::Result<T> fail(d::ErrorCode c, const std::string &msg) {
  return d::Result<T>::fail({c, msg, ""});
}
} // namespace

d::Result<RegisteredSchema> parseLatestSubjectResponse(const std::string &body) {
  try {
    json j = json::parse(body);
    if (!j.is_object() || !j.contains("schema") || !j["schema"].is_string())
      return fail<RegisteredSchema>(d::ErrorCode::Parse, "registry response has no schema");
    RegisteredSchema out;
    out.schemaJson = j["schema"].get<std::string>(); // schema arrives as a JSON-ESCAPED string
    out.id = j.value("id", 0);
    if (out.id <= 0) return fail<RegisteredSchema>(d::ErrorCode::Parse, "registry response has no id");
    return d::Result<RegisteredSchema>::ok(std::move(out));
  } catch (const std::exception &e) {
    return fail<RegisteredSchema>(d::ErrorCode::Parse, std::string("registry response: ") + e.what());
  }
}

d::Result<std::string> parseSchemaByIdResponse(const std::string &body) {
  try {
    json j = json::parse(body);
    if (!j.is_object() || !j.contains("schema") || !j["schema"].is_string())
      return fail<std::string>(d::ErrorCode::Parse, "registry response has no schema");
    return d::Result<std::string>::ok(j["schema"].get<std::string>());
  } catch (const std::exception &e) {
    return fail<std::string>(d::ErrorCode::Parse, std::string("registry response: ") + e.what());
  }
}

d::Result<std::string> SchemaRegistryClient::get(const d::SchemaRegistryRef &ref,
                                                 const std::string &path,
                                                 const d::ICancellationToken &cancel) {
  std::string base = ref.url;
  while (!base.empty() && base.back() == '/') base.pop_back();
  auto url = d::Url::create(base + path);
  if (!url.isOk()) return fail<std::string>(d::ErrorCode::Validation, url.error().message);

  d::Auth auth = d::Auth::none();
  if (!ref.username.empty()) {
    auto b = d::Auth::basic(ref.username, ref.password);
    if (!b.isOk()) return fail<std::string>(b.error().code, b.error().message);
    auth = b.take();
  }
  std::vector<d::Header> hdrs;
  hdrs.push_back(d::Header::create("Accept", "application/vnd.schemaregistry.v1+json").take());
  d::HttpRequest::Parts hp{d::HttpMethod::Get, url.take(), {}, {}, d::HeaderList(std::move(hdrs)),
                           d::Body::none(),   std::move(auth)};
  auto http = d::HttpRequest::create(std::move(hp));
  if (!http.isOk()) return fail<std::string>(http.error().code, http.error().message);
  d::RequestConfig cfg{d::Timeout::fromMillis(kSrTimeoutMs).take(), true};
  auto model = d::RequestModel::create(d::RequestId("sr_get"), "schema registry", 0, cfg, http.take());
  if (!model.isOk()) return fail<std::string>(model.error().code, model.error().message);

  auto resp = blockingFetch(model.take(), cancel);
  if (!resp.isOk()) return d::Result<std::string>::fail(resp.error());
  if (resp.value().statusCode == 404)
    return fail<std::string>(d::ErrorCode::NotFound, "not found (HTTP 404)");
  if (resp.value().statusCode >= 400)
    return fail<std::string>(d::ErrorCode::Network,
                             "HTTP " + std::to_string(resp.value().statusCode) + ": " +
                                 resp.value().body.substr(0, 200));
  return d::Result<std::string>::ok(std::move(resp.value().body));
}

d::Result<RegisteredSchema>
SchemaRegistryClient::latestForSubject(const d::SchemaRegistryRef &ref, const std::string &subject,
                                       const d::ICancellationToken &cancel) {
  auto body = get(ref, "/subjects/" + subject + "/versions/latest", cancel);
  if (!body.isOk()) {
    if (body.error().code == d::ErrorCode::NotFound)
      return fail<RegisteredSchema>(
          d::ErrorCode::NotFound,
          "no schema registered for subject '" + subject + "' — register one first "
          "(Deed does not register schemas)");
    return d::Result<RegisteredSchema>::fail(body.error());
  }
  return parseLatestSubjectResponse(body.value());
}

d::Result<std::string> SchemaRegistryClient::schemaById(const d::SchemaRegistryRef &ref,
                                                        std::int32_t id,
                                                        const d::ICancellationToken &cancel) {
  const std::string key = ref.url + "|" + std::to_string(id);
  const auto now = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (auto it = byId_.find(key); it != byId_.end()) return d::Result<std::string>::ok(it->second);
    if (auto it = negById_.find(key); it != negById_.end() && now < it->second.second)
      return fail<std::string>(d::ErrorCode::Network, it->second.first);
  }
  // Fetch OUTSIDE the lock: a slow registry must not stall a sibling tail that only needs the cache.
  auto body = get(ref, "/schemas/ids/" + std::to_string(id), cancel);
  auto parsed = body.isOk() ? parseSchemaByIdResponse(body.value())
                            : d::Result<std::string>::fail(body.error());
  std::lock_guard<std::mutex> lk(mu_);
  if (parsed.isOk()) {
    byId_[key] = parsed.value();
    negById_.erase(key);
    return parsed;
  }
  negById_[key] = {parsed.error().message, now + kNegativeCacheTtl};
  return parsed;
}

} // namespace core::infra
