// NX-Diffuser updater

#include "gdx_updater.h"

#include "gdx_version.h"
#include "port_log.h"

#include <imgui.h>

#include "ui/UIWidgets.hpp"

#include <cfloat>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#ifdef GDX_ENABLE_UPDATER

#include "gdx_extract_launch.h"
#include "gdx_replace_file.h"
#include "gdx_thread_affinity.h"
#include "gdx_updater_platform.h"

#include "libultraship/bridge/consolevariablebridge.h"
#include "ship/Context.h"
#include "ship/window/Window.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <zip.h>

#include <sys/stat.h>
#include <sys/statvfs.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <thread>

#endif // GDX_ENABLE_UPDATER

namespace {

std::mutex sToastMutex;
std::string sToastMessage;
bool sToastFresh = false;
double sToastExpiry = 0.0;

constexpr double kToastSeconds = 12.0;

} // namespace

namespace gdx::updater {

void Notify(const std::string& message) {
    std::lock_guard<std::mutex> lock(sToastMutex);
    sToastMessage = message;
    sToastFresh = true;
}

std::string Version::ToString() const {
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}

Version CurrentVersion() {
    return { GDX_VERSION_MAJOR, GDX_VERSION_MINOR, GDX_VERSION_PATCH };
}

} // namespace gdx::updater

GdxUpdaterToast::GdxUpdaterToast() : Ship::GuiWindow("gOpenWindows.UpdaterToast", true, "Updater Toast") {
}

void GdxUpdaterToast::Draw() {
    std::string message;
    {
        std::lock_guard<std::mutex> lock(sToastMutex);
        if (sToastFresh) {
            sToastExpiry = ImGui::GetTime() + kToastSeconds;
            sToastFresh = false;
        }
        if (sToastMessage.empty() || ImGui::GetTime() > sToastExpiry) {
            return;
        }
        message = sToastMessage;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 topCenter = viewport->WorkPos + ImVec2(viewport->WorkSize.x * 0.5f, 16.0f);
    ImGui::SetNextWindowPos(topCenter, ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(viewport->WorkSize.x * 0.8f, FLT_MAX));

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoInputs;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 9.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.055f, 0.065f, 0.082f, 0.94f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.35f, 0.65f, 1.0f, 0.45f));

    if (ImGui::Begin("Updater Toast##GdxUpdater", nullptr, flags)) {
        DrawElement();
    }
    ImGui::End();

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

void GdxUpdaterToast::DrawElement() {
    std::string message;
    {
        std::lock_guard<std::mutex> lock(sToastMutex);
        message = sToastMessage;
    }
    ImGui::TextUnformatted("NX-Diffuser update");
    ImGui::TextWrapped("%s", message.c_str());
    ImGui::TextDisabled("Settings > Updates");
}

#ifdef GDX_ENABLE_UPDATER

namespace gdx::updater {

namespace {

constexpr uint64_t kMaxDownloadBytes = 256ull * 1024 * 1024;
constexpr size_t kMaxApiBodyBytes = 4ull * 1024 * 1024;
constexpr size_t kCopyChunk = 64 * 1024;

const char* const kPendingName = "gdiffuser-update.pending";
const char* const kPartName = "gdiffuser-update.zip.part";

// Please don't change these on update :)
const char* const kProtectedNames[] = {
    "fzerox.o2r", "generic.o2r",       "fzerox-disk.o2r", "n64ddipl.o2r",
    "fzerox.sav", "gdiffuser.cfg.json", kPendingName,     kPartName,
};

std::mutex sMutex;
Status sStatus;
std::thread sWorker;
std::atomic_bool sWorkerRunning{ false };
std::atomic_bool sCancel{ false };

// The network stack is brought up on first use and torn down at shutdown
std::mutex sNetMutex;
bool sNetReady = false;

std::string sProgramPath;
std::string sDownloadUrl;
uint64_t sDownloadSize = 0;

std::string sBootMessage;
bool sBootFailed = false;

void SetState(State state, const std::string& message = "") {
    std::lock_guard<std::mutex> lock(sMutex);
    sStatus.state = state;
    sStatus.message = message;
}

void Fail(const std::string& message) {
    gdx_port_logf("[updater] %s\n", message.c_str());
    SetState(State::Failed, message);
}

void FailOrCancel(State cancelledState, const std::string& message) {
    if (sCancel.load()) {
        SetState(cancelledState, "Cancelled. Nothing was changed.");
    } else {
        Fail(message);
    }
}

bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
               return std::tolower(x) == std::tolower(y);
           });
}

