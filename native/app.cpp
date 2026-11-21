#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <dbt.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <uxtheme.h>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#include <shellapi.h>
#include <shobjidl.h>
#include <winrt/base.h>
#include <winrt/Windows.UI.ViewManagement.h>

#include "devices.hpp"
#include "driver_install.hpp"
#include "notifications.hpp"
#include "profile_catalog.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace {
using namespace std::chrono_literals;
using D2D1::ColorF;
using D2D1::Point2F;
using D2D1::RectF;
using D2D1::RoundedRect;

constexpr wchar_t WindowClass[] = L"PeripheralCharge.Window";
constexpr UINT BatteryReady = WM_APP + 1, TrayEvent = WM_APP + 2, SampleChanged = WM_APP + 3;
constexpr UINT_PTR AnimationTimer = 1, PlugTimer = 2, DriverTimer = 4, FirstSampleTimer = 5, HistoryTimer = 6, ChargingTimer = 7;
constexpr int Open = 101, Refresh = 102, Summary = 103, Quit = 104, Publish = 105, InstallDriver = 106;
constexpr float Width = 536, Top = 119, Row = 116, Bottom = 54, Margin = 26;
constexpr int InstallWidth = 154, InstallHeight = 32;

struct Theme {
    unsigned background, card, border, text, secondary, accent, iconBg, track, amber, button;
};
constexpr Theme Dark{0x11151B, 0x1B222C, 0x344050, 0xF2F6FB, 0xABB9CA, 0x64E0C3, 0x233D45, 0x343E4C, 0xFFB880, 0x273341};
constexpr Theme Light{0xEFF3F6, 0xFFFFFF, 0xD7E0E7, 0x14222C, 0x4E6070, 0x087F72, 0xE1F3F1, 0xE5EBF0, 0x9F4928, 0xE5ECEF};

bool DarkMode() {
    DWORD light = 0, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return !light;
}

bool SummaryEnabled() {
    DWORD enabled = 1, size = sizeof(enabled);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\PeripheralCharge", L"CalendarSummary",
                 RRF_RT_REG_DWORD, nullptr, &enabled, &size);
    return enabled != 0;
}

std::wstring Value(const Reading& reading) {
    if (!reading.percent) return L"—";
    return (reading.estimate ? L"≈" : L"") + std::to_wstring(*reading.percent) + L"%";
}

