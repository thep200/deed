#include "infra/transport/ldap/ldap_internal.hpp"

#include <ldap.h>
#include <openldap.h> // ldap_init_fd + LDAP_PROTO_TCP (not in ldap.h)

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace core::infra::ldap_detail {

namespace {

std::string b64(const char *p, std::size_t n) {
  static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string o;
  o.reserve((n + 2) / 3 * 4);
  for (std::size_t i = 0; i < n; i += 3) {
    unsigned v = (unsigned)(unsigned char)p[i] << 16;
    if (i + 1 < n) v |= (unsigned)(unsigned char)p[i + 1] << 8;
    if (i + 2 < n) v |= (unsigned char)p[i + 2];
    o += tbl[(v >> 18) & 63];
    o += tbl[(v >> 12) & 63];
    o += i + 1 < n ? tbl[(v >> 6) & 63] : '=';
    o += i + 2 < n ? tbl[v & 63] : '=';
  }
  return o;
}

bool textual(const berval *v) {
  for (std::size_t i = 0; i < v->bv_len; ++i) {
    unsigned char c = (unsigned char)v->bv_val[i];
    if (c == 0x7f || (c < 0x20 && c != '\t' && c != '\n' && c != '\r')) return false;
  }
  return true;
}

// One entry -> {dn, attributes:{name:[values]}}; binary values base64 under "name;base64".
json entryToJson(LDAP *ld, LDAPMessage *e) {
  json attrs = json::object();
  BerElement *ber = nullptr;
  for (char *a = ldap_first_attribute(ld, e, &ber); a; a = ldap_next_attribute(ld, e, ber)) {
    berval **vals = ldap_get_values_len(ld, e, a);
    json arr = json::array();
    bool bin = false;
    for (berval **v = vals; v && *v; ++v)
      if (!textual(*v)) { bin = true; break; }
    for (berval **v = vals; v && *v; ++v)
      arr.push_back(bin ? b64((*v)->bv_val, (*v)->bv_len) : std::string((*v)->bv_val, (*v)->bv_len));
    attrs[bin ? std::string(a) + ";base64" : std::string(a)] = std::move(arr);
    if (vals) ber_bvecfree(vals);
    ldap_memfree(a);
  }
  if (ber) ber_free(ber, 0);
  char *dn = ldap_get_dn(ld, e);
  json out{{"dn", dn ? dn : ""}, {"attributes", std::move(attrs)}};
  if (dn) ldap_memfree(dn);
  return out;
}

int scopeOf(d::LdapScope s) {
  switch (s) {
  case d::LdapScope::Base: return LDAP_SCOPE_BASE;
  case d::LdapScope::One: return LDAP_SCOPE_ONELEVEL;
  default: return LDAP_SCOPE_SUBTREE;
  }
}

// Owns the RFC 2696 cookie across pages (ldap_parse_pageresponse_control allocates bv_val each time).
struct PageCookie {
  berval bv{0, nullptr};
  void reset() {
    if (bv.bv_val) ber_memfree(bv.bv_val);
    bv.bv_val = nullptr;
    bv.bv_len = 0;
  }
  bool more() const { return bv.bv_len > 0; }
  ~PageCookie() { reset(); }
};

// Pull the paged-results cookie out of a search result's response controls. false -> last page.
bool takePageCookie(LDAP *ld, LDAPControl **ctrls, PageCookie &cookie) {
  cookie.reset();
  if (!ctrls) return false;
  LDAPControl *pr = ldap_control_find(LDAP_CONTROL_PAGEDRESULTS, ctrls, nullptr);
  if (!pr) return false; // server ignored the (non-critical) control -> it answered in one shot
  ber_int_t total = 0;
  if (ldap_parse_pageresponse_control(ld, pr, &total, &cookie.bv) != LDAP_SUCCESS) return false;
  return cookie.more();
}

// ONE page: issue the search (with the paged control when pageSize>0) and pump entries until the
// result message. Returns the same contract as runSearch; `ctrlsOut` carries the response controls.
bool runOnePage(Conn &c, const d::LdapRequest &l, const std::string &filter, int sizeLimit,
                bool wantEntries, int pageSize, PageCookie &cookie, CancelToken &tok,
                Clock::time_point deadline, json *outEntries, int &count, int &rc, std::string &diag,
                LDAPControl ***ctrlsOut, d::ErrorKind &kind, std::string &msg) {
  static char noAttr[] = LDAP_NO_ATTRS; // "1.1"
  std::vector<char *> attrPtrs;
  if (!wantEntries) attrPtrs.push_back(noAttr);
  else
    for (const auto &a : l.attributes())
      if (!a.empty()) attrPtrs.push_back(const_cast<char *>(a.c_str())); // libldap reads only
  attrPtrs.push_back(nullptr);

  LDAPControl *page = nullptr;
  LDAPControl *sctrls[2] = {nullptr, nullptr};
  // iscritical=0: an old/limited server ignores the control and answers unpaged instead of erroring.
  if (pageSize > 0 &&
      ldap_create_page_control(c.ld, pageSize, cookie.more() ? &cookie.bv : nullptr, 0, &page) ==
          LDAP_SUCCESS)
    sctrls[0] = page;

  timeval tl{l.timeLimit(), 0};
  int msgid = -1;
  int r = ldap_search_ext(c.ld, l.baseDn().c_str(), scopeOf(l.scope()), filter.c_str(),
                          attrPtrs.size() > 1 ? attrPtrs.data() : nullptr, 0,
                          sctrls[0] ? sctrls : nullptr, nullptr, l.timeLimit() > 0 ? &tl : nullptr,
                          sizeLimit, &msgid);
  if (page) ldap_control_free(page);
  if (r != LDAP_SUCCESS) {
    if (tok.cancelled()) { kind = d::ErrorKind::Cancelled; msg = "Cancelled"; return false; }
    kind = d::ErrorKind::Protocol; msg = errText(r, "");
    return false;
  }
  for (;;) {
    LDAPMessage *res = nullptr;
    std::string err;
    if (Wait w = waitMsg(c.ld, msgid, LDAP_MSG_ONE, tok, deadline, &res, err); w != Wait::Msg)
      return pumpFail(w, err, kind, msg);
    int type = ldap_msgtype(res);
    if (type == LDAP_RES_SEARCH_ENTRY) {
      ++count;
      if (wantEntries && outEntries) outEntries->push_back(entryToJson(c.ld, res));
      ldap_msgfree(res);
    } else if (type == LDAP_RES_SEARCH_RESULT) {
      char *dm = nullptr;
      ldap_parse_result(c.ld, res, &rc, nullptr, &dm, nullptr, ctrlsOut, 1); // frees res
      if (dm) { diag = dm; ldap_memfree(dm); }
      return true;
    } else {
      ldap_msgfree(res); // referral etc — ignored (referrals off)
    }
  }
}

} // namespace

// Async search across as many RFC 2696 pages as the server offers. `wantEntries` off = existence
// probe ("1.1" = no attributes). true -> protocol result arrived (rc filled); false -> transport fail.
bool runSearch(Conn &c, const d::LdapRequest &l, const std::string &filter, int sizeLimit,
               bool wantEntries, int pageSize, CancelToken &tok, Clock::time_point deadline,
               json *outEntries, int &count, int &rc, std::string &diag, int *pagesOut,
               d::ErrorKind &kind, std::string &msg) {
  // Never ask for a page bigger than what we're allowed to keep.
  if (pageSize > 0 && sizeLimit > 0 && sizeLimit < pageSize) pageSize = sizeLimit;
  PageCookie cookie;
  int pages = 0;
  for (;;) {
    LDAPControl **ctrls = nullptr;
    bool ok = runOnePage(c, l, filter, sizeLimit, wantEntries, pageSize, cookie, tok, deadline,
                         outEntries, count, rc, diag, &ctrls, kind, msg);
    if (!ok) { if (ctrls) ldap_controls_free(ctrls); return false; }
    ++pages;
    if (pagesOut) *pagesOut = pages;
    bool more = pageSize > 0 && takePageCookie(c.ld, ctrls, cookie);
    if (ctrls) ldap_controls_free(ctrls);
    // Stop on a real error, when the server has no more pages, or once we've collected our cap.
    if (rc != LDAP_SUCCESS) return true;
    if (!more) return true;
    if (sizeLimit > 0 && count >= sizeLimit) return true;
  }
}

} // namespace core::infra::ldap_detail
