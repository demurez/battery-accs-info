#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <cfgmgr32.h>
#include <usbioctl.h>
#include <objbase.h>
#include <sddl.h>
#include <setupapi.h>
#include <shellapi.h>
#include <bcrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <array>

#include <cstdio>
#pragma comment(lib,"bcrypt.lib")
#pragma comment(lib,"wintrust.lib")

#include <filesystem>
#include <string>
#include <vector>

#include "../driver/direwolf_interface.h"
#include "driver_install.hpp"
#include "github_download.hpp"

namespace {
constexpr GUID FilterInterface = DIREWOLF_INTERFACE_GUID;

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
        if (!path.empty()) {
            DeleteFileW((path / L"Xbox360Filter64.sys").c_str());
            RemoveDirectoryW(path.c_str());
        }
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

namespace {
std::filesystem::path SystemFolder() {
    wchar_t system[MAX_PATH]{};
    if (!GetSystemDirectoryW(system, MAX_PATH)) return {};
    return system;
}
constexpr wchar_t ReaderName[]=L"BatteryMonitorFlydigiSample.exe";
constexpr wchar_t TaskName[]=L"Battery Monitor - Flydigi charge";

DWORD RunTool(const std::filesystem::path& exe,const std::wstring& arguments) {
    auto command=L"\""+exe.wstring()+L"\" "+arguments;
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if(!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,
                       nullptr,nullptr,&si,&pi))return GetLastError();
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess,INFINITE);
    DWORD code=1;
    GetExitCodeProcess(pi.hProcess,&code);
    CloseHandle(pi.hProcess);
    return code;
}

constexpr wchar_t VendorFile[]=L"Xbox360Filter64.sys";
constexpr wchar_t VendorPath[]=L"driver-package/flydigi-4.2.3.1/Xbox360Filter64.sys";
bool ApprovedBytes(const std::string& bytes) {
    std::array<UCHAR,32> digest{};
    if(BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,
            reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),static_cast<ULONG>(bytes.size()),
            digest.data(),static_cast<ULONG>(digest.size()))<0)return false;
    char hex[65]{};
    for(size_t i=0;i<digest.size();++i)std::snprintf(hex+2*i,3,"%02x",digest[i]);
    return std::string_view(hex)=="04f6e18f5b08bfee0411e2e75897b7dea74f9baf83148e84dc59516ebab58913";
}

bool TrustedFile(const std::filesystem::path& path) {
    WINTRUST_FILE_INFO file{sizeof(file)};
    file.pcwszFilePath=path.c_str();
    WINTRUST_DATA data{sizeof(data)};
    data.dwUIChoice=WTD_UI_NONE;
    data.dwUnionChoice=WTD_CHOICE_FILE;
    data.pFile=&file;
    GUID policy=WINTRUST_ACTION_GENERIC_VERIFY_V2;
    return WinVerifyTrust(nullptr,&policy,&data)==ERROR_SUCCESS;
}

struct ServiceHandle {
    SC_HANDLE h{};
    ~ServiceHandle(){ if(h)CloseServiceHandle(h); }
};

