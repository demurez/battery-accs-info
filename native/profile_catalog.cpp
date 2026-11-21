#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>

#include "github_download.hpp"
#include "profile_catalog.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <ranges>

using namespace winrt::Windows::Data::Json;

namespace {
constexpr size_t ProfileLimit = 16 * 1024, CatalogLimit = 64 * 1024;

bool Filename(const std::wstring& name) {
    return name.size() > 5 && name.size() <= 80 && name.ends_with(L".json") &&
           name.find_first_not_of(L"abcdefghijklmnopqrstuvwxyz0123456789-_", 0) == name.size() - 5;
}

bool Match(const JsonObject& entry, const ProfileHardware& hardware) {
    if (entry.GetNamedNumber(L"vid") != hardware.vid ||
        entry.GetNamedString(L"usbSerial", L"") != winrt::hstring(hardware.usbSerial)) return false;
    bool matchedPid = false;
    for (auto item : entry.GetNamedArray(L"pids")) matchedPid |= item.GetNumber() == hardware.pid;
    if (!matchedPid) return false;
    if (!hardware.usbSerial.empty()) return true;
    double usage = entry.GetNamedNumber(L"usage", 0);
    if (usage && usage != hardware.usage) return false;
    for (auto item : entry.GetNamedArray(L"pages")) if (item.GetNumber() == hardware.page) return true;
    return false;
}

bool Match(const Profile& p, const ProfileHardware& hardware) {
    return p.vid == hardware.vid && (p.usbSerial == hardware.usbSerial) &&
           std::ranges::find(p.pids, hardware.pid) != p.pids.end() &&
           (!hardware.usbSerial.empty() || ((!p.usage || p.usage == hardware.usage) &&
            std::ranges::find(p.pages, hardware.page) != p.pages.end()));
}

bool SafeRemote(const Profile& profile) {
    if (profile.name.size() > 70 || profile.connection.size() > 70 ||
        profile.pids.empty() || profile.pids.size() > 12) return false;
    if (profile.protocol == L"gamepad")
        return profile.vid == 0x045e && profile.pids == std::vector<unsigned>{0x028e} &&
               profile.usbSerial == L"Flydigi_Direwolf_4";
    if (!profile.usbSerial.empty()) return false;
    if (profile.protocol == L"airoha-ystech")
        return profile.vid == 0x1532 && profile.pids == std::vector<unsigned>{0x054e} &&
               profile.pages == std::vector<unsigned>{0xff00} && profile.usage == 1 &&
               profile.millivoltsOffset == 1 && profile.curve.front() >= 2500 &&
               profile.curve.back() <= 4500;
    constexpr std::array<unsigned char, 17> atkQuery{8, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 73};
    return profile.protocol == L"hid-output" && (profile.vid == 0x373b || profile.vid == 0x3554) &&
           profile.usage == 2 && profile.replyId == 8 && profile.batteryOffset == 6 && profile.powerOffset == 7 &&
           std::ranges::all_of(profile.pages, [](unsigned page) { return page == 0xff02 || page == 0xff04; }) &&
           std::ranges::equal(profile.request, atkQuery);
}

std::filesystem::path CacheFolder() {
    PWSTR folder{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &folder))) return {};
    std::filesystem::path cache(folder);
    CoTaskMemFree(folder);
    return cache / L"Battery Monitor" / L"profile-cache";
}

void SaveProfile(const std::filesystem::path& cache, const std::wstring& name, std::string_view bytes) {
    if (cache.empty() || cache.native().size() >= MAX_PATH - 32) return;
    std::error_code error;
    std::filesystem::create_directories(cache, error);
    if (error) return;
    wchar_t temporary[MAX_PATH]{};
    if (!GetTempFileNameW(cache.c_str(), L"bpm", 0, temporary)) return;
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write(bytes.data(), std::streamsize(bytes.size()));
        file.close();
        if (file) MoveFileExW(temporary, (cache / name).c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    DeleteFileW(temporary);
}

void Merge(std::vector<Profile>& local, Profile profile) {
    auto existing = std::ranges::find(local, profile.file, &Profile::file);
    if (existing == local.end()) local.push_back(std::move(profile));
    else *existing = std::move(profile);
}
}

std::vector<Profile> DiscoverProfiles(std::vector<Profile> local) {
    auto hardware = ScanProfileHardware();
    const auto cache = CacheFolder();
    std::error_code error;
    if (!cache.empty()) {
        for (const auto& entry : std::filesystem::directory_iterator(cache, error)) {
            const auto name = entry.path().filename().wstring();
            const auto length = entry.file_size(error);
            if (!Filename(name) || error || length > ProfileLimit) continue;
            try {
                std::ifstream file(entry.path(), std::ios::binary);
                std::string bytes((std::istreambuf_iterator<char>(file)), {});
                auto profile = ParseProfile(bytes, name);
                if (SafeRemote(profile) &&
                    std::ranges::any_of(hardware, [&](const auto& item) { return Match(profile, item); }))
                    Merge(local, std::move(profile));
            } catch (...) {}
        }
    }
    if (hardware.empty()) return local;

    auto catalog = ReadRepositoryFile(L"catalog.json", CatalogLimit);
    if (!catalog) return local;
    try {
        auto json = JsonObject::Parse(winrt::to_hstring(*catalog));
        if (json.GetNamedNumber(L"version") != 1) return local;
        auto profiles = json.GetNamedArray(L"profiles");
        if (profiles.Size() > 256) return local;
        unsigned downloads{};
        for (auto value : profiles) {
            if (downloads >= 12) break;
            try {
                const auto entry = value.GetObject();
                const std::wstring name(entry.GetNamedString(L"file"));
                if (!Filename(name) ||
                    std::ranges::none_of(hardware, [&](const auto& item) { return Match(entry, item); })) continue;
                ++downloads;
                auto bytes = ReadRepositoryFile(L"devices/" + name, ProfileLimit);
                if (!bytes) continue;
                auto profile = ParseProfile(*bytes, name);
                if (!SafeRemote(profile) || std::ranges::none_of(hardware,
                    [&](const auto& item) { return Match(entry, item) && Match(profile, item); })) continue;
                SaveProfile(cache, name, *bytes);
                Merge(local, std::move(profile));
            } catch (...) {}
        }
    } catch (...) {}
    return local;
}
