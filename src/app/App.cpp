#include "app/App.h"

#include <algorithm>
#include <cstdint>
#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM
#include <shellapi.h>
#include <commctrl.h>

#include "app/Messages.h"
#include "platform/Logging.h"
#include "platform/Paths.h"
#include "platform/StringUtil.h"
#include "platform/TimeUtil.h"
#include "resources/Resource.h"
#include "security/DpapiSecretStore.h"
#include "steam/SteamFinancialClient.h"
#include "storage/SettingsStore.h"
#include "tray/TrayMenu.h"
#include "ui/SettingsDialog.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")

namespace app {

namespace {
std::wstring FormatCount(std::int64_t value) {
    return platform::FormatThousands(value);
}
}  // namespace

int App::Run(HINSTANCE instance) {
    instance_ = instance;

    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES;
    ::InitCommonControlsEx(&icc);

    icon_ = static_cast<HICON>(::LoadImageW(
        instance_, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
        0, 0, LR_DEFAULTSIZE | LR_SHARED));
    if (!icon_) {
        icon_ = ::LoadIconW(nullptr, IDI_APPLICATION);
    }

    RegisterAppMessages();

    if (!RegisterWindowClass() || !CreateMainWindow()) {
        ::MessageBoxW(nullptr, L"Failed to create the application window.",
                      L"Steam Sales Tray", MB_OK | MB_ICONERROR);
        return 1;
    }

    platform::Log::Info(L"App", L"Starting up");

    dbOpen_ = db_.Open(platform::GetDatabasePath());
    if (!dbOpen_) {
        // §20: never silently delete; offer rebuild / open folder / continue.
        platform::Log::Error(L"App", L"Could not open local cache");
        const int choice = ::MessageBoxW(
            nullptr,
            L"The local sales cache could not be opened. It may be missing "
            L"or corrupt.\n\n"
            L"Yes\t- Rebuild the cache (your API key is kept)\n"
            L"No\t- Open the data folder\n"
            L"Cancel\t- Continue without cached data",
            L"Steam Sales Tray", MB_YESNOCANCEL | MB_ICONWARNING);
        if (choice == IDYES) {
            const std::wstring db = platform::GetDatabasePath();
            ::DeleteFileW(db.c_str());
            ::DeleteFileW((db + L"-wal").c_str());
            ::DeleteFileW((db + L"-shm").c_str());
            dbOpen_ = db_.Open(db);
        } else if (choice == IDNO) {
            ::ShellExecuteW(nullptr, L"explore",
                            platform::GetDataDirectory().c_str(), nullptr,
                            nullptr, SW_SHOWNORMAL);
        }
    }
    salesWindow_.Initialize(instance_, hwnd_, &db_, icon_);

    security::DpapiSecretStore store;
    const std::wstring initialTip =
        store.HasStoredKey() ? L"Steam Sales - Ready"
                     : L"Steam Sales - API key required";
    if (!tray_.Add(hwnd_, WM_APP_TRAY, icon_, initialTip)) {
        ::MessageBoxW(nullptr, L"Failed to add the notification-area icon.",
                      L"Steam Sales Tray", MB_OK | MB_ICONERROR);
        // Continue running; the app is still usable without the icon.
    }

    // Launch the background sync worker (it owns its own DB + WinHTTP session).
    sync_.Start(hwnd_);

    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // Let the modeless sales window handle its own tab/keyboard navigation.
        const HWND sales = salesWindow_.Handle();
        if (sales && ::IsDialogMessageW(sales, &msg)) {
            continue;
        }
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    platform::Log::Info(L"App", L"Shutting down");
    sync_.Stop();
    salesWindow_.Destroy();
    tray_.Remove();
    db_.Close();
    return static_cast<int>(msg.wParam);
}

void App::RegisterAppMessages() {
    msgTaskbarCreated_ = ::RegisterWindowMessageW(kRegMsgTaskbarCreated);
    msgShowSales_ = ::RegisterWindowMessageW(kRegMsgShowSales);
    msgShowSettings_ = ::RegisterWindowMessageW(kRegMsgShowSettings);
    msgRefresh_ = ::RegisterWindowMessageW(kRegMsgRefresh);
}

bool App::RegisterWindowClass() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &App::WndProcThunk;
    wc.hInstance = instance_;
    wc.hIcon = icon_;
    wc.lpszClassName = kWindowClass;
    return ::RegisterClassExW(&wc) != 0;
}

