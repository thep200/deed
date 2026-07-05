#include "infra/transport/http/native_http_sender.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include "infra/transport/shared/sse_parser.hpp" // pure text/event-stream parser (no transport coupling)
#include "infra/transport/shared/url_util.hpp"

namespace core::infra {
namespace d = core::domain;

namespace {

const d::HttpRequest &httpOf(const d::RequestModel &m) {
  return *std::get_if<d::HttpRequest>(&m.payload());
}

std::string applyPathVariables(std::string url, const d::PathVariableList &vars) {
  for (const auto &v : vars.items()) {
    if (!v.enabled() || v.key().empty()) continue;
    std::string token = ":" + v.key();
    size_t pos = 0;
    while ((pos = url.find(token, pos)) != std::string::npos) {
      size_t end = pos + token.size();
      char nxt = end < url.size() ? url[end] : '/';
      if (nxt == '/' || nxt == '?' || nxt == '&' || end == url.size()) {
        url.replace(pos, token.size(), v.value());
        pos += v.value().size();
      } else {
        pos = end;
      }
    }
  }
  return url;
}

d::ErrorKind mapCprError(cpr::ErrorCode code) {
  switch (code) {
  case cpr::ErrorCode::OPERATION_TIMEDOUT: return d::ErrorKind::Timeout;
  case cpr::ErrorCode::ABORTED_BY_CALLBACK: return d::ErrorKind::Cancelled;
  case cpr::ErrorCode::SSL_CONNECT_ERROR:
  case cpr::ErrorCode::SSL_CERTPROBLEM:
  case cpr::ErrorCode::SSL_CACERT_BADFILE:
  case cpr::ErrorCode::PEER_FAILED_VERIFICATION:
  case cpr::ErrorCode::USE_SSL_FAILED: return d::ErrorKind::Tls;
  default: return d::ErrorKind::Network;
  }
}

d::Cookie parseSetCookie(const std::string &raw) {
  d::Cookie c;
  size_t semi = raw.find(';');
  std::string first = raw.substr(0, semi);
  size_t eq = first.find('=');
  if (eq != std::string::npos) { c.name = first.substr(0, eq); c.value = first.substr(eq + 1); }
  while (semi != std::string::npos) {
    size_t next = raw.find(';', semi + 1);
    std::string attr = raw.substr(semi + 1, (next == std::string::npos ? raw.size() : next) - semi - 1);
    size_t a = attr.find_first_not_of(" \t");
    if (a != std::string::npos) attr = attr.substr(a);
    std::string lower = attr;
    for (auto &ch : lower) ch = (char)::tolower((unsigned char)ch);
    auto valOf = [&](const std::string &s) {
      size_t e = s.find('=');
      return e == std::string::npos ? std::string() : s.substr(e + 1);
    };
    if (lower.rfind("domain=", 0) == 0) c.domain = valOf(attr);
    else if (lower.rfind("path=", 0) == 0) c.path = valOf(attr);
    else if (lower.rfind("expires=", 0) == 0) c.expires = valOf(attr);
    semi = next;
  }
  return c;
}

void applyAuth(cpr::Session &session, cpr::Header &header, cpr::Parameters &params, const d::Auth &auth) {
  auth.match([&](auto &&a) {
    using T = std::decay_t<decltype(a)>;
    if constexpr (std::is_same_v<T, d::AuthBearer>) {
      header["Authorization"] = "Bearer " + a.token;
    } else if constexpr (std::is_same_v<T, d::AuthBasic>) {
      session.SetAuth(cpr::Authentication{a.username, a.password, cpr::AuthMode::BASIC});
    } else if constexpr (std::is_same_v<T, d::AuthApiKey>) {
      if (a.in == d::ApiKeyIn::Query) { params.Add({a.key, a.value}); session.SetParameters(params); }
      else header[a.key] = a.value;
    }
  });
}

bool readBinaryFile(const std::string &path, const core::CancelToken &cancel, std::string &data) {
  FILE *f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  char buf[64 * 1024];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
    if (cancel.cancelled()) { std::fclose(f); return false; }
    data.append(buf, n);
  }
  std::fclose(f);
  return true;
}

void applyBody(cpr::Session &session, const d::Body &body, const core::CancelToken &cancel) {
  body.match([&](auto &&b) {
    using T = std::decay_t<decltype(b)>;
    if constexpr (std::is_same_v<T, d::BodyRaw>) {
      session.SetBody(cpr::Body{b.text});
    } else if constexpr (std::is_same_v<T, d::BodyFormUrlEncoded>) {
      cpr::Payload payload{};
      for (const auto &f : b.fields) if (f.enabled) payload.Add({f.key, f.value});
      session.SetPayload(payload);
    } else if constexpr (std::is_same_v<T, d::BodyMultipart>) {
      cpr::Multipart mp{};
      for (const auto &p : b.parts) {
        if (!p.enabled) continue;
        if (p.kind == d::PartKind::File) mp.parts.emplace_back(p.key, cpr::File{p.filePath});
        else mp.parts.emplace_back(p.key, p.value);
      }
      session.SetMultipart(mp);
    } else if constexpr (std::is_same_v<T, d::BodyBinary>) {
      if (!b.filePath.empty()) { std::string data; if (readBinaryFile(b.filePath, cancel, data)) session.SetBody(cpr::Body{std::move(data)}); }
    }
  });
}

cpr::Response runVerb(cpr::Session &s, d::HttpMethod m) {
  switch (m) {
  case d::HttpMethod::Post: return s.Post();
  case d::HttpMethod::Put: return s.Put();
  case d::HttpMethod::Patch: return s.Patch();
  case d::HttpMethod::Delete: return s.Delete();
  case d::HttpMethod::Head: return s.Head();
  case d::HttpMethod::Options: return s.Options();
  default: return s.Get();
  }
}

// Configure a cpr::Session from the resolved domain HTTP request (shared by unary + SSE). `extraHeaders`
// are merged last (SSE adds Accept / Last-Event-ID). Cancellation is wired via the progress callback.
void configureSession(cpr::Session &session, const d::HttpRequest &h, const d::RequestModel &model,
                      const std::shared_ptr<core::CancelToken> &token,
                      const std::vector<std::pair<std::string, std::string>> &extraHeaders) {
  std::string url = applyPathVariables(h.url().raw(), h.pathVariables());
  std::vector<core::KeyValue> urlQuery;
  urlutil::splitUrlQuery(url, urlQuery);
  session.SetUrl(cpr::Url{url});

  cpr::Parameters params;
  for (const auto &p : h.params().items()) if (p.enabled() && !p.key().empty()) params.Add({p.key(), p.value()});
  for (const auto &p : urlQuery) if (!p.key.empty()) params.Add({p.key, p.value});
  session.SetParameters(params);

  cpr::Header header;
  for (const auto &hd : h.headers().items()) if (hd.enabled() && !hd.name().empty()) header[hd.name()] = hd.value();
  applyAuth(session, header, params, h.auth());
  for (const auto &e : extraHeaders) if (!e.first.empty()) header[e.first] = e.second;
  session.SetHeader(header);

  applyBody(session, h.body(), *token);

  session.SetTimeout(cpr::Timeout{model.config().timeout.value()});
  session.SetRedirect(cpr::Redirect{50L});
  session.SetVerifySsl(cpr::VerifySsl{model.config().tlsEnabledDefault});
  session.SetProgressCallback(cpr::ProgressCallback{
      [token](cpr::cpr_off_t, cpr::cpr_off_t, cpr::cpr_off_t, cpr::cpr_off_t, intptr_t) -> bool {
        return !token->isCancelled();
      }});
}

long long offsetMsFrom(std::chrono::steady_clock::time_point t0) {
  return static_cast<long long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
}
void trimWs(std::string &x) {
  std::size_t a = x.find_first_not_of(" \t\r\n");
  std::size_t b = x.find_last_not_of(" \t\r\n");
  x = (a == std::string::npos) ? std::string() : x.substr(a, b - a + 1);
}

// Native SSE streamer (ported from the legacy core::HttpSender SseStreamer, emitting domain events). We only
// reach here when the request asked for SSE (Accept: text/event-stream), which == httpForcesSse, so the
// response is always parsed as SSE (no Auto/non-SSE unary-body branch). Reconnects with Last-Event-ID on a
// clean/abnormal end (EventSource §7), up to a cap; enforces total event/byte ceilings.
class SseStreamer {
public:
  SseStreamer(const d::HttpRequest &h, const d::RequestModel &model, d::IResponseSink &sink,
              std::shared_ptr<core::CancelToken> token)
      : h_(h), model_(model), sink_(sink), token_(std::move(token)),
        t0_(std::chrono::steady_clock::now()) {}

