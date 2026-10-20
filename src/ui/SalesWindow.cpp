#include "ui/SalesWindow.h"

#include <algorithm>
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

// Layout metrics in 96-DPI units; scaled via SalesWindow::Scale.
constexpr int kMargin = 12;
constexpr int kGap = 8;
constexpr int kBtnW = 88;
constexpr int kBtnH = 26;
constexpr int kBtnGap = 6;
constexpr int kComboW = 160;
constexpr int kComboDropH = 200;
constexpr int kFooterGap = 16;
constexpr int kDefaultW = 520;
constexpr int kDefaultH = 420;
constexpr int kMinW = 400;
constexpr int kMinH = 280;

// Persisted column widths are stored in 96-DPI units too.
constexpr int kDefaultColWidths[4] = {220, 70, 70, 90};
constexpr int kColumnCount = 4;
constexpr int kColProduct = 0;
constexpr int kColAppId = 1;
constexpr int kColUnits = 2;
constexpr int kColWishlist = 3;

// Re-renders the "Updated N min ago" status while the window is visible.
constexpr UINT_PTR kStatusTimerId = 1;
constexpr UINT kStatusTimerMs = 60 * 1000;

constexpr COLORREF kErrorColor = RGB(196, 43, 28);

// ListView group ids (Released/DLC/Unreleased grouping), in display order.
constexpr int kGroupReleased = 0;
constexpr int kGroupDlc = 1;
constexpr int kGroupUnreleased = 2;
constexpr const wchar_t* kGroupTitles[] = {L"Released", L"DLC",
                                           L"Unreleased (wishlists only)"};

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
        return L"Not synced yet";
    }
    const long long secs = platform::SecondsSinceUtc(lastSyncUtc);
    if (secs < 90) {
        return L"Updated just now";
    }
    if (secs < 3600) {
        return L"Updated " + std::to_wstring(secs / 60) + L" min ago";
    }
    if (secs < 86400) {
        return L"Updated " + std::to_wstring(secs / 3600) + L" h ago";
    }
    return L"Updated " + std::to_wstring(secs / 86400) + L" d ago";
}

std::wstring GetText(HWND hwnd) {
    const int len = ::GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    const int copied =
        ::GetWindowTextW(hwnd, text.data(), static_cast<int>(text.size()));
    text.resize(copied > 0 ? static_cast<size_t>(copied) : 0);
    return text;
}

// Width in pixels of `hwnd`'s current text drawn in `font`.
int TextWidth(HWND hwnd, HFONT font) {
    const std::wstring text = GetText(hwnd);
    HDC dc = ::GetDC(hwnd);
    HGDIOBJ old = ::SelectObject(dc, font);
    SIZE size = {};
    ::GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()),
                            &size);
    ::SelectObject(dc, old);
    ::ReleaseDC(hwnd, dc);
    return size.cx;
}

int TextHeight(HWND hwnd, HFONT font) {
    HDC dc = ::GetDC(hwnd);
    HGDIOBJ old = ::SelectObject(dc, font);
    TEXTMETRICW tm = {};
    ::GetTextMetricsW(dc, &tm);
    ::SelectObject(dc, old);
    ::ReleaseDC(hwnd, dc);
    return tm.tmHeight;
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
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = kClassName;
        if (!::RegisterClassExW(&wc)) {
            return false;
        }
        classRegistered = true;
    }

    // Saved geometry is in physical pixels; defaults are scaled to the system
    // DPI (the window's own DPI isn't known until it exists).
    const UINT systemDpi = ::GetDpiForSystem();
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    int w = ::MulDiv(kDefaultW, systemDpi, 96);
    int h = ::MulDiv(kDefaultH, systemDpi, 96);
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

void SalesWindow::CreateFonts() {
    if (font_) {
        ::DeleteObject(font_);
        font_ = nullptr;
    }
    if (boldFont_) {
        ::DeleteObject(boldFont_);
        boldFont_ = nullptr;
    }
    // The standard message font, at this window's DPI.
    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof(ncm);
    ::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0,
                                 dpi_);
    font_ = ::CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW bold = ncm.lfMessageFont;
    bold.lfWeight = FW_SEMIBOLD;
    boldFont_ = ::CreateFontIndirectW(&bold);
}