bool App::CreateMainWindow() {
    // Invisible top-level window: never shown, kept off the taskbar and
    // Alt-Tab via WS_EX_TOOLWINDOW. It is a real top-level (not message-only)
    // window so it receives the broadcast "TaskbarCreated" message.
    hwnd_ = ::CreateWindowExW(
        WS_EX_TOOLWINDOW,
        kWindowClass, kWindowTitle,
        WS_OVERLAPPED,
        CW_USEDEFAULT, CW_USEDEFAULT, 0, 0,
        nullptr, nullptr, instance_, this);
    return hwnd_ != nullptr;
}

LRESULT CALLBACK App::WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam,
                                   LPARAM lParam) {
    App* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<App*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<App*>(
            ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) {
        return self->HandleMessage(hwnd, msg, wParam, lParam);
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT App::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_APP_TRAY) {
        OnTrayCallback(wParam, lParam);
        return 0;
    }
    if (msg == msgTaskbarCreated_ && msgTaskbarCreated_ != 0) {
        // Explorer restarted; re-add our icon.
        tray_.Recreate();
        return 0;
    }
    if (msg == msgShowSales_ && msgShowSales_ != 0) {
        OnShowSales();
        return 0;
    }
    if (msg == msgShowSettings_ && msgShowSettings_ != 0) {
        OnCommand(IDM_SETTINGS);
        return 0;
    }
    if (msg == msgRefresh_ && msgRefresh_ != 0) {
        OnCommand(IDM_REFRESH_NOW);
        return 0;
    }

    switch (msg) {
        case WM_APP_SYNC_STARTED:
        case WM_APP_SYNC_PROGRESS:
        case WM_APP_SYNC_COMPLETED:
        case WM_APP_SYNC_FAILED:
            UpdateTray();
            salesWindow_.Refresh();  // no-op if hidden
            return 0;
        case WM_APP_TOTALS_CHANGED:
            UpdateTray();
            salesWindow_.Refresh();
            MaybeShowNotification();
            return 0;
        case WM_COMMAND:
            OnCommand(LOWORD(wParam));
            return 0;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
        default:
            return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void App::OnTrayCallback(WPARAM wParam, LPARAM lParam) {
    // NOTIFYICON_VERSION_4: event is in LOWORD(lParam); the anchor coordinates
    // are in wParam (screen space).
    const UINT event = LOWORD(lParam);
    POINT pt{GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam)};

    switch (event) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
            OnShowSales();
            break;
        case WM_CONTEXTMENU:
        case WM_RBUTTONUP:
            ShowContextMenu(pt);
            break;
        default:
            break;
    }
}

void App::ShowContextMenu(POINT pt) {
    const tray::SummaryTotals totals = ComputeSummary();
    const UINT cmd = tray::ShowTrayMenu(hwnd_, pt, totals);
    // Return focus to the notification area after the menu closes.
    tray_.SetFocus();
    if (cmd != 0) {
        OnCommand(cmd);
    }
}

void App::OnShowSales() {
    salesWindow_.ShowAndFocus();
}

void App::OnSettings() {
    // Validates a candidate key against Steam by calling GetChangedDates with
    // highwatermark 0. Runs synchronously on this (UI) thread while the modal
    // dialog is open, bounded by the WinHTTP timeouts. Only Success and Auth
    // are treated as definitive (couldTest = true); transient failures let the
    // user save the key anyway.
    ui::KeyValidator validator =
        [](const security::SecureString& keyUtf8) -> ui::KeyTestResult {
        steam::SteamFinancialClient client;
        steam::ChangedDates dates;
        const steam::FinancialResult r =
            client.GetChangedDates(keyUtf8, "0", dates);

        ui::KeyTestResult out;
        switch (r.kind) {
            case steam::ResultKind::Success:
                out.ok = true;
                out.couldTest = true;
                out.message = L"Connection succeeded - the key is valid.";
                break;
            case steam::ResultKind::Auth:
                out.ok = false;
                out.couldTest = true;
                out.message = r.message;
                break;
            default:
                out.ok = false;
                out.couldTest = false;  // inconclusive; allow save
                out.message = r.message.empty()
                                  ? L"Could not test the key right now."
                                  : r.message;
                break;
        }
        return out;
    };

    const ui::SettingsOutcome outcome =
        ui::ShowSettingsDialog(hwnd_, instance_, validator);

    // Apply the outcome to the running worker.
    if (outcome.cacheCleared) {
        sync_.RequestClearCache();  // clears DB + triggers a full resync
    } else if (outcome.forceResyncDays > 0) {
        sync_.RequestForceResync(outcome.forceResyncDays);
    } else if (outcome.keyChanged || outcome.preferencesChanged ||
              outcome.publisherKeyChanged) {
        sync_.NotifySettingsChanged();  // re-read interval, resync with new key
    }

    // Reflect display-only changes (e.g. hide lifetime) immediately, without a
    // resync. The sales window rebuilds its period list; the tray refreshes.
    salesWindow_.OnSettingsChanged();
    if (outcome.keyChanged) {
        UpdateTooltipForKeyState();
    } else {
        UpdateTray();
    }
}

