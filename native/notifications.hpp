#pragma once

#include <string>
#include <vector>

inline constexpr wchar_t AppId[] = L"PeripheralCharge.BatteryMonitor";

void RegisterSummaryShortcut();
void PublishSummary(const std::vector<std::wstring>& rows);
void RemoveSummary();
bool SummaryIsPresent();
