#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <dbt.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <winrt/base.h>

#include "devices.hpp"
#include "driver_install.hpp"
#include "notifications.hpp"
#include "profile_catalog.hpp"

#include <algorithm>
#include <array>
#include <chrono>
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
constexpr UINT BatteryReady = WM_APP + 1, TrayEvent = WM_APP + 2;
constexpr UINT_PTR AnimationTimer = 1, PlugTimer = 2, DriverTimer = 4;
constexpr int Open = 101, Refresh = 102, Summary = 103, Quit = 104, Publish = 105, InstallDriver = 106;
constexpr float Width = 536, Top = 119, Row = 94, Bottom = 54, Margin = 26;
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

struct Card {
    Reading reading{{}, L"Ожидание данных"};
    std::optional<Reading> last;
    float from{}, to{};
};

class Monitor {
public:
    explicit Monitor(std::vector<Profile> profiles)
        : profiles_(std::move(profiles)), cards_(profiles_.size()), enabled_(SummaryEnabled()) {}

    ~Monitor() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_one();
        if (worker_.joinable()) worker_.join();

        if (driverProcess_) CloseHandle(driverProcess_);
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
                DrawDriverButton(*reinterpret_cast<const DRAWITEMSTRUCT*>(l));
                return TRUE;
            }
            break;
        case WM_TIMER:
            if (w == AnimationTimer) {
                if (!IsWindowVisible(window_) || IsIconic(window_) || Motion() >= 1)
                    KillTimer(window_, AnimationTimer);
                if (IsWindowVisible(window_) && !IsIconic(window_)) InvalidateRect(window_, nullptr, FALSE);
            } else if (w == PlugTimer) {
                KillTimer(window_, PlugTimer);
                UpdateDriverOffer();
                Request(true);
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
                Reveal();
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
    std::vector<std::wstring> lastSummary_;
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
            card.reading = std::move(values[i]);
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
        driverOffered_ = matched && !DirewolfDriverPresent() &&
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

    void FinishDriverInstall() {
        if (!driverProcess_ || WaitForSingleObject(driverProcess_, 0) == WAIT_TIMEOUT) return;
        KillTimer(window_, DriverTimer);
        DWORD status = DriverSetupError;
        GetExitCodeProcess(driverProcess_, &status);
        CloseHandle(driverProcess_);
        driverProcess_ = nullptr;
        driverReboot_ = status == ERROR_SUCCESS_REBOOT_REQUIRED || status == ERROR_SUCCESS_RESTART_REQUIRED;
        if (driverReboot_) driverNotice_ = L"Перезапуск активирует драйвер";
        else if (status == DriverDeviceMissing) driverNotice_ = L"Донгл отключён";
        else if (status == DriverPackageMissing) driverNotice_ = L"Пакет пока недоступен";
        else if (status != 0) driverNotice_ = L"Драйвер установить не удалось";
        else driverNotice_ = nullptr;
        UpdateDriverOffer();
        Request(true);
    }

    void DrawDriverButton(const DRAWITEMSTRUCT& item) const {
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
        GetWindowTextW(driverButton_, label, int(std::size(label)));
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
        std::vector<std::wstring> rows;
        auto written = std::min<size_t>(3, profiles_.size());
        for (size_t i = 0; i < written; ++i) {
            auto& card = cards_[i];
            auto value = CardValue(i);
            if (card.last && !card.reading.percent) value += L" ранее";
            rows.push_back(profiles_[i].name + L"  " + value);
        }
        if (profiles_.size() > written)
            rows.push_back(L"Ещё устройств: " + std::to_wstring(profiles_.size() - written));
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
        std::wstring tooltip = L"Заряд устройств";
        for (size_t i = 0; i < std::min<size_t>(4, cards_.size()); ++i)
            tooltip += L"\n" + profiles_[i].name + L"  " + CardValue(i) +
                       (cards_[i].last && !cards_[i].reading.percent ? L" ранее" : L"");
        wcsncpy_s(tray.szTip, tooltip.c_str(), _TRUNCATE);
        if (add) {
            trayAdded_ = Shell_NotifyIconW(NIM_ADD, &tray) != FALSE;
            if (trayAdded_) { tray.uVersion = NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION, &tray); }
        } else if (!Shell_NotifyIconW(NIM_MODIFY, &tray)) AddTray();
    }

    void AddTray() { UpdateTray(true); }

    void Reveal() {
        ShowWindow(window_, IsIconic(window_) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(window_);
        if (GetTickCount64() - lastPoll_ > 20'000) Request();
    }

    void TrayMenu() {
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

    bool IsRefreshPoint(int x, int y) const {
        float dpi = float(GetDpiForWindow(window_)) / 96;
        return x >= (Width - 146) * dpi && x <= (Width - Margin) * dpi &&
               y >= 31 * dpi && y <= 67 * dpi;
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

    void EnsureRender() {
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

        Round(RectF(Width - 146, 31, Width - Margin, 67), 9, theme_.button);
        Text(refreshing_ ? L"Проверка…" : L"Обновить", header_.get(),
             RectF(Width - 132, 39, Width - 34, 62), theme_.text);
        float motion = Motion();
        for (int i = 0; i < VisibleRows(); ++i) {
            size_t index = size_t(scroll_ + i);
            auto& card = cards_[index];
            auto& profile = profiles_[index];
            float y = Top + Row * i;
            Round(RectF(Margin, y, Width - Margin, y + 84), 13, theme_.card);
            Round(RectF(Margin, y, Width - Margin, y + 84), 13, theme_.border, true);
            Round(RectF(Margin + 14, y + 16, Margin + 56, y + 58), 11, theme_.iconBg);
            Symbol(profile.kind, Margin + 14, y + 16);
            float left = Margin + 69, right = Width - Margin - 15;
            const bool offer = driverOffered_ && int(index) == driverRow_;
            const float detailRight = offer ? right - InstallWidth - 12 : right;
            Text(profile.name, name_.get(), RectF(left, y + 11, offer ? detailRight : right - 85, y + 37), theme_.text);
            std::wstring detail;
            if (offer && driverNotice_) detail=driverNotice_;
            else if (offer && card.reading.percent) detail=CardValue(index)+L" · "+card.reading.status;
            else if (offer) detail=VendorFilterInstalled()?L"Доступен автоопрос":L"Драйвер для чтения заряда";
            else if (card.last && !card.reading.percent) detail = L"Последнее значение · " + card.reading.status;
            else if (card.reading.status == L"Подключено") detail = profile.connection;
            else detail = card.reading.status;
            Text(detail, small_.get(), RectF(left, y + 39, detailRight, y + 60), theme_.secondary);
            const bool current = card.reading.percent.has_value();
            const auto chargeColor = !current ? theme_.secondary :
                                     *card.reading.percent <= 20 ? theme_.amber : theme_.accent;
            if (!offer)
                Text(CardValue(index), percent_.get(), RectF(right - 88, y + 7, right, y + 38), chargeColor, true);
            Round(RectF(left, y + 66, right, y + 71), 2.5f, theme_.track);
            float fill = (card.from + (card.to - card.from) * motion) / 100 * (right - left);
            if (fill > 0) Round(RectF(left, y + 66, left + fill, y + 71), 2.5f, chargeColor);
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
