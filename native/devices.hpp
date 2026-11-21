#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct Profile {
    std::wstring file, name, kind, connection, protocol, usbSerial;
    unsigned vid{}, usage{}, timeout{800}, delay{}, replyId{}, batteryOffset{}, powerOffset{};
    unsigned minimum{}, millivoltsOffset{1};
    std::vector<unsigned> pids, pages, curve;
    std::vector<unsigned char> request;
};

enum class PowerSource { Unknown, Battery, External, Charging };
struct Reading {
    std::optional<int> percent;
    std::wstring status;
    bool estimate{};
    PowerSource power{PowerSource::Unknown};
};

struct ProfileHardware {
    unsigned vid{}, pid{}, page{}, usage{};
    std::wstring usbSerial;
};

std::vector<Profile> LoadProfiles(const std::filesystem::path& folder);
Profile ParseProfile(std::string_view raw, std::wstring file);
std::vector<ProfileHardware> ScanProfileHardware();
std::filesystem::path AppFolder();
std::vector<Reading> Poll(const std::vector<Profile>& profiles, bool trace = false);
