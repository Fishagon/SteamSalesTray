#include "tray/TrayMenu.h"

#include "resources/Resource.h"

namespace tray {

namespace {
// Adds a summary row; choosing it opens the Sales window on that period. It
// stays enabled so the figures render at full contrast rather than grayed.
// A tab right-aligns the count column.
void AppendSummaryRow(HMENU menu, UINT id, const wchar_t* label,
                      const std::wstring& value) {
    std::wstring text = std::wstring(label) + L"\t" + value;
    ::AppendMenuW(menu, MF_STRING, id, text.c_str());
}

// Force `hwnd` to the foreground even when the caller lacks foreground rights.
// A right-click on a notification-area icon delivers the input to Explorer (the
// taskbar or the always-on-top hidden-icons flyout), so a plain
// SetForegroundWindow is refused by the foreground-lock rule and the flyout
// stays on top of our menu. Temporarily attaching our input queue to the
// current foreground thread lifts that restriction; we detach immediately
// after. This is the well-known workaround for a refused SetForegroundWindow.
void ForceForeground(HWND hwnd) {
    const HWND foreground = ::GetForegroundWindow();
    const DWORD myThread = ::GetCurrentThreadId();
    const DWORD fgThread =
        foreground ? ::GetWindowThreadProcessId(foreground, nullptr) : 0;

    if (fgThread && fgThread != myThread) {
        ::AttachThreadInput(myThread, fgThread, TRUE);
        ::SetForegroundWindow(hwnd);
        ::AttachThreadInput(myThread, fgThread, FALSE);
    } else {
        ::SetForegroundWindow(hwnd);
    }
}
}  // namespace

UINT ShowTrayMenu(HWND owner, POINT pt, const SummaryTotals& totals) {
    HMENU menu = ::CreatePopupMenu();
    if (!menu) {
        return 0;
    }

    // Disabled caption saying what the figures below are.
    ::AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"Net units sold");

    AppendSummaryRow(menu, IDM_SUMMARY_TODAY,     L"Today",       totals.today);
    AppendSummaryRow(menu, IDM_SUMMARY_YESTERDAY, L"Yesterday",   totals.yesterday);
    AppendSummaryRow(menu, IDM_SUMMARY_7DAY,     L"Last 7 days",  totals.week);
    AppendSummaryRow(menu, IDM_SUMMARY_30DAY,    L"Last 30 days", totals.month);
    if (totals.showLifetime) {
        AppendSummaryRow(menu, IDM_SUMMARY_LIFETIME, L"Lifetime",
                         totals.lifetime);
    }

    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, IDM_VIEW_SALES, L"View Product Sales");
    ::AppendMenuW(menu, MF_STRING, IDM_REFRESH_NOW, L"Refresh Now");
    ::AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"Settings…");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, IDM_OPEN_FINANCIALS,
                  L"Open Steamworks Financials");
    ::AppendMenuW(menu, MF_STRING, IDM_ABOUT, L"About Steam Sales Tray");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");

    // Bold default item = what clicking the tray icon does.
    ::SetMenuDefaultItem(menu, IDM_VIEW_SALES, FALSE);

    // Take the foreground so the notification-area flyout dismisses and our
    // menu owns the z-order. ForceForeground works around the foreground-lock
    // rule that otherwise silently refuses the switch; the trailing WM_NULL post
    // is Microsoft's documented companion (KB Q135788) so the menu dismisses
    // cleanly when the user clicks elsewhere.
    ForceForeground(owner);

    const UINT cmd = static_cast<UINT>(::TrackPopupMenuEx(
        menu,
        TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
        pt.x, pt.y, owner, nullptr));

    ::PostMessageW(owner, WM_NULL, 0, 0);

    ::DestroyMenu(menu);
    return cmd;
}

}  // namespace tray
