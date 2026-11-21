#include "devices.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <Xinput.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Devices.Power.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <span>
#include <stdexcept>

#include "../driver/direwolf_interface.h"
#include "driver_install.hpp"
#include "flydigi_station.hpp"

using namespace winrt::Windows::Data::Json;
using winrt::Windows::Gaming::Input::RawGameController;
using Bytes = std::vector<unsigned char>;

namespace {
struct Handle {
    HANDLE value;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
};

struct HidSet {
    HDEVINFO value;
    ~HidSet() { if (value != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(value); }
};

struct HidInterface {
    std::wstring path;
    unsigned vid{}, pid{};
    HIDP_CAPS caps{};
};

std::vector<HidInterface> EnumerateHid() {
    GUID guid{};
    HidD_GetHidGuid(&guid);
    HidSet set{SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT)};
    std::vector<HidInterface> result;
    if (set.value == INVALID_HANDLE_VALUE) return result;
    for (DWORD i = 0;; ++i) {
        SP_DEVICE_INTERFACE_DATA item{sizeof(item)};
        if (!SetupDiEnumDeviceInterfaces(set.value, nullptr, &guid, i, &item)) break;
        DWORD length{};
        SetupDiGetDeviceInterfaceDetailW(set.value, &item, nullptr, 0, &length, nullptr);
        if (length < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
        std::vector<BYTE> buffer(length);
        auto detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
        detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetailW(set.value, &item, detail, length, nullptr, nullptr)) continue;
        Handle handle{CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_EXISTING, 0, nullptr)};
        if (!handle) continue;
        HIDD_ATTRIBUTES attrs{sizeof(attrs)};
        PHIDP_PREPARSED_DATA data{};
        if (!HidD_GetAttributes(handle.value, &attrs) || !HidD_GetPreparsedData(handle.value, &data)) continue;
        HIDP_CAPS caps{};
        auto status = HidP_GetCaps(data, &caps);
        HidD_FreePreparsedData(data);
        if (status == HIDP_STATUS_SUCCESS)
            result.push_back({detail->DevicePath, attrs.VendorID, attrs.ProductID, caps});
    }
    return result;
}

void TracePacket(const wchar_t* prefix, std::span<const unsigned char> data, bool trace) {
    if (!trace) return;
    std::wcout << prefix << std::hex << std::setfill(L'0');
    for (auto byte : data) std::wcout << L' ' << std::setw(2) << unsigned(byte);
    std::wcout << std::dec << std::setfill(L' ') << L'\n';
}

DWORD Transfer(HANDLE handle, Bytes& data, bool writing, DWORD timeout) {
    Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!event) return 0;
    OVERLAPPED operation{};
    operation.hEvent = event.value;
    DWORD count{};
    BOOL ok = writing ? WriteFile(handle, data.data(), DWORD(data.size()), &count, &operation)
                      : ReadFile(handle, data.data(), DWORD(data.size()), &count, &operation);
    if (ok) return count;
    if (GetLastError() != ERROR_IO_PENDING) return 0;
    if (WaitForSingleObject(event.value, timeout) == WAIT_OBJECT_0)
        return GetOverlappedResult(handle, &operation, &count, FALSE) ? count : 0;
    CancelIoEx(handle, &operation);
    GetOverlappedResult(handle, &operation, &count, TRUE);
    return 0;
}

bool Send(HANDLE handle, const HidInterface& iface, Bytes report, unsigned timeout, bool trace) {
    if (report.size() > iface.caps.OutputReportByteLength) return false;
    report.resize(iface.caps.OutputReportByteLength);
    TracePacket(L"  TX", report, trace);
    return Transfer(handle, report, true, timeout) == report.size();
}

template<typename Extract>
Bytes Listen(HANDLE handle, const HidInterface& iface, unsigned timeout, bool trace, Extract extract) {
    auto deadline = GetTickCount64() + timeout;
    while (true) {
        auto now = GetTickCount64();
        if (now >= deadline) return {};
        Bytes raw(iface.caps.InputReportByteLength);
        if (raw.empty()) return {};
        auto count = Transfer(handle, raw, false, DWORD(deadline - now));
        if (!count) return {};
        raw.resize(count);
        TracePacket(L"  RX", raw, trace);
        if (auto parsed = extract(raw); !parsed.empty()) return parsed;
    }
}

