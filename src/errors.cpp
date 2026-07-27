#include "solari/browser/errors.hpp"

namespace solari::browser {

std::optional<std::string> parseErrorCode(const std::string& body) {
  if (body.empty()) return std::nullopt;
  nlohmann::json parsed;
  try {
    parsed = nlohmann::json::parse(body);
  } catch (...) {
    return std::nullopt;  // not JSON — the TS SDK swallows this too
  }
  if (!parsed.is_object()) return std::nullopt;
  auto it = parsed.find("code");
  if (it == parsed.end() || !it->is_string()) return std::nullopt;
  return it->get<std::string>();
}

void throwHttpError(const std::string& what, int status, const std::string& body) {
  throw SolariError(what + " failed: " + std::to_string(status) + " " + body,
                    status, parseErrorCode(body));
}

}  // namespace solari::browser
