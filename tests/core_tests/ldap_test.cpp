// No live LDAP server needed.
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "core/app/cancellation_token.hpp"
#include "core/domain/request/request_defaults.hpp"
#include "core/infra/export/exporter.hpp"
#include "core/infra/import/importer.hpp"
#include "core/infra/persistence/request_naming.hpp"
#include "core/infra/serialization/field_json.hpp"
#include "infra/transport/ldap/native_ldap_sender.hpp"

using namespace core;
namespace d = core::domain;

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char *msg) {
  if (ok) ++g_pass;
  else { ++g_fail; std::printf("  FAIL[ldap]: %s\n", msg); }
}
bool has(const std::string &hay, const char *needle) { return hay.find(needle) != std::string::npos; }

d::LdapRequest ldapReq(const std::string &url) {
  d::LdapRequest::Parts p{d::Url::create(url).take()};
  p.bindDn = "cn=admin,dc=example,dc=com";
  p.bindPassword = "secret";
  p.baseDn = "dc=example,dc=com";
  p.filter = "(uid=bob)";
  p.attributes = {"cn", "mail"};
  return d::LdapRequest::create(std::move(p)).take();
}
} // namespace

int run_ldap_tests() {
  // --- LdapRequest::create invariants ---
  {
    d::LdapRequest::Parts p{d::Url::create("").take()};
    check(d::LdapRequest::create(std::move(p)).isOk(), "empty url draft creates");
    d::LdapRequest::Parts p2{d::Url::create("http://x").take()};
    check(!d::LdapRequest::create(std::move(p2)).isOk(), "http:// scheme rejected");
    d::LdapRequest::Parts p3{d::Url::create("{{ldap_host}}/x").take()};
    check(d::LdapRequest::create(std::move(p3)).isOk(), "placeholder url accepted");
    d::LdapRequest::Parts p4{d::Url::create("ldaps://x:636").take()};
    check(d::LdapRequest::create(std::move(p4)).isOk(), "ldaps accepted");
    d::LdapRequest::Parts p5{d::Url::create("ldap://x").take()};
    p5.sizeLimit = -1;
    check(!d::LdapRequest::create(std::move(p5)).isOk(), "negative sizeLimit rejected");
    d::LdapRequest::Parts p6{d::Url::create("ldap://x").take()};
    p6.timeLimit = -2;
    check(!d::LdapRequest::create(std::move(p6)).isOk(), "negative timeLimit rejected");
    d::LdapRequest::Parts p7{d::Url::create("ldap://x").take()};
    p7.pageSize = -1;
    check(!d::LdapRequest::create(std::move(p7)).isOk(), "negative pageSize rejected");
    // StartTLS is the ldap:// upgrade — meaningless (and a config smell) on an already-TLS ldaps URL.
    d::LdapRequest::Parts p8{d::Url::create("ldaps://x:636").take()};
    p8.startTls = true;
    check(!d::LdapRequest::create(std::move(p8)).isOk(), "startTls + ldaps rejected");
    d::LdapRequest::Parts p9{d::Url::create("ldap://x:389").take()};
    p9.startTls = true;
    check(d::LdapRequest::create(std::move(p9)).isOk(), "startTls + ldap accepted");
    d::LdapRequest::Parts p10{d::Url::create("ldap://x").take()};
    auto dflt = d::LdapRequest::create(std::move(p10));
    check(dflt.isOk() && !dflt.value().startTls() && dflt.value().pageSize() == 500,
          "defaults: startTls off, pageSize 500");
  }

  // --- composeLdapFilter (group AND via memberOf) ---
  {
    check(d::composeLdapFilter("(uid=bob)", "") == "(uid=bob)", "no group -> filter untouched");
    check(d::composeLdapFilter("uid=bob", "") == "(uid=bob)", "bare filter gets wrapped");
    check(d::composeLdapFilter("", "") == "(objectClass=*)", "empty filter -> match-all");
    check(d::composeLdapFilter("(uid=bob)", "cn=admins,dc=x") ==
              "(&(uid=bob)(memberOf=cn=admins,dc=x))",
          "group ANDed via memberOf");
    check(d::composeLdapFilter("", "cn=g,dc=x") == "(&(objectClass=*)(memberOf=cn=g,dc=x))",
          "empty filter + group");
    check(d::composeLdapFilter("(uid=b)", "cn=we(ird)*x\\y") ==
              "(&(uid=b)(memberOf=cn=we\\28ird\\29\\2ax\\5cy))",
          "group DN escaped per RFC 4515");
  }

  // --- scope token roundtrip ---
  {
    d::LdapScope s;
    check(d::toString(d::LdapScope::Base) == "base" && d::parseLdapScope("base", s) &&
              s == d::LdapScope::Base,
          "scope base roundtrip");
    check(d::toString(d::LdapScope::One) == "one" && d::parseLdapScope("one", s) &&
              s == d::LdapScope::One,
          "scope one roundtrip");
    check(d::toString(d::LdapScope::Sub) == "sub" && d::parseLdapScope("sub", s) &&
              s == d::LdapScope::Sub,
          "scope sub roundtrip");
    check(!d::parseLdapScope("tree", s), "unknown scope rejected");
  }

  // --- Params tab JSON (field_json) roundtrip ---
  {
    d::LdapRequest::Parts p{d::Url::create("ldap://h:389").take()};
    p.bindDn = "cn=svc,dc=x";
    p.bindPassword = "{{pw}}";
    p.baseDn = "ou=people,dc=x";
    p.scope = d::LdapScope::One;
    p.filter = "(uid={{user}})";
    p.attributes = {"cn", "memberOf"};
    p.group = "cn=admins,dc=x";
    p.testPassword = "{{user_pw}}";
    p.sizeLimit = 7;
    p.timeLimit = 3;
    p.startTls = true;
    p.pageSize = 250;
    auto l = d::LdapRequest::create(std::move(p)).take();
    auto r = serial::jsonToLdapParams(serial::ldapParamsToJson(l));
    check(r.isOk(), "params roundtrip parses");
    if (r.isOk()) {
      const auto &q = r.value();
      check(q.bindDn == "cn=svc,dc=x" && q.bindPassword == "{{pw}}" && q.baseDn == "ou=people,dc=x" &&
                q.scope == d::LdapScope::One && q.filter == "(uid={{user}})" &&
                q.attributes == std::vector<std::string>({"cn", "memberOf"}) &&
                q.group == "cn=admins,dc=x" && q.testPassword == "{{user_pw}}" && q.sizeLimit == 7 &&
                q.timeLimit == 3 && q.startTls && q.pageSize == 250,
            "params roundtrip preserves every field");
    }
    check(!serial::jsonToLdapParams("{\"scope\":\"tree\"}").isOk(), "bad scope rejected");
    check(!serial::jsonToLdapParams("{\"sizeLimit\":-5}").isOk(), "negative limit rejected");
    check(!serial::jsonToLdapParams("{\"pageSize\":-1}").isOk(), "negative pageSize rejected");
    check(serial::jsonToLdapParams("").isOk(), "empty text -> defaults");
  }

  // --- filename grammar ---
  {
    check(encodeRequestFilename("k7id", RequestType::Ldap, "", "Check User") ==
              "k7id_ldap_check-user.json",
          "encode: ldap, NO method");
    auto pr = parseRequestFilename("k7id_ldap_check-user.json");
    check(pr.ok && pr.type == RequestType::Ldap && pr.slug == "check-user" && pr.id == "k7id",
          "parse: ldap filename roundtrip");
  }

  // --- defaults ---
  {
    auto payload = d::defaultPayloadFor(RequestType::Ldap);
    const auto &l = std::get<d::LdapRequest>(payload);
    check(l.url().raw() == "ldap://localhost:389" && l.baseDn() == "dc=example,dc=com" &&
              l.scope() == d::LdapScope::Sub && l.sizeLimit() == 100,
          "defaultPayloadFor(Ldap) placeholder target");
  }

  // --- ldapsearch export ---
  {
    d::LdapRequest::Parts p{d::Url::create("ldap://h:389").take()};
    p.bindDn = "cn=admin,dc=x";
    p.bindPassword = "s3cret";
    p.baseDn = "dc=x";
    p.filter = "(uid=bob)";
    p.group = "cn=g,dc=x";
    p.attributes = {"cn", "mail"};
    d::RequestConfig cfg{d::Timeout::fromMillis(1000).take(), true};
    auto m = d::RequestModel::create(d::RequestId("l1"), "L", 0, cfg,
                                     d::LdapRequest::create(std::move(p)).take())
                 .take();
    std::string cmd = toCurl(m);
    check(has(cmd, "ldapsearch -x -H 'ldap://h:389'"), "export: ldapsearch -x -H url");
    check(has(cmd, "-D 'cn=admin,dc=x'") && has(cmd, "-w 's3cret'"), "export: bind dn + password");
    check(has(cmd, "-b 'dc=x' -s sub"), "export: base + scope");
    check(has(cmd, "-z 100") && has(cmd, "-l 10"), "export: size/time limits");
    check(has(cmd, "'(&(uid=bob)(memberOf=cn=g,dc=x))'"), "export: composed filter");
    check(has(cmd, "'cn' 'mail'"), "export: attribute list");
    check(has(cmd, "-E 'pr=500/noprompt'"), "export: paged-results extension");
    check(!has(cmd, "-ZZ"), "export: no -ZZ when startTls off");
  }
  {
    d::LdapRequest::Parts p{d::Url::create("ldap://h:389").take()};
    p.startTls = true;
    p.baseDn = "dc=x";
    p.pageSize = 0;
    d::RequestConfig cfg{d::Timeout::fromMillis(1000).take(), true};
    auto m = d::RequestModel::create(d::RequestId("l2"), "L", 0, cfg,
                                     d::LdapRequest::create(std::move(p)).take())
                 .take();
    std::string cmd = toCurl(m);
    check(has(cmd, "-ZZ"), "export: -ZZ when startTls on");
    check(!has(cmd, "-E "), "export: no -E when pageSize 0");
  }

  // --- import: ldapsearch command line ---
  {
    LdapImporter imp;
    check(imp.canHandle("ldapsearch -x -H ldap://h -b dc=x '(uid=bob)'"), "detect ldapsearch command");
    check(imp.canHandle("ldap://h/dc=x??sub?(uid=bob)"), "detect ldap:// url");
    check(imp.canHandle("ldaps://h/dc=x"), "detect ldaps:// url");
    check(!imp.canHandle("curl https://x"), "cURL not claimed by ldap importer");

    auto r = imp.parse("ldapsearch -x -ZZ -H ldap://dir:389 -D 'cn=admin,dc=x' -w s3cret "
                       "-b 'ou=people,dc=x' -s one -z 20 -l 4 -E 'pr=100/noprompt' "
                       "'(uid=bob)' cn mail");
    check(r.ok && r.model.has_value(), "ldapsearch parses");
    if (r.ok && r.model) {
      const auto &l = std::get<d::LdapRequest>(r.model->payload());
      check(l.url().raw() == "ldap://dir:389", "import: -H url");
      check(l.startTls(), "import: -ZZ -> startTls");
      check(l.bindDn() == "cn=admin,dc=x" && l.bindPassword() == "s3cret", "import: -D/-w");
      check(l.baseDn() == "ou=people,dc=x" && l.scope() == d::LdapScope::One, "import: -b/-s");
      check(l.sizeLimit() == 20 && l.timeLimit() == 4 && l.pageSize() == 100, "import: -z/-l/-E pr=");
      check(l.filter() == "(uid=bob)", "import: filter positional");
      check(l.attributes() == std::vector<std::string>({"cn", "mail"}), "import: attribute positionals");
      check(r.model->type() == RequestType::Ldap, "import: type is ldap");
    }
    auto r2 = imp.parse("ldapsearch -h dir.local -p 1389 -b dc=x --wat '(cn=*)'");
    check(r2.ok, "legacy -h/-p parses");
    if (r2.ok && r2.model) {
      const auto &l = std::get<d::LdapRequest>(r2.model->payload());
      check(l.url().raw() == "ldap://dir.local:1389", "import: -h/-p -> url");
    }
    check(!r2.unknown.empty(), "import: unknown flag collected");
    check(!imp.parse("ldapsearch -x '(uid=bob)'").ok, "no host -> parse error");

    auto r3 = imp.parse("ldaps://dir:636/ou=people,dc=x?cn,mail?one?(uid=bob)");
    check(r3.ok, "RFC 4516 url parses");
    if (r3.ok && r3.model) {
      const auto &l = std::get<d::LdapRequest>(r3.model->payload());
      check(l.url().raw() == "ldaps://dir:636", "url import: host");
      check(l.baseDn() == "ou=people,dc=x", "url import: baseDn");
      check(l.attributes() == std::vector<std::string>({"cn", "mail"}), "url import: attrs");
      check(l.scope() == d::LdapScope::One && l.filter() == "(uid=bob)", "url import: scope + filter");
      check(!l.startTls(), "url import: ldaps -> startTls stays off");
    }
    auto r4 = imp.parse("ldap://dir/dc=x??sub?%28uid%3Dbob%29");
    check(r4.ok, "percent-encoded url parses");
    if (r4.ok && r4.model) {
      const auto &l = std::get<d::LdapRequest>(r4.model->payload());
      check(l.url().raw() == "ldap://dir:389", "url import: default port 389");
      check(l.filter() == "(uid=bob)", "url import: percent-decoded filter");
    }
  }

  // --- cancel releases a HUNG connect (the Cancel-priority guarantee, mirror of the HTTP test) ---
  {
    d::RequestConfig cfg{d::Timeout::fromMillis(60000).take(), true}; // 60s: only Cancel can end this
    auto model = d::RequestModel::create(d::RequestId("lh"), "LH", 0, cfg,
                                         ldapReq("ldap://10.255.255.1:389"))
                     .take();
    core::app::CancellationToken token;
    struct CountingSink final : d::IResponseSink {
      int terminals = 0;
      bool cancelled = false;
      void emit(const d::ResponseEvent &ev) override {
        if (ev.is<d::EvCompleted>() || ev.is<d::EvFailed>()) ++terminals;
        if (const auto *f = ev.get<d::EvFailed>()) cancelled = f->error.kind == d::ErrorKind::Cancelled;
      }
    } sink;
    core::infra::NativeLdapSender ldap;
    const auto t0 = std::chrono::steady_clock::now();
    std::thread canceller([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      token.cancel();
    });
    ldap.execute(model, sink, token);
    canceller.join();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    check(ms < 10000, "cancel: hung ldap connect released well before the 60s timeout");
    check(sink.terminals == 1, "cancel: exactly one terminal event");
    check(sink.cancelled, "cancel: terminal is ErrorKind::Cancelled");
  }

  std::printf("  ldap: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}
