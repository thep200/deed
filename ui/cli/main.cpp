// ui/cli — headless adapter to run Core without a GUI. Drives the domain stack (CoreApiClient) directly;
// no Engine. A blocking observer collects the async ResponseEvents and prints them for the command.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "core/app/core_api_client.hpp"
#include "core/domain/request/request_model.hpp" // build domain models directly for gql/sse/ws
#include "core/infra/import/importer.hpp"
#include "core/infra/serialization/field_json.hpp" // core::serial::kafkaRecordToDisplayJson

using core::app::CoreApiClient;
namespace d = core::domain;

namespace {

// Blocking observer: prints unary responses / errors / stream frames and signals terminal + inbound.
class CliObserver final : public d::IRequestObserver {
public:
  void onEvent(d::RequestExecutionId, const d::ResponseEvent &ev) noexcept override {
    std::lock_guard<std::mutex> lk(m_);
    if (const auto *c = ev.get<d::EvCompleted>()) {
      if (streamOpen_) std::cout << "\n]\n";
      printResponse(c->summary);
      finish(true);
    } else if (const auto *f = ev.get<d::EvFailed>()) {
      if (streamOpen_) std::cout << "\n]\n";
      std::cout << "--- ERROR [" << (int)f->error.kind << "] ---\n" << f->error.message << "\n";
      finish(false);
    } else if (const auto *msg = ev.get<d::EvMessage>()) {
      if (!streamOpen_) { std::cout << "--- STREAM ---\n["; streamOpen_ = true; }
      std::cout << (frames_++ == 0 ? "\n  " : ",\n  ") << msg->payload;
      std::cout.flush();
      ++inbound_;
    } else if (const auto *rec = ev.get<d::EvKafkaRecord>()) {
      if (!streamOpen_) { std::cout << "--- STREAM ---\n["; streamOpen_ = true; }
      std::cout << (frames_++ == 0 ? "\n  " : ",\n  ") << core::serial::kafkaRecordToDisplayJson(rec->record);
      std::cout.flush();
      ++inbound_;
    } else if (const auto *cl = ev.get<d::EvClosed>()) {
      if (streamOpen_) std::cout << "\n]";
      std::cout << "\n--- closed code=" << cl->code.value_or(0)
                << (cl->reason.empty() ? "" : (" reason=" + cl->reason)) << " ---\n";
      finish(true);
    }
    cv_.notify_all();
  }