std::string Basename(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string InstallDir() {
    if (!sProgramPath.empty()) {
        const size_t slash = sProgramPath.find_last_of("/\\");
        if (slash != std::string::npos && slash > 0) {
            return sProgramPath.substr(0, slash);
        }
    }
    return ".";
}

std::string PathIn(const std::string& name) {
    return InstallDir() + "/" + name;
}

bool IsSafePayloadName(const std::string& name) {
    if (name.empty() || name.size() > 128 || name == "." || name == "..") {
        return false;
    }
    if (name.find_first_of("/\\:") != std::string::npos) {
        return false;
    }
    for (const char* protectedName : kProtectedNames) {
        if (EqualsIgnoreCase(name, protectedName)) {
            return false;
        }
    }
    return true;
}

bool FileExists(const std::string& path) {
    struct stat info;
    return stat(path.c_str(), &info) == 0;
}

bool ParseVersion(const std::string& text, Version& out) {
    for (size_t i = 0; i < text.size(); i++) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) {
            continue;
        }
        unsigned major = 0, minor = 0, patch = 0;
        int consumed = 0;
        if (sscanf(text.c_str() + i, "%u.%u.%u%n", &major, &minor, &patch, &consumed) == 3 && consumed > 0) {
            out.major = static_cast<uint16_t>(major);
            out.minor = static_cast<uint16_t>(minor);
            out.patch = static_cast<uint16_t>(patch);
            return true;
        }
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
            i++;
        }
    }
    return false;
}

bool EnsureNetwork() {
    std::lock_guard<std::mutex> lock(sNetMutex);
    if (sNetReady) {
        return true;
    }
    if (!platform::NetworkInit()) {
        return false;
    }
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        platform::NetworkExit();
        return false;
    }
    sNetReady = true;
    return true;
}

void ReleaseNetwork() {
    std::lock_guard<std::mutex> lock(sNetMutex);
    if (!sNetReady) {
        return;
    }
    curl_global_cleanup();
    platform::NetworkExit();
    sNetReady = false;
}

size_t WriteToString(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    const size_t bytes = size * nmemb;
    if (out->size() + bytes > kMaxApiBodyBytes) {
        return 0;
    }
    out->append(ptr, bytes);
    return bytes;
}

size_t WriteToFile(char* ptr, size_t size, size_t nmemb, void* userdata) {
    return fwrite(ptr, size, nmemb, static_cast<FILE*>(userdata));
}

int ProgressCb(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    if (sCancel.load()) {
        return 1;
    }
    if (dltotal > 0 && static_cast<uint64_t>(dltotal) > kMaxDownloadBytes) {
        return 1;
    }
    if (clientp != nullptr && *static_cast<const bool*>(clientp)) {
        std::lock_guard<std::mutex> lock(sMutex);
        sStatus.bytesDone = static_cast<uint64_t>(dlnow < 0 ? 0 : dlnow);
        sStatus.bytesTotal = static_cast<uint64_t>(dltotal < 0 ? 0 : dltotal);
    }
    return 0;
}

bool HttpGet(const std::string& url, std::string* bodyOut, FILE* fileOut, bool isGitHubApi, bool trackProgress,
             std::string& error) {
    if (!EnsureNetwork()) {
        error = "The console's network stack could not be started.";
        return false;
    }

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        error = "Could not initialise the network stack.";
        return false;
    }

    curl_slist* headers = nullptr;
    if (isGitHubApi) {
        headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
        headers = curl_slist_append(headers, "X-GitHub-Api-Version: 2022-11-28");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    const std::string userAgent = std::string("G-Diffuser-Updater/") + GDX_VERSION_STRING;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 45L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &trackProgress);

    if (bodyOut != nullptr) {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToString);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, bodyOut);
    } else {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToFile);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fileOut);
    }

    const CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res == CURLE_ABORTED_BY_CALLBACK) {
        error = sCancel.load() ? "Cancelled." : "The release archive is larger than this updater will accept.";
        return false;
    }
    if (res != CURLE_OK) {
        error = std::string("Network error: ") + curl_easy_strerror(res);
        return false;
    }
    if (httpCode == 403 || httpCode == 429) {
        error = "GitHub is rate limiting this console. Try again in an hour.";
        return false;
    }
    if (httpCode == 404) {
        error = "No published release found for " + std::string(GDX_UPDATE_REPO) + ".";
        return false;
    }
    if (httpCode < 200 || httpCode >= 300) {
        error = "GitHub returned HTTP " + std::to_string(httpCode) + ".";
        return false;
    }
    return true;
}

