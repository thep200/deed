#include "infra/serialization/payload/payload_json.hpp"

#include "core/infra/serialization/field_json.hpp"

namespace core::infra::payload {

namespace {

json tlsToJson(const d::TlsConfig &t) {
  return json{{"enabled", t.enabled()},
              {"insecureSkipVerify", t.insecureSkipVerify()},
              {"caCertPath", t.caCertPath()},
              {"clientCertPath", t.clientCertPath()},
              {"clientKeyPath", t.clientKeyPath()}};
}

d::TlsConfig tlsFromJson(const json &j) {
  if (!j.is_object()) return d::TlsConfig::disabled();
  return d::TlsConfig::create(gb(j, "enabled", false), gb(j, "insecureSkipVerify", false),
                              gs(j, "caCertPath"), gs(j, "clientCertPath"), gs(j, "clientKeyPath"));
}

json protoSourceToJson(const d::ProtoSource &ps) {
  json j;
  ps.match([&](auto &&p) {
    using T = std::decay_t<decltype(p)>;
    if constexpr (std::is_same_v<T, d::ProtoReflection>) {
      j["mode"] = "reflection";
    } else if constexpr (std::is_same_v<T, d::ProtoFiles>) {
      j["mode"] = "protoFiles";
      j["importPaths"] = p.importPaths;
      j["protoFiles"] = p.protoFiles;
    } else if constexpr (std::is_same_v<T, d::ProtoDescriptorSet>) {
      j["mode"] = "descriptorSet";
      j["path"] = p.descriptorSetPath;
    }
  });
  return j;
}

d::Result<d::ProtoSource> protoSourceFromJson(const json &j) {
  std::string mode = j.is_object() ? gs(j, "mode", "reflection") : "reflection";
  if (mode == "protoFiles") {
    std::vector<std::string> imp, files;
    if (auto it = j.find("importPaths"); it != j.end() && it->is_array())
      for (const auto &e : *it)
        if (e.is_string()) imp.push_back(e.get<std::string>());
    if (auto it = j.find("protoFiles"); it != j.end() && it->is_array())
      for (const auto &e : *it)
        if (e.is_string()) files.push_back(e.get<std::string>());
    return d::ProtoSource::files(std::move(imp), std::move(files));
  }
  if (mode == "descriptorSet") return d::ProtoSource::descriptorSet(gs(j, "path"));
  return d::Result<d::ProtoSource>::ok(d::ProtoSource::reflection());
}

} // namespace

json toJson(const d::GrpcRequest &g) {
  return json{{"target", g.target()},
              {"service", g.service()},
              {"method", g.method()},
              {"methodType", d::toString(g.methodType())},
              {"message", g.message().text()},
              {"metadata", serialTo(core::serial::metadataToJson(g.metadata()))},
              {"protoSource", protoSourceToJson(g.protoSource())},
              {"tls", tlsToJson(g.tls())}};
}

d::Result<Payload> parse(d::TypeTag<d::GrpcRequest>, const json &b) {
  d::GrpcRequest::Parts p;
  p.target = gs(b, "target");
  p.service = gs(b, "service");
  p.method = gs(b, "method");
  auto mt = d::parseGrpcMethodType(gs(b, "methodType", "unary"));
  p.methodType = mt ? mt.take() : d::GrpcMethodType::Unary;
  p.message = d::JsonText::of(gs(b, "message", "{}"));
  auto md = serialFrom<d::GrpcMetadata>(b, "metadata", core::serial::jsonToMetadata, "[]");
  if (!md) return d::Result<Payload>::fail(md.error());
  p.metadata = md.take();
  auto ps = protoSourceFromJson(b.value("protoSource", json::object()));
  if (!ps) return d::Result<Payload>::fail(ps.error());
  p.protoSource = ps.take();
  p.tls = tlsFromJson(b.value("tls", json::object()));
  auto r = d::GrpcRequest::create(std::move(p));
  if (!r) return d::Result<Payload>::fail(r.error());
  return d::Result<Payload>::ok(Payload{r.take()});
}

} // namespace core::infra::payload
