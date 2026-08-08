#include "infra/transport/ldap/ldap_internal.hpp"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <ldap.h>
#include <openldap.h> // ldap_init_fd + LDAP_PROTO_TCP (not in ldap.h)

#include <cctype>
#include <chrono>
#include <cstring>
#include <string>

namespace core::infra::ldap_detail {

Target parseTarget(const std::string &raw) {
  Target t;
  std::string rest = raw;
  if (auto pos = raw.find("://"); pos != std::string::npos) {
    std::string sch = raw.substr(0, pos);
    for (auto &c : sch) c = (char)std::tolower((unsigned char)c);
    if (sch == "ldaps") { t.tls = true; t.port = "636"; }
    else if (sch != "ldap") { t.err = "scheme '" + sch + "' not supported (ldap/ldaps)"; return t; }
    rest = raw.substr(pos + 3);
  }
  if (auto slash = rest.find('/'); slash != std::string::npos) rest = rest.substr(0, slash);
  bool v6 = !rest.empty() && rest[0] == '[';
  if (v6) { // [::1]:port
    auto rb = rest.find(']');
    if (rb == std::string::npos) { t.err = "bad ipv6 literal (missing ])"; return t; }
    t.host = rest.substr(1, rb - 1);
    if (rb + 2 < rest.size() && rest[rb + 1] == ':') t.port = rest.substr(rb + 2);
  } else if (auto c = rest.rfind(':'); c == std::string::npos) {
    t.host = rest;
  } else if (rest.find(':') == c) { // exactly one ':' -> host:port
    t.host = rest.substr(0, c);
    t.port = rest.substr(c + 1);
  } else { t.err = "bare ipv6 host needs [brackets]"; return t; }
  if (t.host.empty()) { t.err = "ldap url missing host"; return t; }
  if (t.port.empty()) t.port = t.tls ? "636" : "389";
  t.uri = std::string(t.tls ? "ldaps" : "ldap") + "://" + (v6 ? "[" + t.host + "]" : t.host) + ":" + t.port;
  t.ok = true;
  return t;
}

namespace {

// Non-blocking connect with 100ms cancel/deadline ticks. Returns fd (registered in `ab`) or -1.
int connectHost(const Target &t, CancelToken &tok, Clock::time_point deadline, FdAbort &ab,
                d::ErrorKind &kind, std::string &msg) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *res = nullptr;
  // getaddrinfo blocks uncancellably — DNS is resolver-bounded, same exposure the OS gives curl.
  if (int gai = getaddrinfo(t.host.c_str(), t.port.c_str(), &hints, &res); gai != 0) {
    kind = d::ErrorKind::Network;
    msg = "resolve " + t.host + ": " + gai_strerror(gai);
    return -1;
  }
  int lastErr = 0;
  for (addrinfo *ai = res; ai; ai = ai->ai_next) {
    int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) { lastErr = errno; continue; }
#ifdef SO_NOSIGPIPE
    int one = 1; // macOS: a write() after the cancel shutdown() must return EPIPE, NOT kill the process
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    ab.add(fd);
    bool up = ::connect(fd, ai->ai_addr, (socklen_t)ai->ai_addrlen) == 0;
    bool dead = !up && errno != EINPROGRESS;
    while (!up && !dead) {
      if (tok.cancelled()) {
        ab.del(fd); ::close(fd); freeaddrinfo(res);
        kind = d::ErrorKind::Cancelled; msg = "Cancelled";
        return -1;
      }
      if (Clock::now() >= deadline) {
        ab.del(fd); ::close(fd); freeaddrinfo(res);
        kind = d::ErrorKind::Timeout; msg = "connect " + t.host + ":" + t.port + " timed out";
        return -1;
      }
      pollfd p{fd, POLLOUT, 0};
      int pr = ::poll(&p, 1, 100);
      if (pr < 0 && errno != EINTR) { lastErr = errno; dead = true; break; }
      if (pr > 0) {
        int soerr = 0;
        socklen_t sl = sizeof(soerr);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
        if (soerr == 0) up = true;
        else { lastErr = soerr; dead = true; }
      }
    }
    if (!up) { ab.del(fd); ::close(fd); continue; }
    if (tok.cancelled()) { // cancel raced the last poll tick (shutdown can read as "connected")
      ab.del(fd); ::close(fd); freeaddrinfo(res);
      kind = d::ErrorKind::Cancelled; msg = "Cancelled";
      return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK); // libldap wants a blocking fd
    freeaddrinfo(res);
    return fd;
  }
  freeaddrinfo(res);
  kind = d::ErrorKind::Network;
  msg = "connect " + t.host + ":" + t.port + ": " +
        (lastErr ? std::strerror(lastErr) : "no usable address");
  return -1;
}

// Per-handle TLS options; must run BEFORE the handshake (install_tls / start_tls).
void applyTlsOptions(LDAP *ld, bool verifyTls) {
  int req = verifyTls ? LDAP_OPT_X_TLS_DEMAND : LDAP_OPT_X_TLS_NEVER;
  ldap_set_option(ld, LDAP_OPT_X_TLS_REQUIRE_CERT, &req);
  int zero = 0;
  ldap_set_option(ld, LDAP_OPT_X_TLS_NEWCTX, &zero); // rebuild the ctx so the opts above take effect
}

