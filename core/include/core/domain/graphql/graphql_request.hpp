#pragma once

#include <string>
#include <utility>

#include "core/domain/auth/auth.hpp"
#include "core/domain/common/result.hpp"
#include "core/domain/values/header.hpp"
#include "core/domain/values/json_text.hpp"
#include "core/domain/values/url.hpp"

namespace core::domain {

enum class GqlOperationType { Auto, Query, Mutation, Subscription };
enum class GqlSubTransport { Http, Ws };

struct GraphQlOperation {
  std::string query;         // non-empty (enforced by factory)
  std::string operationName; // optional
  JsonText variables = JsonText::emptyObject();
  GqlOperationType operation = GqlOperationType::Auto;
  bool operator==(const GraphQlOperation &o) const {
    return query == o.query && operationName == o.operationName && variables == o.variables &&
           operation == o.operation;
  }
};

class GraphQlRequest {
public:
  struct Parts {
    Url url;
    GraphQlOperation op;
    HeaderList headers;
    Auth auth = Auth::none();
    GqlSubTransport subTransport = GqlSubTransport::Http;
    std::string wsProtocol; // only when subTransport == Ws
  };

  // Invariants: query non-empty; Subscription requires the Ws transport; wsProtocol must be a known sub-protocol.
  static Result<GraphQlRequest> create(Parts p) {
    if (p.op.query.empty())
      return Result<GraphQlRequest>::fail({ErrorCode::Validation, "graphql query required", "graphql.query"});
    if (p.op.operation == GqlOperationType::Subscription && p.subTransport != GqlSubTransport::Ws)
      return Result<GraphQlRequest>::fail(
          {ErrorCode::Validation, "subscription requires ws transport", "graphql.subTransport"});
    if (p.subTransport == GqlSubTransport::Ws && !p.wsProtocol.empty() &&
        p.wsProtocol != "graphql-transport-ws" && p.wsProtocol != "graphql-ws")
      return Result<GraphQlRequest>::fail(
          {ErrorCode::Validation, "unknown graphql ws protocol: " + p.wsProtocol, "graphql.wsProtocol"});
    return Result<GraphQlRequest>::ok(GraphQlRequest(std::move(p)));
  }

  const Url &url() const noexcept { return p_.url; }
  const GraphQlOperation &op() const noexcept { return p_.op; }
  const HeaderList &headers() const noexcept { return p_.headers; }
  const Auth &auth() const noexcept { return p_.auth; }
  GqlSubTransport subTransport() const noexcept { return p_.subTransport; }
  const std::string &wsProtocol() const noexcept { return p_.wsProtocol; }

  bool operator==(const GraphQlRequest &o) const {
    return p_.url == o.p_.url && p_.op == o.p_.op && p_.headers == o.p_.headers &&
           p_.auth == o.p_.auth && p_.subTransport == o.p_.subTransport &&
           p_.wsProtocol == o.p_.wsProtocol;
  }
  bool operator!=(const GraphQlRequest &o) const { return !(*this == o); }

private:
  explicit GraphQlRequest(Parts p) : p_(std::move(p)) {}
  Parts p_;
};

} // namespace core::domain
