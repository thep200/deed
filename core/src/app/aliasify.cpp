#include "app/aliasify.hpp"

#include <type_traits>

#include "core/infra/variables/variable_resolver.hpp"

namespace core::app {
namespace d = core::domain;

namespace {

// Shared field rewriters; factory reject -> keep the original value/VO.
struct Aliaser {
  const std::vector<std::pair<std::string, std::string>> &vars;

  std::string whole(const std::string &s) const {
    std::string out, key;
    return core::VariableResolver::valueToAlias(s, vars, out, &key) ? out : s;
  }
  std::string prefix(const std::string &s) const {
    std::string out, key;
    return core::VariableResolver::prefixToAlias(s, vars, out, &key) ? out : s;
  }
  d::Url url(const d::Url &u) const { return d::Url::create(prefix(u.raw())).take(); }
  d::HeaderList headers(const d::HeaderList &hl) const {
    std::vector<d::Header> out;
    out.reserve(hl.items().size());
    for (const auto &h : hl.items()) {
      auto r = d::Header::create(h.name(), h.enabled() ? whole(h.value()) : h.value(), h.enabled());
      out.push_back(r ? r.take() : h);
    }
    return d::HeaderList(std::move(out));
  }
  d::QueryParamList params(const d::QueryParamList &pl) const {
    std::vector<d::QueryParam> out;
    out.reserve(pl.items().size());
    for (const auto &p : pl.items()) {
      auto r = d::QueryParam::create(p.key(), p.enabled() ? whole(p.value()) : p.value(), p.enabled());
      out.push_back(r ? r.take() : p);
    }
    return d::QueryParamList(std::move(out));
  }
  d::PathVariableList pathVars(const d::PathVariableList &pl) const {
    std::vector<d::PathVariable> out;
    out.reserve(pl.items().size());
    for (const auto &p : pl.items()) {
      auto r = d::PathVariable::create(p.key(), p.enabled() ? whole(p.value()) : p.value(), p.enabled());
      out.push_back(r ? r.take() : p);
    }
    return d::PathVariableList(std::move(out));
  }
  d::GrpcMetadata metadata(const d::GrpcMetadata &md) const {
    std::vector<d::MetadataEntry> out;
    out.reserve(md.entries().size());
    for (const auto &e : md.entries())
      out.push_back({e.key, e.enabled ? whole(e.value) : e.value, e.enabled});
    auto r = d::GrpcMetadata::create(std::move(out));
    return r ? r.take() : md;
  }
  d::Auth auth(const d::Auth &a) const {
    return a.match([&](auto &&x) -> d::Auth {
      using T = std::decay_t<decltype(x)>;
      if constexpr (std::is_same_v<T, d::AuthNone>) return d::Auth::none();
      else if constexpr (std::is_same_v<T, d::AuthBearer>) {
        auto r = d::Auth::bearer(whole(x.token));
        return r ? r.take() : a;
      } else if constexpr (std::is_same_v<T, d::AuthBasic>) {
        auto r = d::Auth::basic(whole(x.username), whole(x.password));
        return r ? r.take() : a;
      } else { // AuthOAuth2 — alias every user-typed string field, branches kept explicit
        d::AuthOAuth2 o = x;
        o.tokenUrl = whole(o.tokenUrl);
        o.clientId = whole(o.clientId);
        o.clientSecret = whole(o.clientSecret);
        o.scope = whole(o.scope);
        o.username = whole(o.username);
        o.password = whole(o.password);
        auto r = d::Auth::oauth2(std::move(o));
        return r ? r.take() : a;
      }
    });
  }
};

// One overload per payload type; dispatch by overload resolution, so a new type without one is a compile error.
d::RequestModel::Payload aliasTyped(const d::HttpRequest &p, const Aliaser &al) {
  d::HttpRequest::Parts hp{p.method(),           al.url(p.url()),        al.pathVars(p.pathVariables()),
                           al.params(p.params()), al.headers(p.headers()), p.body(),
                           al.auth(p.auth())};
  return d::HttpRequest::create(std::move(hp)).take();
}

d::RequestModel::Payload aliasTyped(const d::GrpcRequest &p, const Aliaser &al) {
  d::GrpcRequest::Parts gp{al.prefix(p.target()), p.service(), p.method(),
                           p.methodType(),        p.message(), al.metadata(p.metadata()),
                           p.protoSource(),       p.tls()};
  return d::GrpcRequest::create(std::move(gp)).take();
}

d::RequestModel::Payload aliasTyped(const d::WebSocketRequest &p, const Aliaser &al) {
  d::WebSocketRequest::Parts wp{al.url(p.url()),   p.subprotocols(), al.headers(p.headers()),
                                al.auth(p.auth()), p.onOpenSend(),   p.defaultSendKind()};
  auto r = d::WebSocketRequest::create(std::move(wp));
  return r ? d::RequestModel::Payload(r.take()) : d::RequestModel::Payload(p);
}

d::RequestModel::Payload aliasTyped(const d::GraphQlRequest &p, const Aliaser &al) {
  // url + headers + auth only (query/variables are body-like -> untouched)
  d::GraphQlRequest::Parts gp{al.url(p.url()),   p.op(),           al.headers(p.headers()),
                              al.auth(p.auth()), p.subTransport(), p.wsProtocol()};
  auto r = d::GraphQlRequest::create(std::move(gp));
  return r ? d::RequestModel::Payload(r.take()) : d::RequestModel::Payload(p);
}

d::RequestModel::Payload aliasTyped(const d::KafkaRequest &p, const Aliaser &al) {
  // brokers (prefix, like url/target) + topic/group (whole); message VALUE is body-like -> untouched
  auto aliasTopic = [&](const d::KafkaTopic &t) {
    auto r = d::KafkaTopic::create(al.whole(t.value()));
    return r ? r.take() : t;
  };
  auto aliasKafkaHeaders = [&](const std::vector<d::KafkaHeader> &hs) {
    std::vector<d::KafkaHeader> out;
    out.reserve(hs.size());
    for (const auto &h : hs) out.push_back({h.key, h.enabled ? al.whole(h.value) : h.value, h.enabled});
    return out;
  };
  auto brokers = d::BrokerList::parse(al.prefix(p.brokers().toBootstrapServers()));
  d::BrokerList newBrokers = brokers ? brokers.take() : p.brokers();
  auto mode = p.match([&](auto &&spec) -> d::KafkaRequest::Mode {
    using S = std::decay_t<decltype(spec)>;
    if constexpr (std::is_same_v<S, d::KafkaProduceSpec>) {
      d::KafkaProduceSpec out = spec;
      out.config.topic = aliasTopic(spec.config.topic);
      if (out.message.key) out.message.key = d::MessageKey{al.whole(spec.message.key->value)};
      out.message.headers = aliasKafkaHeaders(spec.message.headers);
      return out;
    } else {
      d::KafkaConsumeSpec out = spec;
      std::vector<d::KafkaTopic> topics;
      for (const auto &t : spec.config.topics) topics.push_back(aliasTopic(t));
      out.config.topics = std::move(topics);
      auto g = d::ConsumerGroup::create(al.whole(spec.config.group.value()));
      out.config.group = g ? g.take() : spec.config.group;
      return out;
    }
  });
  auto r = d::KafkaRequest::create(newBrokers, p.security(), std::move(mode));
  return r ? d::RequestModel::Payload(r.take()) : d::RequestModel::Payload(p);
}

d::RequestModel::Payload aliasTyped(const d::SoapRequest &p, const Aliaser &al) {
  // url + action + headers + auth; the envelope is body-like -> untouched
  d::SoapRequest::Parts sp{al.url(p.url()), al.whole(p.action()), p.version(), p.envelope(),
                           al.headers(p.headers()), al.auth(p.auth())};
  auto r = d::SoapRequest::create(std::move(sp));
  return r ? d::RequestModel::Payload(r.take()) : d::RequestModel::Payload(p);
}

d::RequestModel::Payload aliasTyped(const d::LdapRequest &p, const Aliaser &al) {
  // url + every string field (dn/filter/group/passwords); scope/limits untouched
  d::LdapRequest::Parts lp{al.url(p.url())};
  lp.startTls = p.startTls();
  lp.bindDn = al.whole(p.bindDn());
  lp.bindPassword = al.whole(p.bindPassword());
  lp.baseDn = al.whole(p.baseDn());
  lp.scope = p.scope();
  lp.filter = al.whole(p.filter());
  lp.attributes = p.attributes();
  for (auto &a : lp.attributes) a = al.whole(a);
  lp.group = al.whole(p.group());
  lp.testPassword = al.whole(p.testPassword());
  lp.sizeLimit = p.sizeLimit();
  lp.timeLimit = p.timeLimit();
  lp.pageSize = p.pageSize();
  auto r = d::LdapRequest::create(std::move(lp));
  return r ? d::RequestModel::Payload(r.take()) : d::RequestModel::Payload(p);
}

} // namespace

d::RequestModel aliasifyModel(const d::RequestModel &model,
                              const std::vector<std::pair<std::string, std::string>> &vars) {
  Aliaser al{vars};
  auto payload = model.match([&](const auto &p) { return aliasTyped(p, al); });
  auto rebuilt = d::RequestModel::create(model.id(), model.name(), model.seq(), model.config(),
                                         std::move(payload));
  return rebuilt.isOk() ? rebuilt.take() : model;
}

} // namespace core::app
