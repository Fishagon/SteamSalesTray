#include "platform/Paths.h"

#include <windows.h>
#include <shlobj.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace platform {

namespace {
constexpr wchar_t kAppFolderName[] = L"SteamSalesTray";

std::wstring JoinData(const std::wstring& fileName) {
    std::wstring dir = GetDataDirectory();
    if (dir.empty()) {
        return {};
    }
    return dir + L"\\" + fileName;
}
}  // namespace

std::wstring GetDataDirectory() {
    PWSTR localAppData = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE,
                                      nullptr, &localAppData)) ||
        !localAppData) {
        if (localAppData) {
            ::CoTaskMemFree(localAppData);
        }
        return {};
    }
    std::wstring dir = localAppData;
    ::CoTaskMemFree(localAppData);

    dir += L"\\";
    dir += kAppFolderName;

    // Create the directory if it does not already exist.
    if (!::CreateDirectoryW(dir.c_str(), nullptr)) {
        if (::GetLastError() != ERROR_ALREADY_EXISTS) {
            return {};
        }
    }
    return dir;
}

std::wstring GetApiKeyPath() {
    return JoinData(L"api-key.dat");
}

// The Financial API Group key (GetApiKeyPath) has no app/user permissions and
// cannot call ISteamApps/GetPartnerAppListForWebAPIKey (Steam returns 403) -
// that needs a normal publisher Web API key, stored separately.
std::wstring GetPublisherApiKeyPath() {
    return JoinData(L"publisher-api-key.dat");
}

std::wstring GetDatabasePath() {
    return JoinData(L"sales.db");
}

std::wstring GetLogPath() {
    return JoinData(L"app.log");
}

std::wstring GetExecutablePath() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD len = ::GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (len == 0) {
            return {};
        }
        if (len < buffer.size()) {
            buffer.resize(len);
            return buffer;
        }
        buffer.resize(buffer.size() * 2);  // truncated; grow and retry
    }
}

}  // namespace platform