  void run() {
    for (int attempt = 0;; ++attempt)
      if (runAttempt(attempt) == Step::Stop) break;
    emitMetaOnce({}); // §3: ensure we counted as "open" even if no chunk arrived
    if (endStatus_ == End::Cancelled) {
      sink_.emit(d::ResponseEvent(d::EvFailed{{d::ErrorKind::Cancelled, endMsg_.empty() ? "Cancelled" : endMsg_, endCode_ ? std::optional<int>(endCode_) : std::nullopt}}));
    } else if (endStatus_ == End::Error) {
      sink_.emit(d::ResponseEvent(d::EvFailed{{d::ErrorKind::Protocol, endMsg_, endCode_ ? std::optional<int>(endCode_) : std::nullopt}}));
    } else {
      d::ApiResponse summary;
      summary.statusCode = endCode_;
      summary.elapsed = std::chrono::milliseconds(offsetMsFrom(t0_));
      sink_.emit(d::ResponseEvent(d::EvCompleted{std::move(summary)}));
    }
  }

private:
  enum class Step { Reconnect, Stop };
  enum class Retry { DoReconnect, Cancelled, CapReached };
  enum class End { Ok, Error, Cancelled };

  struct Conn {
    int status = 0;
    std::vector<d::ResponseHeader> leading;
    bool decided = false;
  };

