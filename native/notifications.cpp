#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <propkey.h>
#include <propvarutil.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.UI.Notifications.h>

#include "devices.hpp"
#include "notifications.hpp"

using namespace winrt::Windows::UI::Notifications;
using namespace winrt::Windows::Data::Xml::Dom;

namespace {
constexpr wchar_t Tag[] = L"battery-summary";
constexpr wchar_t Group[] = L"peripherals";
// A shortcut with a stub activator lets Win32 keep notifications in the center.
constexpr CLSID StubActivator{0x475990df, 0x9f1b, 0x466e, {0x9f, 0x3b, 0x05, 0x85, 0x31, 0xf7, 0xd7, 0xa5}};

void SetString(IPropertyStore* store, REFPROPERTYKEY key, const wchar_t* value) {
    PROPVARIANT property{};
    winrt::check_hresult(InitPropVariantFromString(value, &property));
    HRESULT result = store->SetValue(key, property);
    PropVariantClear(&property);
    winrt::check_hresult(result);
}
}

void RegisterSummaryShortcut() {
    winrt::com_ptr<IShellLinkW> link;
    winrt::check_hresult(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                         IID_PPV_ARGS(link.put())));
    auto executable = AppFolder() / L"BatteryMonitor.exe";
    winrt::check_hresult(link->SetPath(executable.c_str()));
    winrt::check_hresult(link->SetWorkingDirectory(AppFolder().c_str()));
    winrt::check_hresult(link->SetDescription(L"Монитор заряда беспроводных устройств"));
    auto properties = link.as<IPropertyStore>();
    SetString(properties.get(), PKEY_AppUserModel_ID, AppId);
    PROPVARIANT property{};
    winrt::check_hresult(InitPropVariantFromCLSID(StubActivator, &property));
    HRESULT result = properties->SetValue(PKEY_AppUserModel_ToastActivatorCLSID, property);
    PropVariantClear(&property);
    winrt::check_hresult(result);
    winrt::check_hresult(properties->Commit());

    PWSTR programs{};
    winrt::check_hresult(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs));
    std::filesystem::path shortcut(programs);
    CoTaskMemFree(programs);
    shortcut /= L"Battery Monitor.lnk";
    const bool existed = GetFileAttributesW(shortcut.c_str()) != INVALID_FILE_ATTRIBUTES;
    winrt::check_hresult(link.as<IPersistFile>()->Save(shortcut.c_str(), TRUE));
    SHChangeNotify(existed ? SHCNE_UPDATEITEM : SHCNE_CREATE, SHCNF_PATHW | SHCNF_FLUSH,
                   shortcut.c_str(), nullptr);

    // Register the unpackaged AUMID before Start finishes indexing the shortcut.
    const std::wstring registry = std::wstring(L"Software\\Classes\\AppUserModelId\\") + AppId;
    constexpr wchar_t displayName[] = L"Battery Monitor";
    winrt::check_win32(RegSetKeyValueW(HKEY_CURRENT_USER, registry.c_str(), L"DisplayName",
                                      REG_SZ, displayName, sizeof(displayName)));
    wchar_t guid[39]{};
    StringFromGUID2(StubActivator, guid, int(std::size(guid)));
    winrt::check_win32(RegSetKeyValueW(HKEY_CURRENT_USER, registry.c_str(), L"CustomActivator",
                                      REG_SZ, guid, sizeof(guid)));
}

void PublishSummary(const std::vector<std::wstring>& rows) {
    XmlDocument xml;
    xml.LoadXml(L"<toast><visual><binding template='ToastGeneric'>"
                L"<text>Заряд устройств</text><text hint-maxLines='4'/>"
                L"</binding></visual><audio silent='true'/></toast>");
    std::wstring lines;
    for (auto& row : rows) {
        if (!lines.empty()) lines += L'\n';
        lines += row;
    }
    xml.GetElementsByTagName(L"text").Item(1).AppendChild(xml.CreateTextNode(lines));
    ToastNotification toast(xml);
    toast.Tag(Tag);
    toast.Group(Group);
    toast.SuppressPopup(true);
    auto notifier = ToastNotificationManager::CreateToastNotifier(AppId);
    notifier.Show(toast);
}

void RemoveSummary() {
    ToastNotificationManager::History().Remove(Tag, Group, AppId);
}

bool SummaryIsPresent() {
    for (auto toast : ToastNotificationManager::History().GetHistory(AppId))
        if (toast.Tag() == Tag && toast.Group() == Group) return true;
    return false;
}