std::optional<Reading> StandardHid(HANDLE handle, const HidInterface& iface,
                                   const Profile& profile, bool trace) {
    if (!Send(handle, iface, profile.request, profile.timeout, trace)) return {};
    if (profile.delay) Sleep(profile.delay);
    auto reply = Listen(handle, iface, profile.timeout, trace, [&](const Bytes& raw) -> Bytes {
        if (raw.size() <= std::max(profile.batteryOffset, profile.powerOffset) ||
            (profile.replyId < 256 && raw[0] != profile.replyId) ||
            raw[profile.batteryOffset] < profile.minimum || raw[profile.batteryOffset] > 100) return {};
        return raw;
    });
    if (reply.empty()) return {};
    const int level=reply[profile.batteryOffset];
    if(!profile.powerOffset)return Reading{level,L"Подключено"};
    if(!reply[profile.powerOffset])return Reading{level,L"Подключено",false,PowerSource::Battery};
    if(level==100)return Reading{level,L"Заряжено · USB",false,PowerSource::External};
    return Reading{level,L"Заряжается · USB",false,PowerSource::LikelyCharging};
}

Bytes RaceReply(const Bytes& raw, unsigned group, unsigned seq) {
    if (raw.size() < 2 || raw[0] != 2 || raw[1] > raw.size() - 2) return {};
    const size_t limit = size_t(raw[1]) + 2;
    for (size_t start = 2; start + 10 <= limit;) {
        if (raw[start] != 0x50 || raw[start + 1] != 0x49) return {};
        size_t end = start + 10 + raw[start + 8] + (raw[start + 9] << 8);
        if (end > limit) return {};
        if (end >= start + 13 && raw[start + 10] == group && raw[start + 11] == (0x80 | seq))
            return Bytes(raw.begin() + start + 12, raw.begin() + end);
        start = end;
    }
    return {};
}