  void emitMetaOnce(const std::vector<d::ResponseHeader> &leading) {
    if (opened_) return;
    opened_ = true;
    if (!leading.empty()) sink_.emit(d::ResponseEvent(d::EvMetadata{leading}));
  }

  // One completed SSE event -> {"event","id","data"} (data = JSON if parseable, else string) as EvMessage.
  void emitEvent(const SseEvent &e) {
    nlohmann::json env;
    env["event"] = e.event.empty() ? std::string("message") : e.event;
    if (!e.id.empty()) env["id"] = e.id;
    try { env["data"] = nlohmann::json::parse(e.data); }
    catch (...) { env["data"] = e.data; }
    std::string payload = env.dump();
    sink_.emit(d::ResponseEvent(d::EvMessage{d::WsSendKind::Text, payload, static_cast<size_t>(seq_)}));
    ++seq_;
    bytes_ += payload.size();
    lastEventId_ = e.id; // unconditional: an empty `id:` is a valid reset (WHATWG — omit Last-Event-ID then)
  }

  void onHeaderLine(Conn &conn, std::string_view line) {
    std::string s(line);
    if (s.rfind("HTTP/", 0) == 0) {
      std::size_t sp = s.find(' ');
      if (sp != std::string::npos) conn.status = std::atoi(s.c_str() + sp + 1);
      conn.leading.clear();
      return;
    }
    std::size_t c = s.find(':');
    if (c == std::string::npos) return;
    std::string k = s.substr(0, c), v = s.substr(c + 1);
    trimWs(k);
    trimWs(v);
    if (!k.empty()) conn.leading.push_back({k, v});
  }

