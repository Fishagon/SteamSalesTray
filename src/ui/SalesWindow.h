#pragma once

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
// window can be freely resized with persisted geometry.
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

    // Reloads the list from the database for the current period (no-op if the
    // window has not been created / is hidden).
    void Refresh();

    // Rebuilds the period list to reflect changed preferences (e.g. the
    // hide-lifetime toggle). No-op if the window has not been created.
    void OnSettingsChanged();

    void Destroy();

    HWND Handle() const { return hwnd_; }

private:
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(HWND, UINT, WPARAM, LPARAM);

    bool EnsureCreated();
    void CreateChildren();
    void LayoutChildren();
    void PopulatePeriodCombo();
    void ReloadList();
    void UpdateStatusLine();
    SalesPeriod CurrentPeriod() const;
    void SaveGeometry();
    void SaveColumnWidths();

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
    HWND btnClose_ = nullptr;
    HFONT font_ = nullptr;

    // Periods currently in the combo, in display order (Lifetime is omitted
    // when hidden). Maps combo index -> SalesPeriod.
    std::vector<SalesPeriod> periods_;
};

}  // namespace ui