Bytes RaceQuery(HANDLE handle, const HidInterface& iface, unsigned timeout, bool trace,
                unsigned char group, unsigned char seq, std::initializer_list<unsigned char> body) {
    if (iface.caps.OutputReportByteLength != 64 || iface.caps.InputReportByteLength < 2) return {};
    Bytes frame(64);
    std::array<unsigned char, 9> prefix{2, 0x80, static_cast<unsigned char>(4 + body.size()),
                                        0, 0, 0x50, 0x41, group, seq};
    std::copy(prefix.begin(), prefix.end(), frame.begin());
    std::copy(body.begin(), body.end(), frame.begin() + prefix.size());
    unsigned short crc{};
    for (auto byte : frame) {
        crc ^= static_cast<unsigned short>(byte << 8);
        for (int bit = 0; bit < 8; ++bit)
            crc = static_cast<unsigned short>((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    }
    frame[3] = static_cast<unsigned char>(crc);
    frame[4] = static_cast<unsigned char>(crc >> 8);
    for (int attempt = 0;; ++attempt) {
        if (!Send(handle, iface, frame, timeout, trace)) return {};
        Bytes ack(iface.caps.InputReportByteLength);
        ack[0] = 2;
        if (!HidD_GetInputReport(handle, ack.data(), ULONG(ack.size()))) break;
        TracePacket(L"  ACK", std::span(ack).first(2), trace);
        if (ack[1] != 'F') break;
        if (attempt == 2) return {};
        Sleep(50);
    }
    return Listen(handle, iface, timeout, trace, [&](const Bytes& raw) { return RaceReply(raw, group, seq); });
}

std::optional<Reading> Barracuda(HANDLE handle, const HidInterface& iface, const Profile& p, bool trace) {
    HidD_FlushQueue(handle);
    auto seq = static_cast<unsigned char>(GetTickCount64() % 124 + 1);
    auto query = [&](unsigned char group, unsigned char index, std::initializer_list<unsigned char> body) {
        return RaceQuery(handle, iface, p.timeout, trace, group, index, body);
    };
    if (trace) query(0x0E, seq, {1, 1, 0xE3});
    auto remote = query(0x0E, seq + 1, {2, 0xE1, 1});
    Bytes battery;
    try {
        if (remote.empty() || remote[0] == 0) battery = query(6, seq + 2, {1, 0, 0x31});
    } catch (...) {
        query(0x0E, seq + 3, {2, 0xE1, 0});
        throw;
    }
    query(0x0E, seq + 3, {2, 0xE1, 0});
    if (battery.size() <= p.millivoltsOffset + 1 || battery[0] != 0) return {};
    unsigned mv = battery[p.millivoltsOffset] | (battery[p.millivoltsOffset + 1] << 8);
    if (trace) std::wcout << L"  " << mv << L" mV\n";
    if (mv < 2500 || mv > 4500) return {};
    auto pos = std::lower_bound(p.curve.begin(), p.curve.end(), mv);
    int percent = 100;
    if (pos == p.curve.begin()) percent = 0;
    else if (pos != p.curve.end()) {
        const auto low = *(pos - 1);
        percent = static_cast<int>(std::lround((pos - p.curve.begin() - 1) * 10 +
                                                10.0 * (mv - low) / (*pos - low)));
    }
    const bool likelyCharging=p.likelyChargingMv && mv>=p.likelyChargingMv;
    return Reading{percent, likelyCharging?L"Вероятно заряжается":L"Подключено",
                   true, likelyCharging?PowerSource::LikelyCharging:PowerSource::Unknown};
}

std::optional<Reading> XusbBattery(const Profile& p, bool trace) {
    if (p.usbSerial.empty() || p.pids.empty()) return {};
    constexpr GUID xusb{0xec87f1e3, 0xc13b, 0x4100, {0xb5, 0xf7, 0x8b, 0x84, 0xd5, 0x42, 0x60, 0xcb}};
    HidSet set{SetupDiGetClassDevsW(&xusb, nullptr, nullptr, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT)};
    if (set.value == INVALID_HANDLE_VALUE) return {};
    for (DWORD index{};; ++index) {
        SP_DEVICE_INTERFACE_DATA item{sizeof(item)};
        if (!SetupDiEnumDeviceInterfaces(set.value, nullptr, &xusb, index, &item)) break;
        DWORD length{};
        SetupDiGetDeviceInterfaceDetailW(set.value, &item, nullptr, 0, &length, nullptr);
        if (length < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
        std::vector<BYTE> buffer(length);
        auto detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
        detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetailW(set.value, &item, detail, length, nullptr, nullptr)) continue;
        bool match = false;
        for (auto pid : p.pids) {
            wchar_t ids[28]{};
            swprintf_s(ids, L"USB#VID_%04X&PID_%04X#", p.vid, pid);
            std::wstring expected = std::wstring(L"\\\\?\\") + ids + p.usbSerial + L"#";
            if (_wcsnicmp(detail->DevicePath, expected.c_str(), expected.size()) == 0) { match = true; break; }
        }
        if (!match) continue;
        Handle handle{CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_OVERLAPPED, nullptr)};
        if (!handle) {
            if (trace) std::wcout << L"  XUSB open error " << GetLastError() << L'\n';
            return Reading{{}, L"Нет доступа к донглу"};
        }
        Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        if (!event) return Reading{{}, L"Заряд недоступен"};
        OVERLAPPED operation{};
        operation.hEvent = event.value;
        // XUSB 1.2, device zero, gamepad battery. Query only, no device settings changes.
        std::array<BYTE, 4> request{2, 1, 0, BATTERY_DEVTYPE_GAMEPAD};
        std::array<BYTE, 4> reply{};
        DWORD count{};
        BOOL ok = DeviceIoControl(handle.value, 0x8000E018, request.data(), DWORD(request.size()),
                                  reply.data(), DWORD(reply.size()), &count, &operation);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(event.value, p.timeout) == WAIT_OBJECT_0)
                ok = GetOverlappedResult(handle.value, &operation, &count, FALSE);
            else {
                CancelIoEx(handle.value, &operation);
                GetOverlappedResult(handle.value, &operation, &count, TRUE);
            }
        }
        if (trace) {
            if (!ok) std::wcout << L"  XUSB battery error " << GetLastError() << L'\n';
            else TracePacket(L"  XUSB battery", std::span(reply).first(std::min<size_t>(count, reply.size())), true);
        }
        if (ok && count >= reply.size() &&
            (reply[2] == BATTERY_TYPE_ALKALINE || reply[2] == BATTERY_TYPE_NIMH) &&
            reply[3] <= BATTERY_LEVEL_FULL) {
            constexpr int levels[]{0, 25, 60, 100};
            return Reading{levels[reply[3]], L"Уровень по шкале XUSB", true};
        }
        if (ok && count >= reply.size() && reply[2] == BATTERY_TYPE_WIRED)
            return Reading{{}, L"XUSB определяет донгл как проводной"};
        return Reading{{}, L"Донгл не передаёт заряд через XUSB"};
    }
    return {};
}

