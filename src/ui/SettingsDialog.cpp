#include "ui/SettingsDialog.h"

#include <array>
#include <vector>
#include <windows.h>
#include <commctrl.h>

#include "platform/Paths.h"
#include "platform/StartupManager.h"
#include "resources/Resource.h"
#include "security/DpapiSecretStore.h"
#include "storage/SettingsStore.h"

namespace ui {

namespace {

constexpr wchar_t kPasswordChar = 0x25CF;  // ●

struct IntervalOption {
    storage::RefreshInterval value;
    const wchar_t* label;
};

// Display order per plan §12.
constexpr std::array<IntervalOption, 6> kIntervals = {{
    {storage::RefreshInterval::Min15, L"15 minutes"},
    {storage::RefreshInterval::Min30, L"30 minutes"},
    {storage::RefreshInterval::Min60, L"60 minutes"},
    {storage::RefreshInterval::Hours2, L"2 hours"},
    {storage::RefreshInterval::Hours6, L"6 hours"},
    {storage::RefreshInterval::Manual, L"Manual only"},
}};

constexpr int kMinResyncDays = 1;
constexpr int kMaxResyncDays = 180;

// Manual digit parse (no exceptions, matching this codebase's style).
// Returns false for empty/non-numeric input or a value outside the allowed
// [kMinResyncDays, kMaxResyncDays] range.
bool ParseResyncDays(const std::wstring& text, int& days) {
    if (text.empty() || text.size() > 3) {
        return false;
    }
    int value = 0;
    for (wchar_t c : text) {
        if (c < L'0' || c > L'9') {
            return false;
        }
        value = value * 10 + (c - L'0');
    }
    if (value < kMinResyncDays || value > kMaxResyncDays) {
        return false;
    }
    days = value;
    return true;
}

std::wstring TrimWhitespace(const std::wstring& s) {
    const wchar_t* ws = L" \t\r\n\f\v";
    const size_t begin = s.find_first_not_of(ws);
    if (begin == std::wstring::npos) {
        return {};
    }
    const size_t end = s.find_last_not_of(ws);
    return s.substr(begin, end - begin + 1);
}

// Reads an edit control's text, then wipes the temporary buffer.
std::wstring GetEditText(HWND edit) {
    const int len = ::GetWindowTextLengthW(edit);
    if (len <= 0) {
        return {};
    }
    std::wstring buffer(static_cast<size_t>(len) + 1, L'\0');
    const int copied = ::GetWindowTextW(edit, buffer.data(),
                                        static_cast<int>(buffer.size()));
    buffer.resize(copied > 0 ? static_cast<size_t>(copied) : 0);
    return buffer;
}

// Per-dialog state, stashed via DWLP_USER.
struct DialogState {
    HINSTANCE instance = nullptr;
    const KeyValidator* validator = nullptr;
    security::DpapiSecretStore store;
    security::DpapiSecretStore publisherStore{platform::GetPublisherApiKeyPath()};
    SettingsOutcome outcome;

