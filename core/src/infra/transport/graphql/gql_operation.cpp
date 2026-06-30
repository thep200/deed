// gql_operation.cpp — operation detection + GraphQL->HTTP packaging (SPEC_graphql §2/§4). DOMAIN-native.
#include "infra/transport/graphql/graphql.hpp"

#include <cctype>
#include <variant>

#include <nlohmann/json.hpp>

namespace core::gql {
namespace d = core::domain;

d::GqlOperationType effectiveOperation(const d::GraphQlRequest& g) {
    if (g.op().operation != d::GqlOperationType::Auto) return g.op().operation;

    // Scan to the first significant token, skipping whitespace, BOM, and `# line comments`.
    const std::string& q = g.op().query;
    std::size_t i = 0;
    if (q.size() >= 3 && (unsigned char)q[0] == 0xEF && (unsigned char)q[1] == 0xBB && (unsigned char)q[2] == 0xBF)
        i = 3;
    while (i < q.size()) {
        char c = q[i];
        if (std::isspace((unsigned char)c) || c == ',') { ++i; continue; }
        if (c == '#') { while (i < q.size() && q[i] != '\n') ++i; continue; }   // comment to EOL
        break;
    }
    if (i >= q.size()) return d::GqlOperationType::Query;          // empty -> treat as query
    if (q[i] == '{') return d::GqlOperationType::Query;            // shorthand `{ … }` is a query

    std::string word;
    while (i < q.size() && (std::isalpha((unsigned char)q[i]))) word += q[i++];
    if (word == "mutation") return d::GqlOperationType::Mutation;
    if (word == "subscription") return d::GqlOperationType::Subscription;
    return d::GqlOperationType::Query;                             // "query" or anything else -> query
}

d::RequestModel buildHttpModel(const d::RequestModel& model) {
    const d::GraphQlRequest& g = std::get<d::GraphQlRequest>(model.payload());

    // Carry the headers over (Authorization etc.), then add Accept (and Content-Type for the POST body).
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

    // POST body {query, variables, operationName}. (The domain model has no GET-for-query flag.)
    nlohmann::json vars;
    const std::string& vtxt = g.op().variables.text();
    try { vars = nlohmann::json::parse(vtxt.empty() ? "{}" : vtxt); }
    catch (...) { vars = nlohmann::json::object(); }
    nlohmann::json body;
    body["query"] = g.op().query;
    body["variables"] = vars;
    if (!g.op().operationName.empty()) body["operationName"] = g.op().operationName;

    // Parts holds a Url (no default ctor) -> brace-init in member order:
    // method, url, pathVariables, params, headers, body, auth.
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
