// ui/cli — headless adapter to run Core without a GUI (README §3, Phase 1).
// Used for dev/CI and to exercise the full send → response flow.
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "core/engine.hpp"
#include "core/import_export/importer.hpp"

using namespace core;

namespace {

// The CLI's delegate/sink are stack objects that outlive the (blocking) call, so wrap them in
// NON-OWNING shared_ptrs for the Engine's shared_ptr API (C1) — no deleter, no double-free.
std::shared_ptr<IUiDelegate> borrow(IUiDelegate* d) {
    return std::shared_ptr<IUiDelegate>(d, [](IUiDelegate*) {});
}
std::shared_ptr<IStreamSink> borrowSink(IStreamSink* s) {
    return std::shared_ptr<IStreamSink>(s, [](IStreamSink*) {});
}

// Minimal duplex sink for the `ws` command: prints frames, signals on first inbound + on close.
class CliWsSink : public IUiDelegate {
public:
    void onResponse(RequestHandle, const ApiResponse&) override {}
    void onError(RequestHandle, const ApiError&) override {}
    void onStreamOpen(const StreamMeta& m) override {
        std::lock_guard<std::mutex> lk(m_);
        std::cout << "--- WS OPEN (" << m.streamId << ") ---\n[";
    }
    void onStreamEvent(const StreamEvent& ev) override {
        std::lock_guard<std::mutex> lk(m_);
        std::cout << (printed_++ == 0 ? "\n  " : ",\n  ") << ev.payload;
        std::cout.flush();
        if (ev.direction == StreamDirection::Inbound) { ++inbound_; cv_.notify_all(); }
    }
    void onStreamClose(const StreamEnd& end) override {
        std::lock_guard<std::mutex> lk(m_);
        std::cout << "\n]\n--- WS CLOSE code=" << end.statusCode
                  << (end.statusMessage.empty() ? "" : (" reason=" + end.statusMessage))
                  << ", " << end.totalEvents << " frames, " << end.elapsedMs << "ms ---\n";
        closed_ = true; cv_.notify_all();
    }
    bool waitInbound(int n, std::chrono::milliseconds to) {
        std::unique_lock<std::mutex> lk(m_);
        return cv_.wait_for(lk, to, [&] { return inbound_ >= n || closed_; });
    }
    void waitClosed(std::chrono::milliseconds to) {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait_for(lk, to, [&] { return closed_; });
    }
private:
    std::mutex m_;
    std::condition_variable cv_;
    int printed_ = 0, inbound_ = 0;
    bool closed_ = false;
};

// Synchronizing delegate: wait for the terminal callback, then print.
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

    // --- Streaming (SPEC_grpc_streaming §3). The CLI prints the array form directly. ---
    void onStreamOpen(const StreamMeta&) override {
        std::lock_guard<std::mutex> lk(m_);
        std::cout << "--- STREAM ---\n[";
    }
    void onStreamEvent(const StreamEvent& ev) override {
        std::lock_guard<std::mutex> lk(m_);
        std::cout << (ev.seq == 0 ? "\n  " : ",\n  ") << ev.payload;
    }
    void onStreamClose(const StreamEnd& end) override {
        std::lock_guard<std::mutex> lk(m_);
        const char* st = end.status == StreamStatus::Ok ? "Ok"
                       : end.status == StreamStatus::Cancelled ? "Cancelled"
                       : end.status == StreamStatus::Timeout ? "Timeout" : "Error";
        std::cout << "\n]\n--- stream " << st << " code=" << end.statusCode
                  << (end.statusMessage.empty() ? "" : (" msg=" + end.statusMessage)) << ", "
                  << end.totalEvents << " events, " << end.elapsedMs << "ms"
                  << (end.truncated ? " (truncated)" : "") << " ---\n";
        done(end.status == StreamStatus::Ok);
    }

