#include "pch.h"
#include "ConfigFile.h"
#include "ConfigInstance.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <thread>

namespace {

const std::wstring kConfigDirW = L"C:\\VeoxDMA\\RobRun";
const std::string kConfigDirA = "C:\\VeoxDMA\\RobRun";

std::string g_activeConfig = "default";
// A failed startup load must not let menu-close or exit autosave overwrite it.
bool g_hasActiveProfile = false;
wchar_t g_activeConfigPathW[1024] = {};

void EnsureDirectoryExists()
{
    std::error_code ec;
    std::filesystem::create_directories(kConfigDirW, ec);
}

// Config names come straight from the UI, so they are treated as untrusted:
// "..\..\Startup\x" must not escape the config directory, and ':' or '*' must
// not reach CreateFile. A name that fails these checks is rejected (returns
// empty) instead of being rewritten into a different config, so an existing
// name like "my config (1)" keeps working.
std::string SanitizeName(std::string name)
{
    if (name.size() >= 5 && name.substr(name.size() - 5) == ".json")
        name = name.substr(0, name.size() - 5);

    if (name.find("..") != std::string::npos) return {};

    for (char c : name)
    {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F) return {};
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' ||
            c == '\\' || c == '|' || c == '?' || c == '*')
            return {};
    }

    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    while (!name.empty() && name.back() == ' ') name.pop_back();
    if (name.size() > 64) name.resize(64);
    return name;
}

void UpdateActivePath()
{
    std::wstring fullPath = kConfigDirW + L"\\" + std::wstring(g_activeConfig.begin(), g_activeConfig.end()) + L".json";
    wcscpy_s(g_activeConfigPathW, fullPath.c_str());
}

} // namespace

namespace ConfigFile {

const wchar_t* Directory()
{
    EnsureDirectoryExists();
    return kConfigDirW.c_str();
}

const wchar_t* Path()
{
    UpdateActivePath();
    return g_activeConfigPathW;
}

std::string GetConfigDirectory()
{
    EnsureDirectoryExists();
    return kConfigDirA;
}

std::string GetActiveConfigName()
{
    return g_activeConfig;
}

void SetActiveConfigName(const std::string& name)
{
    if (name.empty()) return;
    const std::string safe = SanitizeName(name);
    if (safe.empty()) {
        LOG("[Config] rejected config name '%s'\n", name.c_str());
        return;
    }
    g_activeConfig = safe;
    g_hasActiveProfile = true;
    UpdateActivePath();
}

std::vector<std::string> ListConfigs()
{
    EnsureDirectoryExists();
    std::vector<std::string> configs;
    std::error_code ec;

    for (const auto& entry : std::filesystem::directory_iterator(kConfigDirW, ec))
    {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext == ".json")
        {
            configs.push_back(entry.path().stem().string());
        }
    }

    std::sort(configs.begin(), configs.end());
    return configs;
}

