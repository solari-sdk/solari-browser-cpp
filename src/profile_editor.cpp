#include "solari/browser/profile_editor.hpp"

#include <cctype>
#include <cstdio>
#include <ctime>
#include <thread>

#include "solari/browser/client.hpp"
#include "solari/browser/errors.hpp"

namespace solari::browser {

namespace {

using clock = std::chrono::system_clock;

std::string editorPath(const std::string& profileId, const char* suffix) {
  return "/profiles/" + encodeURIComponent(profileId) + "/editor" + suffix;
}

nlohmann::json parseBody(const std::string& body, const char* what) {
  auto j = nlohmann::json::parse(body, nullptr, false);
  if (j.is_discarded()) throw SolariError(std::string("Solari: unexpected ") + what + " response");
  return j;
}

}  // namespace

clock::time_point parseExpiresAt(const nlohmann::json& v) {
  if (v.is_number_integer()) return clock::time_point(std::chrono::milliseconds(v.get<long long>()));
  if (!v.is_string()) throw SolariError("Solari: login-handoff response has no expiresAt");
  const std::string s = v.get<std::string>();
  if (!s.empty() && s.find_first_not_of("0123456789") == std::string::npos) {
    return clock::time_point(std::chrono::milliseconds(std::stoll(s)));
  }
  int y, mo, d, h, mi, sec, n = 0;
  if (std::sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d%n", &y, &mo, &d, &h, &mi, &sec, &n) != 6) {
    throw SolariError("Solari: unparseable login-handoff expiresAt: " + s);
  }
  // Days since the epoch for a civil date (Howard Hinnant's algorithm): timegm is not on MSVC.
  const long long y2 = mo <= 2 ? y - 1 : y;
  const long long era = (y2 >= 0 ? y2 : y2 - 399) / 400;
  const long long yoe = y2 - era * 400;
  const long long doy = (153 * (mo > 2 ? mo - 3 : mo + 9) + 2) / 5 + d - 1;
  const long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const long long days = era * 146097 + doe - 719468;
  auto tp = clock::time_point(std::chrono::seconds(days * 86400 + h * 3600 + mi * 60 + sec));
  std::string rest = s.substr(static_cast<size_t>(n));
  if (!rest.empty() && rest[0] == '.') {
    size_t i = 1;
    long long frac = 0;
    int digits = 0;
    while (i < rest.size() && std::isdigit(static_cast<unsigned char>(rest[i]))) {
      if (digits < 3) {
        frac = frac * 10 + (rest[i] - '0');
        digits++;
      }
      i++;
    }
    while (digits++ < 3) frac *= 10;
    tp += std::chrono::milliseconds(frac);
    rest = rest.substr(i);
  }
  if (rest == "Z") return tp;
  int oh, om;
  if (rest.size() == 6 && (rest[0] == '+' || rest[0] == '-') &&
      std::sscanf(rest.c_str() + 1, "%2d:%2d", &oh, &om) == 2) {
    const auto off = std::chrono::minutes(oh * 60 + om);
    return rest[0] == '+' ? tp - off : tp + off;
  }
  throw SolariError("Solari: unparseable login-handoff expiresAt: " + s);
}

ProfileEditor parseProfileEditor(Client* client, const std::string& requestedId,
                                 const nlohmann::json& body) {
  auto str = [&](const char* key) -> std::string {
    auto it = body.find(key);
    return it != body.end() && it->is_string() ? it->get<std::string>() : std::string();
  };
  const std::string handoffId = str("handoffId");
  const std::string url = str("url");
  if (handoffId.empty() || url.empty()) {
    throw SolariError("Solari: login-handoff response is missing handoffId or url");
  }
  long long version = 0;
  auto vit = body.find("version");
  if (vit != body.end() && vit->is_number()) version = vit->get<long long>();
  const std::string pid = str("profileId");
  auto eit = body.find("expiresAt");
  return ProfileEditor(client, pid.empty() ? requestedId : pid, handoffId, url,
                       parseExpiresAt(eit == body.end() ? nlohmann::json() : *eit), version);
}

EditorState ProfileEditor::status() {
  const std::string path = editorPath(profileId, "");
  HttpResponse res =
      client_->http().request("GET", path, std::nullopt, "", {"X-Handoff-Id: " + handoffId});
  if (!res.ok()) throwHttpError("Solari GET " + path, res.status, res.body);
  const auto j = parseBody(res.body, "editor status");
  EditorState st;
  // The gateway reports null before the editor page has been opened; that reads as idle.
  auto sit = j.find("editorStatus");
  const std::string s = sit != j.end() && sit->is_string() ? sit->get<std::string>() : std::string("idle");
  if (s == "launching") st.editorStatus = EditorStatus::Launching;
  else if (s == "editing") st.editorStatus = EditorStatus::Editing;
  else if (s == "saving") st.editorStatus = EditorStatus::Saving;
  else if (s == "error") st.editorStatus = EditorStatus::Error;
  auto vit = j.find("version");
  if (vit != j.end() && vit->is_number()) st.version = vit->get<long long>();
  return st;
}

long long ProfileEditor::saveAndClose() {
  const std::string path = editorPath(profileId, "/save");
  HttpResponse res =
      client_->http().request("POST", path, std::nullopt, "", {"X-Handoff-Id: " + handoffId}, true);
  if (!res.ok()) throwHttpError("Solari POST " + path, res.status, res.body);
  const auto j = parseBody(res.body, "editor save");
  auto vit = j.find("version");
  return vit != j.end() && vit->is_number() ? vit->get<long long>() : 0;
}

void ProfileEditor::exit() {
  const std::string path = editorPath(profileId, "");
  HttpResponse res =
      client_->http().request("DELETE", path, std::nullopt, "", {"X-Handoff-Id: " + handoffId}, true);
  if (!res.ok()) throwHttpError("Solari DELETE " + path, res.status, res.body);
}

long long ProfileEditor::waitForSave(std::chrono::milliseconds timeout,
                                     std::chrono::milliseconds poll) {
  auto deadline = expiresAt;
  if (timeout.count() > 0) deadline = std::min(deadline, clock::now() + timeout);
  for (;;) {
    for (const auto& p : client_->profiles.list()) {
      if (p.id == profileId && p.version > startVersion) return p.version;
    }
    if (clock::now() + poll >= deadline) {
      throw SolariError("Solari: timed out waiting for profile " + profileId + " to be saved", 408);
    }
    std::this_thread::sleep_for(poll);
  }
}

}  // namespace solari::browser