void App::UpdateTooltipForKeyState() {
    security::DpapiSecretStore store;
    tray_.SetTooltip(store.HasStoredKey() ? L"Steam Sales - Ready"
                                          : L"Steam Sales - API key required");
}

void App::UpdateTray() {
    const sync::AppStatus st = sync_.GetStatus();
    std::wstring tip;
    switch (st.status) {
        case sync::SyncStatus::MissingApiKey:
            tip = L"Steam Sales - API key required";
            break;
        case sync::SyncStatus::Syncing:
            tip = L"Steam Sales - Updating…";
            break;
        case sync::SyncStatus::AuthenticationFailed:
            tip = L"Steam Sales - Check API key";
            break;
        case sync::SyncStatus::Offline:
        case sync::SyncStatus::ApiError:
        case sync::SyncStatus::ParseError:
        case sync::SyncStatus::DatabaseError:
            tip = L"Steam Sales - Update failed";
            break;
        case sync::SyncStatus::Idle:
        default:
            if (dbOpen_) {
                const std::int64_t today = db_.SumForDate(platform::SteamToday());
                tip = L"Steam Sales - Today: " + FormatCount(today);
                if (!storage::SettingsStore::GetHideLifetime()) {
                    tip += L" | Lifetime: " + FormatCount(db_.SumLifetime());
                }
            } else {
                tip = L"Steam Sales";
            }
            break;
    }
    tray_.SetTooltip(tip);
}

void App::MaybeShowNotification() {
    const sync::AppStatus st = sync_.GetStatus();
    if (!st.notification.empty()) {
        tray_.ShowBalloon(L"Steam sales updated", st.notification);
    }
}

tray::SummaryTotals App::ComputeSummary() {
    tray::SummaryTotals totals;  // defaults to em dash
    if (!dbOpen_) {
        return totals;
    }
    const std::string today = platform::SteamToday();
    totals.today = FormatCount(db_.SumForDate(today));
    totals.yesterday = FormatCount(db_.SumForDate(platform::SteamDateDaysAgo(1)));
    totals.week =
        FormatCount(db_.SumForRange(platform::SteamDateDaysAgo(6), today));
    totals.month =
        FormatCount(db_.SumForRange(platform::SteamDateDaysAgo(29), today));
    totals.showLifetime = !storage::SettingsStore::GetHideLifetime();
    if (totals.showLifetime) {
        totals.lifetime = FormatCount(db_.SumLifetime());
    }
    return totals;
}

void App::OnCommand(UINT id) {
    switch (id) {
        case IDM_VIEW_SALES:
            OnShowSales();
            break;
        case IDM_REFRESH_NOW:
            sync_.RequestManualRefresh();
            break;
        case IDM_SETTINGS:
            OnSettings();
            break;
        case IDM_OPEN_FINANCIALS:
            ::ShellExecuteW(hwnd_, L"open", kSteamworksFinancialsUrl,
                            nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case IDM_ABOUT:
            ::MessageBoxW(hwnd_,
                          L"Steam Sales Tray 0.1.0\n\n"
                          L"Net units sold, from your notification area.",
                          L"About Steam Sales Tray",
                          MB_OK | MB_ICONINFORMATION);
            break;
        case IDM_EXIT:
            ::DestroyWindow(hwnd_);
            break;
        default:
            break;
    }
}

}  // namespace app
