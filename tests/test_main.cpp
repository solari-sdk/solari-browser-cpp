#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "solari/browser/browser.hpp"

using namespace solari::browser;
using nlohmann::json;

// ---------------------------------------------------------------------------
// POST /sessions request shape
// ---------------------------------------------------------------------------
TEST_CASE("buildCreateBody is empty when nothing is set (caller omits the body)") {
  json body = buildCreateBody(CreateSessionOptions{});
  CHECK(body.is_object());
  CHECK(body.empty());
}

TEST_CASE("buildCreateBody omits falsy flags, keeps the true ones") {
  CreateSessionOptions o;
  o.stealth = true;
  o.webBotAuth = true;
  json body = buildCreateBody(o);
  CHECK(body["stealth"] == true);
  CHECK(body["webBotAuth"] == true);
  // recording/captcha are false -> omitted entirely, never sent as `false`.
  CHECK_FALSE(body.contains("recording"));
  CHECK_FALSE(body.contains("captcha"));
  CHECK_FALSE(body.contains("profileId"));
  CHECK_FALSE(body.contains("proxy"));
}

TEST_CASE("buildCreateBody carries profileId, recording, captcha") {
  CreateSessionOptions o;
  o.profileId = "prof_123";
  o.recording = true;
  o.stealth = true;
  o.captcha = true;
  json body = buildCreateBody(o);
  CHECK(body["profileId"] == "prof_123");
  CHECK(body["recording"] == true);
  CHECK(body["captcha"] == true);
  CHECK(body["stealth"] == true);
}

TEST_CASE("buildCreateBody treats an empty profileId as unset (JS falsy)") {
  CreateSessionOptions o;
  o.profileId = "";
  CHECK_FALSE(buildCreateBody(o).contains("profileId"));
}

TEST_CASE("buildCreateBody passes a shorthand proxy through as a bare string") {
  CreateSessionOptions o;
  o.proxy = std::string("us");
  CHECK(buildCreateBody(o)["proxy"] == "us");

  CreateSessionOptions off;
  off.proxy = std::string("off");
  CHECK(buildCreateBody(off)["proxy"] == "off");

  CreateSessionOptions smart;
  smart.proxy = std::string("smart");
  CHECK(buildCreateBody(smart)["proxy"] == "smart");
}

TEST_CASE("buildCreateBody serializes a structured proxy, omitting unset fields") {
  ProxyRequest p;
  p.country = "gb";
  p.tier = "mobile";
  p.session = "warm-1";
  p.sessionDuration = 15;
  CreateSessionOptions o;
  o.stealth = true;
  o.proxy = p;

  json body = buildCreateBody(o);
  const json& px = body["proxy"];
  CHECK(px["country"] == "gb");
  CHECK(px["tier"] == "mobile");
  CHECK(px["session"] == "warm-1");
  CHECK(px["sessionDuration"] == 15);
  CHECK_FALSE(px.contains("asn"));
  CHECK_FALSE(px.contains("state"));
  CHECK_FALSE(px.contains("city"));
}

TEST_CASE("proxyToJson handles the full ProxyRequest surface") {
  ProxyRequest p;
  p.country = "us";
  p.tier = "static";
  p.asn = "20057";
  p.session = "s1";
  p.sessionDuration = 30;
  p.state = "california";
  p.city = "los_angeles";
  json j = proxyToJson(p);
  CHECK(j["country"] == "us");
  CHECK(j["tier"] == "static");
  CHECK(j["asn"] == "20057");
  CHECK(j["session"] == "s1");
  CHECK(j["sessionDuration"] == 30);
  CHECK(j["state"] == "california");
  CHECK(j["city"] == "los_angeles");
}

// ---------------------------------------------------------------------------
// cdpEndpoint derivation
// ---------------------------------------------------------------------------
TEST_CASE("deriveCdpFromWs swaps the /ws/ path prefix for /cdp/") {
  CHECK(deriveCdpFromWs("wss://api.getsolari.com/ws/sess_abc") ==
        "wss://api.getsolari.com/cdp/sess_abc");
  CHECK(deriveCdpFromWs("ws://localhost:4000/ws/sess_abc") ==
        "ws://localhost:4000/cdp/sess_abc");
}

TEST_CASE("deriveCdpFromWs preserves the query string and composite ids") {
  CHECK(deriveCdpFromWs("wss://gw/ws/org.pool.sess-1?token=x") ==
        "wss://gw/cdp/org.pool.sess-1?token=x");
}

