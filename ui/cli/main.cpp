// ui/cli — adapter headless để chạy Core không cần GUI (README §3, Phase 1).
// Dùng cho dev/CI và thử full luồng send → response.
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>

#include "core/engine.hpp"
#include "core/import_export/importer.hpp"

using namespace core;

namespace {

// Delegate đồng bộ hoá: chờ callback terminal rồi in.
class CliDelegate : public IUiDelegate {
public:
    void onResponse(RequestHandle, const ApiResponse& r) override {
        std::lock_guard<std::mutex> lk(m_);
        std::cout << "--- RESPONSE ---\n";
        if (r.statusCode) std::cout << "Status: " << r.statusCode << " " << r.statusText << "\n";
        else std::cout << "Status: " << r.statusText << " (gRPC)\n";
        std::cout << "Time: " << r.elapsedMs << "ms  Size: " << r.sizeBytes << " bytes\n";
        if (!r.headers.empty()) {
            std::cout << "Headers:\n";
            for (const auto& h : r.headers) std::cout << "  " << h.key << ": " << h.value << "\n";
        }
        if (!r.cookies.empty()) {
            std::cout << "Set-Cookie:\n";
            for (const auto& c : r.cookies)
                std::cout << "  " << c.name << "=" << c.value << " (domain=" << c.domain
                          << " path=" << c.path << ")\n";
        }
        std::cout << "Body:\n" << r.body << "\n";
        done(true);
    }
    void onError(RequestHandle, const ApiError& e) override {
        std::lock_guard<std::mutex> lk(m_);
        std::cout << "--- ERROR [" << toString(e.kind) << "] ---\n" << e.message << "\n";
        done(false);
    }
    bool wait() {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [this] { return finished_; });
        return ok_;
    }

private:
    void done(bool ok) { finished_ = true; ok_ = ok; cv_.notify_all(); }
    std::mutex m_;
    std::condition_variable cv_;
    bool finished_ = false;
    bool ok_ = false;
};

int usage() {
    std::cerr <<
        "apicli — headless driver cho Core\n"
        "  apicli tree <root>\n"
        "  apicli send <root> <relPath>\n"
        "  apicli resolve <root> <template>\n"
        "  apicli validate <jsonText>\n"
        "  apicli import-curl <curl command...>\n"
        "  apicli import-grpc <grpc spec...>\n";
    return 2;
}

void printTree(const TreeNode& n, int depth) {
    std::string indent(static_cast<size_t>(depth) * 2, ' ');
    if (n.isFolder) {
        std::cout << indent << (depth ? "v " : "") << n.name << "/\n";
        for (const auto& c : n.children) printTree(c, depth + 1);
    } else {
        std::cout << indent << "- " << n.name << " [" << toString(n.requestType) << " "
                  << n.methodOrType << "]  (" << n.relPath << ")\n";
    }
}

std::string joinArgs(int argc, char** argv, int from) {
    std::string s;
    for (int i = from; i < argc; ++i) { if (i > from) s += " "; s += argv[i]; }
    return s;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    std::string cmd = argv[1];

    try {
        if (cmd == "tree" && argc >= 3) {
            Engine engine(EngineConfig{argv[2], ""});
            printTree(engine.collection().scanTree(), 0);
            return 0;
        }
        if (cmd == "send" && argc >= 4) {
            Engine engine(EngineConfig{argv[2], ""});
            RequestModel m = engine.collection().loadRequest(argv[3]);
            std::cout << "Sending: " << m.name << " (" << toString(m.type) << ")\n";
            CliDelegate del;
            engine.send(m, &del);
            return del.wait() ? 0 : 1;
        }
        if (cmd == "resolve" && argc >= 4) {
            Engine engine(EngineConfig{argv[2], ""});
            std::cout << engine.resolvePreview(joinArgs(argc, argv, 3)) << "\n";
            return 0;
        }
        if (cmd == "validate" && argc >= 3) {
            Engine engine(EngineConfig{".", ""});
            auto v = engine.validateJson(joinArgs(argc, argv, 2));
            if (v.ok) std::cout << "JSON OK\n";
            else std::cout << "JSON error at line " << v.line << ":" << v.col << " — " << v.msg << "\n";
            return v.ok ? 0 : 1;
        }
        if (cmd == "import-curl" && argc >= 3) {
            CurlImporter imp;
            auto r = imp.parse(joinArgs(argc, argv, 2));
            if (!r.ok) { std::cerr << "Import lỗi: " << r.error << "\n"; return 1; }
            const HttpRequest& h = r.model.http;
            std::cout << "Imported HTTP OK: " << h.method << " " << h.url << "\n";
            std::cout << "  headers=" << h.headers.size() << " params=" << h.params.size()
                      << " bodyMode=" << h.body.mode << " auth=" << h.auth.type
                      << " unknown=" << r.unknown.size() << "\n";
            return 0;
        }
        if (cmd == "import-grpc" && argc >= 3) {
            GrpcImporter imp;
            auto r = imp.parse(joinArgs(argc, argv, 2));
            if (!r.ok) { std::cerr << "Import lỗi: " << r.error << "\n"; return 1; }
            std::cout << "Imported gRPC OK: " << r.model.grpc.service << "/" << r.model.grpc.method
                      << " @ " << r.model.grpc.target << "\n";
            return 0;
        }
    } catch (const std::exception& e) {
        std::cerr << "Lỗi: " << e.what() << "\n";
        return 1;
    }
    return usage();
}
