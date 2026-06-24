// gql_operation.cpp — operation detection + GraphQL->HTTP packaging (SPEC_graphql §2/§4).
#include "graphql/graphql.hpp"

#include <cctype>

#include <nlohmann/json.hpp>

namespace core::gql {

GqlOperation effectiveOperation(const GraphQlRequest& g) {
    if (g.operation != GqlOperation::Auto) return g.operation;

    // Scan to the first significant token, skipping whitespace, BOM, and `# line comments`.
    const std::string& q = g.query;
    std::size_t i = 0;
    if (q.size() >= 3 && (unsigned char)q[0] == 0xEF && (unsigned char)q[1] == 0xBB && (unsigned char)q[2] == 0xBF)
        i = 3;
    while (i < q.size()) {
        char c = q[i];
        if (std::isspace((unsigned char)c) || c == ',') { ++i; continue; }
        if (c == '#') { while (i < q.size() && q[i] != '\n') ++i; continue; }   // comment to EOL
        break;
    }
    if (i >= q.size()) return GqlOperation::Query;          // empty -> treat as query
    if (q[i] == '{') return GqlOperation::Query;            // shorthand `{ … }` is a query

    std::string word;
    while (i < q.size() && (std::isalpha((unsigned char)q[i]))) word += q[i++];
    if (word == "mutation") return GqlOperation::Mutation;
    if (word == "subscription") return GqlOperation::Subscription;
    return GqlOperation::Query;                              // "query" or anything else -> query
}

RequestModel buildHttpModel(const RequestModel& model) {
    const GraphQlRequest& g = model.graphql;
    RequestModel m = model;
    m.type = RequestType::Http;
    HttpRequest& h = m.http = HttpRequest{};
    h.url = g.url;
    h.headers = g.headers;   // carry Authorization etc.
    h.auth = g.auth;         // HttpSender applies bearer/basic/apikey (SPEC_graphql §9 Auth)

    auto hasHeader = [&](const char* name) {
        for (const auto& kv : h.headers) {
            if (!kv.enabled) continue;
            std::string k = kv.key;
            for (auto& c : k) c = (char)std::tolower((unsigned char)c);
            if (k == name) return true;
        }
        return false;
    };
    // Prefer the GraphQL-over-HTTP media type; fall back to application/json.
    if (!hasHeader("accept"))
        h.headers.push_back({"Accept", "application/graphql-response+json, application/json", true});

    nlohmann::json vars;
    try { vars = nlohmann::json::parse(g.variablesJson.empty() ? "{}" : g.variablesJson); }
    catch (...) { vars = nlohmann::json::object(); }

    if (g.useGetForQuery) {
        h.method = "GET";
        h.params.push_back({"query", g.query, true});
        if (!g.variablesJson.empty() && g.variablesJson != "{}")
            h.params.push_back({"variables", vars.dump(), true});
        if (!g.operationName.empty()) h.params.push_back({"operationName", g.operationName, true});
        h.body.mode = "none";
    } else {
        h.method = "POST";
        if (!hasHeader("content-type")) h.headers.push_back({"Content-Type", "application/json", true});
        nlohmann::json body;
        body["query"] = g.query;
        body["variables"] = vars;
        if (!g.operationName.empty()) body["operationName"] = g.operationName;
        h.body.mode = "json";
        h.body.json = body.dump();
    }
    return m;
}

} // namespace core::gql
