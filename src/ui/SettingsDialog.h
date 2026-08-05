#pragma once

#include <functional>
#include <string>
#include <windows.h>

#include "security/SecureString.h"

namespace ui {

// Result of validating an API key against Steam. `couldTest` is false when
// validation was not attempted (e.g. offline, or the network client is not
// available yet); in that case `ok` is meaningless.
struct KeyTestResult {
    bool ok = false;
    bool couldTest = false;
    std::wstring message;
};

// Validates a candidate key (UTF-8 bytes). Supplied by the caller so the dialog
// stays decoupled from the network client (wired up in Phase 3).
using KeyValidator =
    std::function<KeyTestResult(const security::SecureString&)>;

// What the dialog changed, so the caller can react (update tray, restart the
// sync worker, etc.).
struct SettingsOutcome {
    bool saved = false;            // user pressed Save
    bool keyChanged = false;       // a new key was stored, or the key removed
    bool cacheCleared = false;     // user cleared the local sales cache
    bool preferencesChanged = false;  // interval/notify/startup changed (sync)
    bool displayChanged = false;      // display-only pref changed (no resync)
    // Publisher key (GetPartnerAppListForWebAPIKey - DLC/Unreleased detection)
    // changed, stored/removed separately from the Financial key above.
    bool publisherKeyChanged = false;
};

// Shows the modal settings dialog. `validator` may be empty (Test Connection
// then reports that testing is unavailable).
SettingsOutcome ShowSettingsDialog(HWND parent, HINSTANCE instance,
                                    const KeyValidator& validator);

}  // namespace ui
