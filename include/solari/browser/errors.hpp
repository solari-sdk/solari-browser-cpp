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