// Shared failure path for both handshakes (implicit ldaps + StartTLS).
bool tlsFail(LDAP *ld, int rc, const char *what, CancelToken &tok, d::ErrorKind &kind,
             std::string &msg) {
  if (tok.cancelled()) { kind = d::ErrorKind::Cancelled; msg = "Cancelled"; return false; }
  char *dm = nullptr;
  ldap_get_option(ld, LDAP_OPT_DIAGNOSTIC_MESSAGE, &dm);
  kind = d::ErrorKind::Tls;
  msg = std::string(what) + ": " + ldap_err2string(rc) + (dm && *dm ? std::string(": ") + dm : "");
  if (dm) ldap_memfree(dm);
  return false;
}

} // namespace

// connect + ldap_init_fd + options + TLS (implicit on ldaps, StartTLS on ldap). false -> kind/msg set.
bool openConn(const Target &t, Clock::time_point deadline, bool verifyTls, bool startTls,
              CancelToken &tok, FdAbort &ab, Conn &out, d::ErrorKind &kind, std::string &msg) {
  out.fd = connectHost(t, tok, deadline, ab, kind, msg);
  if (out.fd < 0) return false;
  out.ab = &ab;
  if (ldap_init_fd(out.fd, LDAP_PROTO_TCP, t.uri.c_str(), &out.ld) != LDAP_SUCCESS || !out.ld) {
    kind = d::ErrorKind::Internal; msg = "ldap_init_fd failed";
    return false;
  }
  int v3 = LDAP_VERSION3;
  ldap_set_option(out.ld, LDAP_OPT_PROTOCOL_VERSION, &v3);
  ldap_set_option(out.ld, LDAP_OPT_REFERRALS, LDAP_OPT_OFF); // v1: never chase referrals
  // Both handshakes block, but a cancel shutdown() on our own fd unblocks them.
  if (t.tls) {
    applyTlsOptions(out.ld, verifyTls);
    int rc = ldap_install_tls(out.ld);
    if (rc != LDAP_SUCCESS) return tlsFail(out.ld, rc, "tls", tok, kind, msg);
  } else if (startTls) {
    // RFC 4513 upgrade on the plain port. Always REQUIRED (never best-effort): silently continuing in
    // cleartext after a failed upgrade would send the bind password in the clear.
    applyTlsOptions(out.ld, verifyTls);
    int rc = ldap_start_tls_s(out.ld, nullptr, nullptr);
    if (rc != LDAP_SUCCESS) return tlsFail(out.ld, rc, "starttls", tok, kind, msg);
  }
  return true;
}

// One protocol message with 100ms cancel/deadline ticks (the CQ-pump idiom grpc_sender uses).
Wait waitMsg(LDAP *ld, int msgid, int all, CancelToken &tok, Clock::time_point deadline,
             LDAPMessage **out, std::string &err) {
  for (;;) {
    if (tok.cancelled()) return Wait::Cancelled;
    if (Clock::now() >= deadline) return Wait::Deadline;
    timeval tv{0, 100 * 1000};
    int r = ldap_result(ld, msgid, all, &tv, out);
    if (r > 0) return Wait::Msg;
    if (r == 0) continue; // tick
    if (tok.cancelled()) return Wait::Cancelled; // our shutdown() surfaces as -1
    int ec = 0;
    ldap_get_option(ld, LDAP_OPT_RESULT_CODE, &ec);
    err = ldap_err2string(ec);
    return Wait::Error;
  }
}

std::string errText(int rc, const std::string &diag) {
  std::string s = std::string(ldap_err2string(rc)) + " (rc=" + std::to_string(rc) + ")";
  if (!diag.empty()) s += ": " + diag;
  return s;
}

// Map a pump failure to a terminal error. Returns false always (caller: `return failPump(...)`-style).
bool pumpFail(Wait w, const std::string &err, d::ErrorKind &kind, std::string &msg) {
  switch (w) {
  case Wait::Cancelled: kind = d::ErrorKind::Cancelled; msg = "Cancelled"; break;
  case Wait::Deadline: kind = d::ErrorKind::Timeout; msg = "ldap operation timed out"; break;
  default: kind = d::ErrorKind::Network; msg = err.empty() ? "connection lost" : err; break;
  }
  return false;
}

// Async simple bind + pump. true -> protocol answer arrived (rc may still be 49); false -> kind/msg set.
bool simpleBind(Conn &c, const std::string &dn, const std::string &pw, CancelToken &tok,
                Clock::time_point deadline, int &rc, std::string &diag, d::ErrorKind &kind,
                std::string &msg) {
  berval cred;
  cred.bv_val = const_cast<char *>(pw.data());
  cred.bv_len = pw.size();
  int msgid = -1;
  int r = ldap_sasl_bind(c.ld, dn.empty() ? nullptr : dn.c_str(), LDAP_SASL_SIMPLE, &cred, nullptr,
                         nullptr, &msgid);
  if (r != LDAP_SUCCESS) {
    if (tok.cancelled()) { kind = d::ErrorKind::Cancelled; msg = "Cancelled"; return false; }
    kind = d::ErrorKind::Network; msg = errText(r, "");
    return false;
  }
  LDAPMessage *res = nullptr;
  std::string err;
  if (Wait w = waitMsg(c.ld, msgid, LDAP_MSG_ALL, tok, deadline, &res, err); w != Wait::Msg)
    return pumpFail(w, err, kind, msg);
  char *dm = nullptr;
  ldap_parse_result(c.ld, res, &rc, nullptr, &dm, nullptr, nullptr, 1); // frees res
  if (dm) { diag = dm; ldap_memfree(dm); }
  return true;
}

} // namespace core::infra::ldap_detail
