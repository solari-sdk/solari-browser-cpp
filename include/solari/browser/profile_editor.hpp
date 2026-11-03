// `ProfileEditor` — the handle `client.profiles.edit(id)` returns for editing an existing profile
// through the profile editor. Send the person to `url`; it works until `expiresAt` (about 30
// minutes). There is no auto-save and no save without closing: `saveAndClose()` stores the profile
// and ends the editor, `exit()` ends it without saving.
//
// An expired or used link fails every call with a SolariError of status 404; an editor that is
// not running with 409; a failed save with 502 (the editor goes to error: `exit()` and start a new `edit`; a second save gets 409). Save and exit are never retried automatically.
#pragma once
#include <chrono>
#include <string>

#include <nlohmann/json.hpp>

namespace solari::browser {

class Client;

/** The editor's state, from `ProfileEditor::status()`. */
enum class EditorStatus { Idle, Launching, Editing, Saving, Error };

/** Result of `ProfileEditor::status()`. */
struct EditorState {
  EditorStatus editorStatus = EditorStatus::Idle;
  long long version = 0;
};

class ProfileEditor {
 public:
  ProfileEditor(Client* client, std::string profileId, std::string handoffId, std::string url,
                std::chrono::system_clock::time_point expiresAt, long long startVersion)
      : client_(client),
        profileId(std::move(profileId)),
        handoffId(std::move(handoffId)),
        url(std::move(url)),
        expiresAt(expiresAt),
        startVersion(startVersion) {}

  std::string profileId;
  std::string handoffId;
  /** The editor link to open in a browser. */
  std::string url;
  std::chrono::system_clock::time_point expiresAt;
  /** The profile's version when the editor was opened. */
  long long startVersion = 0;

  /** `GET /profiles/:id/editor`. */
  EditorState status();

  /** `POST /profiles/:id/editor/save` — store the editor's browser state in the profile and
   *  close the editor. Returns the new version. */
  long long saveAndClose();

  /** `DELETE /profiles/:id/editor` — close the editor without saving. */
  void exit();

  /**
   * Poll `GET /profiles` every `poll` until this profile's version exceeds `startVersion`
   * (someone saved from the editor page) and return it. Throws a SolariError with status 408 when
   * `timeout` passes or the link expires first. A zero `timeout` waits until the link expires.
   */
  long long waitForSave(std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
                        std::chrono::milliseconds poll = std::chrono::milliseconds(2000));

 private:
  Client* client_;
};

/** Parse a `login-handoff` body. `expiresAt` may be ISO-8601 or epoch milliseconds. */
ProfileEditor parseProfileEditor(Client* client, const std::string& requestedId,
                                 const nlohmann::json& body);

/** ISO-8601 (`2030-01-01T00:30:00.000Z`, `Z` or `+hh:mm`) or epoch ms -> time point. Throws SolariError. */
std::chrono::system_clock::time_point parseExpiresAt(const nlohmann::json& v);

}  // namespace solari::browser