HICON BatteryIcon() {
    BITMAPINFO bitmap{};
    bitmap.bmiHeader = {sizeof(BITMAPINFOHEADER), 32, -32, 1, 32, BI_RGB};
    void* pixels{};
    HBITMAP color = CreateDIBSection(nullptr, &bitmap, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!color) return nullptr;
    std::array<BYTE, 128> maskBits{};
    HBITMAP mask = CreateBitmap(32, 32, 1, 1, maskBits.data());
    auto data = static_cast<DWORD*>(pixels);
    std::fill_n(data, 32 * 32, 0);
    for (int y = 8; y < 24; ++y) {
        for (int x = 3; x < 29; ++x) {
            bool frame = x < 26 && (x < 5 || x >= 24 || y < 10 || y >= 22);
            bool cells = x >= 8 && x < 18 && y >= 12 && y < 20;
            bool terminal = x >= 26 && y >= 12 && y < 20;
            if (frame || cells || terminal) data[y * 32 + x] = 0xff64e0c3;
        }
    }
    ICONINFO info{TRUE, 0, 0, mask, color};
    HICON icon = mask ? CreateIconIndirect(&info) : nullptr;
    if (mask) DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

struct CycleData {
    std::int64_t full{}, start{}, seconds{};
    DWORD armed{}, wasFull{}, external{};
    bool operator==(const CycleData&) const = default;
};
struct FullCharge {
    CycleData data;
    ULONGLONG previousTick{};
};
constexpr wchar_t CycleKey[]=L"Software\\PeripheralCharge\\BatteryCycle";
FullCharge LoadCharge(const std::wstring& profile) {
    FullCharge history{};
    auto& d=history.data;
    DWORD size=sizeof(d);
    if(RegGetValueW(HKEY_CURRENT_USER,CycleKey,profile.c_str(),RRF_RT_REG_BINARY,nullptr,&d,&size)==ERROR_SUCCESS &&
       size==sizeof(d) && d.start>=0 && d.full>=0 && d.seconds>=0 && d.armed<=1 && d.wasFull<=1 && d.external<=1)
        return history;
    d={};
    std::int64_t old[2]{};
    size=sizeof(old);
    if(RegGetValueW(HKEY_CURRENT_USER,L"Software\\PeripheralCharge\\FullCharge",profile.c_str(),
                    RRF_RT_REG_BINARY,nullptr,old,&size)==ERROR_SUCCESS && size==sizeof(old) && old[0]>0){
        d.full=old[0];
        d.armed=d.wasFull=old[1]==1;
    }
    return history;
}

void TrackCharge(FullCharge& history,const std::wstring& profile,const Reading& reading) {
    auto d=history.data;
    bool external=reading.power==PowerSource::External || ChargingIndicator(reading.power);
    const auto now=std::time(nullptr);
    bool running=false;
    if(external || reading.percent){
        if(reading.percent){
            const bool full=*reading.percent==100;
            if(full && (!d.wasFull || (external && !d.external))){
                d.full=now; d.armed=1; d.start=0; d.seconds=0;
            }
            d.wasFull=full;
            if(!external && d.armed && (!full || reading.power==PowerSource::Battery)){
                d.start=now; d.armed=0;
            }
            running=d.start && !external;
        }
        d.external=external;
    }
    const auto tick=GetTickCount64();
    if(running && history.previousTick && tick-history.previousTick<=150000)
        d.seconds+=(tick-history.previousTick)/1000;
    history.previousTick=running?tick:0;
    if(d==history.data)return;
    history.data=d;
    RegSetKeyValueW(HKEY_CURRENT_USER,CycleKey,profile.c_str(),REG_BINARY,&d,sizeof(d));
}

std::wstring LocalStamp(std::int64_t stamp) {
    std::tm local{};
    std::time_t t=stamp;
    wchar_t date[32]{};
    if(!localtime_s(&local,&t))std::wcsftime(date,std::size(date),L"%d.%m %H:%M",&local);
    return date;
}
std::wstring ChargeLine(const FullCharge& history,bool voltageEstimate) {
    const auto& d=history.data;
    if(!d.start)return d.armed?(voltageEstimate?L"≈100% · ждём снижения уровня":L"Полный заряд · ждём начала разряда"):L"Ожидание полного цикла";
    auto seconds=d.seconds;
    const auto since=GetTickCount64()-history.previousTick;
    if(history.previousTick && since<=150000)seconds+=since/1000;
    const auto minutes=seconds/60;
    std::wstring duration=minutes>=60?std::to_wstring(minutes/60)+L" ч ":L"";
    duration+=std::to_wstring(minutes%60)+L" мин";
    return (voltageEstimate?L"На батарее ≈":L"На батарее ")+duration+L" · с "+LocalStamp(d.start);
}struct Card {
    Reading reading{{}, L"Ожидание данных"};
    std::optional<Reading> last;
    FullCharge charge{};
    float from{}, to{};
};

class Monitor {
public:
    explicit Monitor(std::vector<Profile> profiles)
        : profiles_(std::move(profiles)), cards_(profiles_.size()), enabled_(SummaryEnabled()) {
        for(size_t i=0;i<cards_.size();++i) cards_[i].charge=LoadCharge(profiles_[i].file);
    }

    ~Monitor() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_one();
        if (worker_.joinable()) worker_.join();
        StopSampleWatch();

        if (driverProcess_) CloseHandle(driverProcess_);
        if(hover_ && IsWindow(hover_))DestroyWindow(hover_);

        if (icon_) DestroyIcon(icon_);
    }

    void Bind(HWND window) { window_ = window; }

    void Start(bool trayOnly) {
        taskbarRestart_ = RegisterWindowMessageW(L"TaskbarCreated");
        SetTheme();
        FindDriverRow();
        icon_ = BatteryIcon();
        if (icon_) {
            SendMessageW(window_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon_));
            SendMessageW(window_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon_));
        }
        GUID hidClass{0x4d1e55b2, 0xf16f, 0x11cf, {0x88, 0xcb, 0, 0x11, 0x11, 0, 0, 0x30}};
        DEV_BROADCAST_DEVICEINTERFACE_W filter{};
        filter.dbcc_size = sizeof(filter);
        filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
        filter.dbcc_classguid = hidClass;
        deviceChanges_ = RegisterDeviceNotificationW(window_, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
        ArmSampleWatch();
        AddTray();
        worker_ = std::thread([this, local = profiles_] { Work(local); });
        if (!trayOnly || !trayAdded_) ShowWindow(window_, SW_SHOW);
    }

    float Height() const { return Top + Row * VisibleRows() + Bottom; }
    LRESULT Message(UINT message, WPARAM w, LPARAM l) {
        if (message && message == taskbarRestart_) { AddTray(); return 0; }
        switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(window_, &paint);
            Paint();
            EndPaint(window_, &paint);
            return 0;
        }
        case WM_SHOWWINDOW: UpdateHistoryTimer(w != 0); break;
        case WM_ERASEBKGND: return 1;
        case WM_SIZE:
            if (render_ && w != SIZE_MINIMIZED) render_->Resize(D2D1::SizeU(LOWORD(l), HIWORD(l)));
            return 0;
        case WM_DPICHANGED: {
            auto rect = reinterpret_cast<RECT*>(l);
            SetWindowPos(window_, nullptr, rect->left, rect->top, rect->right - rect->left,
                         rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
            if (render_) render_->SetDpi(float(LOWORD(w)), float(HIWORD(w)));
            PositionDriverButton();
            return 0;
        }
        case WM_SETTINGCHANGE:
        case WM_DWMCOLORIZATIONCOLORCHANGED:
        case WM_THEMECHANGED: SetTheme(); return 0;
        case WM_MOUSEWHEEL: Scroll(GET_WHEEL_DELTA_WPARAM(w) > 0 ? -1 : 1); return 0;
        case WM_LBUTTONUP:
            if (IsRefreshPoint(GET_X_LPARAM(l), GET_Y_LPARAM(l))) Request();
            return 0;
        case WM_SETCURSOR: {
            POINT point{};
            if (LOWORD(l) == HTCLIENT && GetCursorPos(&point) && ScreenToClient(window_, &point) &&
                IsRefreshPoint(point.x, point.y)) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        }
        case WM_KEYDOWN:
            if (w == VK_F5) Request();
            if (w == VK_ESCAPE) PostMessageW(window_, WM_CLOSE, 0, 0);
            if (w == VK_DOWN || w == VK_UP) Scroll(w == VK_DOWN ? 1 : -1);
            break;
        case WM_COMMAND:
            switch (LOWORD(w)) {
            case Open: Reveal(); break;
            case Refresh: Request(); break;
            case Summary: ToggleSummary(); break;
            case Publish: if (enabled_) SendSummary(true); break;
            case Quit: DestroyWindow(window_); break;
            case InstallDriver:
                if (HIWORD(w) == BN_CLICKED && reinterpret_cast<HWND>(l) == driverButton_)
                    BeginDriverInstall();
                break;
            }
            return 0;
        case WM_DRAWITEM:
            if (w == InstallDriver) {
                DrawActionButton(*reinterpret_cast<const DRAWITEMSTRUCT*>(l));
                return TRUE;
            }
            break;
        case WM_TIMER:
            if(w==ChargingTimer){
                if(IsWindowVisible(window_) && !IsIconic(window_))InvalidateRect(window_,nullptr,FALSE);
                if(hover_ && IsWindowVisible(hover_))InvalidateRect(hover_,nullptr,FALSE);
            } else if (w == HistoryTimer) {
                if(IsWindowVisible(window_)) InvalidateRect(window_,nullptr,FALSE);
                if(hover_ && IsWindowVisible(hover_)) ShowFlyout();
            } else if (w == AnimationTimer) {
                if (!IsWindowVisible(window_) || IsIconic(window_) || Motion() >= 1)
                    KillTimer(window_, AnimationTimer);
                if (IsWindowVisible(window_) && !IsIconic(window_)) InvalidateRect(window_, nullptr, FALSE);
            } else if (w == PlugTimer) {
                KillTimer(window_, PlugTimer);
                UpdateDriverOffer();
                Request(true);
            } else if (w == FirstSampleTimer) {
                KillTimer(window_, FirstSampleTimer);
                if (firstSampleNotice_) {
                    firstSampleNotice_=L"Включите геймпад или переподключите донгл";
                    Request(true);
                }
            } else if (w == DriverTimer) {
                FinishDriverInstall();
            }
            return 0;
        case WM_DEVICECHANGE:
            if (w == DBT_DEVICEARRIVAL || w == DBT_DEVICEREMOVECOMPLETE)
                SetTimer(window_, PlugTimer, 1000, nullptr);
            return TRUE;
        case WM_POWERBROADCAST:
            if (w == PBT_APMRESUMEAUTOMATIC || w == PBT_APMRESUMESUSPEND)
                SetTimer(window_, PlugTimer, 2000, nullptr);
            return TRUE;
        case SampleChanged:
            ArmSampleWatch();
            Request(true);
            return 0;
        case BatteryReady: {
            std::vector<Reading> result;
            std::vector<Profile> updated;
            {
                std::lock_guard lock(mutex_);
                result.swap(pending_);
                updated.swap(pendingProfiles_);
            }
            if (!updated.empty()) ApplyProfiles(std::move(updated));
            if (!result.empty()) Display(std::move(result));
            return 0;
        }
        case TrayEvent:


            if (LOWORD(l) == NIN_SELECT || LOWORD(l) == NIN_KEYSELECT || LOWORD(l) == WM_LBUTTONUP)
                ToggleFlyout(LOWORD(l)==NIN_KEYSELECT);
            else if (LOWORD(l) == WM_CONTEXTMENU || LOWORD(l) == WM_RBUTTONUP) TrayMenu();
            return 0;
        case WM_CLOSE:
            if (!trayAdded_) DestroyWindow(window_);
            else {
                ShowWindow(window_, SW_HIDE);
                KillTimer(window_, AnimationTimer);
                brush_ = nullptr;
                render_ = nullptr;
            }
            return 0;
        case WM_DESTROY: {
            if (deviceChanges_) UnregisterDeviceNotification(deviceChanges_);
            auto tray = TrayData();
            Shell_NotifyIconW(NIM_DELETE, &tray);
            PostQuitMessage(0);
            return 0;
        }
        }
        return DefWindowProcW(window_, message, w, l);
    }