bool WriteEntry(zip_t* archive, zip_uint64_t index, const std::string& destPath, std::string& error) {
    zip_file_t* entry = zip_fopen_index(archive, index, 0);
    if (entry == nullptr) {
        error = "Could not read the release archive.";
        return false;
    }
    FILE* dest = fopen(destPath.c_str(), "wb");
    if (dest == nullptr) {
        zip_fclose(entry);
        error = "Could not write to " + destPath + ".";
        return false;
    }

    std::vector<char> buffer(kCopyChunk);
    bool ok = true;
    for (;;) {
        if (sCancel.load()) {
            error = "Cancelled.";
            ok = false;
            break;
        }
        const zip_int64_t read = zip_fread(entry, buffer.data(), buffer.size());
        if (read < 0) {
            error = "The release archive is corrupt.";
            ok = false;
            break;
        }
        if (read == 0) {
            break;
        }
        if (fwrite(buffer.data(), 1, static_cast<size_t>(read), dest) != static_cast<size_t>(read)) {
            error = "Ran out of space writing " + Basename(destPath) + ".";
            ok = false;
            break;
        }
    }

    if (ok && fflush(dest) != 0) {
        ok = false;
        error = "Ran out of space writing " + Basename(destPath) + ".";
    }
    fclose(dest);
    zip_fclose(entry);

    if (!ok) {
        remove(destPath.c_str());
        return false;
    }
    gdx_storage_commit();
    return true;
}

bool ReadEntryToString(zip_t* archive, zip_uint64_t index, std::string& out, std::string& error) {
    zip_stat_t info;
    if (zip_stat_index(archive, index, 0, &info) != 0 || (info.valid & ZIP_STAT_SIZE) == 0) {
        error = "The release archive is corrupt.";
        return false;
    }
    if (info.size > 1024 * 1024) {
        error = "The release manifest is implausibly large.";
        return false;
    }
    zip_file_t* entry = zip_fopen_index(archive, index, 0);
    if (entry == nullptr) {
        error = "Could not read the release archive.";
        return false;
    }
    out.resize(static_cast<size_t>(info.size));
    const zip_int64_t read = out.empty() ? 0 : zip_fread(entry, out.data(), out.size());
    zip_fclose(entry);
    if (read < 0 || static_cast<size_t>(read) != out.size()) {
        error = "The release manifest is truncated.";
        return false;
    }
    return true;
}

std::map<std::string, std::string> ParseManifest(const std::string& text) {
    std::map<std::string, std::string> hashes;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;

        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        const size_t space = line.find(' ');
        if (space != 64) {
            continue;
        }
        std::string hash = line.substr(0, 64);
        std::string name = line.substr(space);
        name.erase(0, name.find_first_not_of(" *"));
        std::transform(hash.begin(), hash.end(), hash.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (hash.find_first_not_of("0123456789abcdef") != std::string::npos) {
            continue;
        }
        if (!IsSafePayloadName(name)) {
            gdx_port_logf("[updater] ignoring manifest entry '%s'\n", name.c_str());
            continue;
        }
        hashes[name] = hash;
    }
    return hashes;
}

bool HasFreeSpace(uint64_t needed) {
    struct statvfs vfs;
    if (statvfs(InstallDir().c_str(), &vfs) != 0) {
        return true;
    }
    const uint64_t free = static_cast<uint64_t>(vfs.f_bsize) * static_cast<uint64_t>(vfs.f_bavail);
    return free >= needed;
}

void RemoveStaged(const std::vector<std::string>& names) {
    for (const auto& name : names) {
        remove((PathIn(name) + ".new").c_str());
    }
    gdx_storage_commit();
}

