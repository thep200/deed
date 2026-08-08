#include "infra/variables/domain_variable_resolver.hpp"

#include <map>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "core/infra/variables/variable_resolver.hpp"

// Only VALUES are templated (keys/names stay as-is; templating a header NAME is unsupported). A factory
// that rejects a resolved value falls back to the original VO/field, so resolution never breaks a send.
namespace core::infra {
namespace d = core::domain;

namespace {
using VarMap = std::map<std::string, std::string>;

std::string rs(const std::string &s, const VarMap &v) {
  return core::VariableResolver::resolve(s, v).text;
}

d::Url resolveUrl(const d::Url &u, const VarMap &v) { return d::Url::create(rs(u.raw(), v)).take(); }

d::HeaderList resolveHeaders(const d::HeaderList &hl, const VarMap &v) {
  std::vector<d::Header> out;
  out.reserve(hl.items().size());
  for (const auto &h : hl.items()) {
    auto r = d::Header::create(h.name(), rs(h.value(), v), h.enabled());
    out.push_back(r ? r.take() : h); // keep original if a resolved value somehow violates an invariant
  }
  return d::HeaderList(std::move(out));
}

d::QueryParamList resolveParams(const d::QueryParamList &pl, const VarMap &v) {
  std::vector<d::QueryParam> out;
  out.reserve(pl.items().size());
  for (const auto &p : pl.items()) {
    auto r = d::QueryParam::create(p.key(), rs(p.value(), v), p.enabled());
    out.push_back(r ? r.take() : p);
  }
  return d::QueryParamList(std::move(out));
}

d::PathVariableList resolvePathVars(const d::PathVariableList &pl, const VarMap &v) {
  std::vector<d::PathVariable> out;
  out.reserve(pl.items().size());
  for (const auto &p : pl.items()) {
    auto r = d::PathVariable::create(p.key(), rs(p.value(), v), p.enabled());
    out.push_back(r ? r.take() : p);
  }
  return d::PathVariableList(std::move(out));
}

d::GrpcMetadata resolveMetadata(const d::GrpcMetadata &md, const VarMap &v) {
  std::vector<d::MetadataEntry> out;
  out.reserve(md.entries().size());
  for (const auto &e : md.entries()) out.push_back({e.key, rs(e.value, v), e.enabled});
  auto r = d::GrpcMetadata::create(std::move(out));
  return r ? r.take() : md;
}

d::Auth resolveAuth(const d::Auth &a, const VarMap &v) {
  return a.match([&](auto &&x) -> d::Auth {
    using T = std::decay_t<decltype(x)>;
    if constexpr (std::is_same_v<T, d::AuthNone>) {
      return d::Auth::none();
    } else if constexpr (std::is_same_v<T, d::AuthBearer>) {
      auto r = d::Auth::bearer(rs(x.token, v));
      return r ? r.take() : a;
    } else if constexpr (std::is_same_v<T, d::AuthBasic>) {
      auto r = d::Auth::basic(rs(x.username, v), rs(x.password, v));
      return r ? r.take() : a;
    } else { // AuthOAuth2 — {{var}} may sit in any field (incl. the secret); branches kept explicit
      d::AuthOAuth2 o = x;
      o.tokenUrl = rs(o.tokenUrl, v);
      o.clientId = rs(o.clientId, v);
      o.clientSecret = rs(o.clientSecret, v);
      o.scope = rs(o.scope, v);
      o.username = rs(o.username, v);
      o.password = rs(o.password, v);
      auto r = d::Auth::oauth2(std::move(o));
      return r ? r.take() : a;
    }
  });
}

d::Body resolveBody(const d::Body &b, const VarMap &v) {
  return b.match([&](auto &&x) -> d::Body {
    using T = std::decay_t<decltype(x)>;
    if constexpr (std::is_same_v<T, d::BodyRaw>) {
      return d::Body::raw(x.subtype, rs(x.text, v));
    } else if constexpr (std::is_same_v<T, d::BodyFormUrlEncoded>) {
      auto fields = x.fields;
      for (auto &f : fields) f.value = rs(f.value, v);
      return d::Body::formUrlEncoded(std::move(fields));
    } else {
      return b; // None / Multipart / Binary: nothing string-templated to resolve
    }
  });
}

// One overload per payload type; dispatch by overload resolution, so a new type without one is a compile error.
d::RequestModel::Payload resolveTyped(const d::HttpRequest &p, const VarMap &v) {
  d::HttpRequest::Parts hp{p.method(),
                           resolveUrl(p.url(), v),
                           resolvePathVars(p.pathVariables(), v),
                           resolveParams(p.params(), v),
                           resolveHeaders(p.headers(), v),
                           resolveBody(p.body(), v),
                           resolveAuth(p.auth(), v)};
  return d::HttpRequest::create(std::move(hp)).take();
}

d::RequestModel::Payload resolveTyped(const d::GrpcRequest &p, const VarMap &v) {
  d::GrpcRequest::Parts gp{rs(p.target(), v),  p.service(),
                           p.method(),         p.methodType(),
                           d::JsonText::of(rs(p.message().text(), v)),
                           resolveMetadata(p.metadata(), v),
                           p.protoSource(),    p.tls()};
  return d::GrpcRequest::create(std::move(gp)).take();
}

d::RequestModel::Payload resolveTyped(const d::WebSocketRequest &p, const VarMap &v) {
  d::WebSocketRequest::Parts wp{resolveUrl(p.url(), v), p.subprotocols(),
                                resolveHeaders(p.headers(), v), resolveAuth(p.auth(), v),
                                p.onOpenSend(), p.defaultSendKind()};
  for (auto &msg : wp.onOpenSend) msg.payload = rs(msg.payload, v);
  auto r = d::WebSocketRequest::create(std::move(wp));
  return r ? d::RequestModel::Payload(r.take()) : d::RequestModel::Payload(p);
}

d::RequestModel::Payload resolveTyped(const d::GraphQlRequest &p, const VarMap &v) {
  d::GraphQlOperation op = p.op();
  op.query = rs(op.query, v);
  op.variables = d::JsonText::of(rs(op.variables.text(), v));
  d::GraphQlRequest::Parts gp{resolveUrl(p.url(), v), std::move(op), resolveHeaders(p.headers(), v),
                              resolveAuth(p.auth(), v), p.subTransport(), p.wsProtocol()};
  auto r = d::GraphQlRequest::create(std::move(gp));
  return r ? d::RequestModel::Payload(r.take()) : d::RequestModel::Payload(p);
}

d::RequestModel::Payload resolveTyped(const d::SoapRequest &p, const VarMap &v) {
  // url/action/envelope/headers/auth templated; version passes through.
  d::SoapRequest::Parts p2{d::Url::create(rs(p.url().raw(), v)).take()};
  p2.action = rs(p.action(), v);
  p2.version = p.version();
  p2.envelope = rs(p.envelope(), v);
  p2.headers = resolveHeaders(p.headers(), v);
  p2.auth = resolveAuth(p.auth(), v);
  auto r = d::SoapRequest::create(std::move(p2));
  return r ? d::RequestModel::Payload(r.take()) : d::RequestModel::Payload(p);
}

d::RequestModel::Payload resolveTyped(const d::LdapRequest &p, const VarMap &v) {
  // every string field templated (incl. group/testPassword); scope/limits pass through.
  d::LdapRequest::Parts p2{d::Url::create(rs(p.url().raw(), v)).take()};
  p2.startTls = p.startTls();
  p2.bindDn = rs(p.bindDn(), v);
  p2.bindPassword = rs(p.bindPassword(), v);
  p2.baseDn = rs(p.baseDn(), v);
  p2.scope = p.scope();
  p2.filter = rs(p.filter(), v);
  p2.attributes = p.attributes();
  for (auto &a : p2.attributes) a = rs(a, v);
  p2.group = rs(p.group(), v);
  p2.testPassword = rs(p.testPassword(), v);
  p2.sizeLimit = p.sizeLimit();
  p2.timeLimit = p.timeLimit();
  p2.pageSize = p.pageSize();
  auto r = d::LdapRequest::create(std::move(p2));
  return r ? d::RequestModel::Payload(r.take()) : d::RequestModel::Payload(p);
}

d::RequestModel::Payload resolveTyped(const d::KafkaRequest &p, const VarMap &v) {
  // brokers/topic(s)/group/key/value/headers templated
  auto brokers = d::BrokerList::parse(rs(p.brokers().toBootstrapServers(), v));
  d::BrokerList newBrokers = brokers ? brokers.take() : p.brokers();
  auto mode = p.match([&](auto &&spec) -> d::KafkaRequest::Mode {
    using S = std::decay_t<decltype(spec)>;
    if constexpr (std::is_same_v<S, d::KafkaProduceSpec>) {
      d::KafkaProduceSpec out = spec;
      auto t = d::KafkaTopic::create(rs(spec.config.topic.value(), v));
      if (t) out.config.topic = t.take();
      if (out.message.key) out.message.key = d::MessageKey{rs(spec.message.key->value, v)};
      out.message.value.value = rs(spec.message.value.value, v);
      std::vector<d::KafkaHeader> hs;
      for (const auto &h : spec.message.headers)
        hs.push_back({h.key, h.enabled ? rs(h.value, v) : h.value, h.enabled});
      out.message.headers = std::move(hs);
      out.config.schemaRegistry.url = rs(spec.config.schemaRegistry.url, v);
      out.config.schemaRegistry.username = rs(spec.config.schemaRegistry.username, v);
      out.config.schemaRegistry.password = rs(spec.config.schemaRegistry.password, v);
      return out;
    } else {
      d::KafkaConsumeSpec out = spec;
      std::vector<d::KafkaTopic> topics;
      for (const auto &t : spec.config.topics) {
        auto rt = d::KafkaTopic::create(rs(t.value(), v));
        topics.push_back(rt ? rt.take() : t);
      }
      out.config.topics = std::move(topics);
      auto g = d::ConsumerGroup::create(rs(spec.config.group.value(), v));
      out.config.group = g ? g.take() : spec.config.group;
      out.config.schemaRegistry.url = rs(spec.config.schemaRegistry.url, v);
      out.config.schemaRegistry.username = rs(spec.config.schemaRegistry.username, v);
      out.config.schemaRegistry.password = rs(spec.config.schemaRegistry.password, v);
      return out;
    }
  });
  auto r = d::KafkaRequest::create(newBrokers, p.security(), std::move(mode));
  return r ? d::RequestModel::Payload(r.take()) : d::RequestModel::Payload(p);
}

d::RequestModel::Payload resolvePayload(const d::RequestModel &m, const VarMap &v) {
  return m.match([&](const auto &p) { return resolveTyped(p, v); });
}
} // namespace

d::Result<d::RequestModel> DomainVariableResolver::resolve(const d::RequestModel &model,
                                                           const d::VariableScope &scope) const {
  VarMap vars(scope.values.begin(), scope.values.end());
  return d::RequestModel::create(model.id(), model.name(), model.seq(), model.config(),
                                 resolvePayload(model, vars));
}

} // namespace core::infra
