#include "ui/SalesWindow.h"

#include <string>
#include <unordered_map>
#include <vector>
#include <commctrl.h>
#include <shlwapi.h>
#include <windowsx.h>

#include "platform/StringUtil.h"
#include "platform/TimeUtil.h"
#include "resources/Resource.h"
#include "storage/Database.h"
#include "storage/SettingsStore.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shlwapi.lib")

namespace ui {

namespace {
constexpr wchar_t kClassName[] = L"SteamSalesTraySalesWindow";
constexpr int kMargin = 10;
constexpr int kBtnW = 82;
constexpr int kBtnH = 26;
constexpr int kComboW = 150;
constexpr int kRowTop = 40;
constexpr int kTotalW = 180;

constexpr int kDefaultColWidths[4] = {200, 80, 70, 90};
constexpr int kColumnCount = 4;

// ListView group ids (Released/DLC/Unreleased grouping).
constexpr int kGroupReleased = 0;
constexpr int kGroupDlc = 1;
constexpr int kGroupUnreleased = 2;

// Formats a net wishlist balance with an explicit "+" for positive values;
// FormatThousands already renders negatives with a "-" sign.
std::wstring FormatSigned(std::int64_t value) {
    if (value > 0) {
        return L"+" + platform::FormatThousands(value);
    }
    return platform::FormatThousands(value);
}

// Demo and Playtest builds are just marketing copies of a real title with no
// sales/wishlist signal of their own that isn't already reflected in the base
// game, so they're noise in this list - hide by name rather than app_type
// since Steam's Playtest type ("beta") isn't even in its own documented list.
bool IsHiddenBuild(const std::wstring& displayName) {
    return ::StrStrIW(displayName.c_str(), L"demo") != nullptr ||
           ::StrStrIW(displayName.c_str(), L"playtest") != nullptr;
}

std::wstring RelativeUpdated(const std::string& lastSyncUtc) {
    if (lastSyncUtc.empty()) {
        return L"Not yet synced";
    }
    const long long secs = platform::SecondsSinceUtc(lastSyncUtc);
    if (secs < 90) {
        return L"Updated just now";
    }
    if (secs < 3600) {
        return L"Updated " + std::to_wstring(secs / 60) + L" min ago";
    }
    if (secs < 86400) {
        return L"Updated " + std::to_wstring(secs / 3600) + L"h ago";
    }
    return L"Updated " + std::to_wstring(secs / 86400) + L"d ago";
}
}  // namespace

SalesWindow::~SalesWindow() {
    Destroy();
}

bool SalesWindow::Initialize(HINSTANCE instance, HWND mainWindow,
                             storage::Database* db, HICON icon) {
    instance_ = instance;
    mainWindow_ = mainWindow;
    db_ = db;
    icon_ = icon;
    return true;
}

LRESULT CALLBACK SalesWindow::WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam,
                                           LPARAM lParam) {
    SalesWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<SalesWindow*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<SalesWindow*>(
            ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) {
        return self->HandleMessage(hwnd, msg, wParam, lParam);
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool SalesWindow::EnsureCreated() {
    if (hwnd_) {
        return true;
    }

    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &SalesWindow::WndProcThunk;
        wc.hInstance = instance_;
        wc.hIcon = icon_;
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = kClassName;
        if (!::RegisterClassExW(&wc)) {
            return false;
        }
        classRegistered = true;
    }

    int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = 480, h = 380;
    int sx, sy, sw, sh;
    if (storage::SettingsStore::GetSalesWindowRect(sx, sy, sw, sh)) {
        x = sx;
        y = sy;
        w = sw;
        h = sh;
    }

    ::CreateWindowExW(WS_EX_CONTROLPARENT, kClassName, L"Steam Product Sales",
                      WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr,
                      instance_, this);
    return hwnd_ != nullptr;
}

void SalesWindow::CreateChildren() {
    // Use the standard message font for all controls.
    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof(ncm);
    ::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    font_ = ::CreateFontIndirectW(&ncm.lfMessageFont);

    combo_ = ::CreateWindowExW(
        0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
        0, 0, kComboW, 220, hwnd_,
        reinterpret_cast<HMENU>(IDC_SALES_PERIOD), instance_, nullptr);

    status_ = ::CreateWindowExW(
        0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_RIGHT, 0, 0, 10, 10,
        hwnd_, reinterpret_cast<HMENU>(IDC_SALES_STATUS), instance_, nullptr);

    list_ = ::CreateWindowExW(
        WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS |
            LVS_SINGLESEL,
        0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(IDC_SALES_LIST),
        instance_, nullptr);
    ListView_SetExtendedListViewStyle(
        list_, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

    // Group the list into Released / DLC / Unreleased sections - the sales
    // pipeline alone never surfaces DLC or pre-release titles (see
    // SalesSyncService::SyncKnownApps), so this is the only place that
    // distinction shows up.
    ListView_EnableGroupView(list_, TRUE);
    auto insertGroup = [&](int id, const wchar_t* title) {
        LVGROUP group = {};
        group.cbSize = sizeof(group);
        group.mask = LVGF_HEADER | LVGF_GROUPID;
        group.pszHeader = const_cast<wchar_t*>(title);
        group.iGroupId = id;
        ListView_InsertGroup(list_, -1, &group);
    };
    insertGroup(kGroupReleased, L"Released");
    insertGroup(kGroupDlc, L"DLC");
    insertGroup(kGroupUnreleased, L"Unreleased (Wishlist Only)");

    total_ = ::CreateWindowExW(
        0, L"STATIC", L"Total: 0", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
        0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(IDC_SALES_TOTAL),
        instance_, nullptr);

    wishlist_ = ::CreateWindowExW(
        0, L"STATIC", L"Wishlist Balance: 0",
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 10, 10, hwnd_,
        reinterpret_cast<HMENU>(IDC_SALES_WISHLIST), instance_, nullptr);

    btnRefresh_ = ::CreateWindowExW(
        0, L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0,
        kBtnW, kBtnH, hwnd_, reinterpret_cast<HMENU>(IDC_SALES_REFRESH),
        instance_, nullptr);
    btnSettings_ = ::CreateWindowExW(
        0, L"BUTTON", L"Settings", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0,
        kBtnW, kBtnH, hwnd_, reinterpret_cast<HMENU>(IDC_SALES_SETTINGS),
        instance_, nullptr);
    btnClose_ = ::CreateWindowExW(
        0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        0, 0, kBtnW, kBtnH, hwnd_, reinterpret_cast<HMENU>(IDC_SALES_CLOSE),
        instance_, nullptr);

    HWND controls[] = {combo_,      status_,      list_,       total_,
                       wishlist_,   btnRefresh_,  btnSettings_, btnClose_};
    for (HWND c : controls) {
        if (c && font_) {
            ::SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        }
    }

    // ListView columns.
    const wchar_t* headers[kColumnCount] = {L"Product", L"App ID", L"Units",
                                            L"WL Balance"};
    for (int i = 0; i < kColumnCount; ++i) {
        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT | LVCF_SUBITEM;
        col.fmt = (i == 0) ? LVCFMT_LEFT : LVCFMT_RIGHT;
        col.cx = storage::SettingsStore::GetSalesColumnWidth(
            i, kDefaultColWidths[i]);
        col.pszText = const_cast<wchar_t*>(headers[i]);
        col.iSubItem = i;
        ListView_InsertColumn(list_, i, &col);
    }

    PopulatePeriodCombo();
}

void SalesWindow::PopulatePeriodCombo() {
    ::SendMessageW(combo_, CB_RESETCONTENT, 0, 0);
    periods_.clear();

    struct Entry { SalesPeriod period; const wchar_t* label; };
    const Entry all[] = {{SalesPeriod::Today, L"Today"},
                         {SalesPeriod::Yesterday, L"Yesterday"},
                         {SalesPeriod::Last7Days, L"Last 7 Days"},
                         {SalesPeriod::Last30Days, L"Last 30 Days"},
                         {SalesPeriod::Lifetime, L"Lifetime"}};
    const bool hideLifetime = storage::SettingsStore::GetHideLifetime();

    for (const Entry& e : all) {
        if (e.period == SalesPeriod::Lifetime && hideLifetime) {
            continue;
        }
        ::SendMessageW(combo_, CB_ADDSTRING, 0,
                       reinterpret_cast<LPARAM>(e.label));
        periods_.push_back(e.period);
    }

    // Restore the saved period by its enum value; fall back to the first entry
    // if it is no longer present (e.g. Lifetime was just hidden).
    const int savedValue = storage::SettingsStore::GetSalesPeriodIndex();
    int select = 0;
    for (size_t i = 0; i < periods_.size(); ++i) {
        if (static_cast<int>(periods_[i]) == savedValue) {
            select = static_cast<int>(i);
            break;
        }
    }
    ::SendMessageW(combo_, CB_SETCURSEL, select, 0);
}

SalesPeriod SalesWindow::CurrentPeriod() const {
    const LRESULT sel = ::SendMessageW(combo_, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR || sel < 0 ||
        static_cast<size_t>(sel) >= periods_.size()) {
        return SalesPeriod::Today;
    }
    return periods_[static_cast<size_t>(sel)];
}

void SalesWindow::LayoutChildren() {
    RECT rc;
    ::GetClientRect(hwnd_, &rc);
    const int W = rc.right;
    const int H = rc.bottom;
    const int m = kMargin;

    const int buttonsY = H - m - kBtnH;
    ::MoveWindow(combo_, m, m, kComboW, 220, TRUE);
    ::MoveWindow(status_, m + kComboW + 8, m + 3,
                 (W - m) - (m + kComboW + 8), 18, TRUE);

    const int closeX = W - m - kBtnW;
    const int settingsX = closeX - 6 - kBtnW;
    const int refreshX = settingsX - 6 - kBtnW;
    ::MoveWindow(btnRefresh_, refreshX, buttonsY, kBtnW, kBtnH, TRUE);
    ::MoveWindow(btnSettings_, settingsX, buttonsY, kBtnW, kBtnH, TRUE);
    ::MoveWindow(btnClose_, closeX, buttonsY, kBtnW, kBtnH, TRUE);
    ::MoveWindow(total_, m, buttonsY + 5, kTotalW, 18, TRUE);
    const int wishlistX = m + kTotalW + 12;
    const int wishlistW = (refreshX - 8) - wishlistX;
    ::MoveWindow(wishlist_, wishlistX, buttonsY + 5,
                 wishlistW > 0 ? wishlistW : 0, 18, TRUE);

    const int listTop = m + kRowTop - 6;
    const int listHeight = (buttonsY - 8) - listTop;
    ::MoveWindow(list_, m, listTop, W - 2 * m,
                 listHeight > 0 ? listHeight : 0, TRUE);
}

void SalesWindow::ReloadList() {
    if (!list_ || !db_) {
        return;
    }
    ListView_DeleteAllItems(list_);

    // Per-app period data, looked up by appId while building rows below.
    std::vector<storage::ProductUnits> unitsRows;
    std::vector<storage::ProductWishlistBalance> wishlistRows;
    switch (CurrentPeriod()) {
        case SalesPeriod::Today:
            unitsRows = db_->QueryDate(platform::SteamToday());
            wishlistRows =
                db_->QueryWishlistBalanceRowsForDate(platform::SteamToday());
            break;
        case SalesPeriod::Yesterday:
            unitsRows = db_->QueryDate(platform::SteamDateDaysAgo(1));
            wishlistRows = db_->QueryWishlistBalanceRowsForDate(
                platform::SteamDateDaysAgo(1));
            break;
        case SalesPeriod::Last7Days:
            unitsRows = db_->QueryRange(platform::SteamDateDaysAgo(6),
                                        platform::SteamToday());
            wishlistRows = db_->QueryWishlistBalanceRowsForRange(
                platform::SteamDateDaysAgo(6), platform::SteamToday());
            break;
        case SalesPeriod::Last30Days:
            unitsRows = db_->QueryRange(platform::SteamDateDaysAgo(29),
                                        platform::SteamToday());
            wishlistRows = db_->QueryWishlistBalanceRowsForRange(
                platform::SteamDateDaysAgo(29), platform::SteamToday());
            break;
        case SalesPeriod::Lifetime:
            unitsRows = db_->QueryLifetime();
            wishlistRows = db_->QueryWishlistBalanceRowsLifetime();
            break;
    }

    std::unordered_map<std::int64_t, std::int64_t> unitsByApp;
    for (const storage::ProductUnits& row : unitsRows) {
        unitsByApp[row.appId] = row.units;
    }
    std::unordered_map<std::int64_t, std::int64_t> wishlistByApp;
    for (const storage::ProductWishlistBalance& row : wishlistRows) {
        wishlistByApp[row.appId] = row.balance;
    }

    // The driving list is every known app (GetAllProducts), not just the ones
    // with sales this period - a DLC or unreleased title with 0 units still
    // needs a row so its wishlist balance is visible. Demo/Playtest builds and
    // rows with nothing to report this period (0 units and 0 balance) are
    // filtered out entirely (row and totals both), so the two stay consistent
    // with what's actually visible.
    std::int64_t total = 0;
    std::int64_t wishlistBalance = 0;
    int index = 0;
    for (const storage::ProductInfo& product : db_->GetAllProducts()) {
        if (IsHiddenBuild(product.displayName)) {
            continue;
        }
        const int groupId = (product.appType == "dlc")
                                ? kGroupDlc
                                : (product.everSold ? kGroupReleased
                                                    : kGroupUnreleased);
        const auto unitsIt = unitsByApp.find(product.appId);
        const std::int64_t units =
            unitsIt != unitsByApp.end() ? unitsIt->second : 0;
        const auto wishlistIt = wishlistByApp.find(product.appId);
        const std::int64_t balance =
            wishlistIt != wishlistByApp.end() ? wishlistIt->second : 0;
        if (units == 0 && balance == 0) {
            continue;  // nothing to report for this period - just noise
        }

        LVITEMW item = {};
        item.mask = LVIF_TEXT | LVIF_GROUPID;
        item.iItem = index;
        item.iSubItem = 0;
        item.pszText = const_cast<wchar_t*>(product.displayName.c_str());
        item.iGroupId = groupId;
        const int inserted = ListView_InsertItem(list_, &item);

        const std::wstring appId = std::to_wstring(product.appId);
        ListView_SetItemText(list_, inserted, 1,
                             const_cast<wchar_t*>(appId.c_str()));
        const std::wstring unitsText = platform::FormatThousands(units);
        ListView_SetItemText(list_, inserted, 2,
                             const_cast<wchar_t*>(unitsText.c_str()));
        const std::wstring wishlistText = FormatSigned(balance);
        ListView_SetItemText(list_, inserted, 3,
                             const_cast<wchar_t*>(wishlistText.c_str()));

        total += units;
        wishlistBalance += balance;
        ++index;
    }

    const std::wstring totalText =
        L"Total: " + platform::FormatThousands(total);
    ::SetWindowTextW(total_, totalText.c_str());

    const std::wstring wishlistText =
        L"Wishlist Balance: " + FormatSigned(wishlistBalance);
    ::SetWindowTextW(wishlist_, wishlistText.c_str());

    UpdateStatusLine();
}

void SalesWindow::UpdateStatusLine() {
    if (!status_ || !db_) {
        return;
    }
    const std::string lastError = db_->GetLastError();
    std::wstring text;
    if (!lastError.empty()) {
        text = L"⚠ " + platform::Utf8ToWide(lastError);
    } else {
        text = RelativeUpdated(db_->GetLastSuccessfulSyncUtc());
    }
    ::SetWindowTextW(status_, text.c_str());
}

void SalesWindow::SaveGeometry() {
    if (!hwnd_ || ::IsIconic(hwnd_)) {
        return;
    }
    RECT rc;
    ::GetWindowRect(hwnd_, &rc);
    storage::SettingsStore::SetSalesWindowRect(
        rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top);
}

void SalesWindow::SaveColumnWidths() {
    if (!list_) {
        return;
    }
    for (int i = 0; i < kColumnCount; ++i) {
        storage::SettingsStore::SetSalesColumnWidth(
            i, ListView_GetColumnWidth(list_, i));
    }
}

LRESULT SalesWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam,
                                   LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            CreateChildren();
            LayoutChildren();
            return 0;
        case WM_SIZE:
            LayoutChildren();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = 380;
            mmi->ptMinTrackSize.y = 260;
            return 0;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            const int code = HIWORD(wParam);
            if (id == IDC_SALES_PERIOD && code == CBN_SELCHANGE) {
                storage::SettingsStore::SetSalesPeriodIndex(
                    static_cast<int>(CurrentPeriod()));
                ReloadList();
                return 0;
            }
            if (id == IDC_SALES_REFRESH && code == BN_CLICKED) {
                ::SendMessageW(mainWindow_, WM_COMMAND, IDM_REFRESH_NOW, 0);
                ReloadList();
                return 0;
            }
            if (id == IDC_SALES_SETTINGS && code == BN_CLICKED) {
                ::SendMessageW(mainWindow_, WM_COMMAND, IDM_SETTINGS, 0);
                return 0;
            }
            if (id == IDC_SALES_CLOSE && code == BN_CLICKED) {
                ::SendMessageW(hwnd, WM_CLOSE, 0, 0);
                return 0;
            }
            return 0;
        }
        case WM_CLOSE:
            // Hide instead of destroy - the tray app keeps running.
            SaveGeometry();
            SaveColumnWidths();
            ::ShowWindow(hwnd, SW_HIDE);
            return 0;
        case WM_DESTROY:
            SaveGeometry();
            SaveColumnWidths();
            return 0;
        default:
            return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void SalesWindow::ShowAndFocus() {
    if (!EnsureCreated()) {
        return;
    }
    ReloadList();
    ::ShowWindow(hwnd_, SW_SHOW);
    ::SetForegroundWindow(hwnd_);
}

void SalesWindow::Refresh() {
    if (hwnd_ && ::IsWindowVisible(hwnd_)) {
        ReloadList();
    }
}

void SalesWindow::OnSettingsChanged() {
    if (!hwnd_ || !combo_) {
        return;
    }
    PopulatePeriodCombo();  // adds/removes Lifetime per the current setting
    ReloadList();
}

void SalesWindow::Destroy() {
    if (hwnd_) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (font_) {
        ::DeleteObject(font_);
        font_ = nullptr;
    }
}

}  // namespace ui