std::optional<Reading> FilterBattery(bool trace) {
    constexpr GUID guid = DIREWOLF_INTERFACE_GUID;
    HidSet set{SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT)};
    if (set.value == INVALID_HANDLE_VALUE) return {};
    SP_DEVICE_INTERFACE_DATA item{sizeof(item)};
    if (!SetupDiEnumDeviceInterfaces(set.value, nullptr, &guid, 0, &item)) return {};
    DWORD length{};
    SetupDiGetDeviceInterfaceDetailW(set.value, &item, nullptr, 0, &length, nullptr);
    if (length < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) return {};
    std::vector<BYTE> storage(length);
    auto detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());
    detail->cbSize = sizeof(*detail);
    if (!SetupDiGetDeviceInterfaceDetailW(set.value, &item, detail, length, nullptr, nullptr)) return {};
    Handle handle{CreateFileW(detail->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr)};
    if (!handle) return Reading{{}, L"Нет доступа к драйверу Battery Monitor"};
    Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!event) return Reading{{}, L"Запрос заряда недоступен"};

    OVERLAPPED operation{};
    operation.hEvent = event.value;
    DIREWOLF_BATTERY result{};
    DWORD count{};
    BOOL ok = DeviceIoControl(handle.value, DIREWOLF_QUERY_BATTERY, nullptr, 0, &result,
                              sizeof(result), &count, &operation);
    DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    if (!ok && error == ERROR_IO_PENDING) {
        if (WaitForSingleObject(event.value, 1200) == WAIT_OBJECT_0) {
            ok = GetOverlappedResult(handle.value, &operation, &count, FALSE);
            error = ok ? ERROR_SUCCESS : GetLastError();
        } else {
            CancelIoEx(handle.value, &operation);
            GetOverlappedResult(handle.value, &operation, &count, TRUE);
            error = ERROR_TIMEOUT;
        }
    }
    if (trace) std::wcout << L"  Battery Monitor filter: status=" << error
                         << L" bytes=" << count << L'\n';
    if (!ok) return Reading{{}, L"Нет ответа от геймпада"};
    if (count != sizeof(result) || result.Version != DIREWOLF_BATTERY_VERSION ||
        result.Level > 5 || result.Charging > 1 || result.Reserved != 0)
        return Reading{{}, L"Несовместимая версия драйвера"};
    constexpr int levels[]{5, 20, 40, 60, 80, 100};
    return Reading{levels[result.Level], result.Charging ? L"Заряжается" : L"По шкале Flydigi", true,result.Charging?PowerSource::Charging:PowerSource::Unknown};
}

