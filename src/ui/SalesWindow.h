#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>

namespace storage { class Database; }

namespace ui {

// The period selector options, in display order (matches the combo box).
enum class SalesPeriod {
    Today = 0,
    Yesterday,
    Last7Days,
    Last30Days,
    Lifetime,
};

// A small, resizable, modeless native window showing per-product net units for
// a selected period, backed by a Win32 ListView in report mode. Closing hides
// it (the tray app keeps running); the app destroys it on exit.
//
// Not a dialog resource - controls are created and laid out by hand so the
// window can be freely resized with persisted geometry. Layout metrics are in
// 96-DPI units and scaled to the window's current DPI (per-monitor aware).
class SalesWindow {
public:
    SalesWindow() = default;
    ~SalesWindow();

    SalesWindow(const SalesWindow&) = delete;
    SalesWindow& operator=(const SalesWindow&) = delete;

    // `mainWindow` receives WM_COMMAND (IDM_*) for Refresh/Settings; `db` is a
    // UI-thread read connection owned by the caller; `icon` is the app icon.
    bool Initialize(HINSTANCE instance, HWND mainWindow, storage::Database* db,
                    HICON icon);

    // Creates (if needed), shows, brings to front, and reloads data.
    void ShowAndFocus();

    // Like ShowAndFocus, but first switches to `period` (falls back to the
    // first available period if it is hidden).
    void ShowPeriod(SalesPeriod period);

    // Reloads the list from the database for the current period (no-op if the
    // window has not been created / is hidden).
    void Refresh();

    // Reflects whether a sync is in flight (status text + Refresh button).
    void SetSyncing(bool syncing);

    // Rebuilds the period list to reflect changed preferences (e.g. the
    // hide-lifetime toggle). No-op if the window has not been created.
    void OnSettingsChanged();

    // Window-specific keyboard shortcuts (F5 = refresh). Call from the message
    // loop before IsDialogMessage; returns true if the message was consumed.
    bool PreTranslateMessage(const MSG& msg);

    void Destroy();

    HWND Handle() const { return hwnd_; }

private:
    // One visible list row, kept so the list can be sorted before insertion.
    struct Row {
        std::wstring name;
        std::int64_t appId = 0;
        std::int64_t units = 0;
        std::int64_t balance = 0;
        int groupId = 0;
    };

    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(HWND, UINT, WPARAM, LPARAM);

    bool EnsureCreated();
    void CreateChildren();
    void CreateFonts();
    void ApplyFonts();
    void LayoutChildren();
    void PopulatePeriodCombo();
    void ReloadList();
    void SortRows(std::vector<Row>& rows) const;
    void UpdateSortArrows();
    void UpdateStatusLine();
    void RequestRefresh();
    SalesPeriod CurrentPeriod() const;
    void SaveGeometry();
    void SaveColumnWidths();
    int Scale(int value) const { return ::MulDiv(value, dpi_, 96); }

    HINSTANCE instance_ = nullptr;
    HWND mainWindow_ = nullptr;
    storage::Database* db_ = nullptr;
    HICON icon_ = nullptr;

    HWND hwnd_ = nullptr;
    HWND combo_ = nullptr;
    HWND status_ = nullptr;
    HWND list_ = nullptr;
    HWND total_ = nullptr;
    HWND wishlist_ = nullptr;
    HWND btnRefresh_ = nullptr;
    HWND btnSettings_ = nullptr;
    HFONT font_ = nullptr;
    HFONT boldFont_ = nullptr;
    UINT dpi_ = 96;

    bool syncing_ = false;
    bool statusIsError_ = false;
    bool hasEverSynced_ = false;

    // Current sort: column index and direction. Defaults to Units, descending.
    int sortColumn_ = 2;
    bool sortAscending_ = false;

    // Periods currently in the combo, in display order (Lifetime is omitted
    // when hidden). Maps combo index -> SalesPeriod.
    std::vector<SalesPeriod> periods_;
};

}  // namespace ui