  bool onChunk(Conn &conn, SseParser &parser, const SseParser::Emit &onEvent, std::string_view data) {
    if (token_ && token_->cancelled()) return false;
    if (truncated_) return false;
    if (!conn.decided) { conn.decided = true; emitMetaOnce(conn.leading); }
    parser.feed(data.data(), data.size(), onEvent);
    if (seq_ >= kMaxEvents || bytes_ >= kMaxTotalBytes) { truncated_ = true; return false; }
    return true;
  }

  Retry tryReconnect(int attempt) {
    if (attempt >= kMaxRetries) return Retry::CapReached;
    // Sleep in short slices so a cancel is observed promptly (retryMs_ may be up to 60s).
    for (long waited = 0; waited < retryMs_; waited += 50) {
      if (token_ && token_->cancelled()) { endStatus_ = End::Cancelled; return Retry::Cancelled; }
      std::this_thread::sleep_for(std::chrono::milliseconds(std::min<long>(50, retryMs_ - waited)));
    }
    if (token_ && token_->cancelled()) { endStatus_ = End::Cancelled; return Retry::Cancelled; }
    return Retry::DoReconnect;
  }

  Step decideAfterPerform(int attempt, const Conn &conn, const cpr::Response &r) {
    if (token_ && token_->cancelled()) { endStatus_ = End::Cancelled; endCode_ = conn.status; return Step::Stop; }
    if (truncated_) { endStatus_ = End::Ok; endCode_ = static_cast<int>(r.status_code); return Step::Stop; }
    bool fatalHttp = (conn.status == 204) || (conn.status >= 400);
    if (r.error && r.error.code != cpr::ErrorCode::OK) { // network drop -> reconnect up to the cap
      switch (tryReconnect(attempt)) {
      case Retry::DoReconnect: return Step::Reconnect;
      case Retry::Cancelled: return Step::Stop;
      case Retry::CapReached: break;
      }
      endStatus_ = End::Error;
      endCode_ = static_cast<int>(r.status_code);
      endMsg_ = r.error.message;
      return Step::Stop;
    }
    if (fatalHttp) {
      endStatus_ = End::Error;
      endCode_ = conn.status ? conn.status : static_cast<int>(r.status_code);
      endMsg_ = "SSE: unexpected HTTP status";
      return Step::Stop;
    }
    // Clean server close on a 2xx SSE stream -> reconnect (EventSource semantics), up to the cap.
    switch (tryReconnect(attempt)) {
    case Retry::DoReconnect: return Step::Reconnect;
    case Retry::Cancelled: return Step::Stop;
    case Retry::CapReached: break;
    }
    endStatus_ = End::Ok;
    endCode_ = static_cast<int>(r.status_code);
    return Step::Stop;
  }

  Step runAttempt(int attempt) {
    cpr::Session session;
    std::vector<std::pair<std::string, std::string>> extra;
    extra.emplace_back("Accept", "text/event-stream");
    if (!lastEventId_.empty()) extra.emplace_back("Last-Event-ID", lastEventId_);
    configureSession(session, h_, model_, token_, extra);
    session.SetAcceptEncoding(
        cpr::AcceptEncoding{{cpr::AcceptEncodingMethods::deflate, cpr::AcceptEncodingMethods::gzip}});

    Conn conn;
    SseParser parser;
    parser.setLastEventId(lastEventId_); // lastEventId persists across reconnects (per spec)
    parser.setMaxEventBytes(static_cast<std::size_t>(kMaxEventBytes));
    SseParser::Emit onEvent = [this](const SseEvent &e) { emitEvent(e); };
    session.SetHeaderCallback(cpr::HeaderCallback{
        [this, &conn](std::string_view line, intptr_t) -> bool { onHeaderLine(conn, line); return true; }});
    session.SetWriteCallback(cpr::WriteCallback{
        [this, &conn, &parser, &onEvent](std::string_view data, intptr_t) -> bool {
          return onChunk(conn, parser, onEvent, data);
        }});

    cpr::Response r = runVerb(session, h_.method());
    emitMetaOnce(conn.leading);
    if (conn.decided && !truncated_) parser.finish(onEvent); // flush a final event with no trailing newline
    if (parser.retryMs() >= 0) retryMs_ = parser.retryMs();
    return decideAfterPerform(attempt, conn, r);
  }