std::vector<std::string> ReadPending() {
    std::vector<std::string> names;
    FILE* file = fopen(PathIn(kPendingName).c_str(), "rb");
    if (file == nullptr) {
        return names;
    }
    char line[192];
    while (fgets(line, sizeof(line), file) != nullptr) {
        std::string name(line);
        while (!name.empty() && (name.back() == '\n' || name.back() == '\r' || name.back() == ' ')) {
            name.pop_back();
        }
        if (name.empty()) {
            continue;
        }
        if (!IsSafePayloadName(name)) {
            gdx_port_logf("[updater] ignoring staged entry '%s'\n", name.c_str());
            continue;
        }
        names.push_back(name);
    }
    fclose(file);
    return names;
}

bool WritePending(const std::vector<std::string>& names) {
    const std::string path = PathIn(kPendingName);
    FILE* file = fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    bool ok = true;
    for (const auto& name : names) {
        ok = fprintf(file, "%s\n", name.c_str()) > 0 && ok;
    }
    ok = fflush(file) == 0 && ok;
    fclose(file);
    if (!ok) {
        remove(path.c_str());
    }
    gdx_storage_commit();
    return ok;
}

void ClearPending() {
    remove(PathIn(kPendingName).c_str());
    gdx_storage_commit();
}

void RollbackCommitted(const std::vector<std::string>& committed) {
    for (const auto& done : committed) {
        const std::string doneLive = PathIn(done);
        gdx_replace_file((doneLive + ".bak").c_str(), doneLive.c_str());
    }
    gdx_storage_commit();
}

// Swaps the verified `<name>.new` files in, keeping whatever they replace as `<name>.bak`.
bool CommitPayload(const std::vector<std::string>& names, std::string& error) {
    std::vector<std::string> committed;
    for (const auto& name : names) {
        const std::string live = PathIn(name);
        const std::string backup = live + ".bak";
        const std::string staged = live + ".new";

        const bool movedAside = gdx_replace_file(live.c_str(), backup.c_str()) != 0;
        if (!movedAside && FileExists(live)) {
            error = "Could not move the old " + name + " out of the way.";
            RollbackCommitted(committed);
            return false;
        }
        if (gdx_replace_file(staged.c_str(), live.c_str()) == 0) {
            error = "Could not replace " + name + ".";
            if (movedAside) {
                gdx_replace_file(backup.c_str(), live.c_str());
            }
            RollbackCommitted(committed);
            return false;
        }
        committed.push_back(name);
    }
    gdx_storage_commit();
    return true;
}

