#pragma once

#include <atomic>
#include <mutex>
#include <thread>
#include <windows.h>

#include "sync/SyncStatus.h"

namespace storage { class Database; }
namespace steam { class SteamFinancialClient; }
namespace security { class DpapiSecretStore; class SecureString; }

namespace sync {

// Owns the single background worker thread that talks to Steam and writes the
// local database. All network I/O happens here; the UI thread never blocks on
// it. The worker sleeps on events between refreshes (no idle polling, §17).
//
// Communication to the UI (§18, "safer simple design"): the worker writes
// results to SQLite and a mutex-protected status snapshot, then PostMessages a
// payload-free notification to the main window, which reads the snapshot/DB.
class SalesSyncService {
public:
    SalesSyncService() = default;
    ~SalesSyncService();

    SalesSyncService(const SalesSyncService&) = delete;
    SalesSyncService& operator=(const SalesSyncService&) = delete;

    // Starts the worker. `notifyWindow` receives WM_APP_SYNC_* messages.
    bool Start(HWND notifyWindow);

    // Signals the worker to stop and joins it. Safe to call repeatedly.
    void Stop();

    // Wakes the worker to sync now. Repeated calls collapse into one run.
    void RequestManualRefresh();

    // Tells the worker that preferences/key changed (re-reads interval, syncs).
    void NotifySettingsChanged();

    // Requests a full cache clear (reset + full resync) on the worker thread.
    void RequestClearCache();

    // Thread-safe copy of the current status snapshot.
    AppStatus GetStatus() const;

private:
    void Run();
    void RunSynchronization(storage::Database& db,
                            steam::SteamFinancialClient& client,
                            security::DpapiSecretStore& store);
    // Best-effort wishlist balance sync (rolling today/yesterday refresh +
    // bounded per-app backfill). Never fails the caller - logs and moves on,
    // since this is supplementary to the core sales pipeline.
    void SyncWishlists(storage::Database& db,
                       steam::SteamFinancialClient& client,
                       const security::SecureString& key);
    // Best-effort discovery of every app tied to the account (game/dlc/
    // unreleased) via GetPartnerAppListForWebAPIKey - the sales pipeline only
    // ever surfaces apps that have sold something. Called from inside
    // SyncWishlists, before it enumerates known apps, so a freshly-discovered
    // app is included in that same cycle's wishlist fetch.
    //
    // Uses its own publisher-key store (loaded fresh each call, not the
    // Financial key passed elsewhere in this file) - the Financial API Group
    // key has no app/user permissions and Steam rejects it for this endpoint
    // with a 403. Silently skips if no publisher key is stored.
    void SyncKnownApps(storage::Database& db,
                       steam::SteamFinancialClient& client);
    void SetStatus(SyncStatus status, const std::wstring& message);
    void Post(UINT message) const;
    DWORD ComputeWaitMs() const;

    HWND notifyWindow_ = nullptr;
    std::thread thread_;

    HANDLE stopEvent_ = nullptr;      // manual-reset
    HANDLE manualEvent_ = nullptr;    // auto-reset
    HANDLE settingsEvent_ = nullptr;  // auto-reset

    std::atomic<bool> clearCacheRequested_{false};
    ULONGLONG deadlineTick_ = 0;  // next scheduled refresh (GetTickCount64)

    mutable std::mutex statusMutex_;
    AppStatus status_;
};

}  // namespace sync
