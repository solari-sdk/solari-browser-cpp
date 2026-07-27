#include "solari/browser/client.hpp"

#include <utility>

#include "solari/browser/errors.hpp"

namespace solari::browser {
namespace {

/** A string field, or nullopt when absent / not a string. */
std::optional<std::string> jsonStr(const nlohmann::json& j, const char* key) {
  if (!j.is_object()) return std::nullopt;
  auto it = j.find(key);
  if (it == j.end() || !it->is_string()) return std::nullopt;
  return it->get<std::string>();
}

/** A numeric field, or `fallback` when absent / not a number. */
long long jsonNum(const nlohmann::json& j, const char* key, long long fallback) {
  if (!j.is_object()) return fallback;
  auto it = j.find(key);
  if (it == j.end() || !it->is_number()) return fallback;
  return it->get<long long>();
}

nlohmann::json parseJsonBody(const std::string& body, const char* what) {
  try {
    return nlohmann::json::parse(body);
  } catch (...) {
    throw SolariError(std::string("Solari: ") + what +
                      " response was not valid JSON: " + body.substr(0, 256));
  }
}

/**
 * The S4 forward path: the create response carries a presigned URL rather than
 * the blob. A null/absent `url` means "profile exists but is empty" -> JSON null.
 */
StorageState fetchPresignedStorageState(const std::optional<std::string>& url) {
  if (!url || url->empty()) return StorageState(nullptr);
  std::string body;
  try {
    body = fetchUrl(*url, kStorageStateTimeoutMs);
  } catch (const SolariError& e) {
    throw SolariError(std::string("Solari: failed to fetch storageState: ") + e.what(),
                      e.status);
  }
  try {
    return nlohmann::json::parse(body);
  } catch (...) {
    throw SolariError("Solari: storageState response was not valid JSON: " +
                      body.substr(0, 256));
  }
}

}  // namespace

// --- request/response shapes (pure, unit-tested) ----------------------------

nlohmann::json buildCreateBody(const CreateSessionOptions& opts) {
  nlohmann::json body = nlohmann::json::object();
  if (opts.profileId && !opts.profileId->empty()) body["profileId"] = *opts.profileId;
  if (opts.recording) body["recording"] = true;
  if (opts.stealth) body["stealth"] = true;
  if (opts.captcha) body["captcha"] = true;
  if (opts.webBotAuth) body["webBotAuth"] = true;
  if (opts.proxy) body["proxy"] = proxyToJson(*opts.proxy);
  return body;
}

Session parseSessionResponse(const nlohmann::json& data) {
  const auto sessionId = jsonStr(data, "sessionId");
  const auto wsEndpoint = jsonStr(data, "wsEndpoint");
  if (!sessionId || sessionId->empty() || !wsEndpoint || wsEndpoint->empty()) {
    throw SolariError("Solari: unexpected session response: " + data.dump());
  }

  Session s;
  s.id = *sessionId;
  // UPSTREAM endpoints, verbatim — this binding runs no loopback proxy.
  s.wsEndpoint = *wsEndpoint;
  const auto cdp = jsonStr(data, "cdpEndpoint");
  s.cdpEndpoint = (cdp && !cdp->empty()) ? *cdp : deriveCdpFromWs(s.wsEndpoint);
  const auto expiresAt = jsonStr(data, "expiresAt");
  s.expiresAt = (expiresAt && !expiresAt->empty()) ? *expiresAt
                                                   : iso8601FromNow(60LL * 60 * 1000);

  // Inline blob (legacy path). The presigned path is handled in create(), which
  // is the only place that knows whether a profile was requested.
  auto ss = data.find("storageState");
  if (ss != data.end()) s.storageState = *ss;

  auto px = data.find("proxy");
  if (px != data.end() && px->is_object()) {
    ResolvedProxyConfig p;
    if (auto v = jsonStr(*px, "server")) p.server = *v;
    if (auto v = jsonStr(*px, "username")) p.username = *v;
    if (auto v = jsonStr(*px, "password")) p.password = *v;
    if (auto v = jsonStr(*px, "timezoneId")) p.timezoneId = *v;
    if (auto v = jsonStr(*px, "country")) p.country = *v;
    p.tier = jsonStr(*px, "tier");
    s.proxy = std::move(p);
  }
  return s;
}

ReplayUrl parseReplayUrl(const nlohmann::json& data) {
  const auto url = jsonStr(data, "url");
  if (!url || url->empty()) {
    throw SolariError("Solari: unexpected replay-url response: " + data.dump());
  }
  ReplayUrl r;  // defaults: expiresInSeconds = 0, contentEncoding = "gzip"
  r.url = *url;
  r.expiresInSeconds = jsonNum(data, "expiresInSeconds", 0);
  if (auto enc = jsonStr(data, "contentEncoding")) r.contentEncoding = *enc;
  return r;
}

Profile parseProfile(const nlohmann::json& row) {
  Profile p;
  if (auto id = jsonStr(row, "id")) p.id = *id;
  if (auto name = jsonStr(row, "name")) p.name = *name;
  p.raw = row;
  return p;
}

std::vector<Profile> parseProfiles(const nlohmann::json& data) {
  std::vector<Profile> out;
  if (!data.is_array()) return out;
  for (const auto& row : data) {
    if (!row.is_object()) continue;
    out.push_back(parseProfile(row));
  }
  return out;
}

SaveProfileResult parseSaveResult(const nlohmann::json& data) {
  SaveProfileResult r;
  r.version = jsonNum(data, "version", 0);
  r.sizeBytes = jsonNum(data, "sizeBytes", 0);
  return r;
}

ProxyCountries parseProxyCountries(const nlohmann::json& data) {
  ProxyCountries out;
  if (!data.is_object()) return out;
  auto en = data.find("enabled");
  if (en != data.end() && en->is_boolean()) out.enabled = en->get<bool>();
  auto cs = data.find("countries");
  if (cs != data.end() && cs->is_array()) {
    for (const auto& c : *cs) {
      if (c.is_string()) out.countries.push_back(c.get<std::string>());
    }
  }
  return out;
}

// --- sessions ---------------------------------------------------------------

Session SessionsResource::create(const CreateSessionOptions& opts) {
  nlohmann::json body = buildCreateBody(opts);
  std::optional<nlohmann::json> payload;
  if (!body.empty()) payload = body;  // no options -> no body at all

  HttpResponse res = client_->http().request("POST", "/sessions", payload);
  if (!res.ok()) throwHttpError("Solari POST /sessions", res.status, res.body);

  const nlohmann::json data = parseJsonBody(res.body, "session");
  Session session = parseSessionResponse(data);

  // storageState is populated only when a profile was actually requested.
  if (opts.profileId && !opts.profileId->empty()) {
    auto it = data.find("storageStateUrl");
    if (it != data.end()) {
      session.storageState = fetchPresignedStorageState(jsonStr(*it, "url"));
    } else if (!session.storageState) {
      session.storageState = StorageState(nullptr);  // profile exists but empty
    }
  }
  return session;
}

Json SessionsResource::get(const std::string& id) {
  const std::string path = "/sessions/" + encodeURIComponent(id);
  HttpResponse res = client_->http().request("GET", path);
  if (!res.ok()) throwHttpError("Solari GET " + path, res.status, res.body);
  return parseJsonBody(res.body, "session");
}

void SessionsResource::release(const std::string& id) {
  const std::string path = "/sessions/" + encodeURIComponent(id);
  HttpResponse res = client_->http().request("DELETE", path);
  // 404 == already released. Not an error.
  if (!res.ok() && res.status != 404) {
    throwHttpError("Solari DELETE " + path, res.status, res.body);
  }
}

ReplayUrl SessionsResource::getReplayUrl(const std::string& id) {
  const std::string path = "/sessions/" + encodeURIComponent(id) + "/replay-url";
  HttpResponse res = client_->http().request("GET", path);
  if (!res.ok()) throwHttpError("Solari GET " + path, res.status, res.body);
  return parseReplayUrl(parseJsonBody(res.body, "replay-url"));
}

std::string SessionsResource::downloadReplay(const std::string& id) {
  const ReplayUrl replay = getReplayUrl(id);
  return fetchUrl(replay.url, client_->http().timeoutMs());
}

// --- profiles ---------------------------------------------------------------

std::vector<Profile> ProfilesResource::list() {
  HttpResponse res = client_->http().request("GET", "/profiles");
  if (!res.ok()) throwHttpError("Solari GET /profiles", res.status, res.body);
  return parseProfiles(parseJsonBody(res.body, "profiles"));
}

Profile ProfilesResource::create(const std::string& name) {
  nlohmann::json body;
  body["name"] = name;
  HttpResponse res = client_->http().request("POST", "/profiles", body);
  if (!res.ok()) throwHttpError("Solari POST /profiles", res.status, res.body);
  return parseProfile(parseJsonBody(res.body, "profile"));
}

void ProfilesResource::remove(const std::string& id) {
  const std::string path = "/profiles/" + encodeURIComponent(id);
  HttpResponse res = client_->http().request("DELETE", path);
  if (!res.ok() && res.status != 404) {
    throwHttpError("Solari DELETE " + path, res.status, res.body);
  }
}

SaveProfileResult ProfilesResource::save(const std::string& id,
                                         const StorageState& storageState) {
  const std::string path = "/profiles/" + encodeURIComponent(id) + "/save";
  nlohmann::json body;
  body["storageState"] = storageState;
  HttpResponse res = client_->http().request("POST", path, body);
  if (!res.ok()) throwHttpError("Solari POST " + path, res.status, res.body);
  return parseSaveResult(parseJsonBody(res.body, "profile save"));
}

// --- client -----------------------------------------------------------------

Client::Client(const ClientOptions& options) : sessions(this), profiles(this) {
  if (options.apiKey.empty()) throw SolariError("Solari: apiKey is required");
  http_ = std::make_shared<HttpTransport>(
      options.apiKey, options.baseUrl.empty() ? kDefaultBaseUrl : options.baseUrl,
      options.maxAttempts, options.backoffMs, options.timeoutMs);
}

ProxyCountries Client::proxyCountries() {
  HttpResponse res = http_->request("GET", "/proxy/countries");
  if (!res.ok()) throwHttpError("Solari GET /proxy/countries", res.status, res.body);
  return parseProxyCountries(parseJsonBody(res.body, "proxy countries"));
}

}  // namespace solari::browser
