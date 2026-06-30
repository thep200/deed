#include "infra/import/import_service.hpp"

namespace core::infra {
namespace d = core::domain;

// Detection order mirrors the UI (GraphQL first: a cURL whose body is a GraphQL document is GraphQL,
// then plain cURL, then grpcurl).
std::optional<d::ImportKind> ImportService::detect(const std::string &text) const {
  if (graphql_.canHandle(text)) return d::ImportKind::GraphQl;
  if (curl_.canHandle(text)) return d::ImportKind::Curl;
  if (grpc_.canHandle(text)) return d::ImportKind::Grpcurl;
  return std::nullopt;
}

d::Result<d::ImportOutcome> ImportService::import(const std::string &text, d::ImportKind kind) const {
  core::ImportParseResult r; // importers emit a domain RequestModel natively (no request_bridge)
  switch (kind) {
  case d::ImportKind::Curl: r = curl_.parse(text); break;
  case d::ImportKind::Grpcurl: r = grpc_.parse(text); break;
  case d::ImportKind::GraphQl: r = graphql_.parse(text); break;
  }
  if (!r.ok || !r.model) return d::Result<d::ImportOutcome>::fail({d::ErrorCode::Parse, r.error, ""});
  return d::Result<d::ImportOutcome>::ok({std::move(*r.model), std::move(r.unknown)});
}

} // namespace core::infra
