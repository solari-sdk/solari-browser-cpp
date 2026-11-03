#include "solari/browser/errors.hpp"

#include <cctype>
#include <string>

namespace solari::browser {

std::string redactUrl(const std::string& url) {
  const auto schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos || schemeEnd == 0) return "<url>";
  for (std::size_t i = 0; i < schemeEnd; ++i) {
    const char c = url[i];
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '-' || c == '.')) return "<url>";
  }
  const auto hostStart = schemeEnd + 3;
  auto hostEnd = url.find_first_of("/?#", hostStart);
  if (hostEnd == std::string::npos) hostEnd = url.size();
  auto authority = url.substr(hostStart, hostEnd - hostStart);
  const auto at = authority.rfind('@');  // user:password@host
  if (at != std::string::npos) authority = authority.substr(at + 1);
  if (authority.empty()) return "<url>";
  return url.substr(0, schemeEnd + 3) + authority + "/\xE2\x80\xA6";
}

std::string scrubUrl(const std::string& text, const std::string& secretUrl) {
  std::string out = text;
  if (!secretUrl.empty()) {
    const std::string redacted = redactUrl(secretUrl);
    for (std::size_t pos = out.find(secretUrl); pos != std::string::npos; pos = out.find(secretUrl, pos + redacted.size())) {
      out.replace(pos, secretUrl.size(), redacted);
    }
  }
  // Any other URL-looking token: cut it down to scheme://host/…
  std::string result;
  std::size_t i = 0;
  while (i < out.size()) {
    const auto sep = out.find("://", i);
    if (sep == std::string::npos) { result += out.substr(i); break; }
    std::size_t schemeStart = sep;
    while (schemeStart > i && (std::isalnum(static_cast<unsigned char>(out[schemeStart - 1])) || out[schemeStart - 1] == '+' || out[schemeStart - 1] == '-' || out[schemeStart - 1] == '.')) --schemeStart;
    std::size_t end = sep + 3;
    while (end < out.size() && !std::isspace(static_cast<unsigned char>(out[end])) && out[end] != '"' && out[end] != '\'' && out[end] != ')' && out[end] != '>' && out[end] != ',') ++end;
    result += out.substr(i, schemeStart - i);
    result += redactUrl(out.substr(schemeStart, end - schemeStart));
    i = end;
  }
  return result;
}

std::string redactSessionPath(const std::string& text) {
  static const std::string key = "/sessions/";
  std::string out;
  std::size_t i = 0;
  while (i < text.size()) {
    const auto at = text.find(key, i);
    if (at == std::string::npos) { out += text.substr(i); break; }
    const auto idStart = at + key.size();
    auto idEnd = idStart;
    while (idEnd < text.size() && text[idEnd] != '/' && text[idEnd] != '?' && text[idEnd] != '#' &&
           !std::isspace(static_cast<unsigned char>(text[idEnd]))) ++idEnd;
    out += text.substr(i, idStart - i);
    const auto id = text.substr(idStart, idEnd - idStart);
    out += id.size() > 8 ? id.substr(0, 8) + "\xE2\x80\xA6" : id;
    i = idEnd;
  }
  return out;
}

std::string cdpConnectMessage(const std::string& endpoint, const std::string& libraryError) {
  return "Solari: CDP connect to " + redactUrl(endpoint) + " failed: " +
         (libraryError.empty() ? "connect failed" : scrubUrl(libraryError, endpoint));
}

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
  throw SolariError(redactSessionPath(what) + " failed: " + std::to_string(status) + " " + body,
                    status, parseErrorCode(body));
}

}  // namespace solari::browser