  bool wait() {
    std::unique_lock<std::mutex> lk(m_);
    cv_.wait(lk, [this] { return finished_; });
    return ok_;
  }
  bool waitFor(std::chrono::milliseconds to) {
    std::unique_lock<std::mutex> lk(m_);
    return cv_.wait_for(lk, to, [this] { return finished_; });
  }
  void waitInbound(int n, std::chrono::milliseconds to) {
    std::unique_lock<std::mutex> lk(m_);
    cv_.wait_for(lk, to, [&] { return inbound_ >= n || finished_; });
  }
  bool ok() {
    std::lock_guard<std::mutex> lk(m_);
    return ok_;
  }

private:
  void printResponse(const d::ApiResponse &r) {
    std::cout << "--- RESPONSE ---\nStatus: " << r.statusCode << "\nTime: " << r.elapsed.count()
              << "ms  Size: " << r.body.size() << " bytes\n";
    if (!r.headers.empty()) {
      std::cout << "Headers:\n";
      for (const auto &h : r.headers) std::cout << "  " << h.name << ": " << h.value << "\n";
    }
    std::cout << "Body:\n" << r.body << "\n";
  }
  void finish(bool ok) { finished_ = true; ok_ = ok; }
  std::mutex m_;
  std::condition_variable cv_;
  bool finished_ = false, ok_ = false, streamOpen_ = false;
  int frames_ = 0, inbound_ = 0;
};

int usage() {
  std::cerr << "apicli — headless driver for Core (domain stack)\n"
               "  apicli tree <root>\n"
               "  apicli send <root> <relPath> [streamSecs]\n"
               "  apicli resolve <root> <template>\n"
               "  apicli validate <jsonText>\n"
               "  apicli import-curl <curl command...>\n"
               "  apicli import-grpc <grpc spec...>\n"
               "  apicli import-graphql <query | curl...>\n"
               "  apicli grpc-list <host:port>\n"
               "  apicli ws <url> [message]\n"
               "  apicli sse <url> [seconds]\n"
               "  apicli gql <url> <query...>\n";
  return 2;
}

void printTree(const core::TreeNode &n, int depth) {
  std::string indent(static_cast<size_t>(depth) * 2, ' ');
  if (n.isFolder) {
    std::cout << indent << (depth ? "v " : "") << n.name << "/\n";
    for (const auto &c : n.children) printTree(c, depth + 1);
  } else {
    std::cout << indent << "- " << n.name << " [" << core::toString(n.requestType) << " " << n.methodOrType
              << "]  (" << n.relPath << ")\n";
  }
}

std::string joinArgs(int argc, char **argv, int from) {
  std::string s;
  for (int i = from; i < argc; ++i) { if (i > from) s += " "; s += argv[i]; }
  return s;
}

const char *typeLabel(d::RequestType t) {
  switch (t) {
  case d::RequestType::Http: return "http";
  case d::RequestType::Grpc: return "grpc";
  case d::RequestType::GraphQl: return "graphql";
  case d::RequestType::WebSocket: return "ws";
  case d::RequestType::Kafka: return "kafka";
  }
  return "http";
}

// Send a domain model through CoreApiClient and print the result. Handles unary, server-stream/SSE
// (bounded wait then cancel), and WebSocket (wait inbound then close).
int sendDomain(CoreApiClient &client, const d::RequestModel &m, int streamSecs) {
  auto obs = std::make_shared<CliObserver>();
  client.refreshVariableScope();
  auto exec = client.send(m, obs);
  if (!exec) { std::cerr << "send error: " << exec.error().message << "\n"; return 1; }
  core::InteractionKind kind = client.interactionOf(m);
  if (kind == core::InteractionKind::Duplex) { // WebSocket
    obs->waitInbound(1, std::chrono::milliseconds(streamSecs * 1000));
    client.closeStream(exec.value(), 1000, "bye");
    obs->waitFor(std::chrono::milliseconds(4000));
    return 0;
  }
  if (kind == core::InteractionKind::ServerStream || kind == core::InteractionKind::BiDi) {
    if (!obs->waitFor(std::chrono::milliseconds(streamSecs * 1000))) {
      client.cancel(exec.value()); // open-ended (SSE) -> Stop after the window
      obs->wait();
    }
    return 0;
  }
  obs->wait();
  return obs->ok() ? 0 : 1;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) return usage();
  std::string cmd = argv[1];
  try {
    if (cmd == "tree" && argc >= 3) {
      auto client = CoreApiClient::create(CoreApiClient::Config{argv[2]});
      printTree(client->collection().scanTree(), 0);
      return 0;
    }
    if (cmd == "send" && argc >= 4) {
      auto client = CoreApiClient::create(CoreApiClient::Config{argv[2]});
      auto m = client->collection().loadRequest(argv[3]); // domain RequestModel
      std::cout << "Sending: " << m.name() << " (" << typeLabel(m.type()) << ")\n";
      return sendDomain(*client, m, argc >= 5 ? std::atoi(argv[4]) : 8);
    }
    if (cmd == "resolve" && argc >= 4) {
      auto client = CoreApiClient::create(CoreApiClient::Config{argv[2]});
      std::cout << client->resolvePreview(joinArgs(argc, argv, 3)) << "\n";
      return 0;
    }
    if (cmd == "grpc-list" && argc >= 3) {
      auto client = CoreApiClient::create();
      core::domain::GrpcRequest::Parts p;
      p.target = argv[2]; // protoSource defaults to reflection
      auto res = client->listGrpcMethods(core::domain::GrpcRequest::create(std::move(p)).take());
      if (!res.isOk()) { std::cerr << "reflection error: " << res.error().message << "\n"; return 1; }
      for (const auto &m : res.value())
        std::cout << m.service << "/" << m.method << "  [" << core::domain::toString(m.type) << "]\n";
      return 0;
    }
    if (cmd == "gql" && argc >= 4) {
      auto client = CoreApiClient::create();
      d::GraphQlOperation op;
      op.query = joinArgs(argc, argv, 3);
      d::GraphQlRequest::Parts gp{d::Url::create(argv[2]).take(), op, d::HeaderList{}, d::Auth::none(),
                                  d::GqlSubTransport::Http, ""};
      auto gql = d::GraphQlRequest::create(std::move(gp));
      if (!gql) { std::cerr << "model error: " << gql.error().message << "\n"; return 1; }
      auto m = d::RequestModel::create(d::RequestId(""), "CLI GraphQL", 0,
                                       d::RequestConfig{d::Timeout::fromMillis(1800000).take(), true},
                                       gql.take())
                   .take();
      std::cout << "GraphQL: " << argv[2] << "\n";
      return sendDomain(*client, m, 8);
    }
    if (cmd == "sse" && argc >= 3) {
      auto client = CoreApiClient::create();
      std::vector<d::Header> hs;
      hs.push_back(d::Header::create("Accept", "text/event-stream").take()); // SSE trigger
      d::HttpRequest::Parts hp{d::HttpMethod::Get, d::Url::create(argv[2]).take(), d::PathVariableList{},
                               d::QueryParamList{}, d::HeaderList{std::move(hs)}, d::Body::none(),
                               d::Auth::none()};
      auto m = d::RequestModel::create(d::RequestId(""), "CLI SSE", 0,
                                       d::RequestConfig{d::Timeout::fromMillis(1800000).take(), true},
                                       d::HttpRequest::create(std::move(hp)).take())
                   .take();
      std::cout << "SSE: " << argv[2] << "\n";
      return sendDomain(*client, m, argc >= 4 ? std::atoi(argv[3]) : 4);
    }
    if (cmd == "ws" && argc >= 3) {
      auto client = CoreApiClient::create();
      d::WebSocketRequest::Parts wp{d::Url::create(argv[2]).take()};
      std::string msg = argc >= 4 ? joinArgs(argc, argv, 3) : std::string();
      if (!msg.empty()) wp.onOpenSend.push_back({d::WsSendKind::Text, msg});
      auto ws = d::WebSocketRequest::create(std::move(wp));
      if (!ws) { std::cerr << "model error: " << ws.error().message << "\n"; return 1; }
      auto m = d::RequestModel::create(d::RequestId(""), "CLI WebSocket", 0,
                                       d::RequestConfig{d::Timeout::fromMillis(1800000).take(), true},
                                       ws.take())
                   .take();
      std::cout << "Connecting: " << argv[2] << "\n";
      return sendDomain(*client, m, 8);
    }
    if (cmd == "validate" && argc >= 3) {
      auto client = CoreApiClient::create();
      auto st = client->validateJson(d::JsonText::of(joinArgs(argc, argv, 2)));
      if (st.isOk()) { std::cout << "JSON OK\n"; return 0; }
      std::cout << "JSON error: " << st.error().message << "\n";
      return 1;
    }
    if (cmd == "import-curl" && argc >= 3) {
      core::CurlImporter imp;
      auto r = imp.parse(joinArgs(argc, argv, 2)); // domain RequestModel out
      if (!r.ok || !r.model) { std::cerr << "Import error: " << r.error << "\n"; return 1; }
      const auto &h = std::get<core::domain::HttpRequest>(r.model->payload());
      std::cout << "Imported HTTP OK: " << core::domain::toString(h.method()) << " " << h.url().raw()
                << "\n  headers=" << h.headers().size() << " params=" << h.params().size()
                << " unknown=" << r.unknown.size() << "\n";
      return 0;
    }
    if (cmd == "import-grpc" && argc >= 3) {
      core::GrpcImporter imp;
      auto r = imp.parse(joinArgs(argc, argv, 2));
      if (!r.ok || !r.model) { std::cerr << "Import error: " << r.error << "\n"; return 1; }
      const auto &g = std::get<core::domain::GrpcRequest>(r.model->payload());
      std::cout << "Imported gRPC OK: target=" << g.target() << " (Service/Method picked after import)\n";
      return 0;
    }
    if (cmd == "import-graphql" && argc >= 3) {
      core::GraphQlImporter imp;
      auto r = imp.parse(joinArgs(argc, argv, 2));
      if (!r.ok || !r.model) { std::cerr << "Import error: " << r.error << "\n"; return 1; }
      const auto &g = std::get<core::domain::GraphQlRequest>(r.model->payload());
      std::cout << "Imported GraphQL OK: query: " << g.op().query << "\n";
      return 0;
    }
  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }
  return usage();
}
