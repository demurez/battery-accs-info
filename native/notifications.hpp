#pragma once

#include <string>
#include <optional>
#include <vector>

inline constexpr wchar_t AppId[] = L"PeripheralCharge.BatteryMonitor";

void RegisterSummaryShortcut();
struct SummaryDevice {
    std::wstring name, kind, value, status;
    std::optional<int> percent;
    bool stale{};
    bool operator==(const SummaryDevice&) const = default;
};
void PublishSummary(const std::vector<SummaryDevice>& devices);
void RemoveSummary();
bool SummaryIsPresent();
