#pragma once

#include <windows.h>

inline constexpr int DriverDeviceMissing = 2;
inline constexpr int DriverPackageMissing = 3;
inline constexpr int DriverSetupError = 4;

bool DirewolfPresent();
bool DirewolfDriverPresent();
bool DirewolfPackagePublished();
HANDLE StartDirewolfDriverInstall(HWND owner, DWORD& error);
int InstallDirewolfDriver();
