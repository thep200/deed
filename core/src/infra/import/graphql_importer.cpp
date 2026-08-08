#include <cctype>

#include "core/infra/import/importer.hpp"
#include "infra/import/shell_tokenize.hpp"

namespace core {

namespace {

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

ImportParseResult GraphQlImporter::parse(const std::string& input) const {
    namespace d = core::domain;
    std::string t = trim(input);
    if (!startsWithGqlKeyword(t)) return {false, std::nullopt, {}, "not a GraphQL document"};

    // The user fills the endpoint URL after import; operation Auto + Http transport keeps create() valid.
    d::GraphQlOperation op;
    op.query = t;
    d::GraphQlRequest::Parts gp{d::Url::create("").take(), op, d::HeaderList{}, d::Auth::none(),
                                d::GqlSubTransport::Http, ""};
    auto gql = d::GraphQlRequest::create(std::move(gp));
    if (!gql) return {false, std::nullopt, {}, gql.error().message};
    auto model = d::RequestModel::create(d::RequestId(""), "Imported GraphQL", 0,
                                         d::RequestConfig{d::Timeout::fromMillis(1800000).take(), true},
                                         gql.take());
    return {true, model.take(), {}, ""};
}

} // namespace core
