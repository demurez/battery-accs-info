#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <cfgmgr32.h>
#include <objbase.h>
#include <sddl.h>
#include <setupapi.h>
#include <shellapi.h>

#include <filesystem>
#include <string>
#include <vector>

#include "../driver/direwolf_interface.h"
#include "driver_install.hpp"
#include "github_download.hpp"

namespace {
constexpr GUID FilterInterface = DIREWOLF_INTERFACE_GUID;

constexpr const wchar_t* Package[]{
    L"PeripheralChargeDirewolf.inf",
    L"PeripheralChargeDirewolf.sys",
    L"PeripheralChargeDirewolf.cat",
};

bool IsAdministrator() {
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID admins{};
    if (!AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &admins)) return false;
    BOOL member{};
    const BOOL ok = CheckTokenMembership(nullptr, admins, &member);
    FreeSid(admins);
    return ok && member;
}

struct StagingDirectory {
    std::filesystem::path path;

    ~StagingDirectory() {
        if (path.empty()) return;
        for (auto entry : Package) DeleteFileW((path / entry).c_str());
        RemoveDirectoryW(path.c_str());
    }

    bool Create(const std::filesystem::path& system) {
        GUID id{};
        wchar_t name[39]{};
        if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, name, int(std::size(name)))) return false;
        auto candidate = system / (std::wstring(L"BatteryMonitor-") + name);
        PSECURITY_DESCRIPTOR descriptor{};
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", SDDL_REVISION_1, &descriptor, nullptr)) return false;
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
        const BOOL created = CreateDirectoryW(candidate.c_str(), &attributes);
        LocalFree(descriptor);
        if (created) path = std::move(candidate);
        return created != FALSE;
    }

    bool Download() const {
        for (auto entry : Package) {
            auto bytes = ReadRepositoryFile(std::wstring(L"driver-package/") + entry, 2 * 1024 * 1024);
            if (!bytes || bytes->empty()) return false;
            HANDLE output = CreateFileW((path / entry).c_str(), GENERIC_WRITE,
                0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (output == INVALID_HANDLE_VALUE) return false;
            DWORD written{};
            const BOOL ok = WriteFile(output, bytes->data(), DWORD(bytes->size()), &written, nullptr);
            CloseHandle(output);
            if (!ok || written != bytes->size()) return false;
        }
        return true;
    }
};
}

bool DirewolfPresent() {
    wchar_t id[] = DIREWOLF_USB_INSTANCE;
    DEVINST device{};
    if (CM_Locate_DevNodeW(&device, id, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return false;

    ULONG type{}, bytes{};
    if (CM_Get_DevNode_Registry_PropertyW(device, CM_DRP_HARDWAREID, &type, nullptr, &bytes, 0)
            != CR_BUFFER_SMALL || bytes < 2 * sizeof(wchar_t) || bytes > 16384) return false;
    std::vector<wchar_t> ids(bytes / sizeof(wchar_t) + 2);
    if (CM_Get_DevNode_Registry_PropertyW(device, CM_DRP_HARDWAREID, &type, ids.data(), &bytes, 0)
            != CR_SUCCESS || type != REG_MULTI_SZ) return false;
    for (const wchar_t* item = ids.data(); *item; item += wcslen(item) + 1) {
        if (_wcsicmp(item, DIREWOLF_USB_HARDWARE_ID) == 0) return true;
    }
    return false;
}

bool DirewolfDriverPresent() {
    GUID guid = FilterInterface;
    ULONG chars{};
    return CM_Get_Device_Interface_List_SizeW(&chars, &guid, nullptr, CM_GET_DEVICE_INTERFACE_LIST_PRESENT)
               == CR_SUCCESS && chars > 1;
}

bool DirewolfPackagePublished() {
    auto marker = ReadRepositoryFile(L"driver-package/version.txt", 64);
    return marker && (*marker == "direwolf-package-v1" || *marker == "direwolf-package-v1\n" ||
                      *marker == "direwolf-package-v1\r\n");
}

HANDLE StartDirewolfDriverInstall(HWND owner, DWORD& error) {
    std::wstring self(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, self.data(), DWORD(self.size()));
    if (!length || length >= self.size()) {
        error = ERROR_FILE_NOT_FOUND;
        return nullptr;
    }
    SHELLEXECUTEINFOW shell{};
    shell.cbSize = sizeof(shell);
    shell.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    shell.hwnd = owner;
    shell.lpVerb = L"runas";
    shell.lpFile = self.c_str();
    shell.lpParameters = L"--install-direwolf-driver";
    shell.nShow = SW_HIDE;
    if (ShellExecuteExW(&shell)) {
        error = ERROR_SUCCESS;
        return shell.hProcess;
    }
    error = GetLastError();
    return nullptr;
}

int InstallDirewolfDriver() {
    if (!DirewolfPresent()) return DriverDeviceMissing;
    if (DirewolfDriverPresent()) return 0;
    if (!IsAdministrator()) return ERROR_ELEVATION_REQUIRED;
    if (!DirewolfPackagePublished()) return DriverPackageMissing;

    wchar_t system[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(system, UINT(std::size(system)));
    if (!length || length >= std::size(system)) return DriverSetupError;
    StagingDirectory staging;
    if (!staging.Create(system)) return DriverSetupError;
    if (!staging.Download()) return DriverPackageMissing;
    const auto inf = staging.path / Package[0];
    wchar_t extension[64]{};
    GetPrivateProfileStringW(L"Version", L"ExtensionId", L"", extension, DWORD(std::size(extension)), inf.c_str());
    if (_wcsicmp(extension, L"{d15c182e-1c2e-43c3-a8ed-1818491bc430}") != 0) return DriverSetupError;
    SP_INF_SIGNER_INFO_W signer{};
    signer.cbSize = sizeof(signer);
    if (!SetupVerifyInfFileW(inf.c_str(), nullptr, &signer)) return DriverSetupError;

    const auto exe = std::filesystem::path(system) / L"pnputil.exe";
    auto command = L"\"" + exe.wstring() + L"\" /add-driver \"" + inf.wstring() + L"\" /install";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!DirewolfPresent()) return DriverDeviceMissing;
    if (DirewolfDriverPresent()) return 0;
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) return DriverSetupError;
    CloseHandle(process.hThread);
    DWORD status = DriverSetupError;
    if (WaitForSingleObject(process.hProcess, INFINITE) == WAIT_OBJECT_0)
        GetExitCodeProcess(process.hProcess, &status);
    CloseHandle(process.hProcess);
    if (!status && !DirewolfDriverPresent()) return ERROR_SUCCESS_REBOOT_REQUIRED;
    return static_cast<int>(status);
}
