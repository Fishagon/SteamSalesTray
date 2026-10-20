#pragma once

#include <string>
#include <windows.h>

namespace tray {

// Summary figures shown as rows at the top of the menu. Defaults to an
// hyphen placeholder until real totals are available (Phase 4+).
struct SummaryTotals {
    std::wstring today     = L"-";
    std::wstring yesterday = L"-";
    std::wstring week      = L"-";
    std::wstring month     = L"-";
    std::wstring lifetime  = L"-";
    bool showLifetime = true;  // when false, the Lifetime row is omitted
};

// Builds and displays the tray context menu at screen point `pt`, owned by
// `owner`. Returns the selected command id (an IDM_* value), or 0 if dismissed.
UINT ShowTrayMenu(HWND owner, POINT pt, const SummaryTotals& totals);

}  // namespace tray
