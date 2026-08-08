// Cancel model: WE own the socket — connect by hand, ldap_init_fd() hands it to libldap — so the cancel
// hook can shutdown() it, and every wait is a 100ms ldap_result tick re-checking the token. No blocking
// call survives a cancel, including a connect parked on a dead peer.
#include "infra/transport/ldap/native_ldap_sender.hpp"

#include <ldap.h>
#include <openldap.h> // ldap_init_fd + LDAP_PROTO_TCP (not in ldap.h)

#include <chrono>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "core/domain/response/response_event.hpp"
#include "infra/transport/shared/cancel_token.hpp"
#include "infra/transport/ldap/ldap_internal.hpp"

namespace core::infra {
namespace d = core::domain;
using nlohmann::json;
using Clock = std::chrono::steady_clock;
using namespace ldap_detail;

d::Status NativeLdapSender::executeTyped(const d::RequestModel &resolved, const d::LdapRequest &l,
                                         d::IResponseSink &sink,
                                         const d::ICancellationToken &cancel) {
  const auto t0 = Clock::now();
  auto fail = [&](d::ErrorKind k, std::string m, std::optional<int> code = std::nullopt) {
    sink.emit(d::ResponseEvent(d::EvFailed{{k, std::move(m), code}}));
    return d::ok();
  };
  if (l.url().raw().empty()) return fail(d::ErrorKind::Protocol, "ldap url required");
  Target tgt = parseTarget(l.url().raw());
  if (!tgt.ok) return fail(d::ErrorKind::Protocol, tgt.err);

  // Cancel bridge (cancel_token.hpp): per-call token + fd abort list; the hook fires NOW if already cancelled.
  auto token = core::linkCancel(cancel);
  auto ab = std::make_shared<FdAbort>();
  cancel.onCancel([ab] { ab->abort(); });

  long long tmo = resolved.config().timeout.millis();
  if (tmo <= 0) tmo = 30000;
  const auto deadline = t0 + std::chrono::milliseconds(tmo);
  const bool verify = resolved.config().tlsEnabledDefault;

  d::ErrorKind kind = d::ErrorKind::Internal;
  std::string msg;
  Conn conn;
  if (!openConn(tgt, deadline, verify, l.startTls(), *token, *ab,conn, kind, msg)) return fail(kind, msg);

  int rc = 0;
  std::string diag;
  if (!simpleBind(conn, l.bindDn(), l.bindPassword(), *token, deadline, rc, diag, kind, msg))
    return fail(kind, msg);
  if (rc != LDAP_SUCCESS) return fail(d::ErrorKind::Protocol, "bind: " + errText(rc, diag), rc);

  json entries = json::array();
  int matched = 0, pages = 0;
  if (!runSearch(conn, l, d::composeLdapFilter(l.filter(), l.group()), l.sizeLimit(), true,
                 l.pageSize(), *token, deadline, &entries, matched, rc, diag, &pages, kind, msg))
    return fail(kind, msg);
  if (rc != LDAP_SUCCESS && rc != LDAP_SIZELIMIT_EXCEEDED)
    return fail(d::ErrorKind::Protocol, "search: " + errText(rc, diag), rc);

  std::string verdict = matched > 0 ? "MATCH" : "NO_MATCH";
  // Group set + 0 hits -> probe the BASE filter to tell "absent" from "present but not in the group".
  if (matched == 0 && !l.group().empty()) {
    int n2 = 0, rc2 = 0;
    std::string diag2;
    if (!runSearch(conn, l, d::composeLdapFilter(l.filter(), ""), 1, false, 0, *token, deadline,
                   nullptr, n2, rc2, diag2, nullptr, kind, msg)) {
      if (kind == d::ErrorKind::Cancelled) return fail(kind, msg); // probe is best-effort otherwise
    } else if (n2 > 0) {
      verdict = "NOT_IN_GROUP";
    }
  }

  // Bind-test (search-then-bind): exactly one match -> re-bind as that DN on a FRESH connection.
  int testRc = -1;
  if (!l.testPassword().empty() && matched > 0) {
    if (matched > 1) {
      verdict = "AMBIGUOUS"; // refuse to guess which DN owns the password
    } else {
      std::string dn = entries[0].value("dn", "");
      if (dn.empty()) return fail(d::ErrorKind::Internal, "matched entry has no dn");
      Conn c2;
      if (!openConn(tgt, deadline, verify, l.startTls(), *token, *ab,c2, kind, msg)) return fail(kind, msg);
      std::string diagB;
      int rcB = 0;
      if (!simpleBind(c2, dn, l.testPassword(), *token, deadline, rcB, diagB, kind, msg))
        return fail(kind, msg);
      testRc = rcB;
      if (rcB == LDAP_SUCCESS) verdict = "CREDENTIALS_OK";
      else if (rcB == LDAP_INVALID_CREDENTIALS) { verdict = "INVALID_CREDENTIALS"; if (!diagB.empty()) diag = diagB; }
      else return fail(d::ErrorKind::Protocol, "test bind: " + errText(rcB, diagB), rcB);
    }
  }

  const int finalRc = testRc >= 0 ? testRc : rc;
  json body{{"verdict", verdict}, {"matched", matched},   {"resultCode", finalRc},
            {"diagnostic", diag}, {"pages", pages},       {"entries", std::move(entries)}};
  d::ApiResponse resp;
  resp.statusCode = finalRc;
  resp.body = body.dump(2, ' ', false, json::error_handler_t::replace); // odd server bytes never throw
  resp.elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0);
  sink.emit(d::ResponseEvent(d::EvCompleted{std::move(resp)}));
  return d::ok();
}

} // namespace core::infra
