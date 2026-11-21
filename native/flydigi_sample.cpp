#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fltuser.h>
#include <array>
#include <cstddef>
#include <tlhelp32.h>
#include "driver_install.hpp"
#include <cfgmgr32.h>
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "fltLib.lib")
#pragma comment(lib, "advapi32.lib")

// This one-shot reader runs from Program Files. Never retain the vendor port
// between polls, or take it from the official service.
static bool stationRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return true;
    PROCESSENTRY32W p{sizeof(p)};
    bool found = !Process32FirstW(snap, &p);
    if (!found) do {
        if (!_wcsicmp(p.szExeFile, L"SpaceStationService.exe")) { found = true; break; }
    } while (Process32NextW(snap, &p));
    CloseHandle(snap);
    return found;
}

int main() {
    if (stationRunning()) return 1;
    DEVINST instance{};
    wchar_t id[]=L"USB\\VID_045E&PID_028E\\Flydigi_Direwolf_4";
    if(CM_Locate_DevNodeW(&instance,id,CM_LOCATE_DEVNODE_NORMAL)!=CR_SUCCESS) return 1;
    SC_HANDLE manager=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT);
    if(manager){
        SC_HANDLE service=OpenServiceW(manager,L"Xbox360Filter64.sys",SERVICE_START|SERVICE_QUERY_STATUS);
        if(service){
            SERVICE_STATUS status{};
            if(QueryServiceStatus(service,&status) && status.dwCurrentState==SERVICE_STOPPED)
                StartServiceW(service,0,nullptr);
            CloseServiceHandle(service);
        }
        CloseServiceHandle(manager);
    }
    HANDLE port{};
    if (FAILED(FilterConnectCommunicationPort(L"\\XboxFilterPort", 0, nullptr, 0, nullptr, &port))) return 2;
    HANDLE dev = CreateFileW(L"\\\\.\\XboxFilter", GENERIC_ALL, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    DWORD count{}, ready{};
    if (!DeviceIoControl(dev, 0xc07fe008, nullptr, 0, &ready, sizeof(ready), &count, nullptr)) return 3;
    if (!ready) return SamplerPortNotReady;
    std::array<BYTE,15> request{0xa5,0x10,static_cast<BYTE>(GetTickCount64())};
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return 4;
    alignas(FILTER_MESSAGE_HEADER) std::array<BYTE,64> message{};
    const auto deadline = GetTickCount64() + 1500;
    bool sent = false;
    while (GetTickCount64() < deadline) {
        ResetEvent(ov.hEvent);
        HRESULT hr = FilterGetMessage(port, reinterpret_cast<PFILTER_MESSAGE_HEADER>(message.data()), (DWORD)message.size(), &ov);
        if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_IO_PENDING)) return 5;
        if (!sent) {
            if (!DeviceIoControl(dev, 0xc07fe004, request.data(), (DWORD)request.size(), nullptr, 0, &count, nullptr)) return 6;
            sent = true;
        }
        if (WaitForSingleObject(ov.hEvent, 1500) != WAIT_OBJECT_0 ||
            !GetOverlappedResult(port,&ov,&count,FALSE)) {
            CancelIoEx(port,&ov);
            GetOverlappedResult(port,&ov,&count,TRUE);
            return 7;
        }
        if (count < 56 || message[46]!=0xa5 || message[47]!=0x10 || message[48]!=0x84) continue;
        DWORD status=message[55];
        if ((status & 15)>5 || (status >> 4)>1) continue;
        FILETIME ft{};
        GetSystemTimeAsFileTime(&ft);
        DWORD sample[]{ft.dwLowDateTime,ft.dwHighDateTime,status};
        return RegSetKeyValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\Battery Monitor\\Flydigi",L"Sample",
                               REG_BINARY,sample,sizeof(sample)) == ERROR_SUCCESS ? 0 : 8;
    }
    return 9;
}