TEST_CASE("deriveCdpFromWs leaves non-/ws/ endpoints untouched") {
  CHECK(deriveCdpFromWs("wss://gw/cdp/sess_abc") == "wss://gw/cdp/sess_abc");
  CHECK(deriveCdpFromWs("wss://gw/devtools/browser/x") == "wss://gw/devtools/browser/x");
  CHECK(deriveCdpFromWs("not a url") == "not a url");
  CHECK(deriveCdpFromWs("") == "");
  CHECK(deriveCdpFromWs("wss://gw") == "wss://gw");  // no path at all
}

TEST_CASE("deriveCdpFromWs only rewrites the PATH, never the host") {
  // A host that merely contains "ws" must not be mangled.
  CHECK(deriveCdpFromWs("wss://ws.example.com/health") ==
        "wss://ws.example.com/health");
}

// ---------------------------------------------------------------------------
// POST /sessions response parsing
// ---------------------------------------------------------------------------
TEST_CASE("parseSessionResponse maps the 201 body onto Session") {
  json body = {{"sessionId", "sess_1"},
               {"wsEndpoint", "wss://gw/ws/sess_1"},
               {"cdpEndpoint", "wss://gw/cdp/sess_1"},
               {"expiresAt", "2026-07-16T10:00:00.000Z"}};
  Session s = parseSessionResponse(body);
  CHECK(s.id == "sess_1");
  // UPSTREAM endpoints, verbatim — no loopback proxy rewrite.
  CHECK(s.wsEndpoint == "wss://gw/ws/sess_1");
  CHECK(s.cdpEndpoint == "wss://gw/cdp/sess_1");
  CHECK(s.expiresAt == "2026-07-16T10:00:00.000Z");
  CHECK_FALSE(s.storageState.has_value());
  CHECK_FALSE(s.proxy.has_value());
}

TEST_CASE("parseSessionResponse derives cdpEndpoint when the gateway omits it") {
  json body = {{"sessionId", "sess_1"}, {"wsEndpoint", "wss://gw/ws/sess_1"}};
  Session s = parseSessionResponse(body);
  CHECK(s.cdpEndpoint == "wss://gw/cdp/sess_1");
}

TEST_CASE("parseSessionResponse defaults a missing expiresAt to an ISO instant") {
  json body = {{"sessionId", "sess_1"}, {"wsEndpoint", "wss://gw/ws/sess_1"}};
  Session s = parseSessionResponse(body);
  REQUIRE(s.expiresAt.size() == 24);        // 2026-07-16T09:30:00.000Z
  CHECK(s.expiresAt.back() == 'Z');
  CHECK(s.expiresAt[10] == 'T');
}

TEST_CASE("parseSessionResponse rejects a response missing sessionId/wsEndpoint") {
  CHECK_THROWS_AS(parseSessionResponse(json::object()), SolariError);
  CHECK_THROWS_AS(parseSessionResponse(json{{"sessionId", "s"}}), SolariError);
  CHECK_THROWS_AS(parseSessionResponse(json{{"wsEndpoint", "wss://gw/ws/s"}}),
                  SolariError);
  CHECK_THROWS_AS(parseSessionResponse(json(nullptr)), SolariError);
}

TEST_CASE("parseSessionResponse distinguishes absent from empty storageState") {
  json none = {{"sessionId", "s"}, {"wsEndpoint", "wss://gw/ws/s"}};
  CHECK_FALSE(parseSessionResponse(none).storageState.has_value());  // no profile

  json empty = {{"sessionId", "s"},
                {"wsEndpoint", "wss://gw/ws/s"},
                {"storageState", nullptr}};
  Session e = parseSessionResponse(empty);
  REQUIRE(e.storageState.has_value());  // profile exists...
  CHECK(e.storageState->is_null());     // ...but is empty

  json full = {{"sessionId", "s"},
               {"wsEndpoint", "wss://gw/ws/s"},
               {"storageState", {{"cookies", json::array({{{"name", "a"}}})}}}};
  Session f = parseSessionResponse(full);
  REQUIRE(f.storageState.has_value());
  CHECK(f.storageState->contains("cookies"));
}

TEST_CASE("parseSessionResponse parses the resolved proxy config") {
  // The gateway sends confirmation only (timezone/country/tier); any
  // credential keys an older gateway might include are ignored by design.
  json body = {{"sessionId", "s"},
               {"wsEndpoint", "wss://gw/ws/s"},
               {"proxy",
                {{"timezoneId", "America/Los_Angeles"},
                 {"country", "us"},
                 {"tier", "mobile"}}}};
  Session s = parseSessionResponse(body);
  REQUIRE(s.proxy.has_value());
  CHECK(s.proxy->timezoneId == "America/Los_Angeles");
  CHECK(s.proxy->country == "us");
  REQUIRE(s.proxy->tier.has_value());
  CHECK(s.proxy->tier.value() == "mobile");
}