void SalesWindow::ApplyFonts() {
    HWND controls[] = {combo_,    status_,     list_,       wishlist_,
                       btnRefresh_, btnSettings_};
    for (HWND c : controls) {
        if (c && font_) {
            ::SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        }
    }
    if (total_ && boldFont_) {
        ::SendMessageW(total_, WM_SETFONT, reinterpret_cast<WPARAM>(boldFont_),
                       TRUE);
    }
}

void SalesWindow::CreateChildren() {
    dpi_ = ::GetDpiForWindow(hwnd_);
    CreateFonts();

    combo_ = ::CreateWindowExW(
        0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
        0, 0, Scale(kComboW), Scale(kComboDropH), hwnd_,
        reinterpret_cast<HMENU>(IDC_SALES_PERIOD), instance_, nullptr);

    btnRefresh_ = ::CreateWindowExW(
        0, L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0,
        Scale(kBtnW), Scale(kBtnH), hwnd_,
        reinterpret_cast<HMENU>(IDC_SALES_REFRESH), instance_, nullptr);
    btnSettings_ = ::CreateWindowExW(
        0, L"BUTTON", L"Settings…", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0,
        Scale(kBtnW), Scale(kBtnH), hwnd_,
        reinterpret_cast<HMENU>(IDC_SALES_SETTINGS), instance_, nullptr);

    list_ = ::CreateWindowExW(
        0, WC_LISTVIEWW, nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS |
            LVS_SINGLESEL,
        0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(IDC_SALES_LIST),
        instance_, nullptr);
    ListView_SetExtendedListViewStyle(
        list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

    // Group the list into Released / DLC / Unreleased sections - the sales
    // pipeline alone never surfaces DLC or pre-release titles (see
    // SalesSyncService::SyncKnownApps), so this is the only place that
    // distinction shows up. Groups with no rows are not drawn.
    ListView_EnableGroupView(list_, TRUE);
    for (int id : {kGroupReleased, kGroupDlc, kGroupUnreleased}) {
        LVGROUP group = {};
        group.cbSize = sizeof(group);
        group.mask = LVGF_HEADER | LVGF_GROUPID | LVGF_STATE;
        group.pszHeader = const_cast<wchar_t*>(kGroupTitles[id]);
        group.iGroupId = id;
        group.stateMask = LVGS_COLLAPSIBLE;
        group.state = LVGS_COLLAPSIBLE;
        ListView_InsertGroup(list_, -1, &group);
    }

    total_ = ::CreateWindowExW(
        0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
        0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(IDC_SALES_TOTAL),
        instance_, nullptr);
    wishlist_ = ::CreateWindowExW(
        0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 10,
        10, hwnd_, reinterpret_cast<HMENU>(IDC_SALES_WISHLIST), instance_,
        nullptr);
    status_ = ::CreateWindowExW(
        0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_RIGHT | SS_ENDELLIPSIS | SS_NOPREFIX, 0, 0,
        10, 10, hwnd_, reinterpret_cast<HMENU>(IDC_SALES_STATUS), instance_,
        nullptr);

    ApplyFonts();

    // ListView columns.
    const wchar_t* headers[kColumnCount] = {L"Product", L"App ID", L"Units",
                                            L"Wishlists"};
    for (int i = 0; i < kColumnCount; ++i) {
        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT | LVCF_SUBITEM;
        col.fmt = (i == kColProduct) ? LVCFMT_LEFT : LVCFMT_RIGHT;
        col.cx = Scale(storage::SettingsStore::GetSalesColumnWidth(
            i, kDefaultColWidths[i]));
        col.pszText = const_cast<wchar_t*>(headers[i]);
        col.iSubItem = i;
        ListView_InsertColumn(list_, i, &col);
    }
    UpdateSortArrows();

    PopulatePeriodCombo();
    ::EnableWindow(btnRefresh_, !syncing_);
}

void SalesWindow::PopulatePeriodCombo() {
    ::SendMessageW(combo_, CB_RESETCONTENT, 0, 0);
    periods_.clear();

    struct Entry { SalesPeriod period; const wchar_t* label; };
    const Entry all[] = {{SalesPeriod::Today, L"Today"},
                         {SalesPeriod::Yesterday, L"Yesterday"},
                         {SalesPeriod::Last7Days, L"Last 7 days"},
                         {SalesPeriod::Last30Days, L"Last 30 days"},
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
    if (!list_) {
        return;
    }
    RECT rc;
    ::GetClientRect(hwnd_, &rc);
    const int W = rc.right;
    const int H = rc.bottom;
    const int m = Scale(kMargin);
    const int btnW = Scale(kBtnW);
    const int btnH = Scale(kBtnH);

    // Top row: period selector on the left, actions on the right. The combo's
    // visible height is font-driven, so center everything on the taller one.
    RECT comboRc;
    ::GetWindowRect(combo_, &comboRc);
    const int comboH = comboRc.bottom - comboRc.top;
    const int rowH = std::max(comboH, btnH);

    ::MoveWindow(combo_, m, m + (rowH - comboH) / 2, Scale(kComboW),
                 Scale(kComboDropH), TRUE);
    const int settingsX = W - m - btnW;
    const int refreshX = settingsX - Scale(kBtnGap) - btnW;
    const int btnY = m + (rowH - btnH) / 2;
    ::MoveWindow(btnRefresh_, refreshX, btnY, btnW, btnH, TRUE);
    ::MoveWindow(btnSettings_, settingsX, btnY, btnW, btnH, TRUE);

    // Footer: totals on the left, sync status right-aligned in what remains.
    const int lineH = TextHeight(total_, boldFont_);
    const int footerY = H - m - lineH;
    const int totalW = TextWidth(total_, boldFont_);
    ::MoveWindow(total_, m, footerY, totalW, lineH, TRUE);
    const int wishlistX = m + totalW + Scale(kFooterGap);
    const int wishlistW = TextWidth(wishlist_, font_);
    ::MoveWindow(wishlist_, wishlistX, footerY, wishlistW, lineH, TRUE);
    const int statusX = wishlistX + wishlistW + Scale(kFooterGap);
    ::MoveWindow(status_, statusX, footerY, std::max(0, (W - m) - statusX),
                 lineH, TRUE);

    const int listTop = m + rowH + Scale(kGap);
    const int listBottom = footerY - Scale(kGap);
    ::MoveWindow(list_, m, listTop, std::max(0, W - 2 * m),
                 std::max(0, listBottom - listTop), TRUE);
}

void SalesWindow::SortRows(std::vector<Row>& rows) const {
    const int column = sortColumn_;
    const bool ascending = sortAscending_;
    std::stable_sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
        int cmp = 0;
        switch (column) {
            case kColAppId:
                cmp = (a.appId > b.appId) - (a.appId < b.appId);
                break;
            case kColUnits:
                cmp = (a.units > b.units) - (a.units < b.units);
                break;
            case kColWishlist:
                cmp = (a.balance > b.balance) - (a.balance < b.balance);
                break;
            default:
                break;
        }
        if (cmp == 0) {
            // Product column, and the tie-breaker for numeric columns
            // (always A-Z so equal rows don't flip when toggling direction).
            const int byName = ::StrCmpLogicalW(a.name.c_str(), b.name.c_str());
            if (column != kColProduct) {
                return byName < 0;
            }
            cmp = byName;
        }
        return ascending ? cmp < 0 : cmp > 0;
    });
}

