// In-game updater for NX-Diffuser releases.

#pragma once

#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

#include "ship/window/gui/GuiWindow.h"

namespace gdx {

inline constexpr bool kGdxHasUpdater =
#if defined(GDX_ENABLE_UPDATER)
    true;
#else
    false;
#endif

namespace updater {

enum class State {
    Idle,            // nothing has been attempted yet
    Checking,        // querying the GitHub releases API
    UpToDate,        // the running version is current
    UpdateAvailable, // a newer release exists
    Downloading,     // fetching the release zip
    Installing,      // unpacking, hashing and staging the payload
    Installed,       // staged and verified
    Failed,          // see Status::message
};

struct Version {
    uint16_t major = 0;
    uint16_t minor = 0;
    uint16_t patch = 0;

    bool operator>(const Version& o) const {
        return std::tie(major, minor, patch) > std::tie(o.major, o.minor, o.patch);
    }
    std::string ToString() const;
};

struct Status {
    State state = State::Idle;
    std::string latestTag;    // tag_name
    std::string releaseName;  // release title
    std::string releaseNotes; // markdown body
    std::string message;      // error or otherwise
    bool hasDownload = false; // false when the release ships no installable archive
    uint64_t bytesDone = 0;
    uint64_t bytesTotal = 0;
    std::vector<std::string> stagedFiles; // verified payload waiting for the next boot
};

void SetProgramPath(const char* argv0);

void ApplyPendingUpdate();

void Init();

void Shutdown();

bool IsBusy();

void CheckForUpdates(bool silent);

void DownloadAndInstall();

void Cancel();

Status GetStatus();

bool RestartIntoNewBuild();

Version CurrentVersion();

void DrawUpdatesPanel();

void Notify(const std::string& message);

} // namespace updater
} // namespace gdx

class GdxUpdaterToast final : public Ship::GuiWindow {
  public:
    GdxUpdaterToast();

    void Draw() override;
    void DrawElement() override;

  protected:
    void InitElement() override {
    }
    void UpdateElement() override {
    }
};