Reading Gamepad(const Profile& profile, bool trace) {
    if (_wcsicmp(profile.usbSerial.c_str(), L"Flydigi_Direwolf_4") == 0) {
        if (DirewolfPresent()) {
            if (auto station = FlydigiStationBattery()) return *station;
            DWORD sample[3]{}, size=sizeof(sample);
            if (RegGetValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\Battery Monitor\\Flydigi",L"Sample",
                             RRF_RT_REG_BINARY,nullptr,sample,&size)==ERROR_SUCCESS && size==sizeof(sample)) {
                FILETIME now{};
                GetSystemTimeAsFileTime(&now);
                const auto current=(static_cast<ULONGLONG>(now.dwHighDateTime)<<32)|now.dwLowDateTime;
                const auto observed=(static_cast<ULONGLONG>(sample[1])<<32)|sample[0];
                const DWORD level=sample[2]&15;
                if (current>=observed && current-observed<900000000ULL && level<=5 && sample[2]>>4<=1) {
                    constexpr int levels[]{5,20,40,60,80,100};
                    return Reading{levels[level],sample[2]>>4 ? L"Заряжается" : L"По шкале Flydigi",true,sample[2]>>4?PowerSource::Charging:PowerSource::Unknown};
                }
            }
        }
        if (auto reading = FilterBattery(trace)) return *reading;
    }
    auto xusb = XusbBattery(profile, trace);
    if (xusb && xusb->percent && !trace) return *xusb;
    if (trace) {
        // windowsapp.lib can bind these names to XInputUap, which uses a different backend.
        auto desktop = LoadLibraryExW(L"xinput1_4.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        auto stateQuery = desktop ? GetProcAddress(desktop, "XInputGetState") : nullptr;
        auto batteryQuery = desktop ? GetProcAddress(desktop, "XInputGetBatteryInformation") : nullptr;
        using StateQuery = DWORD (WINAPI*)(DWORD, XINPUT_STATE*);
        using BatteryQuery = DWORD (WINAPI*)(DWORD, BYTE, XINPUT_BATTERY_INFORMATION*);
        for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
            XINPUT_STATE state{};
            XINPUT_BATTERY_INFORMATION battery{};
            DWORD status = stateQuery ? std::bit_cast<StateQuery>(stateQuery)(i, &state) : ERROR_PROC_NOT_FOUND;
            DWORD batteryStatus = batteryQuery ? std::bit_cast<BatteryQuery>(batteryQuery)(
                i, BATTERY_DEVTYPE_GAMEPAD, &battery) : ERROR_PROC_NOT_FOUND;
            std::wcout << L"  XInput " << i << L": state=" << status << L" battery=" << batteryStatus;
            if (batteryStatus == ERROR_SUCCESS)
                std::wcout << L" type=" << unsigned(battery.BatteryType)
                           << L" level=" << unsigned(battery.BatteryLevel);
            std::wcout << L'\n';
        }
        if (desktop) FreeLibrary(desktop);
    }
    try {
        bool found = false;
        for (auto pad : RawGameController::RawGameControllers()) {
            std::wstring name = pad.DisplayName().c_str(), lowered = name;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(), towlower);
            // 045E:028E belongs to generic XInput devices too, so it cannot identify this pad.
            bool named = lowered.find(L"direwolf 4") != std::wstring::npos ||
                         lowered.find(L"冰原狼 4") != std::wstring::npos;
            bool matchedId = profile.usbSerial.empty() && profile.vid && profile.vid == pad.HardwareVendorId() &&
                             std::ranges::find(profile.pids, pad.HardwareProductId()) != profile.pids.end();
            if (!named && !matchedId) continue;
            found = true;
            auto report = pad.TryGetBatteryReport();
            if (!report) continue;
            auto full = report.FullChargeCapacityInMilliwattHours();
            auto left = report.RemainingCapacityInMilliwattHours();
            if (trace) std::wcout << L"  Windows.Gaming.Input: " << name
                                  << L" remaining=" << (left ? left.Value() : -1)
                                  << L" full=" << (full ? full.Value() : -1) << L'\n';
            if (left && full && left.Value() >= 0 && full.Value() > 0)
                return {std::clamp(int(std::lround(100.0 * left.Value() / full.Value())), 0, 100),
                        L"По данным Windows"};
        }
        if (xusb) return *xusb;
        return {{}, found ? L"Windows не передаёт заряд" : L"Геймпад не обнаружен"};
    } catch (const winrt::hresult_error& error) {
        if (trace) std::wcout << L"  WinRT: " << error.message().c_str() << L'\n';
        return xusb.value_or(Reading{{}, L"Данные геймпада недоступны"});
    }
}