// REGRESSION GUARD for the shipped fix: the resolved-proxy type carries NO
// credentials. ResolvedProxyConfig used to carry server/username/password; the
// gateway stopped sending them (they disclose the proxy vendor's account), so
// this binding parsed empty strings forever — a contract that was not merely
// dead but misleading. C++ has no reflection, so the guard is twofold: the
// static_asserts below stop compiling if a credential member is re-added, and
// the runtime check proves a gateway still emitting the old keys cannot smuggle
// them through the parser.
template <typename T, typename = void>
struct has_server : std::false_type {};
template <typename T>
struct has_server<T, std::void_t<decltype(std::declval<T>().server)>> : std::true_type {};
template <typename T, typename = void>
struct has_username : std::false_type {};
template <typename T>
struct has_username<T, std::void_t<decltype(std::declval<T>().username)>> : std::true_type {};
template <typename T, typename = void>
struct has_password : std::false_type {};
template <typename T>
struct has_password<T, std::void_t<decltype(std::declval<T>().password)>> : std::true_type {};

static_assert(!has_server<ResolvedProxyConfig>::value,
              "ResolvedProxyConfig must not carry the proxy vendor's address");
static_assert(!has_username<ResolvedProxyConfig>::value,
              "ResolvedProxyConfig must not carry the proxy vendor's account");
static_assert(!has_password<ResolvedProxyConfig>::value,
              "ResolvedProxyConfig must not carry the proxy vendor's password");

TEST_CASE("a gateway still sending proxy credentials cannot smuggle them through") {
  json body = {{"sessionId", "s"},
               {"wsEndpoint", "wss://gw/ws/s"},
               {"proxy",
                {{"timezoneId", "America/Los_Angeles"},
                 {"country", "us"},
                 {"tier", "mobile"},
                 {"server", "http://resi.vendor.example:8000"},
                 {"username", "acct-12345"},
                 {"password", "hunter2"}}}};
  Session s = parseSessionResponse(body);
  REQUIRE(s.proxy.has_value());
  CHECK(s.proxy->timezoneId == "America/Los_Angeles");
  CHECK(s.proxy->country == "us");
  // Nothing the caller can reach carries the account.
  CHECK(s.proxy->timezoneId.find("vendor") == std::string::npos);
  CHECK(s.proxy->country.find("acct-") == std::string::npos);
}

TEST_CASE("parseSessionResponse tolerates a proxy without a tier") {
  json body = {{"sessionId", "s"},
               {"wsEndpoint", "wss://gw/ws/s"},
               {"proxy",
                {{"server", "http://x:1"},
                 {"username", "u"},
                 {"password", "p"},
                 {"timezoneId", "UTC"},
                 {"country", "gb"}}}};
  Session s = parseSessionResponse(body);
  REQUIRE(s.proxy.has_value());
  CHECK_FALSE(s.proxy->tier.has_value());
}

// ---------------------------------------------------------------------------
// GET /sessions/:id/replay-url
// ---------------------------------------------------------------------------
TEST_CASE("parseReplayUrl defaults expiresInSeconds=0 and contentEncoding=gzip") {
  ReplayUrl r = parseReplayUrl(json{{"url", "https://s3/replay.ndjson.gz"}});
  CHECK(r.url == "https://s3/replay.ndjson.gz");
  CHECK(r.expiresInSeconds == 0);
  CHECK(r.contentEncoding == "gzip");
}

TEST_CASE("parseReplayUrl keeps the gateway's values when present") {
  ReplayUrl r = parseReplayUrl(json{{"url", "https://s3/r"},
                                    {"expiresInSeconds", 900},
                                    {"contentEncoding", "identity"}});
  CHECK(r.expiresInSeconds == 900);
  CHECK(r.contentEncoding == "identity");
}

TEST_CASE("parseReplayUrl rejects a body with no url") {
  CHECK_THROWS_AS(parseReplayUrl(json::object()), SolariError);
  CHECK_THROWS_AS(parseReplayUrl(json{{"url", ""}}), SolariError);
  CHECK_THROWS_AS(parseReplayUrl(json{{"expiresInSeconds", 900}}), SolariError);
}

