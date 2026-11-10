// Errors thrown by the Solari browser SDK. Mirrors the reference `SolariError`
// in sdk/src/index.ts: ONE exception type carrying an optional HTTP `status` and
// an optional machine-readable `code` lifted out of the gateway's JSON error
// body (`{ error, code, ... }`).
#pragma once
#include <optional>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace solari::browser {

/**
 * Machine-readable `code` values the gateway puts in an error body. Branch on
 * `SolariError::code` rather than the human-readable message:
 *
 *   catch (const solari::browser::SolariError& e) {
 *     if (e.code == solari::browser::error_code::ConcurrencyLimitExceeded) ...
 *   }
 */
namespace error_code {
/** 403 — stealth / proxy / captcha asked for on a plan that doesn't include it. */
inline constexpr const char* FeatureRequiresPlan = "FeatureRequiresPlan";
/** 429 — the org is at its concurrent-session cap. */
inline constexpr const char* ConcurrencyLimitExceeded = "ConcurrencyLimitExceeded";
/** 403 — a plan quota (e.g. stored profiles) is exhausted. */
inline constexpr const char* PlanLimitExceeded = "PlanLimitExceeded";
/** The acquired browser failed its health probe. */
inline constexpr const char* BrowserUnhealthy = "BrowserUnhealthy";
/**
 * 404 — the gateway refused a session id (malformed, forged, or another org's)
 * and acted on nothing. Only meaningful on a 404; see
 * `SessionsResource::release` for why a 404 is not blanket-success there.
 */
inline constexpr const char* InvalidSessionId = "InvalidSessionId";
/**
 * 503 on `POST /sessions` — a proxy tier you explicitly requested cannot be served right now. No session
 * was created. The client does not retry it.
 */
inline constexpr const char* ProxyTierUnavailable = "ProxyTierUnavailable";
}  // namespace error_code

/** Every failure the SDK raises. `status` is unset for transport-level errors. */
class SolariError : public std::runtime_error {
 public:
  std::optional<int> status;
  std::optional<std::string> code;

  explicit SolariError(const std::string& message,
                       std::optional<int> statusCode = std::nullopt,
                       std::optional<std::string> errorCode = std::nullopt)
      : std::runtime_error(message),
        status(statusCode),
        code(std::move(errorCode)) {}
};

/**
 * `scheme://host[:port]/…` of a URL, for error messages. A CDP endpoint, a signed replay URL and a
 * preview URL carry their credential in the path or query, so those never go into an error. Text
 * that is not a URL becomes `<url>`.
 */
std::string redactUrl(const std::string& url);

/**
 * `text` with every occurrence of `secretUrl` replaced by its redacted form, and then every other
 * URL-looking token (`scheme://…`) redacted too: the transport libraries print the URL they were
 * given, sometimes in a normalised spelling that no longer matches byte for byte.
 */
std::string scrubUrl(const std::string& text, const std::string& secretUrl);

/**
 * `text` with the session id in every `/sessions/<id>` cut to its first 8 characters. The session id
 * is the credential inside the ws/cdp URLs, so it does not belong in error text or logs.
 */
std::string redactSessionPath(const std::string& text);

/**
 * `body` with the session id of `what` (`... /sessions/<id>/...`), in any spelling, and every `"sessionId":"..."` field cut to
 * their first 8 characters. The id is the credential inside the ws/cdp URLs and the gateway's error bodies used to echo it
 * (the replay-url 404), so it must not reach an error message through the body either.
 */
std::string redactBody(const std::string& what, const std::string& body);

/** The text of a failed CDP connect, with the endpoint and any URL in the library's message redacted. */
std::string cdpConnectMessage(const std::string& endpoint, const std::string& libraryError);

/**
 * Lift `code` out of a JSON error body. Returns nullopt when the body isn't
 * JSON, isn't an object, or has no *string* `code` — mirroring the reference
 * SDK's `try { JSON.parse(text) } catch {}` + `typeof parsed.code === "string"`.
 */
std::optional<std::string> parseErrorCode(const std::string& body);

/**
 * Throw `SolariError("<what> failed: <status> <body>")` with `status` and any
 * parsed `code` attached. `what` is the request description, e.g.
 * "Solari POST /sessions".
 */
[[noreturn]] void throwHttpError(const std::string& what, int status,
                                 const std::string& body);

}  // namespace solari::browser