unsigned Number(JsonObject json, const wchar_t* key, unsigned fallback = 0) {
    auto n = json.GetNamedNumber(key, fallback);
    if (n < 0 || n > 65535 || std::floor(n) != n) throw std::runtime_error("Invalid profile number");
    return static_cast<unsigned>(n);
}

std::vector<unsigned> Numbers(JsonObject json, const wchar_t* key) {
    std::vector<unsigned> values;
    for (auto value : json.GetNamedArray(key, JsonArray{})) {
        auto n = value.GetNumber();
        if (n < 0 || n > 65535 || std::floor(n) != n) throw std::runtime_error("Invalid profile array");
        values.push_back(static_cast<unsigned>(n));
    }
    return values;
}
}

std::filesystem::path AppFolder() {
    std::wstring path(32768, L'\0');
    auto count = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    if (!count || count >= path.size()) throw std::runtime_error("Cannot locate application folder");
    path.resize(count);
    return std::filesystem::path(path).parent_path();
}

Profile ParseProfile(std::string_view raw, std::wstring file) {
    if (raw.starts_with("\xEF\xBB\xBF")) raw.remove_prefix(3);
    auto json = JsonObject::Parse(winrt::to_hstring(raw));
    Profile p;
    p.file = std::move(file);
    p.name = json.GetNamedString(L"name").c_str();
    p.kind = json.GetNamedString(L"kind", L"device").c_str();
    p.connection = json.GetNamedString(L"connection").c_str();
    p.protocol = json.GetNamedString(L"protocol").c_str();
    p.usbSerial = json.GetNamedString(L"usbSerial", L"").c_str();
    p.vid = Number(json, L"vid");
    p.usage = Number(json, L"usage");
    p.timeout = Number(json, L"timeoutMs", 800);
    p.delay = Number(json, L"delayMs");
    p.replyId = Number(json, L"replyId", 256);
    p.batteryOffset = Number(json, L"batteryOffset");
    p.powerOffset = Number(json, L"powerOffset");
    p.minimum = Number(json, L"minimum");
    p.millivoltsOffset = Number(json, L"millivoltsOffset", 1);
    p.pids = Numbers(json, L"pids");
    p.pages = Numbers(json, L"pages");
    p.curve = Numbers(json, L"voltageCurve");
    // Older cached profiles for the 2021 receiver predate this optional field.
    const bool barracuda2021=p.protocol==L"airoha-ystech" && p.vid==0x1532 &&
                             p.pids.size()==1 && p.pids.front()==0x054e;
    p.likelyChargingMv = Number(json,L"likelyChargingMv",barracuda2021?4180:0);
    if(p.likelyChargingMv && (p.likelyChargingMv<4000 || p.likelyChargingMv>4500))
        throw std::runtime_error("Invalid charging voltage threshold");
    for (unsigned byte : Numbers(json, L"request")) {
        if (byte > 255) throw std::runtime_error("Invalid report byte");
        p.request.push_back(static_cast<unsigned char>(byte));
    }
    if (p.name.empty() || p.timeout < 1 || p.timeout > 3000 || p.delay > 500)
        throw std::runtime_error("Invalid name or timeout");
    if (p.protocol == L"hid-output" && (p.request.empty() || p.minimum > 100 || p.replyId > 256))
        throw std::runtime_error("Invalid HID report settings");
    if (p.protocol == L"airoha-ystech" &&
        (p.curve.size() != 11 || std::adjacent_find(p.curve.begin(), p.curve.end(),
            std::greater_equal<unsigned>()) != p.curve.end()))
        throw std::runtime_error("Invalid voltage curve");
    if (p.protocol != L"gamepad" && p.protocol != L"hid-output" && p.protocol != L"airoha-ystech")
        throw std::runtime_error("Unknown protocol");
    if (p.protocol != L"gamepad" && (!p.vid || p.pids.empty() || p.pages.empty()))
        throw std::runtime_error("Missing VID, PID or usage page");
    return p;
}

