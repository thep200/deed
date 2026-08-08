#pragma once

#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

#include <ldap.h>

#include <nlohmann/json.hpp>

#include "core/domain/response/response_event.hpp"
#include "infra/transport/shared/cancel_token.hpp"
#include "infra/transport/typed_sender.hpp"

namespace core::infra::ldap_detail {
namespace d = core::domain;
using nlohmann::json;
using Clock = std::chrono::steady_clock;

// Live sockets of ONE execute() call; cancel shutdown()s them. shutdown, never close: libldap still owns
// the fd (ldap_unbind closes it), so no fd-reuse race — same rule socket_abort.hpp documents for curl.
struct FdAbort {
  std::mutex mu;
  std::vector<int> fds;
  bool aborted = false;
  void add(int fd) {
    std::lock_guard<std::mutex> lk(mu);
    if (aborted) ::shutdown(fd, SHUT_RDWR); // cancel already landed -> kill the op it feeds
    fds.push_back(fd);
  }
  void del(int fd) {
    std::lock_guard<std::mutex> lk(mu);
    fds.erase(std::remove(fds.begin(), fds.end(), fd), fds.end());
  }
  void abort() {
    std::lock_guard<std::mutex> lk(mu);
    aborted = true;
    for (int fd : fds) ::shutdown(fd, SHUT_RDWR);
  }
};

// libldap connection over our fd. dtor: unregister from the abort list FIRST, then unbind (closes fd).
struct Conn {
  LDAP *ld = nullptr;
  int fd = -1;
  FdAbort *ab = nullptr;
  ~Conn() {
    if (ab && fd >= 0) ab->del(fd);
    if (ld) ldap_unbind_ext_s(ld, nullptr, nullptr); // owns + closes fd
    else if (fd >= 0) ::close(fd);
  }
};

struct Target {
  std::string host, port = "389", uri, err;
  bool tls = false, ok = false;
};

Target parseTarget(const std::string &raw);

bool openConn(const Target &t, Clock::time_point deadline, bool verifyTls, bool startTls,
              CancelToken &tok, FdAbort &ab, Conn &out, d::ErrorKind &kind, std::string &msg);

enum class Wait { Msg, Cancelled, Deadline, Error };

Wait waitMsg(LDAP *ld, int msgid, int all, CancelToken &tok, Clock::time_point deadline,
             LDAPMessage **out, std::string &err);

std::string errText(int rc, const std::string &diag);

bool pumpFail(Wait w, const std::string &err, d::ErrorKind &kind, std::string &msg);

bool simpleBind(Conn &c, const std::string &dn, const std::string &pw, CancelToken &tok,
                Clock::time_point deadline, int &rc, std::string &diag, d::ErrorKind &kind,
                std::string &msg);

bool runSearch(Conn &c, const d::LdapRequest &l, const std::string &filter, int sizeLimit,
               bool wantEntries, int pageSize, CancelToken &tok, Clock::time_point deadline,
               json *outEntries, int &count, int &rc, std::string &diag, int *pagesOut,
               d::ErrorKind &kind, std::string &msg);

} // namespace core::infra::ldap_detail