void SalesWindow::UpdateSortArrows() {
    HWND header = ListView_GetHeader(list_);
    for (int i = 0; i < kColumnCount; ++i) {
        HDITEMW hdi = {};
        hdi.mask = HDI_FORMAT;
        Header_GetItem(header, i, &hdi);
        hdi.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (i == sortColumn_) {
            hdi.fmt |= sortAscending_ ? HDF_SORTUP : HDF_SORTDOWN;
        }
        Header_SetItem(header, i, &hdi);
    }
}

void SalesWindow::ReloadList() {
    if (!list_ || !db_) {
        return;
    }

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
    std::vector<Row> rows;
    std::int64_t total = 0;
    std::int64_t wishlistBalance = 0;
    int groupCounts[3] = {};
    for (const storage::ProductInfo& product : db_->GetAllProducts()) {
        if (IsHiddenBuild(product.displayName)) {
            continue;
        }
        const auto unitsIt = unitsByApp.find(product.appId);
        const std::int64_t units =
            unitsIt != unitsByApp.end() ? unitsIt->second : 0;
        const auto wishlistIt = wishlistByApp.find(product.appId);
        const std::int64_t balance =
            wishlistIt != wishlistByApp.end() ? wishlistIt->second : 0;
        if (units == 0 && balance == 0) {
            continue;  // nothing to report for this period - just noise
        }

        Row row;
        row.name = product.displayName;
        row.appId = product.appId;
        row.units = units;
        row.balance = balance;
        row.groupId = (product.appType == "dlc")
                          ? kGroupDlc
                          : (product.everSold ? kGroupReleased
                                              : kGroupUnreleased);
        ++groupCounts[row.groupId];
        total += units;
        wishlistBalance += balance;
        rows.push_back(std::move(row));
    }
    SortRows(rows);

    ::SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list_);
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        const Row& row = rows[static_cast<size_t>(i)];
        LVITEMW item = {};
        item.mask = LVIF_TEXT | LVIF_GROUPID;
        item.iItem = i;
        item.pszText = const_cast<wchar_t*>(row.name.c_str());
        item.iGroupId = row.groupId;
        const int inserted = ListView_InsertItem(list_, &item);

        const std::wstring appId = std::to_wstring(row.appId);
        ListView_SetItemText(list_, inserted, kColAppId,
                             const_cast<wchar_t*>(appId.c_str()));
        const std::wstring unitsText = platform::FormatThousands(row.units);
        ListView_SetItemText(list_, inserted, kColUnits,
                             const_cast<wchar_t*>(unitsText.c_str()));
        const std::wstring wishlistText = FormatSigned(row.balance);
        ListView_SetItemText(list_, inserted, kColWishlist,
                             const_cast<wchar_t*>(wishlistText.c_str()));
    }

    // Group headers carry their row counts, e.g. "Released (3)".
    for (int id : {kGroupReleased, kGroupDlc, kGroupUnreleased}) {
        std::wstring header = kGroupTitles[id];
        header += L" (" + std::to_wstring(groupCounts[id]) + L")";
        LVGROUP group = {};
        group.cbSize = sizeof(group);
        group.mask = LVGF_HEADER;
        group.pszHeader = header.data();
        ListView_SetGroupInfo(list_, id, &group);
    }
    ::SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
    ::InvalidateRect(list_, nullptr, TRUE);

    const std::wstring totalText =
        platform::FormatThousands(total) + (total == 1 ? L" unit" : L" units");
    ::SetWindowTextW(total_, totalText.c_str());
    const std::wstring wishlistText =
        FormatSigned(wishlistBalance) +
        (wishlistBalance == 1 || wishlistBalance == -1 ? L" wishlist"
                                                       : L" wishlists");
    ::SetWindowTextW(wishlist_, wishlistText.c_str());

    hasEverSynced_ = !db_->GetLastSuccessfulSyncUtc().empty();
    UpdateStatusLine();
}

