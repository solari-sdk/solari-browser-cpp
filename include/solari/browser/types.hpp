// Wire types for the Solari browser control plane, mirroring the reference
// `sdk/src/index.ts`. Loosely-typed blobs (storageState, the full profile row)
// stay as raw nlohmann::json so a gateway field addition never forces an SDK bump.
#pragma once
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "solari/browser/http.hpp"

namespace solari::browser {

using Json = nlohmann::json;

/**
 * A Playwright `storageState` blob — `{cookies: [...], origins: [...]}`. Kept
 * raw: the SDK never needs to interpret it, only to move it between a profile
 * and a browser.
 */
using StorageState = nlohmann::json;

/** Construction options for `Client`. There is NO env-var fallback: pass the key. */
struct ClientOptions {
  /** Required. Format: `slr_live_<id>_<secret>`. */
  std::string apiKey;
  /** Gateway origin. Override for staging / self-hosted. */
  std::string baseUrl = kDefaultBaseUrl;
  /** TOTAL attempts per request (not retries). */
  int maxAttempts = kDefaultMaxAttempts;
  /** Fixed (non-exponential) pause between attempts, in ms. */
  long backoffMs = kDefaultBackoffMs;
  /** Per-attempt request timeout, in ms. */
  long timeoutMs = kDefaultTimeoutMs;
};

/** Managed proxy egress request. Every field is optional. */
struct ProxyRequest {
  /** ISO-3166-1 alpha-2, lowercase. Gateway default: "us". */
  std::optional<std::string> country;
  /** "residential" (default, rotating), "static" (fixed ISP IP), or "mobile". */
  std::optional<std::string> tier;
  /** Pin egress to a specific ASN (e.g. "20057" for AT&T Mobility). */
  std::optional<std::string> asn;
  /** Sticky-session id (alnum + dash, <=32 chars). Pins the egress IP. */
  std::optional<std::string> session;
  /** Sticky lifetime in minutes (1-30, default 10). Only with `session`. */
  std::optional<int> sessionDuration;
  /** US-only geo narrowing (e.g. "california"). */
  std::optional<std::string> state;
  /** US-only city pin (e.g. "los_angeles"). */
  std::optional<std::string> city;
};

/**
 * What goes in the `proxy` field of a create request — mirrors the TS union
 * `string | ProxyRequest`. The string form takes a country ("us"), or the
 * sentinels "off" / "smart":
 *
 *   opts.proxy = std::string("smart");
 *   opts.proxy = ProxyRequest{.country = "gb", .tier = "mobile"};
 */
using ProxySpec = std::variant<std::string, ProxyRequest>;

/** JSON for a `ProxySpec`, as sent under `proxy`. Unset ProxyRequest fields are omitted. */
Json proxyToJson(const ProxySpec& spec);

/** Coarse confirmation of the proxy the gateway resolved for a session.
 *  Carries NO credentials by design: egress is applied server-side, so a
 *  caller never dials the proxy and never needs its address or account. */
struct ResolvedProxyConfig {
  std::string timezoneId;
  std::string country;
  std::optional<std::string> tier;  // residential | static | mobile
};

/** Options for `client.sessions.create()`. Falsy fields are omitted from the body. */
struct CreateSessionOptions {
  /** Attach a stored browser profile (cookies + localStorage). */
  std::optional<std::string> profileId;
  /** Enable session recording. */
  bool recording = false;
  /** Enable the runtime stealth shim. */
  bool stealth = false;
  /** Managed captcha solving. Requires `stealth`. */
  bool captcha = false;
  /** Managed proxy egress. Requires `stealth`. */
  std::optional<ProxySpec> proxy;
};

/**
 * A live browser session.
 *
 * `wsEndpoint` / `cdpEndpoint` are the UPSTREAM gateway URLs — unlike the TS SDK
 * this binding runs no loopback proxy, so what the gateway returned is what you
 * get. Both are capability URLs: the session id is HMAC-signed, so the upgrade
 * needs no `Authorization` header. Treat them as secrets.
 */
struct Session {
  std::string id;
  /** Playwright wire protocol (`/ws/<id>`). No C++ client exists — see README. */
  std::string wsEndpoint;
  /** Raw CDP (`/cdp/<id>`). Drivable by a third-party CDP client, or CdpConnection. */
  std::string cdpEndpoint;
  /** Plan-tier deadline (ISO 8601 UTC); the session auto-releases at this point. */
  std::string expiresAt;
  /**
   * Absent (`!storageState`) = no profile attached. Present-but-JSON-null
   * (`storageState->is_null()`) = the profile exists but is empty. Present and
   * an object = the profile's cookies/origins.
   */
  std::optional<StorageState> storageState;
  /** Set only when a managed proxy was requested. */
  std::optional<ResolvedProxyConfig> proxy;
};

/** A stored browser profile. `raw` is the full row the platform returned. */
struct Profile {
  std::string id;
  std::string name;
  Json raw;
};

/** Result of `client.profiles.save()`. Both default to 0 when the gateway omits them. */
struct SaveProfileResult {
  long long version = 0;
  long long sizeBytes = 0;
};

/** Presigned rrweb replay link. */
struct ReplayUrl {
  std::string url;
  /** Defaults to 0 when the gateway omits it. */
  long long expiresInSeconds = 0;
  /** Defaults to "gzip" when the gateway omits it. */
  std::string contentEncoding = "gzip";
};

/** `GET /proxy/countries` — supported egress countries + whether creds are configured. */
struct ProxyCountries {
  /** False when this gateway has no residential proxy credentials. */
  bool enabled = false;
  /** Lowercase ISO-3166-1 alpha-2 codes, sorted. */
  std::vector<std::string> countries;
};

}  // namespace solari::browser