void CheckWorker(bool silent) {
    SetState(State::Checking, "Checking Releases...");

    const std::string url = "https://api.github.com/repos/" + std::string(GDX_UPDATE_REPO) + "/releases/latest";
    std::string body;
    std::string error;
    if (!HttpGet(url, &body, nullptr, true, false, error)) {
        if (silent) {
            gdx_port_logf("[updater] background check failed: %s\n", error.c_str());
            SetState(State::Idle);
        } else {
            FailOrCancel(State::Idle, error);
        }
        return;
    }

    const nlohmann::json release = nlohmann::json::parse(body, nullptr, false);
    if (release.is_discarded() || !release.is_object()) {
        Fail("GitHub sent a response this build could not read.");
        return;
    }

    const std::string tag = release.value("tag_name", "");
    Version latest;
    if (!ParseVersion(tag, latest) && !ParseVersion(release.value("name", ""), latest)) {
        Fail("Could not read a version number out of release tag '" + tag + "'.");
        return;
    }

    std::string assetUrl;
    std::string assetName;
    uint64_t assetSize = 0;
    if (release.contains("assets") && release["assets"].is_array()) {
        for (const auto& asset : release["assets"]) {
            std::string name = asset.value("name", "");
            std::string lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lower.size() < 4 || lower.compare(lower.size() - 4, 4, ".zip") != 0) {
                continue;
            }
            const bool namesSwitch = lower.find("switch") != std::string::npos;
            if (assetUrl.empty() || namesSwitch) {
                assetUrl = asset.value("browser_download_url", "");
                assetName = name;
                assetSize = asset.value("size", 0ull);
            }
            if (namesSwitch) {
                break;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(sMutex);
        sStatus.latestTag = tag;
        sStatus.releaseName = release.value("name", tag);
        sStatus.releaseNotes = release.value("body", "");
        sStatus.hasDownload = !assetUrl.empty();
        sStatus.bytesDone = 0;
        sStatus.bytesTotal = assetSize;
        sStatus.stagedFiles.clear();
        sDownloadUrl = assetUrl;
        sDownloadSize = assetSize;
    }

    if (!(latest > CurrentVersion())) {
        SetState(State::UpToDate, "NX-Diffuser " + CurrentVersion().ToString() + " is the latest release.");
        if (!silent) {
            Notify("NX-Diffuser " + CurrentVersion().ToString() + " is up to date.");
        }
        return;
    }

    if (assetUrl.empty()) {
        SetState(State::UpdateAvailable,
                 "Release " + tag + " has no downloadable archive. Install it manually from the release page.");
    } else {
        SetState(State::UpdateAvailable, "NX-Diffuser " + tag + " is available (" + assetName + ").");
    }
    Notify("NX-Diffuser " + tag + " is available.");
}

void InstallWorker(std::string url, uint64_t expectedSize) {
    SetState(State::Downloading, "Downloading...");
    {
        std::lock_guard<std::mutex> lock(sMutex);
        sStatus.bytesDone = 0;
        sStatus.stagedFiles.clear();
    }

    if (expectedSize > 0 && !HasFreeSpace(expectedSize * 2)) {
        Fail("Not enough free space on the SD card. About " + std::to_string((expectedSize * 2) / (1024 * 1024)) +
             " MB is needed.");
        return;
    }

    const std::string zipPath = PathIn(kPartName);
    std::vector<std::string> staged;
    std::string error;

    RemoveStaged(ReadPending());
    ClearPending();

    FILE* zipFile = fopen(zipPath.c_str(), "wb");
    if (zipFile == nullptr) {
        Fail("Could not write to " + InstallDir() + ".");
        return;
    }
    const bool downloaded = HttpGet(url, nullptr, zipFile, false, true, error);
    const bool flushed = fflush(zipFile) == 0;
    fclose(zipFile);
    gdx_storage_commit();
    if (!downloaded || !flushed) {
        remove(zipPath.c_str());
        gdx_storage_commit();
        FailOrCancel(State::UpdateAvailable, downloaded ? "Ran out of space downloading the update." : error);
        return;
    }

    SetState(State::Installing, "Verifying download...");

    int zipError = 0;
    zip_t* archive = zip_open(zipPath.c_str(), ZIP_RDONLY, &zipError);
    if (archive == nullptr) {
        remove(zipPath.c_str());
        gdx_storage_commit();
        Fail("The downloaded archive could not be opened.");
        return;
    }

    std::map<std::string, zip_uint64_t> entries;
    std::vector<std::string> ambiguous;
    uint64_t payloadBytes = 0;
    const zip_int64_t entryCount = zip_get_num_entries(archive, 0);
    for (zip_int64_t i = 0; i < entryCount; i++) {
        const char* rawName = zip_get_name(archive, static_cast<zip_uint64_t>(i), 0);
        if (rawName == nullptr || rawName[0] == '\0') {
            continue;
        }
        if (rawName[strlen(rawName) - 1] == '/') {
            continue;
        }
        const std::string name = Basename(rawName);
        if (name.empty()) {
            continue;
        }
        if (!entries.emplace(name, static_cast<zip_uint64_t>(i)).second) {
            ambiguous.push_back(name);
        }
        zip_stat_t info;
        if (zip_stat_index(archive, static_cast<zip_uint64_t>(i), 0, &info) == 0 && (info.valid & ZIP_STAT_SIZE)) {
            payloadBytes += info.size;
        }
    }

    const auto manifestEntry = entries.find("SHA256SUMS.txt");
    if (manifestEntry == entries.end()) {
        zip_close(archive);
        remove(zipPath.c_str());
        gdx_storage_commit();
        Fail("The release archive has no SHA256SUMS.txt.");
        return;
    }

    std::string manifestText;
    if (!ReadEntryToString(archive, manifestEntry->second, manifestText, error)) {
        zip_close(archive);
        remove(zipPath.c_str());
        gdx_storage_commit();
        Fail(error);
        return;
    }

    const std::map<std::string, std::string> expected = ParseManifest(manifestText);
    if (expected.empty()) {
        zip_close(archive);
        remove(zipPath.c_str());
        gdx_storage_commit();
        Fail("SHA256SUMS.txt listed no files this updater is allowed to install.");
        return;
    }

    if (!HasFreeSpace(payloadBytes + kCopyChunk)) {
        zip_close(archive);
        remove(zipPath.c_str());
        gdx_storage_commit();
        Fail("Not enough free space on the SD card to unpack the update.");
        return;
    }

    bool unpacked = true;
    for (const auto& [name, hash] : expected) {
        if (std::find(ambiguous.begin(), ambiguous.end(), name) != ambiguous.end()) {
            error = "The release archive contains more than one " + name + ".";
            unpacked = false;
            break;
        }
        const auto entry = entries.find(name);
        if (entry == entries.end()) {
            error = "SHA256SUMS.txt lists " + name + ", but the archive does not contain it.";
            unpacked = false;
            break;
        }

        SetState(State::Installing, "Unpacking " + name + "...");
        const std::string stagedPath = PathIn(name) + ".new";
        if (!WriteEntry(archive, entry->second, stagedPath, error)) {
            unpacked = false;
            break;
        }
        staged.push_back(name);

        SetState(State::Installing, "Verifying " + name + "...");
        if (gdx::GdxExtractFileSha256(stagedPath.c_str()) != hash) {
            error = name + " failed its checksum. Nothing was changed.";
            unpacked = false;
            break;
        }
    }

    zip_close(archive);
    remove(zipPath.c_str());
    gdx_storage_commit();

    if (!unpacked) {
        RemoveStaged(staged);
        FailOrCancel(State::UpdateAvailable, error);
        return;
    }

    SetState(State::Installing, "Staging files for restart...");
    if (!WritePending(staged)) {
        RemoveStaged(staged);
        Fail("Could not record the staged update in " + InstallDir() + ".");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(sMutex);
        sStatus.stagedFiles = staged;
        sStatus.state = State::Installed;
        sStatus.message = "Downloaded and verified. Restart NX-Diffuser to finish installing.";
    }
    Notify("Update ready. Restart to install it.");
}

void RunAsync(std::function<void()> work) {
    bool expected = false;
    if (!sWorkerRunning.compare_exchange_strong(expected, true)) {
        return;
    }
    if (sWorker.joinable()) {
        sWorker.join();
    }
    sCancel.store(false);
    sWorker = std::thread([work = std::move(work)]() {
        gdx_thread_affinity_pin("updater", GDX_CORE_WORKER);
        work();
        sWorkerRunning.store(false);
    });
}

struct WorkerJoiner {
    ~WorkerJoiner() {
        Shutdown();
    }
};
WorkerJoiner sWorkerJoiner;

} // namespace

