// Pure fixtures, no network — runIntrospection's HTTP leg is covered by manual/e2e runs.
#include <cstdio>
#include <string>

#include "infra/transport/graphql/gql_introspection.hpp"

using namespace core;

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char *msg) {
  if (ok) ++g_pass;
  else { ++g_fail; std::printf("  FAIL[gql_introspection]: %s\n", msg); }
}
bool has(const std::string &hay, const char *needle) { return hay.find(needle) != std::string::npos; }

// One fixture covering every SDL construct the printer handles.
const char *kFixture = R"({"data":{"__schema":{
  "queryType":{"name":"Root"},
  "mutationType":null,
  "subscriptionType":null,
  "types":[
    {"kind":"OBJECT","name":"Root","description":"The root.","interfaces":[],
     "fields":[
       {"name":"hero","description":"A hero.","args":[
          {"name":"episode","description":null,"type":{"kind":"SCALAR","name":"Episode","ofType":null},"defaultValue":"JEDI"}],
        "type":{"kind":"NON_NULL","name":null,"ofType":{"kind":"OBJECT","name":"Character","ofType":null}},
        "isDeprecated":false,"deprecationReason":null},
       {"name":"oldField","description":null,"args":[],
        "type":{"kind":"SCALAR","name":"String","ofType":null},
        "isDeprecated":true,"deprecationReason":"use hero"}]},
    {"kind":"INTERFACE","name":"Node","description":null,"interfaces":[],
     "fields":[{"name":"id","description":null,"args":[],
        "type":{"kind":"NON_NULL","name":null,"ofType":{"kind":"SCALAR","name":"ID","ofType":null}},
        "isDeprecated":false,"deprecationReason":null}]},
    {"kind":"OBJECT","name":"Character","description":null,
     "interfaces":[{"kind":"INTERFACE","name":"Node","ofType":null}],
     "fields":[{"name":"friends","description":null,"args":[],
        "type":{"kind":"LIST","name":null,"ofType":{"kind":"OBJECT","name":"Character","ofType":null}},
        "isDeprecated":false,"deprecationReason":null}]},
    {"kind":"UNION","name":"SearchResult","description":null,
     "possibleTypes":[{"kind":"OBJECT","name":"Root","ofType":null},{"kind":"OBJECT","name":"Character","ofType":null}]},
    {"kind":"ENUM","name":"Episode","description":null,
     "enumValues":[
       {"name":"JEDI","description":null,"isDeprecated":false,"deprecationReason":null},
       {"name":"OLDHOPE","description":null,"isDeprecated":true,"deprecationReason":"renamed"}]},
    {"kind":"INPUT_OBJECT","name":"ReviewInput","description":null,
     "inputFields":[{"name":"stars","description":null,
        "type":{"kind":"SCALAR","name":"Int","ofType":null},"defaultValue":"5"}]},
    {"kind":"SCALAR","name":"DateTime","description":"ISO-8601."},
    {"kind":"SCALAR","name":"String","description":null},
    {"kind":"OBJECT","name":"__Type","description":null,"interfaces":[],"fields":[]}
  ],
  "directives":[
    {"name":"skip","description":null,"locations":["FIELD"],"args":[]},
    {"name":"auth","description":"Requires auth.","locations":["FIELD_DEFINITION","OBJECT"],
     "args":[{"name":"role","description":null,"type":{"kind":"SCALAR","name":"String","ofType":null},"defaultValue":"\"USER\""}]}
  ]}}})";

} // namespace

int run_gql_introspection_tests() {
  // The query itself: classic field set, no post-2021 additions (old-server compatibility).
  {
    const std::string &q = gql::introspectionQuery();
    check(has(q, "queryType"), "query asks for queryType");
    check(has(q, "fields(includeDeprecated: true)"), "query asks deprecated fields");
    check(has(q, "defaultValue"), "query asks defaultValue");
    check(!has(q, "specifiedByURL"), "query stays pre-2021 (no specifiedByURL)");
    check(!has(q, "isRepeatable"), "query stays pre-2021 (no isRepeatable)");
  }

  {
    auto r = gql::sdlFromIntrospectionJson(kFixture);
    check(r.isOk(), "fixture renders");
    std::string sdl = r.isOk() ? r.take() : std::string();
    check(has(sdl, "schema {\n  query: Root\n}"), "non-default root -> schema block");
    check(has(sdl, "\"\"\"The root.\"\"\""), "type description -> docstring");
    check(has(sdl, "type Root {"), "object type");
    check(has(sdl, "hero(episode: Episode = JEDI): Character!"), "field with arg default + NON_NULL");
    check(has(sdl, "oldField: String @deprecated(reason: \"use hero\")"), "deprecated field");
    check(has(sdl, "interface Node {"), "interface type");
    check(has(sdl, "type Character implements Node {"), "implements clause");
    check(has(sdl, "friends: [Character]"), "LIST type ref");
    check(has(sdl, "union SearchResult = Root | Character"), "union");
    check(has(sdl, "OLDHOPE @deprecated(reason: \"renamed\")"), "deprecated enum value");
    check(has(sdl, "input ReviewInput {"), "input type");
    check(has(sdl, "stars: Int = 5"), "input field default");
    check(has(sdl, "scalar DateTime"), "custom scalar kept");
    check(!has(sdl, "scalar String"), "built-in scalar skipped");
    check(!has(sdl, "__Type"), "__ meta types skipped");
    check(has(sdl, "directive @auth(role: String = \"USER\") on FIELD_DEFINITION | OBJECT"),
          "custom directive");
    check(!has(sdl, "directive @skip"), "built-in directive skipped");
  }

  {
    check(gql::sdlFromIntrospectionJson(R"({"__schema":{"types":[]}})").isOk(),
          "bare __schema (no data wrapper) accepted");
    auto err = gql::sdlFromIntrospectionJson(
        R"({"errors":[{"message":"introspection is disabled"}]})");
    check(!err.isOk() && err.error().message == "introspection is disabled",
          "errors[] body fails with the server message");
    check(!gql::sdlFromIntrospectionJson(R"({"data":{}})").isOk(), "missing __schema fails");
    check(!gql::sdlFromIntrospectionJson("not json").isOk(), "non-JSON body fails");
  }

  std::printf("  gql_introspection: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}
