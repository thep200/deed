#include "infra/transport/graphql/graphql.hpp"

#include <cctype>
#include <variant>

#include <nlohmann/json.hpp>

#include "infra/transport/graphql/gql_payload.hpp"

namespace core::gql {
namespace d = core::domain;

nlohmann::json operationPayload(const d::GraphQlOperation& op) {
    nlohmann::json vars;
    const std::string& vtxt = op.variables.text();
    try { vars = nlohmann::json::parse(vtxt.empty() ? "{}" : vtxt); }
    catch (...) { vars = nlohmann::json::object(); }
    nlohmann::json payload;
    payload["query"] = op.query;
    payload["variables"] = std::move(vars);
    if (!op.operationName.empty()) payload["operationName"] = op.operationName;
    return payload;
}

d::RequestModel buildHttpModel(const d::RequestModel& model) {
    const d::GraphQlRequest& g = std::get<d::GraphQlRequest>(model.payload());

    auto hasHeader = [&](const char* name) {
        for (const auto& h : g.headers().items()) {
            if (!h.enabled()) continue;
            std::string k = h.name();
            for (auto& c : k) c = (char)std::tolower((unsigned char)c);
            if (k == name) return true;
        }
        return false;
    };
    std::vector<d::Header> hdrs = g.headers().items();
    if (!hasHeader("accept"))
        hdrs.push_back(d::Header::create("Accept", "application/graphql-response+json, application/json").take());
    if (!hasHeader("content-type"))
        hdrs.push_back(d::Header::create("Content-Type", "application/json").take());

    // The domain model has no GET-for-query flag — always POST.
    nlohmann::json body = operationPayload(g.op());

    // Parts holds a Url (no default ctor) -> brace-init in member order.
    d::HttpRequest::Parts hp{d::HttpMethod::Post,
                             g.url(),
                             {},
                             {},
                             d::HeaderList(std::move(hdrs)),
                             d::Body::raw(d::RawSubtype::Json, body.dump()),
                             g.auth()};
    auto http = d::HttpRequest::create(std::move(hp)).take();

    return d::RequestModel::create(model.id(), model.name(), model.seq(), model.config(), http).take();
}

} // namespace core::gql