void SetProgramPath(const char* argv0) {
    if (argv0 != nullptr && argv0[0] != '\0') {
        sProgramPath = argv0;
    }
}

void ApplyPendingUpdate() {
    const std::vector<std::string> pending = ReadPending();
    if (pending.empty()) {
        return;
    }

    for (const auto& name : pending) {
        if (!FileExists(PathIn(name) + ".new")) {
            gdx_port_logf("[updater] staged update is missing %s.new\n", name.c_str());
            RemoveStaged(pending);
            ClearPending();
            sBootFailed = true;
            sBootMessage = "The staged update was incomplete and has been discarded. Please try again.";
            return;
        }
    }

    gdx_port_logf("[updater] committing %zu staged file(s)\n", pending.size());
    std::string error;
    if (!CommitPayload(pending, error)) {
        gdx_port_logf("[updater] %s\n", error.c_str());
        RemoveStaged(pending);
        ClearPending();
        sBootFailed = true;
        sBootMessage = error + " Nothing was changed.";
        return;
    }
    ClearPending();

    const std::string self = sProgramPath.empty() ? "G-Diffuser.nro" : Basename(sProgramPath);
    const bool replacedSelf = std::any_of(pending.begin(), pending.end(),
                                          [&self](const std::string& name) { return EqualsIgnoreCase(name, self); });
    if (!replacedSelf) {
        return;
    }

    if (platform::QueueNextLoad(sProgramPath.empty() ? PathIn(self) : sProgramPath)) {
        exit(0);
    }
    gdx_port_logf("[updater] update applied but this loader cannot relaunch homebrew\n");
    sBootMessage = "The update was installed, but NX-Diffuser could not relaunch itself into it. Quit and start "
                   "NX-Diffuser again to run the new version.";
}