    bool wait() {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [this] { return finished_; });
        return ok_;
    }
    // Bounded wait: returns true if the call finished within `to`, false on timeout (caller cancels).
    bool waitFor(std::chrono::milliseconds to) {
        std::unique_lock<std::mutex> lk(m_);
        return cv_.wait_for(lk, to, [this] { return finished_; });
    }
    bool ok() {
        std::lock_guard<std::mutex> lk(m_);
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
        "apicli — headless driver for Core\n"
        "  apicli tree <root>\n"
        "  apicli send <root> <relPath>\n"
        "  apicli resolve <root> <template>\n"
        "  apicli validate <jsonText>\n"
        "  apicli import-curl <curl command...>\n"
        "  apicli import-grpc <grpc spec...>\n"
        "  apicli import-graphql <query | curl...>\n"
        "  apicli ws <url> [message]\n"
        "  apicli sse <url> [seconds]\n"
        "  apicli gql <url> <query...>\n";
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
            // Same routing the UI uses (§4): WebSocket -> a duplex SESSION; methods that stream
            // responses (server-streaming/bidi/SSE) -> openStream; unary + client-streaming -> send.
            InteractionKind kind = engine.interactionOf(m);
            int streamSecs = (argc >= 5) ? std::atoi(argv[4]) : 8;   // bound for open-ended streams (SSE/WS)
            if (kind == InteractionKind::Duplex) {                   // WebSocket
                CliWsSink sink;
                SessionHandle h = engine.openSession(m, borrowSink(&sink));
                sink.waitInbound(1, std::chrono::milliseconds(streamSecs * 1000));
                engine.closeSession(h, 1000, "bye");
                sink.waitClosed(std::chrono::milliseconds(4000));
                return 0;
            }
            CliDelegate del;
            if (kind == InteractionKind::ServerStream || kind == InteractionKind::BiDi) {
                StreamHandle h = engine.openStream(m, borrowSink(&del));
                if (!del.waitFor(std::chrono::milliseconds(streamSecs * 1000))) {
                    engine.cancelStream(h);   // open-ended (e.g. SSE) -> Stop after the window
                    del.wait();
                }
            } else {
                engine.send(m, borrow(&del));
                del.wait();
            }
            return del.ok() ? 0 : 1;
        }
        if (cmd == "resolve" && argc >= 4) {
            Engine engine(EngineConfig{argv[2], ""});
            std::cout << engine.resolvePreview(joinArgs(argc, argv, 3)) << "\n";
            return 0;
        }
        if (cmd == "grpc-list" && argc >= 3) {   // grpc-list <host:port> — reflection method dump
            Engine engine(EngineConfig{".", ""});
            GrpcRequest g; g.target = argv[2]; g.protoSource.mode = "reflection";
            std::string err;
            auto methods = engine.listGrpcMethods(g, err);
            if (!err.empty()) { std::cerr << "reflection error: " << err << "\n"; return 1; }
            for (const auto& m : methods)
                std::cout << m.service << "/" << m.method << "  [" << m.methodType << "]\n";
            return 0;
        }
        if (cmd == "gql" && argc >= 4) {   // gql <url> <query...> — GraphQL query/mutation over HTTP
            Engine engine(EngineConfig{".", ""});
            RequestModel m;
            m.type = RequestType::GraphQL;
            m.graphql.url = argv[2];
            m.graphql.query = joinArgs(argc, argv, 3);
            std::cout << "GraphQL: " << m.graphql.url << "\n";
            CliDelegate del;
            engine.send(m, borrow(&del));   // query/mutation routes to HTTP under the hood
            return del.wait() ? 0 : 1;
        }
        if (cmd == "sse" && argc >= 3) {   // sse <url> [seconds] — stream events, then Stop
            Engine engine(EngineConfig{".", ""});
            RequestModel m;
            m.type = RequestType::Http;
            m.http.method = "GET";
            m.http.url = argv[2];
            // Trigger SSE the standard way — via the Accept header (no explicit streamMode).
            m.http.headers.push_back({"Accept", "text/event-stream", true});
            std::cout << "SSE: " << m.http.url << "\n";
            CliDelegate del;
            StreamHandle h = engine.openStream(m, borrowSink(&del));
            int secs = argc >= 4 ? std::atoi(argv[3]) : 4;
            std::this_thread::sleep_for(std::chrono::seconds(secs > 0 ? secs : 4));
            engine.cancelStream(h);   // SSE is open-ended -> Stop after a window
            del.wait();
            return 0;
        }
        if (cmd == "ws" && argc >= 3) {   // ws <url> [message] — connect, (send), echo, close
            Engine engine(EngineConfig{".", ""});
            RequestModel m;
            m.type = RequestType::WebSocket;
            m.ws.url = argv[2];
            std::string msg = argc >= 4 ? joinArgs(argc, argv, 3) : std::string();
            if (!msg.empty()) m.ws.onOpenSend.push_back(msg);   // sent right after open
            std::cout << "Connecting: " << m.ws.url << "\n";
            CliWsSink sink;
            SessionHandle h = engine.openSession(m, borrowSink(&sink));
            if (!msg.empty()) sink.waitInbound(1, std::chrono::milliseconds(8000));  // wait for the echo
            else sink.waitInbound(1, std::chrono::milliseconds(3000));
            engine.closeSession(h, 1000, "bye");
            sink.waitClosed(std::chrono::milliseconds(4000));
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
            if (!r.ok) { std::cerr << "Import error: " << r.error << "\n"; return 1; }
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
            if (!r.ok) { std::cerr << "Import error: " << r.error << "\n"; return 1; }
            std::cout << "Imported gRPC OK: " << r.model.grpc.service << "/" << r.model.grpc.method
                      << " @ " << r.model.grpc.target << "\n";
            return 0;
        }
        if (cmd == "import-graphql" && argc >= 3) {
            GraphQlImporter imp;
            auto r = imp.parse(joinArgs(argc, argv, 2));
            if (!r.ok) { std::cerr << "Import error: " << r.error << "\n"; return 1; }
            const GraphQlRequest& g = r.model.graphql;
            std::cout << "Imported GraphQL OK: url=" << g.url << " op=" << (int)g.operation
                      << " vars=" << g.variablesJson << " auth=" << g.auth.type << "\n  query: " << g.query << "\n";
            return 0;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    return usage();
}