int EnsureFilter() {
    auto system=SystemFolder();
    if(system.empty())return DriverSetupError;
    const auto dest=system/L"drivers"/VendorFile;
    if(GetFileAttributesW(dest.c_str())==INVALID_FILE_ATTRIBUTES){
        auto bytes=ReadRepositoryFile(VendorPath,65536);
        if(!bytes)return DriverPackageMissing;
        if(!ApprovedBytes(*bytes))return DriverSetupError;
        StagingDirectory stage;
        if(!stage.Create(system))return DriverSetupError;
        const auto file=stage.path/VendorFile;
        HANDLE h=CreateFileW(file.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(h==INVALID_HANDLE_VALUE)return DriverSetupError;
        DWORD written{};
        BOOL ok=WriteFile(h,bytes->data(),static_cast<DWORD>(bytes->size()),&written,nullptr);
        CloseHandle(h);
        if(!ok || written!=bytes->size() || !TrustedFile(file))return DriverSetupError;
        if(!DirewolfPresent())return DriverDeviceMissing;
        if(!CopyFileW(file.c_str(),dest.c_str(),TRUE) && GetLastError()!=ERROR_FILE_EXISTS)return DriverSetupError;
    }
    if(!TrustedFile(dest))return DriverSetupError;
    ServiceHandle scm{OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT|SC_MANAGER_CREATE_SERVICE)};
    if(!scm.h)return DriverSetupError;
    constexpr DWORD rights=SERVICE_START|SERVICE_QUERY_STATUS;
    ServiceHandle service{OpenServiceW(scm.h,VendorFile,rights)};
    if(!service.h && GetLastError()==ERROR_SERVICE_DOES_NOT_EXIST){
        service.h=CreateServiceW(scm.h,VendorFile,VendorFile,rights,SERVICE_KERNEL_DRIVER,SERVICE_DEMAND_START,
            SERVICE_ERROR_NORMAL,L"\\SystemRoot\\System32\\drivers\\Xbox360Filter64.sys",nullptr,nullptr,L"FltMgr\0",nullptr,nullptr);
    }
    if(!service.h)return DriverSetupError;
    SERVICE_STATUS status{};
    if(!QueryServiceStatus(service.h,&status))return DriverSetupError;
    if(status.dwCurrentState==SERVICE_RUNNING)return 0;
    const wchar_t* key=L"SYSTEM\\CurrentControlSet\\Services\\Xbox360Filter64.sys\\Instances";
    const wchar_t instance[]=L"Xbox360Filter64.sys Instance";
    const wchar_t altitude[]=L"396999";
    DWORD flags{};
    const auto child=std::wstring(key)+L"\\"+instance;
    if(RegSetKeyValueW(HKEY_LOCAL_MACHINE,key,L"DefaultInstance",REG_SZ,instance,sizeof(instance)) ||
       RegSetKeyValueW(HKEY_LOCAL_MACHINE,child.c_str(),L"Altitude",REG_SZ,altitude,sizeof(altitude)) ||
       RegSetKeyValueW(HKEY_LOCAL_MACHINE,child.c_str(),L"Flags",REG_DWORD,&flags,sizeof(flags)))return DriverSetupError;
    if(!StartServiceW(service.h,0,nullptr) && GetLastError()!=ERROR_SERVICE_ALREADY_RUNNING)return DriverSetupError;
    return 0;
}
bool CycleDirewolfPort() {
    GUID hubInterface{0xf18a0e88,0xc30c,0x11d0,{0x88,0x15,0x00,0xa0,0xc9,0x06,0xbe,0xd8}};
    wchar_t id[]=DIREWOLF_USB_INSTANCE;
    DEVINST device{},parent{};
    wchar_t parentId[MAX_DEVICE_ID_LEN]{}, driverKey[256]{};
    DWORD port{},type{},size=sizeof(port);
    if(CM_Locate_DevNodeW(&device,id,CM_LOCATE_DEVNODE_NORMAL)!=CR_SUCCESS ||
       CM_Get_Parent(&parent,device,0)!=CR_SUCCESS ||
       CM_Get_Device_IDW(parent,parentId,MAX_DEVICE_ID_LEN,0)!=CR_SUCCESS ||
       CM_Get_DevNode_Registry_PropertyW(device,CM_DRP_ADDRESS,&type,&port,&size,0)!=CR_SUCCESS ||
       type!=REG_DWORD || !port)return false;
    size=sizeof(driverKey);
    if(CM_Get_DevNode_Registry_PropertyW(device,CM_DRP_DRIVER,&type,driverKey,&size,0)!=CR_SUCCESS ||
       type!=REG_SZ || !size || driverKey[255])return false;
    ULONG chars{};
    if(CM_Get_Device_Interface_List_SizeW(&chars,&hubInterface,parentId,
        CM_GET_DEVICE_INTERFACE_LIST_PRESENT)!=CR_SUCCESS || chars<2)return false;
    std::vector<wchar_t> path(chars);
    if(CM_Get_Device_Interface_ListW(&hubInterface,parentId,path.data(),chars,
        CM_GET_DEVICE_INTERFACE_LIST_PRESENT)!=CR_SUCCESS || !path[0])return false;
    HANDLE hub=CreateFileW(path.data(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,
                           OPEN_EXISTING,0,nullptr);
    if(hub==INVALID_HANDLE_VALUE)return false;
    alignas(USB_NODE_CONNECTION_DRIVERKEY_NAME) BYTE buffer[2048]{};
    auto key=reinterpret_cast<PUSB_NODE_CONNECTION_DRIVERKEY_NAME>(buffer);
    key->ConnectionIndex=port;
    DWORD count{};
    // Match the port's current driver key as well as the exact PnP instance.
    bool match=DeviceIoControl(hub,IOCTL_USB_GET_NODE_CONNECTION_DRIVERKEY_NAME,buffer,sizeof(buffer),
                               buffer,sizeof(buffer),&count,nullptr) &&
               count<=sizeof(buffer) && count>=offsetof(USB_NODE_CONNECTION_DRIVERKEY_NAME,DriverKeyName)+sizeof(wchar_t) &&
               key->ActualLength<=count &&
               wcsnlen(key->DriverKeyName,(sizeof(buffer)-offsetof(USB_NODE_CONNECTION_DRIVERKEY_NAME,DriverKeyName))/2)==wcslen(driverKey) &&
               !_wcsicmp(key->DriverKeyName,driverKey);
    USB_CYCLE_PORT_PARAMS cycle{};
    cycle.ConnectionIndex=port;
    bool cycled=match && DirewolfPresent() && DeviceIoControl(hub,IOCTL_USB_HUB_CYCLE_PORT,&cycle,sizeof(cycle),
                                        &cycle,sizeof(cycle),&count,nullptr) && cycle.StatusReturned==0;
    CloseHandle(hub);
    return cycled;
}
int InstallVendorSampler() {
    const auto resource=FindResourceW(nullptr,MAKEINTRESOURCEW(301),RT_RCDATA);
    const DWORD size=resource?SizeofResource(nullptr,resource):0;
    const void* bytes=resource?LockResource(LoadResource(nullptr,resource)):nullptr;
    if(!size || !bytes)return DriverSetupError;
    const auto dest=SystemFolder()/ReaderName;
    StagingDirectory stage;
    if(!stage.Create(SystemFolder()))return DriverSetupError;
    const auto temporary=stage.path/ReaderName;
    HANDLE h=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h==INVALID_HANDLE_VALUE)return DriverSetupError;
    DWORD written{};
    const bool ok=WriteFile(h,bytes,size,&written,nullptr) && written==size;
    CloseHandle(h);
    if(!ok || !MoveFileExW(temporary.c_str(),dest.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){
        DeleteFileW(temporary.c_str());
        return DriverSetupError;
    }
    const std::wstring task=L"/TN \""+std::wstring(TaskName)+L"\"";
    // System32 is protected. Only this embedded, argument-free reader runs elevated.
    if(RunTool(SystemFolder()/L"schtasks.exe",L"/Create /F "+task+L" /SC MINUTE /MO 1 /RU SYSTEM /TR \"\\\""+
                           dest.wstring()+L"\\\"\"")!=0){
        // Keep the installed reader for a retry or an existing scheduled task.
        return DriverSetupError;
    }

    DWORD enabled=1;
    if(RegSetKeyValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\Battery Monitor\\Flydigi",L"AutoPoll",
        REG_DWORD,&enabled,sizeof(enabled))!=ERROR_SUCCESS)return DriverSetupError;
    DWORD result=RunTool(dest,L"");
    if(result!=SamplerPortNotReady)return 0;
    Sleep(500);
    result=RunTool(dest,L"");
    if(result!=SamplerPortNotReady)return 0;
    if(CycleDirewolfPort()){
        for(int attempt=0;attempt<8;++attempt){
            Sleep(1000);
            result=RunTool(dest,L"");
            if(result==0)return 0;
            if(result!=1 && result!=SamplerPortNotReady)return 0;
        }
    }
    return DriverReconnectNeeded;
}
}

bool VendorFilterInstalled(){
    auto path=SystemFolder()/L"drivers"/L"Xbox360Filter64.sys";
    return GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES;
}
bool VendorSamplerInstalled(){
    DWORD enabled{},size=sizeof(enabled);
    if(RegGetValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\Battery Monitor\\Flydigi",L"AutoPoll",RRF_RT_REG_DWORD,nullptr,&enabled,&size)!=ERROR_SUCCESS || !enabled)return false;
    auto path=SystemFolder()/ReaderName;
    return GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES;
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
    const int status=EnsureFilter();
    if(status)return status;
    return InstallVendorSampler();
}