// ---------------------------------------------------------------------------
// profiles
// ---------------------------------------------------------------------------
TEST_CASE("parseProfiles pulls {id,name} and keeps the full row in raw") {
  json body = json::array({
      json{{"id", "prof_1"}, {"name", "logged-in"}, {"sizeBytes", 4096}, {"version", 3}},
      json{{"id", "prof_2"}, {"name", "fresh"}},
  });
  auto profiles = parseProfiles(body);
  REQUIRE(profiles.size() == 2);
  CHECK(profiles[0].id == "prof_1");
  CHECK(profiles[0].name == "logged-in");
  // Extra platform columns survive on `raw` without an SDK bump.
  CHECK(profiles[0].raw["sizeBytes"] == 4096);
  CHECK(profiles[0].raw["version"] == 3);
  CHECK(profiles[1].id == "prof_2");
  CHECK(profiles[1].name == "fresh");
}

TEST_CASE("parseProfiles skips non-object rows and non-array bodies") {
  auto mixed = parseProfiles(json::array({json{{"id", "p1"}, {"name", "n"}}, 42,
                                          json(nullptr), "str"}));
  REQUIRE(mixed.size() == 1);
  CHECK(mixed[0].id == "p1");

  CHECK(parseProfiles(json::object()).empty());
  CHECK(parseProfiles(json(nullptr)).empty());
  CHECK(parseProfiles(json::array()).empty());
}

TEST_CASE("parseProfile tolerates missing id/name") {
  Profile p = parseProfile(json{{"editorStatus", "idle"}});
  CHECK(p.id.empty());
  CHECK(p.name.empty());
  CHECK(p.raw["editorStatus"] == "idle");
}

TEST_CASE("parseSaveResult defaults version/sizeBytes to 0") {
  SaveProfileResult empty = parseSaveResult(json::object());
  CHECK(empty.version == 0);
  CHECK(empty.sizeBytes == 0);

  SaveProfileResult r = parseSaveResult(json{{"version", 4}, {"sizeBytes", 8192}});
  CHECK(r.version == 4);
  CHECK(r.sizeBytes == 8192);
}

// ---------------------------------------------------------------------------
// GET /proxy/countries
// ---------------------------------------------------------------------------
TEST_CASE("parseProxyCountries reads {enabled, countries}") {
  ProxyCountries c = parseProxyCountries(
      json{{"enabled", true}, {"countries", json::array({"br", "gb", "us"})}});
  CHECK(c.enabled == true);
  CHECK(c.countries == std::vector<std::string>{"br", "gb", "us"});
}

TEST_CASE("parseProxyCountries defaults to disabled/empty and skips junk entries") {
  ProxyCountries none = parseProxyCountries(json::object());
  CHECK(none.enabled == false);
  CHECK(none.countries.empty());

  ProxyCountries mixed = parseProxyCountries(
      json{{"enabled", false}, {"countries", json::array({"us", 7, nullptr})}});
  CHECK(mixed.countries == std::vector<std::string>{"us"});
}

// ---------------------------------------------------------------------------
// errors
// ---------------------------------------------------------------------------
TEST_CASE("parseErrorCode lifts a string code out of a JSON error body") {
  auto code = parseErrorCode(
      R"({"error":"Stealth mode requires a paid plan","code":"FeatureRequiresPlan","feature":"stealth"})");
  REQUIRE(code.has_value());
  CHECK(code.value() == error_code::FeatureRequiresPlan);
}

TEST_CASE("parseErrorCode returns nullopt for anything that isn't a string code") {
  CHECK_FALSE(parseErrorCode("").has_value());
  CHECK_FALSE(parseErrorCode("<html>502 Bad Gateway</html>").has_value());
  CHECK_FALSE(parseErrorCode(R"({"error":"nope"})").has_value());
  CHECK_FALSE(parseErrorCode(R"({"code":42})").has_value());       // not a string
  CHECK_FALSE(parseErrorCode(R"(["FeatureRequiresPlan"])").has_value());  // not an object
}

TEST_CASE("throwHttpError attaches status + code and quotes the body") {
  bool threw = false;
  try {
    throwHttpError("Solari POST /sessions", 429,
                   R"({"error":"Too many sessions","code":"ConcurrencyLimitExceeded"})");
  } catch (const SolariError& e) {
    threw = true;
    REQUIRE(e.status.has_value());
    CHECK(e.status.value() == 429);
    REQUIRE(e.code.has_value());
    CHECK(e.code.value() == error_code::ConcurrencyLimitExceeded);
    const std::string msg = e.what();
    CHECK(msg.find("Solari POST /sessions failed: 429") != std::string::npos);
    CHECK(msg.find("Too many sessions") != std::string::npos);
  }
  CHECK(threw);
}