    // Snapshot of the initial preference values, to detect changes.
    storage::RefreshInterval initialInterval = storage::RefreshInterval::Min30;
    bool initialNotify = false;
    bool initialStartup = false;
    bool initialHideLifetime = false;
};

void SetKeyStatusLabel(HWND dlg, const DialogState& state) {
    const bool has = state.store.HasStoredKey();
    ::SetDlgItemTextW(dlg, IDC_STATIC_KEYSTATUS,
                      has ? L"A stored API key is configured. Enter a new key "
                            L"to replace it."
                          : L"No API key stored. Enter one to enable syncing.");
}

void SetPublisherKeyStatusLabel(HWND dlg, const DialogState& state) {
    const bool has = state.publisherStore.HasStoredKey();
    ::SetDlgItemTextW(
        dlg, IDC_STATIC_PUBLISHER_KEYSTATUS,
        has ? L"A stored publisher key is configured. Enter a new key to "
              L"replace it."
            : L"No publisher key stored - DLC/Unreleased titles won't be "
              L"detected.");
}

void PopulateIntervalCombo(HWND dlg, storage::RefreshInterval current) {
    HWND combo = ::GetDlgItem(dlg, IDC_COMBO_INTERVAL);
    int selectIndex = 0;
    for (size_t i = 0; i < kIntervals.size(); ++i) {
        ::SendMessageW(combo, CB_ADDSTRING, 0,
                       reinterpret_cast<LPARAM>(kIntervals[i].label));
        ::SendMessageW(combo, CB_SETITEMDATA, i,
                       static_cast<LPARAM>(static_cast<int>(kIntervals[i].value)));
        if (kIntervals[i].value == current) {
            selectIndex = static_cast<int>(i);
        }
    }
    ::SendMessageW(combo, CB_SETCURSEL, selectIndex, 0);
}

storage::RefreshInterval SelectedInterval(HWND dlg) {
    HWND combo = ::GetDlgItem(dlg, IDC_COMBO_INTERVAL);
    const LRESULT sel = ::SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR) {
        return storage::RefreshInterval::Min30;
    }
    const LRESULT data = ::SendMessageW(combo, CB_GETITEMDATA, sel, 0);
    return static_cast<storage::RefreshInterval>(static_cast<int>(data));
}

void OnInitDialog(HWND dlg, DialogState* state) {
    // Cue banner shown when the edit is empty and unfocused.
    ::SendDlgItemMessageW(dlg, IDC_EDIT_APIKEY, EM_SETCUEBANNER, TRUE,
                          reinterpret_cast<LPARAM>(
                              L"Enter a new key to replace the stored one"));
    ::SendDlgItemMessageW(dlg, IDC_EDIT_APIKEY, EM_SETPASSWORDCHAR,
                          kPasswordChar, 0);
    ::SendDlgItemMessageW(dlg, IDC_EDIT_APIKEY, EM_SETLIMITTEXT, 512, 0);

    ::SendDlgItemMessageW(
        dlg, IDC_EDIT_PUBLISHER_APIKEY, EM_SETCUEBANNER, TRUE,
        reinterpret_cast<LPARAM>(
            L"Your normal Steamworks Web API key (not the Financial key)"));
    ::SendDlgItemMessageW(dlg, IDC_EDIT_PUBLISHER_APIKEY, EM_SETPASSWORDCHAR,
                          kPasswordChar, 0);
    ::SendDlgItemMessageW(dlg, IDC_EDIT_PUBLISHER_APIKEY, EM_SETLIMITTEXT, 512,
                         0);

    ::SendDlgItemMessageW(dlg, IDC_EDIT_RESYNC_DAYS, EM_SETLIMITTEXT, 3, 0);
    ::SetDlgItemTextW(dlg, IDC_EDIT_RESYNC_DAYS, L"45");

    SetKeyStatusLabel(dlg, *state);
    SetPublisherKeyStatusLabel(dlg, *state);

    state->initialInterval = storage::SettingsStore::GetRefreshInterval();
    state->initialNotify = storage::SettingsStore::GetNotifyOnNewUnits();
    state->initialStartup = platform::StartupManager::IsEnabled();
    state->initialHideLifetime = storage::SettingsStore::GetHideLifetime();

    PopulateIntervalCombo(dlg, state->initialInterval);
    ::CheckDlgButton(dlg, IDC_CHECK_NOTIFY,
                     state->initialNotify ? BST_CHECKED : BST_UNCHECKED);
    ::CheckDlgButton(dlg, IDC_CHECK_STARTUP,
                     state->initialStartup ? BST_CHECKED : BST_UNCHECKED);
    ::CheckDlgButton(dlg, IDC_CHECK_HIDELIFETIME,
                     state->initialHideLifetime ? BST_CHECKED : BST_UNCHECKED);

    ::SetForegroundWindow(dlg);
}

void OnToggleShowKey(HWND dlg) {
    const bool show = ::IsDlgButtonChecked(dlg, IDC_CHECK_SHOWKEY) == BST_CHECKED;
    ::SendDlgItemMessageW(dlg, IDC_EDIT_APIKEY, EM_SETPASSWORDCHAR,
                          show ? 0 : kPasswordChar, 0);
    ::InvalidateRect(::GetDlgItem(dlg, IDC_EDIT_APIKEY), nullptr, TRUE);

    ::SendDlgItemMessageW(dlg, IDC_EDIT_PUBLISHER_APIKEY, EM_SETPASSWORDCHAR,
                          show ? 0 : kPasswordChar, 0);
    ::InvalidateRect(::GetDlgItem(dlg, IDC_EDIT_PUBLISHER_APIKEY), nullptr,
                     TRUE);
}

void OnTestConnection(HWND dlg, DialogState* state) {
    std::wstring text = TrimWhitespace(GetEditText(::GetDlgItem(dlg,
                                                    IDC_EDIT_APIKEY)));
    if (text.empty()) {
        ::SetDlgItemTextW(dlg, IDC_STATIC_TESTRESULT,
                          L"Enter a key above to test it.");
        return;
    }
    security::SecureString keyUtf8 = security::WideToSecureUtf8(text);
    security::WipeWString(text);

    if (!state->validator || !*state->validator) {
        ::SetDlgItemTextW(dlg, IDC_STATIC_TESTRESULT,
                          L"Live testing is not available in this build.");
        return;
    }
    ::SetDlgItemTextW(dlg, IDC_STATIC_TESTRESULT, L"Testing…");
    const KeyTestResult result = (*state->validator)(keyUtf8);
    ::SetDlgItemTextW(dlg, IDC_STATIC_TESTRESULT, result.message.c_str());
}

void OnClearCache(HWND dlg, DialogState* state) {
    if (::MessageBoxW(dlg,
                      L"Delete all locally cached sales data?\n\n"
                      L"Your API key is kept. The next sync will re-download "
                      L"your full history, which may take a while.",
                      L"Clear Local Sales Cache",
                      MB_YESNO | MB_ICONWARNING) != IDYES) {
        return;
    }
    // The actual clear + resync is performed by the sync worker after the
    // dialog closes (see App::OnSettings), so it stays off the open UI-thread
    // DB connection.
    state->outcome.cacheCleared = true;
    ::SetDlgItemTextW(dlg, IDC_STATIC_TESTRESULT,
                      L"Local sales cache will be cleared and re-synced.");
}

void OnForceResync(HWND dlg, DialogState* state) {
    int days = 0;
    if (!ParseResyncDays(GetEditText(::GetDlgItem(dlg, IDC_EDIT_RESYNC_DAYS)),
                         days)) {
        ::SetDlgItemTextW(
            dlg, IDC_STATIC_TESTRESULT,
            (L"Enter a number of days between " +
             std::to_wstring(kMinResyncDays) + L" and " +
             std::to_wstring(kMaxResyncDays) + L".").c_str());
        return;
    }
    const std::wstring prompt =
        L"Re-fetch the last " + std::to_wstring(days) +
        L" day(s) of sales directly from Steam?\n\n"
        L"This overwrites local data for those dates only - other dates are "
        L"unaffected.";
    if (::MessageBoxW(dlg, prompt.c_str(), L"Force Resync",
                      MB_YESNO | MB_ICONWARNING) != IDYES) {
        return;
    }
    // The actual resync is performed by the sync worker after the dialog
    // closes (see App::OnSettings), same pattern as Clear Cache above.
    state->outcome.forceResyncDays = days;
    ::SetDlgItemTextW(
        dlg, IDC_STATIC_TESTRESULT,
        (L"Will resync the last " + std::to_wstring(days) +
         L" day(s) after you close Settings.").c_str());
}

void OnRemoveKey(HWND dlg, DialogState* state) {
    if (!state->store.HasStoredKey()) {
        ::SetDlgItemTextW(dlg, IDC_STATIC_TESTRESULT, L"No stored key to remove.");
        return;
    }
    if (::MessageBoxW(dlg,
                      L"Remove the stored Steam Financial API key?\n\n"
                      L"Syncing will stop until you enter a new key. Cached "
                      L"sales remain unless you also clear the cache.",
                      L"Remove Stored API Key",
                      MB_YESNO | MB_ICONWARNING) != IDYES) {
        return;
    }
    if (state->store.Remove()) {
        state->outcome.keyChanged = true;
        SetKeyStatusLabel(dlg, *state);
        ::SetDlgItemTextW(dlg, IDC_STATIC_TESTRESULT, L"Stored API key removed.");
    } else {
        ::SetDlgItemTextW(dlg, IDC_STATIC_TESTRESULT,
                          L"Could not remove the stored key.");
    }
}

void OnRemovePublisherKey(HWND dlg, DialogState* state) {
    if (!state->publisherStore.HasStoredKey()) {
        ::SetDlgItemTextW(dlg, IDC_STATIC_TESTRESULT,
                          L"No stored publisher key to remove.");
        return;
    }
    if (::MessageBoxW(dlg,
                      L"Remove the stored publisher API key?\n\n"
                      L"DLC and unreleased titles will stop being detected "
                      L"until you enter a new key. Sales/wishlist syncing is "
                      L"unaffected.",
                      L"Remove Stored Publisher API Key",
                      MB_YESNO | MB_ICONWARNING) != IDYES) {
        return;
    }
    if (state->publisherStore.Remove()) {
        state->outcome.publisherKeyChanged = true;
        SetPublisherKeyStatusLabel(dlg, *state);
        ::SetDlgItemTextW(dlg, IDC_STATIC_TESTRESULT,
                          L"Stored publisher key removed.");
    } else {
        ::SetDlgItemTextW(dlg, IDC_STATIC_TESTRESULT,
                          L"Could not remove the stored publisher key.");
    }
}

// Returns true if the dialog should close (EndDialog with IDOK).
bool OnSave(HWND dlg, DialogState* state) {
    // 1) Persist non-secret preferences.
    const storage::RefreshInterval interval = SelectedInterval(dlg);
    const bool notify =
        ::IsDlgButtonChecked(dlg, IDC_CHECK_NOTIFY) == BST_CHECKED;
    const bool startup =
        ::IsDlgButtonChecked(dlg, IDC_CHECK_STARTUP) == BST_CHECKED;

    if (interval != state->initialInterval) {
        storage::SettingsStore::SetRefreshInterval(interval);
        state->outcome.preferencesChanged = true;
    }
    if (notify != state->initialNotify) {
        storage::SettingsStore::SetNotifyOnNewUnits(notify);
        state->outcome.preferencesChanged = true;
    }
    if (startup != state->initialStartup) {
        if (startup) {
            platform::StartupManager::Enable();
        } else {
            platform::StartupManager::Disable();
        }
        state->outcome.preferencesChanged = true;
    }

    const bool hideLifetime =
        ::IsDlgButtonChecked(dlg, IDC_CHECK_HIDELIFETIME) == BST_CHECKED;
    if (hideLifetime != state->initialHideLifetime) {
        storage::SettingsStore::SetHideLifetime(hideLifetime);
        state->outcome.displayChanged = true;  // display-only; no resync
    }

    // 2) Store a new key if one was entered (empty edit = keep existing).
    std::wstring text = GetEditText(::GetDlgItem(dlg, IDC_EDIT_APIKEY));
    std::wstring trimmed = TrimWhitespace(text);
    security::WipeWString(text);

    if (!trimmed.empty()) {
        security::SecureString keyUtf8 = security::WideToSecureUtf8(trimmed);
        security::WipeWString(trimmed);

        // If a validator is available and testing succeeds/fails definitively,
        // honor it; if it cannot test (offline / not yet wired), store anyway.
        if (state->validator && *state->validator) {
            const KeyTestResult result = (*state->validator)(keyUtf8);
            if (result.couldTest && !result.ok) {
                ::SetDlgItemTextW(dlg, IDC_STATIC_TESTRESULT,
                                  result.message.c_str());
                ::MessageBoxW(dlg,
                              L"The key was rejected by Steam and was not "
                              L"saved. Your previous key (if any) is unchanged.",
                              L"Steam Sales Tray", MB_OK | MB_ICONERROR);
                return false;
            }
        }

        if (!state->store.Store(keyUtf8)) {
            ::MessageBoxW(dlg,
                          L"Failed to protect and save the API key.",
                          L"Steam Sales Tray", MB_OK | MB_ICONERROR);
            return false;
        }
        state->outcome.keyChanged = true;
    } else {
        security::WipeWString(trimmed);
    }

    // 3) Store a new publisher key if one was entered (no validator wired for
    // this one - GetPartnerAppListForWebAPIKey failures just surface as a
    // silently-empty DLC/Unreleased section, per the wishlist sync's
    // best-effort philosophy).
    std::wstring publisherText =
        GetEditText(::GetDlgItem(dlg, IDC_EDIT_PUBLISHER_APIKEY));
    std::wstring publisherTrimmed = TrimWhitespace(publisherText);
    security::WipeWString(publisherText);

    if (!publisherTrimmed.empty()) {
        security::SecureString publisherKeyUtf8 =
            security::WideToSecureUtf8(publisherTrimmed);
        security::WipeWString(publisherTrimmed);

        if (!state->publisherStore.Store(publisherKeyUtf8)) {
            ::MessageBoxW(dlg,
                          L"Failed to protect and save the publisher API key.",
                          L"Steam Sales Tray", MB_OK | MB_ICONERROR);
            return false;
        }
        state->outcome.publisherKeyChanged = true;
    } else {
        security::WipeWString(publisherTrimmed);
    }

    state->outcome.saved = true;
    return true;
}

INT_PTR CALLBACK DialogProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_INITDIALOG) {
        auto* state = reinterpret_cast<DialogState*>(lParam);
        ::SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
        OnInitDialog(dlg, state);
        return TRUE;
    }

    auto* state = reinterpret_cast<DialogState*>(
        ::GetWindowLongPtrW(dlg, DWLP_USER));
    if (!state) {
        return FALSE;
    }

    switch (msg) {
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_CHECK_SHOWKEY:
                    if (HIWORD(wParam) == BN_CLICKED) {
                        OnToggleShowKey(dlg);
                    }
                    return TRUE;
                case IDC_BUTTON_TEST:
                    OnTestConnection(dlg, state);
                    return TRUE;
                case IDC_BUTTON_CLEARCACHE:
                    OnClearCache(dlg, state);
                    return TRUE;
                case IDC_BUTTON_FORCERESYNC:
                    OnForceResync(dlg, state);
                    return TRUE;
                case IDC_BUTTON_REMOVEKEY:
                    OnRemoveKey(dlg, state);
                    return TRUE;
                case IDC_BUTTON_REMOVE_PUBLISHERKEY:
                    OnRemovePublisherKey(dlg, state);
                    return TRUE;
                case IDOK:
                    if (OnSave(dlg, state)) {
                        ::EndDialog(dlg, IDOK);
                    }
                    return TRUE;
                case IDCANCEL:
                    ::EndDialog(dlg, IDCANCEL);
                    return TRUE;
                default:
                    break;
            }
            return FALSE;
        case WM_CLOSE:
            ::EndDialog(dlg, IDCANCEL);
            return TRUE;
        default:
            return FALSE;
    }
}

}  // namespace

SettingsOutcome ShowSettingsDialog(HWND parent, HINSTANCE instance,
                                   const KeyValidator& validator) {
    DialogState state;
    state.instance = instance;
    state.validator = &validator;

    ::DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_SETTINGS), parent,
                      &DialogProc, reinterpret_cast<LPARAM>(&state));
    return state.outcome;
}

}  // namespace ui
