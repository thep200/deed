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

// Split "pkg.Service/Method" -> service, method.
bool splitServiceMethod(const std::string& sm, std::string& service, std::string& method) {
    size_t slash = sm.rfind('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= sm.size()) return false;
    service = sm.substr(0, slash);
    method = sm.substr(slash + 1);
    return true;
}

} // namespace

bool GrpcImporter::canHandle(const std::string& input) const {
    std::string t = lower(trim(input));
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

ImportResult GrpcImporter::parse(const std::string& input) const {
    ImportResult res;
    RequestModel m;
    m.type = RequestType::Grpc;
    m.name = "Imported gRPC";
    GrpcRequest& g = m.grpc;
    g.methodType = "unary";
    g.protoSource.mode = "reflection";

    std::string trimmed = trim(input);
    bool isGrpcurl = lower(trimmed).rfind("grpcurl", 0) == 0;

    if (isGrpcurl) {
        // (a) grpcurl [-plaintext] -d '{...}' -H 'k: v' host:port pkg.Service/Method
        auto tokens = shellTokenize(trimmed);
        bool plaintext = false;
        bool tlsSeen = false;
        std::vector<std::string> positionals;
        for (size_t i = 1; i < tokens.size(); ++i) {
            const std::string& tk = tokens[i];
            if (tk == "-plaintext") { plaintext = true; tlsSeen = true; }
            else if (tk == "-insecure") { g.tls.insecureSkipVerify = true; tlsSeen = true; }
            else if (tk == "-d") {
                if (i + 1 < tokens.size()) g.message = tokens[++i];
            } else if (tk == "-H" || tk == "-rpc-header") {
                if (i + 1 < tokens.size()) {
                    std::string hv = tokens[++i];
                    size_t colon = hv.find(':');
                    KeyValue kv;
                    kv.key = trim(colon == std::string::npos ? hv : hv.substr(0, colon));
                    kv.value = trim(colon == std::string::npos ? "" : hv.substr(colon + 1));
                    g.metadata.push_back(kv);
                }
            } else if (tk == "-import-path") {
                if (i + 1 < tokens.size()) { g.protoSource.importPaths.push_back(tokens[++i]); g.protoSource.mode = "protoFiles"; }
            } else if (tk == "-proto") {
                if (i + 1 < tokens.size()) { g.protoSource.files.push_back(tokens[++i]); g.protoSource.mode = "protoFiles"; }
            } else if (!tk.empty() && tk[0] == '-') {
                res.unknown.push_back(tk);
            } else {
                positionals.push_back(tk);
            }
        }
        // positionals: host:port  pkg.Service/Method
        if (positionals.size() >= 1) g.target = positionals[0];
        if (positionals.size() >= 2) {
            if (!splitServiceMethod(positionals[1], g.service, g.method)) {
                res.error = "cannot parse Service/Method from: " + positionals[1];
                return res;
            }
        }
        g.tls.enabled = tlsSeen ? !plaintext : true; // grpcurl defaults to TLS unless -plaintext
        if (g.message.empty()) g.message = "{}";
    } else {
        // (b) [grpc://|grpcs://]host:port/pkg.Service/Method
        std::string s = trimmed;
        bool secure = true;
        if (lower(s).rfind("grpcs://", 0) == 0) { secure = true; s = s.substr(8); }
        else if (lower(s).rfind("grpc://", 0) == 0) { secure = false; s = s.substr(7); }
        g.tls.enabled = secure;

        // host:port is the part before the first '/'.
        size_t slash = s.find('/');
        if (slash == std::string::npos) {
            res.error = "missing Service/Method (format host:port/pkg.Service/Method)";
            return res;
        }
        g.target = s.substr(0, slash);
        std::string sm = s.substr(slash + 1);
        if (!splitServiceMethod(sm, g.service, g.method)) {
            res.error = "cannot parse Service/Method from: " + sm;
            return res;
        }
        g.message = "{}";
    }

    if (g.target.empty() || g.service.empty() || g.method.empty()) {
        res.error = "missing target/service/method";
        return res;
    }
    res.ok = true;
    res.model = std::move(m);
    return res;
}

} // namespace core