void Init() {
    gdx_port_logf("[updater] NX-Diffuser %s, install directory %s\n", GDX_VERSION_STRING, InstallDir().c_str());

    if (!sBootMessage.empty()) {
        SetState(sBootFailed ? State::Failed : State::Idle, sBootMessage);
        Notify(sBootMessage);
    }

    if (CVarGetInteger("gSettings.Updater.CheckOnBoot", 0)) {
        CheckForUpdates(true);
    }
}

void Shutdown() {
    sCancel.store(true);
    if (sWorker.joinable()) {
        sWorker.join();
    }
    ReleaseNetwork();
}

bool IsBusy() {
    return sWorkerRunning.load();
}

void Cancel() {
    sCancel.store(true);
}

void CheckForUpdates(bool silent) {
    RunAsync([silent]() { CheckWorker(silent); });
}

void DownloadAndInstall() {
    std::string url;
    uint64_t size = 0;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        url = sDownloadUrl;
        size = sDownloadSize;
    }
    if (url.empty()) {
        Fail("No release archive to install. Check for updates first.");
        return;
    }
    RunAsync([url, size]() { InstallWorker(url, size); });
}

Status GetStatus() {
    std::lock_guard<std::mutex> lock(sMutex);
    return sStatus;
}

bool RestartIntoNewBuild() {
    const std::string nro = sProgramPath.empty() ? PathIn("G-Diffuser.nro") : sProgramPath;
    if (!platform::QueueNextLoad(nro)) {
        gdx_port_logf("[updater] could not queue %s for relaunch\n", nro.c_str());
        return false;
    }
    if (auto window = Ship::Context::GetInstance()->GetWindow()) {
        window->Close();
    }
    return true;
}

} // namespace gdx::updater

#else // GDX_ENABLE_UPDATER

namespace gdx::updater {

void SetProgramPath(const char*) {
}
void ApplyPendingUpdate() {
}
void Init() {
}
void Shutdown() {
}
bool IsBusy() {
    return false;
}
void CheckForUpdates(bool) {
}
void DownloadAndInstall() {
}
void Cancel() {
}
Status GetStatus() {
    return {};
}
bool RestartIntoNewBuild() {
    return false;
}

} // namespace gdx::updater

#endif // GDX_ENABLE_UPDATER