  static constexpr int kRetryDefaultMs = 3000;
  static constexpr int kMaxRetries = 10;
  static constexpr std::uint64_t kMaxEventBytes = 8ull * 1024 * 1024;
  static constexpr std::uint64_t kMaxEvents = 100000;
  static constexpr std::uint64_t kMaxTotalBytes = 64ull * 1024 * 1024;

  const d::HttpRequest &h_;
  const d::RequestModel &model_;
  d::IResponseSink &sink_;
  std::shared_ptr<core::CancelToken> token_;
  std::chrono::steady_clock::time_point t0_;

  bool opened_ = false;
  bool truncated_ = false;
  std::uint64_t seq_ = 0;
  std::uint64_t bytes_ = 0;
  std::string lastEventId_;
  long retryMs_ = kRetryDefaultMs;
  End endStatus_ = End::Ok;
  int endCode_ = 0;
  std::string endMsg_;
};

} // namespace

d::Status NativeHttpSender::execute(const d::RequestModel &model, d::IResponseSink &sink,
                                    const d::ICancellationToken &cancel) {
  const d::HttpRequest &h = httpOf(model);

  auto token = std::make_shared<core::CancelToken>();
  if (cancel.cancelled()) token->cancel();
  { std::lock_guard<std::mutex> lk(mu_); token_ = token; }

  // SSE: stream the response natively (no legacy HttpSender).
  if (d::acceptsEventStream(h)) {
    SseStreamer(h, model, sink, token).run();
    { std::lock_guard<std::mutex> lk(mu_); token_.reset(); }
    return d::ok();
  }

  // --- Unary, native on domain types via cpr ---
  cpr::Session session;
  configureSession(session, h, model, token, {});

  auto start = std::chrono::steady_clock::now();
  cpr::Response r = runVerb(session, h.method());
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - start);
  { std::lock_guard<std::mutex> lk(mu_); token_.reset(); }

  if (token->isCancelled()) {
    sink.emit(d::ResponseEvent(d::EvFailed{{d::ErrorKind::Cancelled, "Cancelled", {}}}));
    return d::ok();
  }
  if (r.error) {
    sink.emit(d::ResponseEvent(d::EvFailed{{mapCprError(r.error.code), r.error.message, {}}}));
    return d::ok();
  }

  d::ApiResponse resp;
  resp.statusCode = (int)r.status_code;
  resp.body = std::move(r.text);
  resp.elapsed = elapsed;
  for (const auto &kv : r.header) {
    resp.headers.push_back({kv.first, kv.second});
    // Case-insensitive "set-cookie" check WITHOUT allocating a lowercased copy of every header name.
    static const std::string kSetCookie = "set-cookie";
    bool isSetCookie = kv.first.size() == kSetCookie.size();
    for (size_t i = 0; isSetCookie && i < kSetCookie.size(); ++i)
      isSetCookie = (char)::tolower((unsigned char)kv.first[i]) == kSetCookie[i];
    if (isSetCookie) resp.cookies.push_back(parseSetCookie(kv.second));
  }
  sink.emit(d::ResponseEvent(d::EvCompleted{std::move(resp)}));
  return d::ok();
}

d::Status NativeHttpSender::close(int, std::string) {
  std::lock_guard<std::mutex> lk(mu_);
  if (token_) token_->cancel();
  return d::ok();
}

} // namespace core::infra