std::vector<Profile> LoadProfiles(const std::filesystem::path& folder) {
    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::directory_iterator(folder))
        if (entry.is_regular_file() && entry.path().extension() == L".json") paths.push_back(entry.path());
    std::ranges::sort(paths);
    std::vector<Profile> profiles;
    for (const auto& path : paths) {
        try {
            std::ifstream file(path, std::ios::binary);
            if (!file) throw std::runtime_error("Cannot open profile");
            std::string raw((std::istreambuf_iterator<char>(file)), {});
            profiles.push_back(ParseProfile(raw, path.filename().wstring()));
        } catch (...) {
            throw std::runtime_error("Invalid device profile: " + winrt::to_string(path.filename().wstring()));
        }
    }
    if (profiles.empty()) throw std::runtime_error("No JSON device profiles found");
    return profiles;
}

std::vector<ProfileHardware> ScanProfileHardware() {
    std::vector<ProfileHardware> found;
    for (const auto& iface : EnumerateHid()) {
        if (iface.caps.UsagePage >= 0xff00 && iface.caps.InputReportByteLength && iface.caps.OutputReportByteLength)
            found.push_back({iface.vid, iface.pid, iface.caps.UsagePage, iface.caps.Usage, {}});
    }
    if (DirewolfPresent()) found.push_back({0x045e, 0x028e, 0, 0, L"Flydigi_Direwolf_4"});
    return found;
}

std::vector<Reading> Poll(const std::vector<Profile>& profiles, bool trace) {
    auto interfaces = EnumerateHid();
    if (trace) {
        for (const auto& iface : interfaces)
            std::wcout << L"HID " << std::hex << iface.vid << L":" << iface.pid
                       << L" usage=" << iface.caps.UsagePage << L":" << iface.caps.Usage << std::dec
                       << L" in=" << iface.caps.InputReportByteLength
                       << L" out=" << iface.caps.OutputReportByteLength << L'\n';
    }
    std::vector<Reading> results;
    for (const auto& p : profiles) {
        if (trace) std::wcout << L"\n" << p.name << L'\n';
        if (p.protocol == L"gamepad") {
            results.push_back(Gamepad(p, trace));
            continue;
        }
        Reading reading{{}, L"Приёмник недоступен"};
        for (auto pid : p.pids) {
            if (reading.percent) break;
            std::vector<const HidInterface*> candidates;
            for (const auto& iface : interfaces) {
                if (iface.vid != p.vid || iface.pid != pid || (p.usage && iface.caps.Usage != p.usage) ||
                    !iface.caps.InputReportByteLength || !iface.caps.OutputReportByteLength ||
                    std::ranges::find(p.pages, iface.caps.UsagePage) == p.pages.end()) continue;
                candidates.push_back(&iface);
            }
            if (p.protocol == L"hid-output") {
                std::stable_partition(candidates.begin(), candidates.end(), [&](const HidInterface* iface) {
                    return iface->caps.OutputReportByteLength == p.request.size();
                });
            }
            for (const auto* candidate : candidates) {
                const auto& iface = *candidate;
                Handle handle{CreateFileW(iface.path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                          FILE_FLAG_OVERLAPPED, nullptr)};
                if (!handle) {
                    if (trace) std::wcout << L"  open error " << GetLastError() << L'\n';
                    reading.status = L"Нет доступа к HID";
                    continue;
                }
                auto value = p.protocol == L"hid-output" ? StandardHid(handle.value, iface, p, trace)
                                                         : Barracuda(handle.value, iface, p, trace);
                if (value) { reading = std::move(*value); break; }
                reading.status = L"Устройство не ответило";
            }
        }
        results.push_back(std::move(reading));
    }
    return results;
}
