#include <cctype>
#include <string>
#include <vector>

#include "core/infra/import/importer.hpp"
#include "infra/import/shell_tokenize.hpp"
#include "infra/transport/shared/dto_common.hpp"

namespace core {

namespace d = core::domain;

namespace {

// Mutable parse scratch only — not the persisted model.
struct SProtoSource {
    std::string mode = "reflection"; // reflection|protoFiles|descriptorSet
    std::vector<std::string> importPaths, files;
    std::string descriptorSetPath;
};
struct STls {
    bool enabled = true;
    bool insecureSkipVerify = false;
    std::string caCertPath, clientCertPath, clientKeyPath;
};
struct SGrpc {
    std::string target, service, method;
    std::string message;
    std::vector<KeyValue> metadata;
    SProtoSource protoSource;
    STls tls;
};
struct Acc {
    std::vector<std::string> unknown;
    std::string error;
};

} // namespace

bool GrpcImporter::canHandle(const std::string& input) const {
    std::string t = lower(trim(input.substr(0, 16)));   // prefix only; avoid copying a huge paste
    if (t.rfind("grpcurl", 0) == 0) return true;
    if (t.rfind("grpc://", 0) == 0 || t.rfind("grpcs://", 0) == 0) return true;

    // Shorthand host:port/pkg.Service/Method — strict so it isn't mistaken for a plain HTTP URL.
    std::string s = trim(input);
    if (s.empty() || s.find(' ') != std::string::npos) return false;
    size_t slash = s.find('/');
    if (slash == std::string::npos) return false;
    std::string host = s.substr(0, slash);
    if (host.find(':') == std::string::npos) return false;              // need host:port
    std::string rest = s.substr(slash + 1);                            // pkg.Service/Method
    size_t slash2 = rest.find('/');
    if (slash2 == std::string::npos) return false;                     // need Service/Method
    std::string svc = rest.substr(0, slash2);
    std::string method = rest.substr(slash2 + 1);
    if (svc.find('.') == std::string::npos) return false;              // Service must be dotted
    if (method.empty() || method.find('/') != std::string::npos) return false; // exactly 2 segments
    return std::isupper(static_cast<unsigned char>(method[0])) != 0;   // Method PascalCase
}

namespace {

void addGrpcMetadata(SGrpc& g, const std::string& hv) {
    size_t colon = hv.find(':');
    KeyValue kv;
    kv.key = trim(colon == std::string::npos ? hv : hv.substr(0, colon));
    kv.value = trim(colon == std::string::npos ? "" : hv.substr(colon + 1));
    g.metadata.push_back(kv);
}

// grpcurl flags that consume the following token as their value.
bool grpcurlFlagTakesValue(const std::string& tk) {
    return tk == "-d" || tk == "-H" || tk == "-rpc-header" || tk == "-import-path" || tk == "-proto";
}
void applyGrpcurlValueFlag(const std::string& flag, const std::string& v, SGrpc& g) {
    if (flag == "-d") g.message = v;
    else if (flag == "-H" || flag == "-rpc-header") addGrpcMetadata(g, v);
    else if (flag == "-import-path") { g.protoSource.importPaths.push_back(v); g.protoSource.mode = "protoFiles"; }
    else if (flag == "-proto") { g.protoSource.files.push_back(v); g.protoSource.mode = "protoFiles"; }
}

// Service/Method is intentionally skipped on import — the user picks the RPC from the dropdown.
void parseGrpcurl(const std::vector<std::string>& tokens, SGrpc& g, Acc& acc) {
    bool plaintext = false;
    bool tlsSeen = false;
    std::vector<std::string> positionals;
    for (size_t i = 1; i < tokens.size(); ++i) {
        const std::string& tk = tokens[i];
        if (tk == "-plaintext") { plaintext = true; tlsSeen = true; }
        else if (tk == "-insecure") { g.tls.insecureSkipVerify = true; tlsSeen = true; }
        else if (grpcurlFlagTakesValue(tk)) { if (i + 1 < tokens.size()) applyGrpcurlValueFlag(tk, tokens[++i], g); }
        else if (!tk.empty() && tk[0] == '-') acc.unknown.push_back(tk);
        else positionals.push_back(tk);
    }
    if (!positionals.empty()) g.target = positionals[0]; // positionals: host:port [pkg.Service/Method]
    g.tls.enabled = tlsSeen ? !plaintext : true;         // grpcurl defaults to TLS unless -plaintext
    if (g.message.empty()) g.message = "{}";
}

// [grpc://|grpcs://]host:port/pkg.Service/Method
void parseGrpcShorthand(const std::string& trimmed, SGrpc& g, Acc& acc) {
    std::string s = trimmed;
    bool secure = true;
    if (lower(s).rfind("grpcs://", 0) == 0) { secure = true; s = s.substr(8); }
    else if (lower(s).rfind("grpc://", 0) == 0) { secure = false; s = s.substr(7); }
    g.tls.enabled = secure;

    size_t slash = s.find('/');
    if (slash == std::string::npos) {
        acc.error = "missing Service/Method (format host:port/pkg.Service/Method)";
        return;
    }
    g.target = s.substr(0, slash);   // host:port only; Service/Method skipped (picked after import)
    g.message = "{}";
}

d::RequestModel buildGrpcDomain(const SGrpc& g) {
    std::vector<d::MetadataEntry> md;
    for (const auto& kv : g.metadata) md.push_back({kv.key, kv.value, kv.enabled});
    auto mdR = d::GrpcMetadata::create(std::move(md));

    d::ProtoSource ps = d::ProtoSource::reflection();
    if (g.protoSource.mode == "protoFiles") {
        auto r = d::ProtoSource::files(g.protoSource.importPaths, g.protoSource.files);
        if (r) ps = r.take();
    } else if (g.protoSource.mode == "descriptorSet") {
        auto r = d::ProtoSource::descriptorSet(g.protoSource.descriptorSetPath);
        if (r) ps = r.take();
    }

    d::GrpcRequest::Parts gp; // no Url member -> default-constructible
    gp.target = g.target;
    gp.service = g.service;
    gp.method = g.method;
    gp.message = d::JsonText::of(g.message);
    gp.metadata = mdR ? mdR.take() : d::GrpcMetadata::empty();
    gp.protoSource = ps;
    gp.tls = d::TlsConfig::create(g.tls.enabled, g.tls.insecureSkipVerify, g.tls.caCertPath,
                                  g.tls.clientCertPath, g.tls.clientKeyPath);
    auto grpc = d::GrpcRequest::create(std::move(gp)).take();
    d::RequestConfig cfg{d::Timeout::fromMillis(1800000).take(), g.tls.enabled}; // TLS -> per-request Config
    return d::RequestModel::create(d::RequestId(""), "Imported gRPC", 0, cfg, grpc).take();
}

} // namespace

ImportParseResult GrpcImporter::parse(const std::string& input) const {
    Acc acc;
    SGrpc g;

    std::string trimmed = trim(input);
    if (lower(trimmed).rfind("grpcurl", 0) == 0) parseGrpcurl(shellTokenize(trimmed), g, acc);
    else parseGrpcShorthand(trimmed, g, acc);
    if (!acc.error.empty()) return {false, std::nullopt, {}, acc.error};

    if (g.target.empty())   // only the target is required; Service/Method are picked after import
        return {false, std::nullopt, {}, "missing target (host:port)"};
    return {true, buildGrpcDomain(g), acc.unknown, ""};
}

} // namespace core
