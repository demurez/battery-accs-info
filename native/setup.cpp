#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <filesystem>
#include <stdexcept>
#include <string>

namespace {
constexpr wchar_t AppName[] = L"Battery Monitor";

std::filesystem::path SourceFolder() {
    std::wstring full(32768, L'\0');
    DWORD size = GetModuleFileNameW(nullptr, full.data(), DWORD(full.size()));
    if (!size || size >= full.size()) throw std::runtime_error("Setup path unavailable");
    full.resize(size);
    return std::filesystem::path(full).parent_path();
}

std::filesystem::path AppInstallFolder() {
    PWSTR local{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &local)))
        throw std::runtime_error("LocalAppData unavailable");
    std::filesystem::path path(local);
    CoTaskMemFree(local);
    return path / AppName;
}

void CopyApplication(const std::filesystem::path& source, const std::filesystem::path& target) {
    if (!std::filesystem::is_regular_file(source / L"BatteryMonitor.exe") ||
        !std::filesystem::is_directory(source / L"devices"))
        throw std::runtime_error("Incomplete application package");
    if (std::filesystem::exists(target) && std::filesystem::equivalent(source, target)) return;
    std::filesystem::create_directories(target / L"devices");
    std::filesystem::copy_file(source / L"BatteryMonitor.exe", target / L"BatteryMonitor.exe",
                               std::filesystem::copy_options::overwrite_existing);
    for (const auto& entry : std::filesystem::directory_iterator(source / L"devices"))
        if (entry.is_regular_file() && entry.path().extension() == L".json")
            std::filesystem::copy_file(entry.path(), target / L"devices" / entry.path().filename(),
                                       std::filesystem::copy_options::overwrite_existing);
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR commandLine, int) {
    if (*commandLine) return ERROR_INVALID_PARAMETER;
    try {
        const auto target = AppInstallFolder();
        CopyApplication(SourceFolder(), target);
        MessageBoxW(nullptr, L"Battery Monitor установлен.", AppName, MB_ICONINFORMATION);
        const auto app = target / L"BatteryMonitor.exe";
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", app.c_str(), nullptr,
                                                    target.c_str(), SW_SHOWNORMAL)) <= 32)
            return ERROR_FILE_NOT_FOUND;
        return 0;
    } catch (const std::exception&) {
        MessageBoxW(nullptr, L"Не удалось скопировать файлы Battery Monitor. "
                            L"Проверьте комплект установки и закройте работающий экземпляр программы.",
                    AppName, MB_ICONERROR);
        return 1;
    }
}