void SalesWindow::UpdateStatusLine() {
    if (!status_ || !db_) {
        return;
    }
    const std::string lastError = db_->GetLastError();
    std::wstring text;
    statusIsError_ = false;
    if (syncing_) {
        text = L"Updating…";
    } else if (!lastError.empty()) {
        text = L"⚠ " + platform::Utf8ToWide(lastError);
        statusIsError_ = true;
    } else {
        text = RelativeUpdated(db_->GetLastSuccessfulSyncUtc());
    }
    ::SetWindowTextW(status_, text.c_str());
    LayoutChildren();  // footer widths follow their text
    ::InvalidateRect(status_, nullptr, TRUE);
}

void SalesWindow::RequestRefresh() {
    ::SendMessageW(mainWindow_, WM_COMMAND, IDM_REFRESH_NOW, 0);
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
            i, ::MulDiv(ListView_GetColumnWidth(list_, i), 96, dpi_));
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
            mmi->ptMinTrackSize.x = Scale(kMinW);
            mmi->ptMinTrackSize.y = Scale(kMinH);
            return 0;
        }
        case WM_DPICHANGED: {
            const UINT oldDpi = dpi_;
            dpi_ = HIWORD(wParam);
            CreateFonts();
            ApplyFonts();
            for (int i = 0; i < kColumnCount; ++i) {
                ListView_SetColumnWidth(
                    list_, i,
                    ::MulDiv(ListView_GetColumnWidth(list_, i), dpi_, oldDpi));
            }
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            ::SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                           suggested->right - suggested->left,
                           suggested->bottom - suggested->top,
                           SWP_NOZORDER | SWP_NOACTIVATE);
            LayoutChildren();
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            // Statics sit on the window-colored background; the status line
            // is de-emphasized (or red for an error).
            HDC dc = reinterpret_cast<HDC>(wParam);
            const HWND ctl = reinterpret_cast<HWND>(lParam);
            COLORREF color = ::GetSysColor(COLOR_WINDOWTEXT);
            if (ctl == status_) {
                color = statusIsError_ ? kErrorColor
                                       : ::GetSysColor(COLOR_GRAYTEXT);
            }
            ::SetTextColor(dc, color);
            ::SetBkColor(dc, ::GetSysColor(COLOR_WINDOW));
            return reinterpret_cast<LRESULT>(::GetSysColorBrush(COLOR_WINDOW));
        }
        case WM_CTLCOLORBTN:
            // Themed push buttons paint their corners with this brush.
            return reinterpret_cast<LRESULT>(::GetSysColorBrush(COLOR_WINDOW));
        case WM_TIMER:
            if (wParam == kStatusTimerId) {
                UpdateStatusLine();
            }
            return 0;
        case WM_NOTIFY: {
            const auto* hdr = reinterpret_cast<const NMHDR*>(lParam);
            if (hdr->hwndFrom != list_) {
                break;
            }
            if (hdr->code == LVN_COLUMNCLICK) {
                const int column =
                    reinterpret_cast<const NMLISTVIEW*>(lParam)->iSubItem;
                if (column == sortColumn_) {
                    sortAscending_ = !sortAscending_;
                } else {
                    // Names read best A-Z; numbers biggest-first.
                    sortColumn_ = column;
                    sortAscending_ = (column == kColProduct);
                }
                UpdateSortArrows();
                ReloadList();
                return 0;
            }
            if (hdr->code == LVN_GETEMPTYMARKUP) {
                auto* markup = reinterpret_cast<NMLVEMPTYMARKUP*>(lParam);
                markup->dwFlags = EMF_CENTERED;
                const wchar_t* text =
                    hasEverSynced_
                        ? L"No sales or wishlist activity in this period."
                        : L"No sales data yet.\nAdd your Steam Financial API "
                          L"key in Settings to start syncing.";
                wcsncpy_s(markup->szMarkup, text, _TRUNCATE);
                return TRUE;
            }
            break;
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
                RequestRefresh();
                return 0;
            }
            if (id == IDC_SALES_SETTINGS && code == BN_CLICKED) {
                ::SendMessageW(mainWindow_, WM_COMMAND, IDM_SETTINGS, 0);
                return 0;
            }
            if (id == IDCANCEL) {
                // Escape (routed here by IsDialogMessage).
                ::SendMessageW(hwnd, WM_CLOSE, 0, 0);
                return 0;
            }
            return 0;
        }
        case WM_CLOSE:
            // Hide instead of destroy - the tray app keeps running.
            SaveGeometry();
            SaveColumnWidths();
            ::KillTimer(hwnd, kStatusTimerId);
            ::ShowWindow(hwnd, SW_HIDE);
            return 0;
        case WM_DESTROY:
            SaveGeometry();
            SaveColumnWidths();
            ::KillTimer(hwnd, kStatusTimerId);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool SalesWindow::PreTranslateMessage(const MSG& msg) {
    if (!hwnd_ || msg.message != WM_KEYDOWN || msg.wParam != VK_F5) {
        return false;
    }
    if (msg.hwnd != hwnd_ && !::IsChild(hwnd_, msg.hwnd)) {
        return false;
    }
    if (!syncing_) {
        RequestRefresh();
    }
    return true;
}