static bool WriteProfile(const std::string& name, bool replaceExisting)
{
    EnsureDirectoryExists();
    if (name.empty() && !g_hasActiveProfile) return false;
    const std::string target = name.empty() ? g_activeConfig : SanitizeName(name);
    if (target.empty()) return false;

    const std::filesystem::path fullPath =
        std::filesystem::path(kConfigDirW) / (target + ".json");
    // Serialize first: a throw here must leave the live file untouched.
    std::string text;
    try {
        text = ConfigInstance.ToJson().dump(2);
    }
    catch (const std::exception& e) {
        LOG("[Config] serialize failed: %s\n", e.what());
        return false;
    }

    // Write a temp file, then rename over the target. A crash or power loss
    // mid-save then leaves the previous config intact instead of a truncated
    // file that fails to parse (and silently loads a different config).
    // A separate temp file per writer also protects simultaneous creates
    // from different running instances.
    wchar_t tmpPath[MAX_PATH];
    if (!GetTempFileNameW(kConfigDirW.c_str(), L"cfg", 0, tmpPath))
        return false;
    {
        std::ofstream out(std::filesystem::path(tmpPath), std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
        if (!out.good()) {
            LOG("[Config] write failed for %ls\n", tmpPath);
            DeleteFileW(tmpPath);
            return false;
        }
    }

    // Create must reject an existing name at the instant of the rename.
    const DWORD flags = replaceExisting ? MOVEFILE_REPLACE_EXISTING : 0;
    DWORD error = ERROR_SUCCESS;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (MoveFileExW(tmpPath, fullPath.c_str(), flags)) {
            g_activeConfig = target;
            g_hasActiveProfile = true;
            UpdateActivePath();
            return true;
        }
        error = GetLastError();
        if (error != ERROR_SHARING_VIOLATION || attempt == 2) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    LOG("[Config] write to %ls failed (Windows error %lu)\n", fullPath.c_str(), error);
    DeleteFileW(tmpPath);
    return false;
}

bool Save(const std::string& name)
{
    return WriteProfile(name, true);
}

bool Load(const std::string& name)
{
    EnsureDirectoryExists();
    const std::string target = name.empty() ? g_activeConfig : SanitizeName(name);
    if (target.empty()) return false;

    std::filesystem::path fullPath = std::filesystem::path(kConfigDirW) / (target + ".json");
    std::ifstream in(fullPath, std::ios::binary);
    if (!in) {
        std::error_code ec;
        if (!name.empty() || std::filesystem::exists(fullPath, ec) || ec)
            return false;
        auto list = ListConfigs();
        if (list.empty())
            return CreateConfig("default");
        return Load(list.front());
    }

    try {
        json parsed;
        in >> parsed;
        if (!parsed.is_object()) return false;
        auto candidate = ConfigInstance;
        candidate.FromJson(parsed);
        ConfigInstance = std::move(candidate);
        g_activeConfig = target;
        g_hasActiveProfile = true;
        UpdateActivePath();
    }
    catch (const std::exception& e) {
        LOG("[Config] parse failed for %ls: %s\n", fullPath.c_str(), e.what());
        return false;
    }

    LOG("[Config] loaded %ls\n", fullPath.c_str());
    return true;
}

bool CreateConfig(const std::string& name)
{
    return !name.empty() && WriteProfile(name, false);
}

bool RenameConfig(const std::string& oldName, const std::string& newName)
{
    if (oldName.empty() || newName.empty() || oldName == newName) return false;
    EnsureDirectoryExists();

    const std::string safeOld = SanitizeName(oldName);
    const std::string safeNew = SanitizeName(newName);
    if (safeOld.empty() || safeNew.empty() || safeOld == safeNew) {
        LOG("[Config] rejected rename '%s' -> '%s'\n", oldName.c_str(), newName.c_str());
        return false;
    }

    std::filesystem::path oldPath = std::filesystem::path(kConfigDirW) / (safeOld + ".json");
    std::filesystem::path newPath = std::filesystem::path(kConfigDirW) / (safeNew + ".json");

    std::error_code ec;
    if (!std::filesystem::exists(oldPath, ec)) return false;

    std::filesystem::rename(oldPath, newPath, ec);
    if (ec) {
        LOG("[Config] rename %ls -> %ls failed: %s\n", oldPath.c_str(), newPath.c_str(), ec.message().c_str());
        return false;
    }

    if (g_activeConfig == safeOld) {
        g_activeConfig = safeNew;
        UpdateActivePath();
    }

    LOG("[Config] renamed %ls -> %ls\n", oldPath.c_str(), newPath.c_str());
    return true;
}

bool DeleteConfig(const std::string& name)
{
    const std::string safe = SanitizeName(name);
    if (safe.empty()) {
        LOG("[Config] rejected config name '%s'\n", name.c_str());
        return false;
    }
    EnsureDirectoryExists();

    std::filesystem::path path = std::filesystem::path(kConfigDirW) / (safe + ".json");
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return false;

    std::filesystem::remove(path, ec);
    if (ec) return false;

    if (g_activeConfig != safe) return true;
    g_hasActiveProfile = false;
    const auto list = ListConfigs();
    return list.empty() ? CreateConfig("default") : Load(list.front());
}

} // namespace ConfigFile
