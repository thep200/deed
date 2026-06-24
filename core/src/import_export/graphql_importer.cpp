// graphql_importer.cpp — import a GraphQL request (SPEC_graphql): a raw document that starts with
// query/mutation/subscription. (cURL-body import was intentionally removed — GraphQL is not imported
// from cURL.)
#include <cctype>

#include "core/import_export/importer.hpp"

namespace core {

namespace {

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Does the (trimmed) text start with a GraphQL operation keyword followed by a boundary?
bool startsWithGqlKeyword(const std::string& t) {
    for (const char* kw : {"query", "mutation", "subscription"}) {
        size_t n = std::char_traits<char>::length(kw);
        if (t.size() >= n && t.compare(0, n, kw) == 0) {
            char nxt = t.size() > n ? t[n] : ' ';
            if (std::isspace((unsigned char)nxt) || nxt == '{' || nxt == '(') return true;
        }
    }
    return false;
}

} // namespace

bool GraphQlImporter::canHandle(const std::string& input) const {
    return startsWithGqlKeyword(trim(input));   // raw GraphQL document only
}

ImportResult GraphQlImporter::parse(const std::string& input) const {
    ImportResult res;
    std::string t = trim(input);
    if (!startsWithGqlKeyword(t)) { res.error = "not a GraphQL document"; return res; }

    RequestModel m;
    m.type = RequestType::GraphQL;
    m.name = "Imported GraphQL";
    m.graphql.query = t;          // the user fills the endpoint URL after import
    m.graphql.variablesJson = "{}";
    res.ok = true;
    res.model = std::move(m);
    return res;
}

} // namespace core