namespace gdx::updater {

namespace {

std::string FormatBytes(uint64_t bytes) {
    char buffer[64];
    if (bytes >= 1024ull * 1024) {
        snprintf(buffer, sizeof(buffer), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else {
        snprintf(buffer, sizeof(buffer), "%llu KB", static_cast<unsigned long long>(bytes / 1024));
    }
    return buffer;
}

void DrawStatusBlock(const Status& status) {
    ImGui::Text("Installed: NX-Diffuser %s", GDX_VERSION_STRING);
    if (!status.latestTag.empty()) {
        ImGui::Text("Latest on GitHub: %s", status.latestTag.c_str());
    }

    ImVec4 color;
    switch (status.state) {
        case State::Failed:
            color = ImVec4(1.0f, 0.45f, 0.45f, 1.0f);
            break;
        case State::UpdateAvailable:
        case State::Installed:
            color = ImVec4(0.45f, 1.0f, 0.55f, 1.0f);
            break;
        default:
            color = ImVec4(0.75f, 0.75f, 0.75f, 1.0f);
            break;
    }
    if (!status.message.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(color, "%s", status.message.c_str());
        ImGui::PopTextWrapPos();
    }

    if (status.state == State::Installed && !status.stagedFiles.empty()) {
        std::string replaced;
        for (const auto& file : status.stagedFiles) {
            replaced += (replaced.empty() ? "" : ", ") + file;
        }
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s will be replaced on restart", replaced.c_str());
        ImGui::PopTextWrapPos();
    }

    if (status.state == State::Downloading) {
        if (status.bytesTotal > 0) {
            const float fraction = static_cast<float>(status.bytesDone) / static_cast<float>(status.bytesTotal);
            const std::string overlay = FormatBytes(status.bytesDone) + " / " + FormatBytes(status.bytesTotal);
            ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, 0.0f), overlay.c_str());
        } else {
            ImGui::Text("Downloaded %s...", FormatBytes(status.bytesDone).c_str());
        }
    }
}

void DrawActions(const Status& status) {
    const bool busy = IsBusy();

    UIWidgets::ButtonOptions checkOptions = {};
    checkOptions.color = UIWidgets::Colors::LightBlue;
    checkOptions.disabled = busy;
    checkOptions.disabledTooltip = "An update job is already running.";
    checkOptions.tooltip = "Check whether a newer NX-Diffuser release has been published.";
    if (UIWidgets::Button("Check for Updates", checkOptions)) {
        CheckForUpdates(false);
    }

    if (status.state == State::UpdateAvailable && status.hasDownload) {
        UIWidgets::ButtonOptions installOptions = {};
        installOptions.color = UIWidgets::Colors::Green;
        installOptions.disabled = busy;
        installOptions.disabledTooltip = "An update job is already running.";
        installOptions.tooltip = "Download and verify the release, then swap it in on the next start. Your saves, "
                                 "settings, and archives are untouched.";
        if (UIWidgets::Button("Download and Install", installOptions)) {
            ImGui::OpenPopup("##gdxupdateinstall");
        }
    }

    if (busy) {
        UIWidgets::ButtonOptions cancelOptions = {};
        cancelOptions.color = UIWidgets::Colors::Gray;
        cancelOptions.tooltip = "Stop the transfer. Nothing already on disk is changed.";
        if (UIWidgets::Button("Cancel", cancelOptions)) {
            Cancel();
        }
    }

    if (status.state == State::Installed) {
        UIWidgets::ButtonOptions restartOptions = {};
        restartOptions.color = UIWidgets::Colors::Green;
        restartOptions.tooltip = "Quit and relaunch to finish installing the downloaded version.";
        if (UIWidgets::Button("Restart Now", restartOptions)) {
            ImGui::OpenPopup("##gdxupdaterestart");
        }
    }

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("##gdxupdateinstall", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(("NX-Diffuser " + status.latestTag +
                                " will be downloaded and checked now, then swapped in the\n"
                                "next time NX-Diffuser starts.\n\n"
                                "Your saves, settings, and archives are preserved.")
                                   .c_str());
        ImGui::Separator();
        if (ImGui::Button("Install")) {
            DownloadAndInstall();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    static std::string sRestartError;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("##gdxupdaterestart", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("NX-Diffuser will close, swap the new files in as it starts, and\n"
                               "reopen on the new version. Unsaved progress is lost.");
        if (!sRestartError.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", sRestartError.c_str());
        }
        ImGui::Separator();
        if (ImGui::Button("Restart")) {
            if (RestartIntoNewBuild()) {
                ImGui::CloseCurrentPopup();
            } else {
                sRestartError = "This loader cannot relaunch homebrew. Quit NX-Diffuser and start it\n"
                                "again from the homebrew menu.";
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            sRestartError.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void DrawReleaseNotes(const Status& status) {
    if (status.releaseNotes.empty()) {
        return;
    }
    ImGui::SeparatorText(status.releaseName.empty() ? "Release notes" : status.releaseName.c_str());
    ImGui::BeginChild("GdxUpdaterReleaseNotes", ImVec2(0.0f, 180.0f), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(status.releaseNotes.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

} // namespace

void DrawUpdatesPanel() {
    if constexpr (!gdx::kGdxHasUpdater) {
        ImGui::TextDisabled("The in-game updater is built for the console release only.");
        return;
    }

    const Status status = GetStatus();
    DrawStatusBlock(status);
    ImGui::Spacing();
    DrawActions(status);
    DrawReleaseNotes(status);
    ImGui::Spacing();
    ImGui::TextDisabled("Updates come from github.com/%s", GDX_UPDATE_REPO);
}

} // namespace gdx::updater