private:
    std::vector<Profile> profiles_;
    std::vector<Card> cards_;
    HWND window_{};
    HICON icon_{};
    HDEVNOTIFY deviceChanges_{};
    UINT taskbarRestart_{};
    bool enabled_{}, registered_{}, trayAdded_{};
    bool refreshing_{true}, summaryFailed_{};
    HWND driverButton_{};
    HANDLE driverProcess_{};
    int driverRow_{-1};
    bool driverOffered_{}, driverReboot_{};

    const wchar_t* driverNotice_{};
    std::vector<SummaryDevice> lastSummary_;
    std::wstring lastTime_;
    Theme theme_ = Dark;
    int scroll_{};
    ULONGLONG animation_{}, lastPoll_{};
    winrt::com_ptr<ID2D1Factory> direct2d_;
    winrt::com_ptr<IDWriteFactory> directWrite_;
    winrt::com_ptr<ID2D1HwndRenderTarget> render_;
    winrt::com_ptr<ID2D1SolidColorBrush> brush_;
    winrt::com_ptr<IDWriteTextFormat> title_, name_, header_, small_, percent_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    bool stopping_{}, requested_{};
    std::vector<Reading> pending_;
    std::vector<Profile> pendingProfiles_;

    int VisibleRows() const { return static_cast<int>(std::min<size_t>(5, cards_.size())); }

    void Work(std::vector<Profile> active) {
        winrt::init_apartment();
        std::unique_lock lock(mutex_);
        auto interval = 120s;
        bool discover = true;
        do {
            requested_ = false;
            lock.unlock();
            // Publish the bundled readings first. Network access never blocks the initial local scan.
            if (discover) {
                std::vector<Reading> local;
                try { local = Poll(active); }
                catch (...) { local.assign(active.size(), {{}, L"Ошибка чтения устройства"}); }
                lock.lock();
                if (stopping_) break;
                pending_ = std::move(local);
                PostMessageW(window_, BatteryReady, 0, 0);
                lock.unlock();
                try { active = DiscoverProfiles(active); } catch (...) {}
            }
            std::vector<Reading> readings;
            try { readings = Poll(active); }
            catch (...) { readings.assign(active.size(), {{}, L"Ошибка чтения устройства"}); }
            bool any = std::ranges::any_of(readings, [](const Reading& r) { return r.percent.has_value(); });
            interval = any ? 120s : 60s;
            lock.lock();
            if (stopping_) break;
            pending_ = std::move(readings);
            if (discover) { pendingProfiles_ = active; discover = false; }
            PostMessageW(window_, BatteryReady, 0, 0);
            wake_.wait_for(lock, interval, [this] { return requested_ || stopping_; });
        } while (!stopping_);
        lock.unlock();
        winrt::uninit_apartment();
    }

    void ApplyProfiles(std::vector<Profile> updated) {
        auto oldHeight = Height();
        std::vector<Card> cards(updated.size());
        for (size_t i = 0; i < updated.size(); ++i) {
            const auto previous = std::ranges::find(profiles_, updated[i].file, &Profile::file);
            if (previous != profiles_.end()) cards[i] = std::move(cards_[previous - profiles_.begin()]);
            else cards[i].charge=LoadCharge(updated[i].file);
        }
        profiles_ = std::move(updated);
        cards_ = std::move(cards);
        scroll_ = std::min(scroll_, static_cast<int>(cards_.size()) - VisibleRows());
        if (Height() != oldHeight) {
            RECT window{};
            GetWindowRect(window_, &window);
            const int extra = MulDiv(int(Height() - oldHeight), int(GetDpiForWindow(window_)), 96);
            SetWindowPos(window_, nullptr, 0, 0, window.right - window.left, window.bottom - window.top + extra,
                         SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER);
        }
        FindDriverRow();
    }

    void Request(bool enqueue = false) {
        if (refreshing_ && !enqueue) return;
        refreshing_ = true;
        if(flyoutOpen_ && miniRefresh_){SetWindowTextW(miniRefresh_,L"Проверка…"); EnableWindow(miniRefresh_,FALSE);}
        {
            std::lock_guard lock(mutex_);
            requested_ = true;
        }
        wake_.notify_one();
        InvalidateRect(window_, nullptr, FALSE);
    }

    void Display(std::vector<Reading> values) {
        float previous = Motion();
        bool animate = false;
        for (size_t i = 0; i < cards_.size(); ++i) {
            auto& card = cards_[i];
            card.from += (card.to - card.from) * previous;
            card.reading=std::move(values[i]);
            TrackCharge(card.charge,profiles_[i].file,card.reading);
            if (int(i)==driverRow_ && card.reading.percent && firstSampleNotice_) {
                firstSampleNotice_=nullptr;
                KillTimer(window_, FirstSampleTimer);
            }
            if (card.reading.percent) card.last = card.reading;
            card.to = card.last && card.last->percent ? float(*card.last->percent) : 0;
            animate |= card.from != card.to;
        }
        animation_ = GetTickCount64();
        BOOL systemAnimation = TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &systemAnimation, 0);
        if (!systemAnimation || !IsWindowVisible(window_) || IsIconic(window_) || !animate) animation_ = 0;
        else SetTimer(window_, AnimationTimer, 16, nullptr);
        refreshing_ = false;
        lastPoll_ = GetTickCount64();
        SYSTEMTIME now{};
        GetLocalTime(&now);
        wchar_t time[16]{};
        GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &now, L"HH:mm", time, int(std::size(time)));
        lastTime_ = time;
        UpdateHistoryTimer(IsWindowVisible(window_)!=FALSE);
        UpdateTray();
        if (enabled_) SendSummary(false);
        UpdateDriverOffer();
        InvalidateRect(window_, nullptr, FALSE);
    }

    void PositionDriverButton() {
        if (!driverButton_) return;
        const int index = driverRow_ - scroll_;
        if (!driverOffered_ || index < 0 || index >= VisibleRows()) {
            ShowWindow(driverButton_, SW_HIDE);
            return;
        }
        const int dpi = int(GetDpiForWindow(window_));
        const int x = int(Width - Margin - 15) - InstallWidth;
        const int y = int(Top + Row * index) + 22;
        SetWindowPos(driverButton_, nullptr, MulDiv(x, dpi, 96), MulDiv(y, dpi, 96),
            MulDiv(InstallWidth, dpi, 96), MulDiv(InstallHeight, dpi, 96),
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    void FindDriverRow() {
        const auto profile = std::ranges::find_if(profiles_, [](const Profile& item) {
            return item.protocol == L"gamepad" && _wcsicmp(item.usbSerial.c_str(), L"Flydigi_Direwolf_4") == 0;
        });
        driverRow_ = profile == profiles_.end() ? -1 : static_cast<int>(profile - profiles_.begin());
        if (driverRow_ >= 0 && !driverButton_) {
            driverButton_ = CreateWindowExW(0, L"BUTTON", L"Установить драйвер",
                WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, window_,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(InstallDriver)), nullptr, nullptr);
        }
        UpdateDriverOffer();
    }

    void UpdateDriverOffer() {
        if (!driverButton_) return;
        const bool matched = driverRow_ >= 0 && DirewolfPresent();
        driverOffered_ = matched && !ChargingIndicator(cards_[driverRow_].reading.power) && !DirewolfDriverPresent() &&
                         (VendorFilterInstalled() ? !VendorSamplerInstalled() && cards_[driverRow_].reading.status != L"По данным Space Station" : !cards_[driverRow_].reading.percent.has_value());
        if (!matched && !driverProcess_) {
            driverNotice_ = nullptr;
            driverReboot_ = false;
        }
        EnableWindow(driverButton_, driverOffered_ && !driverProcess_ && !driverReboot_);
        SetWindowTextW(driverButton_, driverProcess_ ? L"Установка…" :
                       driverReboot_ ? L"Перезапустите ПК" : VendorFilterInstalled() ? L"Включить автоопрос" : L"Установить драйвер");
        PositionDriverButton();
        InvalidateRect(window_, nullptr, FALSE);
    }

    void BeginDriverInstall() {
        UpdateDriverOffer();
        if (!driverOffered_ || driverProcess_ || driverReboot_) return;
        driverNotice_ = nullptr;
        const wchar_t* text=VendorFilterInstalled()
            ? L"Включить опрос заряда раз в минуту с помощью задачи Windows от имени SYSTEM?"
            : L"Скачать подписанный фильтр Flydigi из репозитория Battery Monitor, зарегистрировать его и включить опрос заряда раз в минуту с помощью задачи Windows от имени SYSTEM?";
        if(MessageBoxW(window_,text,L"Заряд Direwolf 4",MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)==IDYES)
            LaunchDriverInstall();
    }
    void LaunchDriverInstall() {
        DWORD error{};
        driverProcess_ = StartDirewolfDriverInstall(window_, error);
        if (driverProcess_) {
            SetTimer(window_, DriverTimer, 250, nullptr);
        } else {
            driverNotice_ = error == ERROR_CANCELLED ? L"Установка отменена" : L"Не удалось запустить установку";
        }
        UpdateDriverOffer();
    }

    HKEY sampleKey_{};
    HANDLE sampleEvent_{}, sampleWait_{};
    const wchar_t* firstSampleNotice_{};

    void StopSampleWatch() {
        if(sampleWait_) UnregisterWaitEx(sampleWait_,INVALID_HANDLE_VALUE);
        if(sampleKey_) RegCloseKey(sampleKey_);
        if(sampleEvent_) CloseHandle(sampleEvent_);
        sampleWait_=nullptr;
        sampleKey_=nullptr;
        sampleEvent_=nullptr;
    }

    static void CALLBACK NotifySample(void* context,BOOLEAN) {
        PostMessageW(static_cast<HWND>(context),SampleChanged,0,0);
    }

    void ArmSampleWatch() {
        StopSampleWatch();
        if(RegOpenKeyExW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\Battery Monitor\\Flydigi",0,
                         KEY_NOTIFY,&sampleKey_)!=ERROR_SUCCESS)return;
        sampleEvent_=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if(!sampleEvent_ ||
           RegNotifyChangeKeyValue(sampleKey_,FALSE,REG_NOTIFY_CHANGE_LAST_SET,sampleEvent_,TRUE)!=ERROR_SUCCESS ||
           !RegisterWaitForSingleObject(&sampleWait_,sampleEvent_,NotifySample,window_,INFINITE,WT_EXECUTEONLYONCE))
            StopSampleWatch();
    }
    void FinishDriverInstall() {
        if (!driverProcess_ || WaitForSingleObject(driverProcess_, 0) == WAIT_TIMEOUT) return;
        KillTimer(window_, DriverTimer);
        DWORD status = DriverSetupError;
        GetExitCodeProcess(driverProcess_, &status);
        CloseHandle(driverProcess_);
        driverProcess_ = nullptr;
        driverReboot_ = status == ERROR_SUCCESS_REBOOT_REQUIRED || status == ERROR_SUCCESS_RESTART_REQUIRED;
        if(status==0 || status==DriverReconnectNeeded) {
            firstSampleNotice_=status==DriverReconnectNeeded
                ? L"Выньте и снова вставьте USB-донгл" : L"Ожидание заряда от геймпада";
            if(status==0)SetTimer(window_,FirstSampleTimer,12000,nullptr);
            ArmSampleWatch();
        }
        if (driverReboot_) driverNotice_ = L"Перезапуск активирует драйвер";
        else if (status == DriverDeviceMissing) driverNotice_ = L"Донгл отключён";
        else if (status == DriverPackageMissing) driverNotice_ = L"Пакет пока недоступен";
        else if (status != 0 && status != DriverReconnectNeeded) driverNotice_ = L"Драйвер установить не удалось";
        else driverNotice_ = nullptr;
        UpdateDriverOffer();
        Request(true);
    }

    void DrawActionButton(const DRAWITEMSTRUCT& item) const {
        auto rgb = [](unsigned color) {
            return RGB((color >> 16) & 255, (color >> 8) & 255, color & 255);
        };
        const int dpi = int(GetDpiForWindow(window_));
        const auto rect = item.rcItem;
        HBRUSH outer = CreateSolidBrush(rgb(theme_.card));
        FillRect(item.hDC, &rect, outer);
        DeleteObject(outer);
        const bool disabled = (item.itemState & ODS_DISABLED) != 0;
        HBRUSH fill = CreateSolidBrush(rgb(theme_.button));
        HPEN border = CreatePen(PS_SOLID, 1, rgb(item.itemState & ODS_SELECTED ? theme_.accent : theme_.border));
        HGDIOBJ oldBrush = SelectObject(item.hDC, fill), oldPen = SelectObject(item.hDC, border);
        RoundRect(item.hDC, rect.left, rect.top, rect.right, rect.bottom, MulDiv(8, dpi, 96), MulDiv(8, dpi, 96));
        SelectObject(item.hDC, oldPen);
        SelectObject(item.hDC, oldBrush);
        DeleteObject(fill);
        DeleteObject(border);
        HFONT font = CreateFontW(-MulDiv(13, dpi, 96), 0, 0, 0, FW_SEMIBOLD,
                                 FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HGDIOBJ previousFont = font ? SelectObject(item.hDC, font) : nullptr;
        SetBkMode(item.hDC, TRANSPARENT);
        SetTextColor(item.hDC, rgb(disabled ? theme_.secondary : theme_.accent));
        wchar_t label[64]{};
        GetWindowTextW(item.hwndItem, label, int(std::size(label)));
        RECT text = rect;
        DrawTextW(item.hDC, label, -1, &text, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (font) { SelectObject(item.hDC, previousFont); DeleteObject(font); }
        if (item.itemState & ODS_FOCUS) {
            RECT inset = rect;
            InflateRect(&inset, -3, -3);
            DrawFocusRect(item.hDC, &inset);
        }
    }

    std::wstring CardValue(size_t index) const {
        auto& card = cards_[index];
        return Value(card.last ? *card.last : card.reading);
    }

    void SendSummary(bool force) {
        if (!force && lastSummary_.empty() &&
            std::ranges::none_of(cards_, [](const Card& card) { return card.last.has_value(); })) return;
        std::vector<SummaryDevice> rows;
        for(size_t i=0;i<cards_.size();++i){
            const auto& card=cards_[i];
            rows.push_back({profiles_[i].name, profiles_[i].kind, CardValue(i), card.reading.status,
                            card.last?card.last->percent:card.reading.percent,
                            card.last.has_value() && !card.reading.percent});
        }
        if (!force && rows == lastSummary_) return;
        try {
            if (!registered_) { RegisterSummaryShortcut(); registered_ = true; }
            PublishSummary(rows);
            lastSummary_ = std::move(rows);
            summaryFailed_ = false;
        } catch (const winrt::hresult_error& error) {
            OutputDebugStringW((L"Battery Monitor notification: " + std::wstring(error.message()) + L"\n").c_str());
            summaryFailed_ = true;
        }
        InvalidateRect(window_, nullptr, FALSE);
    }

    void ToggleSummary() {
        enabled_ = !enabled_;
        DWORD value = enabled_;
        RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\PeripheralCharge", L"CalendarSummary",
                        REG_DWORD, &value, sizeof(value));
        if (enabled_) SendSummary(true);
        else {
            try { RemoveSummary(); } catch (const winrt::hresult_error&) {}
            lastSummary_.clear();
            summaryFailed_ = false;
        }
        InvalidateRect(window_, nullptr, FALSE);
    }

    NOTIFYICONDATAW TrayData() const {
        NOTIFYICONDATAW tray{};
        tray.cbSize = sizeof(tray);
        tray.hWnd = window_;
        tray.uID = 1;
        return tray;
    }

    void UpdateTray(bool add = false) {
        auto tray = TrayData();
        tray.uFlags = NIF_TIP | NIF_SHOWTIP | NIF_ICON | NIF_MESSAGE;
        tray.uCallbackMessage = TrayEvent;
        tray.hIcon = icon_ ? icon_ : LoadIconW(nullptr, IDI_APPLICATION);
        std::wstring tip;
        for(size_t i=0;i<cards_.size();++i){
            // Show the current reading only. A dash means there is no current sample.
            auto line=profiles_[i].name.substr(0,28)+L"  "+Value(cards_[i].reading);
            if(tip.size()+line.size()+1>=std::size(tray.szTip)){
                if(tip.size()+2<std::size(tray.szTip))tip+=L"\n…";
                break;
            }
            if(!tip.empty())tip+=L'\n';
            tip+=line;
        }
        wcscpy_s(tray.szTip,tip.empty()?L"Battery Monitor":tip.c_str());
        if (hover_ && IsWindowVisible(hover_)) ShowFlyout();
        if (add) {
            trayAdded_ = Shell_NotifyIconW(NIM_ADD, &tray) != FALSE;
            if (trayAdded_) { tray.uVersion = NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION, &tray); }
        } else if (!Shell_NotifyIconW(NIM_MODIFY, &tray)) AddTray();
    }

    bool chargeMotion_{};
    winrt::com_ptr<ID2D1PathGeometry> lightning_;

    void DrawCharging(ID2D1RenderTarget* target,ID2D1SolidColorBrush* brush,
                      D2D1_POINT_2F icon,D2D1_RECT_F track,unsigned accent,bool light){
        if(!lightning_){
            winrt::com_ptr<ID2D1PathGeometry> shape;
            winrt::com_ptr<ID2D1GeometrySink> sink;
            if(FAILED(direct2d_->CreatePathGeometry(shape.put())) || FAILED(shape->Open(sink.put())))return;
            sink->BeginFigure(Point2F(2,-10),D2D1_FIGURE_BEGIN_FILLED);
            const D2D1_POINT_2F edges[]{Point2F(-7,2),Point2F(-1,2),Point2F(-3,10),Point2F(7,-3),Point2F(1,-3)};
            sink->AddLines(edges,UINT32(std::size(edges)));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            if(FAILED(sink->Close()))return;
            lightning_=std::move(shape);
        }
        D2D1_MATRIX_3X2_F before;
        target->GetTransform(&before);
        target->SetTransform(D2D1::Matrix3x2F::Translation(icon.x,icon.y)*before);
        brush->SetColor(ColorF(accent));
        target->FillGeometry(lightning_.get(),brush);
        target->SetTransform(before);
        if(!chargeMotion_ || track.right<=track.left)return;
        const float width=track.right-track.left;
        const float phase=float(GetTickCount64()%1500)/1500.f;
        const float trail=std::min(92.f,width*.55f);
        const float head=track.left+phase*(width+trail);
        target->PushAxisAlignedClip(track,D2D1_ANTIALIAS_MODE_ALIASED);
        for(int i=0;i<18;++i){
            const float u=float(i)/18;
            brush->SetColor(ColorF(light?0x14334a:0xffffff,0.1f+0.76f*u*u));
            const float x=head-trail+u*trail;
            target->FillRectangle(RectF(x,track.top,x+trail/18+.25f,track.bottom),brush);
        }
        target->PopAxisAlignedClip();
    }    HWND hover_{};
    winrt::com_ptr<ID2D1HwndRenderTarget> hoverTarget_;
    int hoverOffset_{}, hoverLimit_{};
    HWND miniRefresh_{}, miniOpen_{};
    bool flyoutOpen_{};
    ULONGLONG lastTrayClick_{}, lastFlyoutDismiss_{};

    void CloseFlyout() {
        flyoutOpen_=false;
        if(hover_)ShowWindow(hover_,SW_HIDE);
        UpdateHistoryTimer(IsWindowVisible(window_)!=FALSE);
    }

    void ToggleFlyout(bool keyboard) {
        auto now=GetTickCount64();
        if(now-lastTrayClick_<150)return;
        lastTrayClick_=now;
        if(flyoutOpen_){CloseFlyout();return;}
        if(now-lastFlyoutDismiss_<200)return;
        flyoutOpen_=true;
        ShowFlyout();
        if(!hover_){flyoutOpen_=false;return;}
        SetForegroundWindow(hover_);
        SetFocus(keyboard?miniRefresh_:hover_);
    }

    public:
    bool HandleMiniKey(const MSG& message) {
        if(!flyoutOpen_ || (message.hwnd!=hover_ && !IsChild(hover_,message.hwnd)))return false;
        if(message.message==WM_KEYDOWN && message.wParam==VK_ESCAPE){CloseFlyout();return true;}
        if(message.message==WM_KEYDOWN && message.wParam==VK_TAB){
            SetFocus(GetFocus()==miniRefresh_?miniOpen_:miniRefresh_);
            return true;
        }
        if(message.message==WM_KEYDOWN && message.wParam==VK_RETURN &&
           (message.hwnd==miniRefresh_ || message.hwnd==miniOpen_)){
            SendMessageW(message.hwnd,BM_CLICK,0,0);
            return true;
        }
        return false;
    }
    private:

    void UpdateHistoryTimer(bool mainVisible) {
        const bool visible=(mainVisible && !IsIconic(window_)) || (hover_ && IsWindowVisible(hover_));
        BOOL motion=TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&motion,0);
        if(visible && motion && std::ranges::any_of(cards_,[](const Card& c){return ChargingIndicator(c.reading.power);}))
            SetTimer(window_,ChargingTimer,66,nullptr);
        else KillTimer(window_,ChargingTimer);
        chargeMotion_=motion!=FALSE;
        if(visible && std::ranges::any_of(cards_,[](const Card& card){return card.charge.previousTick!=0;}))
            SetTimer(window_,HistoryTimer,60000,nullptr);
        else KillTimer(window_,HistoryTimer);
    }

    static LRESULT CALLBACK HoverProcedure(HWND hwnd,UINT msg,WPARAM w,LPARAM l){
        if(msg==WM_NCCREATE){
            auto create=reinterpret_cast<CREATESTRUCTW*>(l);
            SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto self=reinterpret_cast<Monitor*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(self && msg==WM_PAINT){
            PAINTSTRUCT ps{};
            BeginPaint(hwnd,&ps);
            try { self->PaintHover(hwnd); } catch(const winrt::hresult_error&){ self->hoverTarget_=nullptr; }
            EndPaint(hwnd,&ps);
            return 0;
        }
        if(msg==WM_ERASEBKGND)return 1;
        if(self && msg==WM_ACTIVATE && LOWORD(w)==WA_INACTIVE && self->flyoutOpen_){
            self->lastFlyoutDismiss_=GetTickCount64();
            self->CloseFlyout();
            return 0;
        }
        if(self && msg==WM_COMMAND && HIWORD(w)==BN_CLICKED){
            if(LOWORD(w)==Refresh)self->Request();
            if(LOWORD(w)==Open){self->CloseFlyout();self->Reveal();}
            return 0;
        }
        if(self && msg==WM_DRAWITEM){
            self->DrawFlyoutCommand(*reinterpret_cast<const DRAWITEMSTRUCT*>(l));
            return TRUE;
        }
        if(msg==WM_MOUSEACTIVATE)return self && self->flyoutOpen_?MA_ACTIVATE:MA_NOACTIVATE;
        if(self && msg==WM_MOUSEWHEEL){
            self->hoverOffset_=std::clamp(self->hoverOffset_-GET_WHEEL_DELTA_WPARAM(w)/WHEEL_DELTA*44,0,self->hoverLimit_);
            InvalidateRect(hwnd,nullptr,FALSE);
            return 0;
        }
        return DefWindowProcW(hwnd,msg,w,l);
    }

    bool flyoutGlass_{};
    bool flyoutSeeThrough_{};
    unsigned flyoutAccent_{Dark.accent};
    winrt::com_ptr<IDWriteTextFormat> flyoutIcons_;
    bool FlyoutDark() const { return theme_.background==Dark.background; }
    unsigned FooterColor() const { return FlyoutDark()?0x252525:0xf3f3f3; }

    static LRESULT CALLBACK CommandButtonProc(HWND hwnd,UINT msg,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR){
        if(msg==WM_MOUSEMOVE){
            TRACKMOUSEEVENT tracking{sizeof(tracking),TME_LEAVE,hwnd,0};
            TrackMouseEvent(&tracking);
            InvalidateRect(hwnd,nullptr,FALSE);
        } else if(msg==WM_MOUSELEAVE) InvalidateRect(hwnd,nullptr,FALSE);
        return DefSubclassProc(hwnd,msg,w,l);
    }

    void DrawFlyoutCommand(const DRAWITEMSTRUCT& item) const {
        const int dpi=GetDpiForWindow(item.hwndItem);
        auto px=[dpi](int x){return MulDiv(x,dpi,96);};
        auto rgb=[](unsigned c){return RGB(c>>16,(c>>8)&255,c&255);};
        POINT mouse{};
        GetCursorPos(&mouse);
        const bool hot=WindowFromPoint(mouse)==item.hwndItem;
        const bool pressed=item.itemState&ODS_SELECTED, disabled=item.itemState&ODS_DISABLED;
        HDC dc{};
        auto buffer=BeginBufferedPaint(item.hDC,&item.rcItem,BPBF_TOPDOWNDIB,nullptr,&dc);
        if(!buffer)dc=item.hDC;
        const int saved=SaveDC(dc);
        SelectObject(dc,GetStockObject(DC_BRUSH));
        SelectObject(dc,GetStockObject(NULL_PEN));
        SetDCBrushColor(dc,rgb(FooterColor()));
        FillRect(dc,&item.rcItem,(HBRUSH)GetStockObject(DC_BRUSH));
        RECT r=item.rcItem;
        if(!disabled && (hot || pressed)){
            SetDCBrushColor(dc,rgb(FlyoutDark()?(pressed?0x303030:0x393939):(pressed?0xe9e9e9:0xe4e4e4)));
            RoundRect(dc,r.left,r.top,r.right,r.bottom,px(8),px(8));
        }
        SetBkMode(dc,TRANSPARENT);
        SetTextColor(dc,rgb(disabled?(FlyoutDark()?0x929292:0x747474):(FlyoutDark()?0xffffff:0x191919)));
        auto draw=[&](const wchar_t* value,const wchar_t* family,int size,RECT bounds){
            HFONT font=CreateFontW(-px(size),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
                0,0,CLEARTYPE_QUALITY,0,family);
            auto previous=SelectObject(dc,font);
            DrawTextW(dc,value,-1,&bounds,DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX);
            SelectObject(dc,previous);
            DeleteObject(font);
        };
        r.left+=px(10);
        draw(item.CtlID==Refresh?L"\uE72C":L"\uE8A7",L"Segoe Fluent Icons",16,r);
        r.left+=px(25);
        wchar_t label[64]{};
        GetWindowTextW(item.hwndItem,label,64);
        draw(label,L"Segoe UI Variable Text",14,r);
        if((item.itemState&ODS_FOCUS) && !(item.itemState&ODS_NOFOCUSRECT)){
            RECT focus=item.rcItem; InflateRect(&focus,-2,-2); DrawFocusRect(dc,&focus);
        }
        RestoreDC(dc,saved);
        if(buffer){
            BufferedPaintSetAlpha(buffer,&item.rcItem,255);
            EndBufferedPaint(buffer,TRUE);
        }
    }

    void StyleFlyout(){
        const BOOL dark=DarkMode();
        HIGHCONTRASTW hc{sizeof(hc)};
        SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(hc),&hc,0);
        DWORD transparency=1,bytes=sizeof(transparency);
        RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"EnableTransparency",RRF_RT_REG_DWORD,nullptr,&transparency,&bytes);
        DwmSetWindowAttribute(hover_,DWMWA_USE_IMMERSIVE_DARK_MODE,&dark,sizeof(dark));
        const bool glass=transparency && !(hc.dwFlags&HCF_HIGHCONTRASTON);
        // The system transient backdrop sets its own tint. The composition accent
        // supplies the live background while Direct2D controls the overlay opacity.
        struct AccentPolicy { int state, flags; DWORD color; int animation; };
        struct AttributeData { int attribute; void* data; SIZE_T size; };
        using SetAttribute=BOOL(WINAPI*)(HWND,AttributeData*);
        static const auto setAttribute=std::bit_cast<SetAttribute>(GetProcAddress(
            GetModuleHandleW(L"user32.dll"),"SetWindowCompositionAttribute"));
        DWM_SYSTEMBACKDROP_TYPE material=DWMSBT_NONE;
        DwmSetWindowAttribute(hover_,DWMWA_SYSTEMBACKDROP_TYPE,&material,sizeof(material));
        AccentPolicy accent{glass?3:0,0,0,0}; // ACCENT_ENABLE_BLURBEHIND / ACCENT_DISABLED
        AttributeData attribute{19,&accent,sizeof(accent)}; // WCA_ACCENT_POLICY
        flyoutSeeThrough_=setAttribute && setAttribute(hover_,&attribute) && glass;
        if(glass && !flyoutSeeThrough_)material=DWMSBT_TRANSIENTWINDOW;
        bool backdrop=flyoutSeeThrough_;
        if(material==DWMSBT_TRANSIENTWINDOW)
            backdrop=SUCCEEDED(DwmSetWindowAttribute(hover_,DWMWA_SYSTEMBACKDROP_TYPE,&material,sizeof(material)));
        const int extent=backdrop?-1:0;
        const MARGINS margins{extent,extent,extent,extent};
        flyoutGlass_=SUCCEEDED(DwmExtendFrameIntoClientArea(hover_,&margins)) && extent==-1;
        flyoutAccent_=theme_.accent;
        try {
            using namespace winrt::Windows::UI::ViewManagement;
            auto c=UISettings().GetColorValue(dark?UIColorType::AccentLight2:UIColorType::AccentDark1);
            flyoutAccent_=(unsigned(c.R)<<16)|(unsigned(c.G)<<8)|c.B;
        } catch(const winrt::hresult_error&) {}
        hoverTarget_=nullptr;
        InvalidateRect(hover_,nullptr,FALSE);
    }

    void PaintHover(HWND hwnd){
        EnsureFactories();
        RECT rc{}; GetClientRect(hwnd,&rc);
        const auto pixels=D2D1::SizeU(rc.right,rc.bottom);
        const float dpi=float(GetDpiForWindow(hwnd));
        if(!hoverTarget_)
            winrt::check_hresult(direct2d_->CreateHwndRenderTarget(
                D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),dpi,dpi),
                D2D1::HwndRenderTargetProperties(hwnd,pixels),hoverTarget_.put()));
        auto rt=hoverTarget_.get();
        if(rt->GetPixelSize().width!=pixels.width || rt->GetPixelSize().height!=pixels.height)
            winrt::check_hresult(rt->Resize(pixels));
        rt->SetDpi(dpi,dpi);
        winrt::com_ptr<ID2D1SolidColorBrush> pen;
        winrt::check_hresult(rt->CreateSolidColorBrush(ColorF(theme_.text),pen.put()));
        const float width=rt->GetSize().width;
        const float footerTop=rt->GetSize().height-56;
        const bool dark=FlyoutDark();
        const unsigned primary=dark?0xffffff:0x191919, secondary=dark?0xc8c8c8:0x606060;
        if(!flyoutIcons_)winrt::check_hresult(directWrite_->CreateTextFormat(L"Segoe Fluent Icons",nullptr,
            DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,20,L"ru-RU",flyoutIcons_.put()));
        auto text=[&](const std::wstring& value,IDWriteTextFormat* font,D2D1_RECT_F bounds,unsigned color,bool right=false){
            pen->SetColor(ColorF(color));
            font->SetTextAlignment(right?DWRITE_TEXT_ALIGNMENT_TRAILING:DWRITE_TEXT_ALIGNMENT_LEADING);
            rt->DrawText(value.c_str(),UINT32(value.size()),font,bounds,pen.get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);
        };
        auto band=[&](float left,float top,float right,float bottom,float radius,unsigned color){
            pen->SetColor(ColorF(color));
            rt->FillRoundedRectangle(RoundedRect(RectF(left,top,right,bottom),radius,radius),pen.get());
        };
        rt->BeginDraw();
        rt->Clear(!flyoutGlass_?ColorF(dark?0x252525:0xf9f9f9):
            flyoutSeeThrough_?ColorF(dark?0x151515:0xffffff,dark?.62f:.67f):ColorF(0,dark?.30f:0.f));
        text(L"Заряд устройств",name_.get(),RectF(20,15,width-110,44),primary);
        if(!lastTime_.empty())text(lastTime_,small_.get(),RectF(width-108,18,width-20,42),secondary,true);
        rt->PushAxisAlignedClip(RectF(0,50,width,footerTop),D2D1_ANTIALIAS_MODE_ALIASED);
        float rowTop=50.f-hoverOffset_;
        for(size_t i=0;i<cards_.size();++i){
            const float y=rowTop;
            const float extra=cards_[i].charge.data.full?18.f:0.f;
            rowTop+=88.f+extra;
            if(y+83+extra<50 || y>=rt->GetSize().height)continue;
            const auto& c=cards_[i];
            const auto level=c.last?c.last->percent:c.reading.percent;
            const unsigned accent=!c.reading.percent?secondary:*c.reading.percent<=20?theme_.amber:flyoutAccent_;
            const auto& kind=profiles_[i].kind;
            text(kind==L"mouse"?L"\uE962":kind==L"headset"?L"\uE95B":kind==L"gamepad"?L"\uE7FC":L"\uE772",
                 flyoutIcons_.get(),RectF(20,y+8,48,y+39),primary);
            text(profiles_[i].name,header_.get(),RectF(58,y+6,width-88,y+30),primary);
            text(CardValue(i),header_.get(),RectF(width-82,y+6,width-20,y+30),primary,true);
            std::wstring status=int(i)==driverRow_ && firstSampleNotice_ && !c.reading.percent ?
                firstSampleNotice_:c.reading.status;
            if(c.last && !c.reading.percent)status=L"Ранее · "+status;
            auto detail=profiles_[i].connection;
            if(!status.empty() && status!=detail)detail+=(detail.empty()?L"":L" · ")+status;
            text(detail,small_.get(),RectF(58,y+28,width-20,y+48),secondary);
            text(ChargeLine(c.charge,profiles_[i].protocol==L"airoha-ystech"),small_.get(),RectF(58,y+47,width-20,y+66),secondary);
            if(c.charge.data.full)text((profiles_[i].protocol==L"airoha-ystech"?L"Оценка 100% · ":L"Полный заряд · ")+LocalStamp(c.charge.data.full),small_.get(),
                                       RectF(58,y+65,width-20,y+85),secondary);
            band(58,y+72+extra,width-20,y+75+extra,1.5f,dark?0x656565:0xc2c2c2);
            const float end=level?58+(width-78)*(*level)/100.f:58;
            if(end>58)band(58,y+72+extra,end,y+75+extra,1.5f,accent);
            if(ChargingIndicator(c.reading.power))
                DrawCharging(rt,pen.get(),Point2F(width-94,y+17),
                    RectF(58,y+72+extra,end>58?end:width-20,y+75+extra),flyoutAccent_,!dark);
        }
        rt->PopAxisAlignedClip();
        band(0,footerTop,width,rt->GetSize().height,0,FooterColor());
        band(0,footerTop,width,footerTop+1,0,dark?0x383838:0xe1e1e1);
        if(FAILED(rt->EndDraw()))hoverTarget_=nullptr;
    }
    void ShowFlyout() {
        auto tray=TrayData();
        NOTIFYICONIDENTIFIER id{sizeof(id)};
        id.hWnd=tray.hWnd;
        id.uID=tray.uID;
        RECT icon{};
        if(FAILED(Shell_NotifyIconGetRect(&id,&icon))){
            POINT point{}; GetCursorPos(&point);
            icon={point.x,point.y,point.x+1,point.y+1};
        }
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromRect(&icon,MONITOR_DEFAULTTONEAREST),&monitor);
        const auto work=monitor.rcWork;
        if(!hover_){
            WNDCLASSW cls{};
            cls.lpfnWndProc=HoverProcedure;
            cls.hInstance=GetModuleHandleW(nullptr);
            cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);
            cls.lpszClassName=L"PeripheralCharge.TrayDetails";
            if(!RegisterClassW(&cls) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return;
            hover_=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST,cls.lpszClassName,
                L"Заряд устройств",WS_POPUP|WS_CLIPCHILDREN,icon.left,icon.top,1,1,window_,nullptr,cls.hInstance,this);
            if(!hover_)return;
            auto button=[&](int id,const wchar_t* label){
                HWND command=CreateWindowExW(0,L"BUTTON",label,WS_CHILD|WS_TABSTOP|BS_OWNERDRAW,
                    0,0,1,1,hover_,reinterpret_cast<HMENU>(INT_PTR(id)),cls.hInstance,nullptr);
                if(command)SetWindowSubclass(command,CommandButtonProc,1,0);
                return command;
            };
            miniRefresh_=button(Refresh,L"Обновить");
            miniOpen_=button(Open,L"Открыть приложение");
            StyleFlyout();
            DWM_WINDOW_CORNER_PREFERENCE corners=DWMWCP_ROUND;
            DwmSetWindowAttribute(hover_,DWMWA_WINDOW_CORNER_PREFERENCE,&corners,sizeof(corners));
        }
        const int dpi=GetDpiForWindow(hover_);
        const int width=std::min<int>(work.right-work.left,MulDiv(408,dpi,96));
        int contentHeight=(flyoutOpen_?108:64)+int(cards_.size())*88;
        for(const auto& c:cards_)if(c.charge.data.full)contentHeight+=18;
        const int height=std::min<int>(work.bottom-work.top,MulDiv(contentHeight,dpi,96));
        hoverLimit_=std::max(0,contentHeight-MulDiv(height,96,dpi));
        hoverOffset_=IsWindowVisible(hover_)?std::min(hoverOffset_,hoverLimit_):0;
        int x=std::clamp<int>((icon.left+icon.right-width)/2,work.left,work.right-width);
        int y=icon.top-height-6;
        if(y<work.top)y=icon.bottom+6;
        y=std::clamp<int>(y,work.top,work.bottom-height);
        SetWindowPos(hover_,HWND_TOPMOST,x,y,width,height,SWP_NOACTIVATE|SWP_SHOWWINDOW);
        auto px=[dpi](int value){return MulDiv(value,dpi,96);};
        UINT show=flyoutOpen_?SWP_SHOWWINDOW:SWP_HIDEWINDOW;
        SetWindowPos(miniRefresh_,nullptr,px(10),height-px(45),px(122),px(34),SWP_NOACTIVATE|SWP_NOZORDER|show);
        SetWindowPos(miniOpen_,nullptr,width-px(204),height-px(45),px(194),px(34),SWP_NOACTIVATE|SWP_NOZORDER|show);
        SetWindowTextW(miniRefresh_,refreshing_?L"Проверка…":L"Обновить");
        EnableWindow(miniRefresh_,!refreshing_);
        InvalidateRect(hover_,nullptr,FALSE);
        UpdateHistoryTimer(IsWindowVisible(window_)!=FALSE);
    }
    void AddTray() { UpdateTray(true); }

    void Reveal() {
        ShowWindow(window_, IsIconic(window_) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(window_);
        if (GetTickCount64() - lastPoll_ > 20'000) Request();
    }

    void TrayMenu() {
        if(flyoutOpen_)CloseFlyout();
        HMENU menu = CreatePopupMenu();
        if (!menu) return;
        AppendMenuW(menu, MF_STRING, Open, L"Открыть");
        AppendMenuW(menu, MF_STRING, Refresh, L"Проверить заряд");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (enabled_ ? MF_CHECKED : 0), Summary, L"Сводка в Win+N");
        AppendMenuW(menu, MF_STRING | (enabled_ ? 0 : MF_GRAYED), Publish, L"Показать сводку снова");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, Quit, L"Выход");
        POINT point{};
        GetCursorPos(&point);
        SetForegroundWindow(window_);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, point.x, point.y, 0, window_, nullptr);
        PostMessageW(window_, WM_NULL, 0, 0);
        DestroyMenu(menu);
    }

    void Scroll(int amount) {
        scroll_ = std::clamp(scroll_ + amount, 0, static_cast<int>(cards_.size()) - VisibleRows());
        PositionDriverButton();
        InvalidateRect(window_, nullptr, FALSE);
    }

    static D2D1_RECT_F RefreshBounds() { return RectF(Width-Margin-120,26,Width-Margin,62); }
    bool IsRefreshPoint(int x, int y) const {
        float dpi = float(GetDpiForWindow(window_)) / 96;
        const auto r=RefreshBounds(); return x>=r.left*dpi && x<=r.right*dpi && y>=r.top*dpi && y<=r.bottom*dpi;
    }

    float Motion() const {
        if (!animation_) return 1;
        float t = std::min(1.0f, float(GetTickCount64() - animation_) / 280);
        return 1 - (1 - t) * (1 - t) * (1 - t);
    }

    void SetTheme() {
        BOOL dark = DarkMode();
        theme_ = dark ? Dark : Light;
        DwmSetWindowAttribute(window_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        if(hover_)StyleFlyout();
        if (driverButton_) InvalidateRect(driverButton_, nullptr, TRUE);
        InvalidateRect(window_, nullptr, FALSE);
    }

    void Font(winrt::com_ptr<IDWriteTextFormat>& out, float size, DWRITE_FONT_WEIGHT weight) {
        winrt::check_hresult(directWrite_->CreateTextFormat(L"Segoe UI", nullptr, weight,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"ru-RU", out.put()));
        out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER};
        winrt::com_ptr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(directWrite_->CreateEllipsisTrimmingSign(out.get(), ellipsis.put())))
            out->SetTrimming(&trim, ellipsis.get());
    }

    void EnsureFactories() {
        if (!direct2d_) winrt::check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, direct2d_.put()));
        if (!directWrite_) {
            winrt::check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(directWrite_.put())));
            Font(title_, 31, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            Font(name_, 17, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            Font(header_, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            Font(small_, 13, DWRITE_FONT_WEIGHT_NORMAL);
            Font(percent_, 25, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        }
    }
    void EnsureRender() {
        EnsureFactories();
        if (!render_) {
            RECT rect{};
            GetClientRect(window_, &rect);
            float dpi = float(GetDpiForWindow(window_));
            winrt::check_hresult(direct2d_->CreateHwndRenderTarget(
                D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), dpi, dpi),
                D2D1::HwndRenderTargetProperties(window_, D2D1::SizeU(rect.right, rect.bottom)), render_.put()));
        }
        if (!brush_) winrt::check_hresult(render_->CreateSolidColorBrush(ColorF(theme_.text), brush_.put()));
    }

    void Round(D2D1_RECT_F rect, float radius, unsigned shade, bool outline = false) {
        brush_->SetColor(ColorF(shade));
        auto rounded = RoundedRect(rect, radius, radius);
        if (outline) render_->DrawRoundedRectangle(rounded, brush_.get(), 1);
        else render_->FillRoundedRectangle(rounded, brush_.get());
    }

    void Line(float x, float y, float endX, float endY, unsigned shade) {
        brush_->SetColor(ColorF(shade));
        render_->DrawLine(Point2F(x, y), Point2F(endX, endY), brush_.get(), 1.65f);
    }

    void Text(std::wstring_view value, IDWriteTextFormat* format, D2D1_RECT_F rect,
              unsigned shade, bool right = false) {
        brush_->SetColor(ColorF(shade));
        format->SetTextAlignment(right ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
        render_->DrawText(value.data(), UINT32(value.size()), format, rect, brush_.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void Symbol(const std::wstring& kind, float x, float y) {
        auto c = theme_.accent;
        if (kind == L"mouse") {
            Round(RectF(x + 12, y + 7, x + 30, y + 34), 8, c, true);
            Line(x + 21, y + 8, x + 21, y + 17, c);
            Line(x + 12, y + 18, x + 30, y + 18, c);
        } else if (kind == L"headset") {
            Round(RectF(x + 10, y + 8, x + 32, y + 31), 10, c, true);
            Round(RectF(x + 8, y + 21, x + 15, y + 32), 2, c);
            Round(RectF(x + 27, y + 21, x + 34, y + 32), 2, c);
        } else if (kind == L"gamepad") {
            Round(RectF(x + 7, y + 11, x + 35, y + 30), 8, c, true);
            Line(x + 13, y + 20, x + 20, y + 20, c);
            Line(x + 16.5f, y + 16.5f, x + 16.5f, y + 23.5f, c);
            Round(RectF(x + 25, y + 17, x + 28, y + 20), 2, c);
            Round(RectF(x + 29, y + 21, x + 32, y + 24), 2, c);
        } else {
            Round(RectF(x + 9, y + 14, x + 31, y + 28), 3, c, true);
            Line(x + 32, y + 18, x + 34, y + 18, c);
            Line(x + 32, y + 24, x + 34, y + 24, c);
        }
    }

    void Paint() {
        try { EnsureRender(); }
        catch (const winrt::hresult_error&) { return; }
        render_->BeginDraw();
        render_->Clear(ColorF(theme_.background));
        Text(L"Заряд устройств", title_.get(), RectF(Margin, 24, Width - 154, 68), theme_.text);
        size_t available = std::ranges::count_if(cards_, [](const Card& card) { return card.reading.percent.has_value(); });
        auto count = L"Есть данные для " + std::to_wstring(available) + L" из " + std::to_wstring(cards_.size());
        if (cards_.size() > static_cast<size_t>(VisibleRows()))
            count += L"  ·  " + std::to_wstring(scroll_ + 1) + L"–" + std::to_wstring(scroll_ + VisibleRows());
        Text(count, small_.get(), RectF(Margin, 77, Width - Margin, 104), theme_.secondary);

        const auto button=RefreshBounds();
        Round(button,9,theme_.button);
        header_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        header_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        brush_->SetColor(ColorF(theme_.text));
        const std::wstring_view caption=refreshing_?L"Проверка…":L"Обновить";
        render_->DrawText(caption.data(),UINT32(caption.size()),header_.get(),button,brush_.get());
        float motion = Motion();
        for (int i = 0; i < VisibleRows(); ++i) {
            size_t index = size_t(scroll_ + i);
            auto& card = cards_[index];
            auto& profile = profiles_[index];
            float y = Top + Row * i;
            Round(RectF(Margin, y, Width - Margin, y + 106), 13, theme_.card);
            Round(RectF(Margin, y, Width - Margin, y + 106), 13, theme_.border, true);
            Round(RectF(Margin + 14, y + 16, Margin + 56, y + 58), 11, theme_.iconBg);
            Symbol(profile.kind, Margin + 14, y + 16);
            float left = Margin + 69, right = Width - Margin - 15;
            const bool offer = driverOffered_ && int(index) == driverRow_;
            const float detailRight = offer ? right - InstallWidth - 12 : right;
            Text(profile.name, name_.get(), RectF(left, y + 11, offer ? detailRight : right - 85, y + 37), theme_.text);
            std::wstring detail;
            if (int(index)==driverRow_ && firstSampleNotice_ && !card.reading.percent) detail=firstSampleNotice_;
            else if (offer && driverNotice_) detail=driverNotice_;
            else if (offer && card.reading.percent) detail=CardValue(index)+L" · "+card.reading.status;
            else if (offer) detail=VendorFilterInstalled()?L"Доступен автоопрос":L"Драйвер для чтения заряда";
            else if (card.last && !card.reading.percent) detail = L"Последнее значение · " + card.reading.status;
            else if (card.reading.status == L"Подключено") detail = profile.connection;
            else detail = card.reading.status;
            Text(detail, small_.get(), RectF(left, y + 39, detailRight, y + 60), theme_.secondary);
            Text(ChargeLine(card.charge,profile.protocol==L"airoha-ystech"), small_.get(), RectF(left,y+60,right,y+81),theme_.secondary);
            const bool current = card.reading.percent.has_value();
            const auto chargeColor = !current ? theme_.secondary :
                                     *card.reading.percent <= 20 ? theme_.amber : theme_.accent;
            if (!offer)
                Text(CardValue(index), percent_.get(), RectF(right - 88, y + 7, right, y + 38), chargeColor, true);
            Round(RectF(left, y + 88, right, y + 93), 2.5f, theme_.track);
            float fill = (card.from + (card.to - card.from) * motion) / 100 * (right - left);
            if (fill > 0) Round(RectF(left, y + 88, left + fill, y + 93), 2.5f, chargeColor);
            if(!offer && ChargingIndicator(card.reading.power))
                DrawCharging(render_.get(),brush_.get(),Point2F(right-84,y+23),
                    RectF(left,y+88,fill>0?left+fill:right,y+93),theme_.accent,theme_.background==Light.background);
        }
        const float foot = Top + Row * VisibleRows() + 8;
        const auto footer = summaryFailed_ ? L"Сводка Windows недоступна" :
                            enabled_ ? L"Сводка в Win+N" : L"Сводка отключена";
        Text(footer, small_.get(), RectF(Margin, foot, Width - Margin, foot + 23), theme_.secondary);
        if (!lastTime_.empty())
            Text(L"Проверено в " + lastTime_, small_.get(), RectF(Width - 200, foot, Width - Margin, foot + 23),
                 theme_.secondary, true);
        if (render_->EndDraw() == D2DERR_RECREATE_TARGET) { brush_ = nullptr; render_ = nullptr; }
    }
};

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_NCCREATE) {
        auto create = reinterpret_cast<CREATESTRUCTW*>(l);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        static_cast<Monitor*>(create->lpCreateParams)->Bind(window);
        return DefWindowProcW(window, message, w, l);
    }
    auto monitor = reinterpret_cast<Monitor*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    return monitor ? monitor->Message(message, w, l) : DefWindowProcW(window, message, w, l);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR arguments, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    try {
        const std::wstring_view mode(arguments);
        if (mode == L"--install-direwolf-driver") return InstallDirewolfDriver();
        if (mode == L"--check-direwolf") return DirewolfPresent() ? 0 : DriverDeviceMissing;
        auto command = std::wstring_view(GetCommandLineW());
        const bool tray = command.find(L" --tray") != command.npos;
        SetCurrentProcessExplicitAppUserModelID(AppId);
        if (command.find(L" --check-summary") != command.npos) return SummaryIsPresent() ? 0 : 3;
        HANDLE singleton = CreateMutexW(nullptr, FALSE, L"Local\\PeripheralCharge.BatteryMonitor");
        DWORD mutexStatus = GetLastError();
        if (singleton && mutexStatus == ERROR_ALREADY_EXISTS) {
            if (auto other = FindWindowW(WindowClass, nullptr)) PostMessageW(other, WM_COMMAND, Open, 0);
            CloseHandle(singleton);
            return 0;
        }
        // Keep the mutex handle open until the process leaves its message loop.
        Monitor monitor(LoadProfiles(AppFolder() / L"devices"));
        WNDCLASSW klass{};
        klass.hInstance = instance;
        klass.lpszClassName = WindowClass;
        klass.lpfnWndProc = WindowProcedure;
        klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        winrt::check_bool(RegisterClassW(&klass));
        DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
        DWORD ex = 0;
        UINT dpi = GetDpiForSystem();
        RECT rect{0, 0, MulDiv(LONG(Width), dpi, 96), MulDiv(LONG(monitor.Height()), dpi, 96)};
        AdjustWindowRectExForDpi(&rect, style, FALSE, ex, dpi);
        auto window = CreateWindowExW(ex, WindowClass, L"Battery Monitor", style, CW_USEDEFAULT, CW_USEDEFAULT,
                                      rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
                                      instance, &monitor);
        winrt::check_bool(window);
        monitor.Start(tray);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if(monitor.HandleMiniKey(message))continue;
            if (message.message == WM_KEYDOWN && message.wParam == VK_TAB && IsDialogMessageW(window, &message))
                continue;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (singleton) CloseHandle(singleton);
        return int(message.wParam);
    } catch (const winrt::hresult_error& error) {
        MessageBoxW(nullptr, error.message().c_str(), L"Battery Monitor", MB_ICONERROR);
    } catch (const std::exception& error) {
        MessageBoxW(nullptr, winrt::to_hstring(error.what()).c_str(), L"Battery Monitor", MB_ICONERROR);
    }
    return 1;
}