void SalesWindow::ShowAndFocus() {
    if (!EnsureCreated()) {
        return;
    }
    ReloadList();
    ::ShowWindow(hwnd_, ::IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
    ::SetForegroundWindow(hwnd_);
    ::SetFocus(list_);
    ::SetTimer(hwnd_, kStatusTimerId, kStatusTimerMs, nullptr);
}

void SalesWindow::ShowPeriod(SalesPeriod period) {
    storage::SettingsStore::SetSalesPeriodIndex(static_cast<int>(period));
    if (hwnd_) {
        PopulatePeriodCombo();  // re-selects the just-saved period
    }
    ShowAndFocus();
}

void SalesWindow::Refresh() {
    if (hwnd_ && ::IsWindowVisible(hwnd_)) {
        ReloadList();
    }
}

void SalesWindow::SetSyncing(bool syncing) {
    if (syncing == syncing_) {
        return;
    }
    syncing_ = syncing;
    if (!hwnd_) {
        return;
    }
    if (syncing && ::GetFocus() == btnRefresh_) {
        ::SetFocus(list_);  // a disabled button would strand keyboard focus
    }
    ::EnableWindow(btnRefresh_, !syncing);
    UpdateStatusLine();
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
    if (boldFont_) {
        ::DeleteObject(boldFont_);
        boldFont_ = nullptr;
    }
}

}  // namespace ui