TEST_CASE("throwHttpError leaves code unset when the body has none") {
  try {
    throwHttpError("Solari GET /profiles", 500, "internal error");
  } catch (const SolariError& e) {
    CHECK(e.status.value() == 500);
    CHECK_FALSE(e.code.has_value());
  }
}

TEST_CASE("every documented error code constant is exposed") {
  CHECK(std::string(error_code::FeatureRequiresPlan) == "FeatureRequiresPlan");
  CHECK(std::string(error_code::ConcurrencyLimitExceeded) == "ConcurrencyLimitExceeded");
  CHECK(std::string(error_code::PlanLimitExceeded) == "PlanLimitExceeded");
  CHECK(std::string(error_code::BrowserUnhealthy) == "BrowserUnhealthy");
}

// ---------------------------------------------------------------------------
// HTTP transport: request shape + retry policy
// ---------------------------------------------------------------------------
TEST_CASE("HttpTransport builds the POST /sessions request shape") {
  HttpTransport ht("slr_live_id_secret", "https://api.getsolari.com/");
  CHECK(ht.baseUrl() == "https://api.getsolari.com");  // trailing slash stripped

  CreateSessionOptions o;
  o.stealth = true;
  PreparedRequest r =
      ht.prepare("POST", "/sessions", true, buildCreateBody(o).dump());
  CHECK(r.method == "POST");
  CHECK(r.url == "https://api.getsolari.com/sessions");
  CHECK(r.hasBody);

  auto has = [&](const std::string& h) {
    for (const auto& x : r.headers)
      if (x == h) return true;
    return false;
  };
  CHECK(has("Authorization: Bearer slr_live_id_secret"));
  CHECK(has("Content-Type: application/json"));
  CHECK(json::parse(r.body)["stealth"] == true);
}

TEST_CASE("HttpTransport defaults to the us-west region URL") {
  HttpTransport ht("k");
  CHECK(ht.baseUrl() == std::string(kDefaultBaseUrl));
  CHECK(ht.baseUrl() == "https://api.getsolari.com");
}

TEST_CASE("HttpTransport requires an apiKey and a baseUrl") {
  CHECK_THROWS_AS(HttpTransport("", "https://gw"), SolariError);
  CHECK_THROWS_AS(HttpTransport("k", ""), SolariError);
}

TEST_CASE("only 502/503/504 are retryable") {
  CHECK(isRetryableStatus(502));
  CHECK(isRetryableStatus(503));
  CHECK(isRetryableStatus(504));
  // Everything else is terminal — notably 429 (cap) and 5xx that isn't a gateway hop.
  CHECK_FALSE(isRetryableStatus(500));
  CHECK_FALSE(isRetryableStatus(501));
  CHECK_FALSE(isRetryableStatus(429));
  CHECK_FALSE(isRetryableStatus(404));
  CHECK_FALSE(isRetryableStatus(403));
  CHECK_FALSE(isRetryableStatus(200));
}

TEST_CASE("the wire defaults match the reference SDK") {
  CHECK(kDefaultMaxAttempts == 2);
  CHECK(kDefaultBackoffMs == 500);   // FIXED backoff, not exponential
  CHECK(kDefaultTimeoutMs == 90000);
  CHECK(kStorageStateTimeoutMs == 8000);

  ClientOptions o;
  CHECK(o.baseUrl == "https://api.getsolari.com");
  CHECK(o.maxAttempts == 2);
  CHECK(o.backoffMs == 500);
  CHECK(o.timeoutMs == 90000);
}

// ---------------------------------------------------------------------------
// path encoding + client construction
// ---------------------------------------------------------------------------
TEST_CASE("encodeURIComponent matches JS semantics for ids in paths") {
  CHECK(encodeURIComponent("sess_abc-123") == "sess_abc-123");
  CHECK(encodeURIComponent("a-_.!~*'()") == "a-_.!~*'()");  // unreserved set
  CHECK(encodeURIComponent("org/../etc") == "org%2F..%2Fetc");
  CHECK(encodeURIComponent("a b") == "a%20b");
}

TEST_CASE("Client requires an apiKey") {
  ClientOptions o;
  CHECK_THROWS_AS(Client{o}, SolariError);
  o.apiKey = "slr_live_id_secret";
  CHECK_NOTHROW(Client{o});  // construction is offline: no request is made
}

TEST_CASE("Client honours a baseUrl override") {
  ClientOptions o;
  o.apiKey = "k";
  o.baseUrl = "http://localhost:4000";
  Client c(o);
  CHECK(c.http().baseUrl() == "http://localhost:4000");
  CHECK(c.http().timeoutMs() == 90000);
}
