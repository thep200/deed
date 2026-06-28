#include <algorithm>
#include <cctype>

#include "core/import_export/importer.hpp"
#include "import_export/shell_tokenize.hpp"

namespace core {

namespace {

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

} // namespace

bool GrpcImporter::canHandle(const std::string& input) const {
    std::string t = lower(trim(input.substr(0, 16)));   // prefix only; avoid copying a huge paste (L8)
    if (t.rfind("grpcurl", 0) == 0) return true;                         // grpcurl command
    if (t.rfind("grpc://", 0) == 0 || t.rfind("grpcs://", 0) == 0) return true;  // explicit scheme

    // Shorthand host:port/pkg.Service/Method — STRICT so it isn't mistaken for an HTTP URL:
    //   - no whitespace,
    //   - host must have ':' (host:port),
    //   - path = "Service/Method" exactly 2 segments, Service has a '.' (namespace),
    //     Method starts with an UPPERCASE letter (PascalCase).
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

// Push a "key: value" header token onto g.metadata (value may be empty / colon-less).
void addGrpcMetadata(GrpcRequest& g, const std::string& hv) {
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
void applyGrpcurlValueFlag(const std::string& flag, const std::string& v, GrpcRequest& g) {
    if (flag == "-d") g.message = v;
    else if (flag == "-H" || flag == "-rpc-header") addGrpcMetadata(g, v);
    else if (flag == "-import-path") { g.protoSource.importPaths.push_back(v); g.protoSource.mode = "protoFiles"; }
    else if (flag == "-proto") { g.protoSource.files.push_back(v); g.protoSource.mode = "protoFiles"; }
}

// (a) grpcurl [-plaintext] -d '{...}' -H 'k: v' host:port pkg.Service/Method
// Parses flags into g; unknown flags go to res.unknown. The RPC (Service/Method) is INTENTIONALLY
// skipped on import — we only take target/message/metadata/tls; the user picks the RPC from the dropdown.
void parseGrpcurl(const std::vector<std::string>& tokens, GrpcRequest& g, ImportResult& res) {
    bool plaintext = false;
    bool tlsSeen = false;
    std::vector<std::string> positionals;
    for (size_t i = 1; i < tokens.size(); ++i) {
        const std::string& tk = tokens[i];
        if (tk == "-plaintext") { plaintext = true; tlsSeen = true; }
        else if (tk == "-insecure") { g.tls.insecureSkipVerify = true; tlsSeen = true; }
        else if (grpcurlFlagTakesValue(tk)) { if (i + 1 < tokens.size()) applyGrpcurlValueFlag(tk, tokens[++i], g); }
        else if (!tk.empty() && tk[0] == '-') res.unknown.push_back(tk);
        else positionals.push_back(tk);
    }
    if (!positionals.empty()) g.target = positionals[0]; // positionals: host:port [pkg.Service/Method]
    g.tls.enabled = tlsSeen ? !plaintext : true;         // grpcurl defaults to TLS unless -plaintext
    if (g.message.empty()) g.message = "{}";
}

// (b) [grpc://|grpcs://]host:port/pkg.Service/Method. Sets g.target/tls, or res.error on a bad format.
void parseGrpcShorthand(const std::string& trimmed, GrpcRequest& g, ImportResult& res) {
    std::string s = trimmed;
    bool secure = true;
    if (lower(s).rfind("grpcs://", 0) == 0) { secure = true; s = s.substr(8); }
    else if (lower(s).rfind("grpc://", 0) == 0) { secure = false; s = s.substr(7); }
    g.tls.enabled = secure;

    size_t slash = s.find('/'); // host:port is the part before the first '/'
    if (slash == std::string::npos) {
        res.error = "missing Service/Method (format host:port/pkg.Service/Method)";
        return;
    }
    g.target = s.substr(0, slash);   // host:port only; Service/Method skipped (picked after import)
    g.message = "{}";
}

} // namespace

ImportResult GrpcImporter::parse(const std::string& input) const {
    ImportResult res;
    RequestModel m;
    m.type = RequestType::Grpc;
    m.name = "Imported gRPC";
    GrpcRequest& g = m.grpc;
    g.methodType = "unary";
    g.protoSource.mode = "reflection";

    std::string trimmed = trim(input);
    if (lower(trimmed).rfind("grpcurl", 0) == 0) parseGrpcurl(shellTokenize(trimmed), g, res);
    else parseGrpcShorthand(trimmed, g, res);
    if (!res.error.empty()) return res;

    if (g.target.empty()) {   // only the target is required; Service/Method are picked after import
        res.error = "missing target (host:port)";
        return res;
    }
    m.config.tls = g.tls.enabled;   // TLS now lives in the per-request Config (RequestConfig)
    res.ok = true;
    res.model = std::move(m);
    return res;
}

} // namespace core
