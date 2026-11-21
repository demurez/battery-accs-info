#pragma once

#include <windows.h>

inline constexpr int DriverDeviceMissing = 2;
inline constexpr int DriverPackageMissing = 3;
inline constexpr int DriverSetupError = 4;

bool DirewolfPresent();
bool DirewolfDriverPresent();

HANDLE StartDirewolfDriverInstall(HWND owner, DWORD& error);
int InstallDirewolfDriver();
bool VendorFilterInstalled();
bool VendorSamplerInstalled();
inline constexpr int SamplerPortNotReady = 10;
inline constexpr int DriverReconnectNeeded = 11;
