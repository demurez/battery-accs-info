#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include "github_download.hpp"

namespace {
struct HttpHandle {
    HINTERNET value{};
    ~HttpHandle() { if (value) WinHttpCloseHandle(value); }
};
constexpr wchar_t Host[] = L"raw.githubusercontent.com";
constexpr wchar_t Prefix[] = L"/demurez/battery-accs-info/main/";
}

std::optional<std::string> ReadRepositoryFile(std::wstring_view path, size_t limit) {
    if (path.empty() || path.size() > 160 || path.front() == L'/' ||
        path.find(L"..") != path.npos ||
        path.find_first_not_of(L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_/.") != path.npos)
        return {};
    HttpHandle session{WinHttpOpen(L"Battery Monitor/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.value || !WinHttpSetTimeouts(session.value, 2500, 2500, 2500, 2500)) return {};
    HttpHandle connection{WinHttpConnect(session.value, Host, INTERNET_DEFAULT_HTTPS_PORT, 0)};
    if (!connection.value) return {};
    std::wstring route = Prefix + std::wstring(path);
    HttpHandle request{WinHttpOpenRequest(connection.value, L"GET", route.c_str(), nullptr,
                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
    if (!request.value) return {};
    DWORD flags = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_DISABLE_FEATURE, &flags, sizeof(flags)) ||
        !WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.value, nullptr)) return {};
    DWORD status{}, statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX) ||
        status != HTTP_STATUS_OK) return {};

    std::string body;
    const auto deadline = GetTickCount64() + 5000;
    for (;;) {
        char buffer[4096];
        DWORD received{};
        if (GetTickCount64() >= deadline || !WinHttpReadData(request.value, buffer, sizeof(buffer), &received) ||
            received > limit - body.size()) return {};
        if (!received) return body;
        body.append(buffer, received);
    }
}
