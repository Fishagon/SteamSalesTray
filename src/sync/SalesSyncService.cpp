#include "sync/SalesSyncService.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

#include "app/Messages.h"
#include "platform/Logging.h"
#include "platform/Paths.h"
#include "platform/StringUtil.h"
#include "platform/TimeUtil.h"
#include "security/DpapiSecretStore.h"
#include "security/SecureString.h"
#include "steam/SteamFinancialClient.h"
#include "storage/Database.h"
#include "storage/SettingsStore.h"
#include "sync/SalesAggregator.h"

namespace sync {

namespace {
// Skip an immediate startup sync if the last success was this recent (§22).
constexpr long long kStartupFreshnessSeconds = 300;  // 5 minutes

// Bounds how many historical dates a single app can backfill per sync cycle,
// so one wishlist pass can't turn a sync into a multi-minute API crawl.
constexpr int kWishlistBackfillStepsPerCycle = 45;

int IntervalMinutes() {
    return static_cast<int>(storage::SettingsStore::GetRefreshInterval());
}

// Maps a network/API result kind to a worker status + whether it is fatal for
// this pass (i.e. we must not advance the high-water mark).
SyncStatus StatusFromKind(steam::ResultKind kind) {
    switch (kind) {
        case steam::ResultKind::Offline:
        case steam::ResultKind::RateLimited:
        case steam::ResultKind::ServerError:
            return SyncStatus::Offline;
        case steam::ResultKind::Auth:
            return SyncStatus::AuthenticationFailed;
        case steam::ResultKind::Parse:
            return SyncStatus::ParseError;
        case steam::ResultKind::TooLarge:
        case steam::ResultKind::Other:
        default:
            return SyncStatus::ApiError;
    }
}

struct AppLifetime {
    std::wstring name;
    std::int64_t units = 0;
};

// Snapshot of lifetime net units per app, for notification deltas.
std::unordered_map<std::int64_t, AppLifetime> LifetimeSnapshot(
    storage::Database& db) {
    std::unordered_map<std::int64_t, AppLifetime> map;
    for (const storage::ProductUnits& row : db.QueryLifetime()) {
        map[row.appId] = {row.displayName, row.units};
    }
    return map;
}

// Builds a consolidated "new units" message from positive per-app increases.
// Returns empty if there were none (§15: never notify for negative changes).
std::wstring BuildNotification(
    const std::unordered_map<std::int64_t, AppLifetime>& before,
    const std::unordered_map<std::int64_t, AppLifetime>& after,
    std::int64_t todayNet) {
    std::wstring body;
    int lines = 0;
    for (const auto& [appId, info] : after) {
        std::int64_t prev = 0;
        auto it = before.find(appId);
        if (it != before.end()) {
            prev = it->second.units;
        }
        const std::int64_t delta = info.units - prev;
        if (delta > 0 && lines < 6) {
            body += info.name + L": +" + platform::FormatThousands(delta) + L"\n";
            ++lines;
        }
    }
    if (body.empty()) {
        return {};
    }
    body += L"Today: " + platform::FormatThousands(todayNet) + L" net units";
    return body;
}
}  // namespace

SalesSyncService::~SalesSyncService() {
    Stop();
    if (stopEvent_) ::CloseHandle(stopEvent_);
    if (manualEvent_) ::CloseHandle(manualEvent_);
    if (settingsEvent_) ::CloseHandle(settingsEvent_);
}

bool SalesSyncService::Start(HWND notifyWindow) {
    notifyWindow_ = notifyWindow;
    stopEvent_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);      // manual
    manualEvent_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);   // auto
    settingsEvent_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr); // auto
    if (!stopEvent_ || !manualEvent_ || !settingsEvent_) {
        return false;
    }
    thread_ = std::thread([this] { Run(); });
    return true;
}

void SalesSyncService::Stop() {
    if (thread_.joinable()) {
        if (stopEvent_) {
            ::SetEvent(stopEvent_);
        }
        thread_.join();
    }
}

void SalesSyncService::RequestManualRefresh() {
    if (manualEvent_) ::SetEvent(manualEvent_);
}

void SalesSyncService::NotifySettingsChanged() {
    if (settingsEvent_) ::SetEvent(settingsEvent_);
}

void SalesSyncService::RequestClearCache() {
    clearCacheRequested_.store(true);
    if (manualEvent_) ::SetEvent(manualEvent_);
}

AppStatus SalesSyncService::GetStatus() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return status_;
}

void SalesSyncService::SetStatus(SyncStatus status,
                                 const std::wstring& message) {
    std::lock_guard<std::mutex> lock(statusMutex_);
    status_.status = status;
    status_.message = message;
    status_.lastAttempt = std::chrono::system_clock::now();
}

void SalesSyncService::Post(UINT message) const {
    if (notifyWindow_) {
        ::PostMessageW(notifyWindow_, message, 0, 0);
    }
}

DWORD SalesSyncService::ComputeWaitMs() const {
    if (IntervalMinutes() == 0) {
        return INFINITE;  // manual only
    }
    const ULONGLONG now = ::GetTickCount64();
    if (now >= deadlineTick_) {
        return 0;
    }
    const ULONGLONG remaining = deadlineTick_ - now;
    return static_cast<DWORD>(
        remaining > MAXDWORD ? MAXDWORD : remaining);
}

void SalesSyncService::Run() {
    // Worker-thread-owned resources (WinHTTP session, DB connection).
    storage::Database db;
    db.Open(platform::GetDatabasePath());
    steam::SteamFinancialClient client;
    security::DpapiSecretStore store;

    auto resetDeadline = [&] {
        const int minutes = IntervalMinutes();
        deadlineTick_ = (minutes == 0)
                            ? 0
                            : ::GetTickCount64() +
                                  static_cast<ULONGLONG>(minutes) * 60'000ULL;
    };

    // Startup: sync now unless the last success was very recent (§22).
    const long long age =
        platform::SecondsSinceUtc(db.GetLastSuccessfulSyncUtc());
    if (age > kStartupFreshnessSeconds) {
        RunSynchronization(db, client, store);
    } else {
        SetStatus(SyncStatus::Idle, L"");
    }
    resetDeadline();

    HANDLE handles[3] = {stopEvent_, manualEvent_, settingsEvent_};
    for (;;) {
        const DWORD wait =
            ::WaitForMultipleObjects(3, handles, FALSE, ComputeWaitMs());
        if (wait == WAIT_OBJECT_0) {
            break;  // stop
        }
        // Any of: manual (obj+1), settings (obj+2), or timeout => run a sync.
        RunSynchronization(db, client, store);
        resetDeadline();
    }
}

void SalesSyncService::RunSynchronization(storage::Database& db,
                                          steam::SteamFinancialClient& client,
                                          security::DpapiSecretStore& store) {
    Post(app::WM_APP_SYNC_STARTED);
    SetStatus(SyncStatus::Syncing, L"Updating…");

    if (!db.IsOpen() && !db.Open(platform::GetDatabasePath())) {
        SetStatus(SyncStatus::DatabaseError, L"Could not open the local cache.");
        Post(app::WM_APP_SYNC_FAILED);
        return;
    }

    if (clearCacheRequested_.exchange(false)) {
        db.ClearAllSales();
    }

    // Decrypt the key only for the duration of this pass.
    security::SecureString key;
    if (!store.HasStoredKey() || !store.Load(key) || key.Empty()) {
        SetStatus(SyncStatus::MissingApiKey, L"No API key configured.");
        Post(app::WM_APP_SYNC_FAILED);
        return;
    }

    const std::string hwm = db.GetChangedDatesHighwatermark();
    const bool initial = (hwm == "0" || hwm.empty());
    const bool notifyEnabled = storage::SettingsStore::GetNotifyOnNewUnits();

    steam::ChangedDates changed;
    steam::FinancialResult r = client.GetChangedDates(key, hwm, changed);
    if (!r.ok()) {
        SetStatus(StatusFromKind(r.kind), r.message);
        db.SetLastError(platform::WideToUtf8(r.message));
        platform::Log::Warn(L"Sync",
                            L"GetChangedDates failed, httpStatus=" +
                                std::to_wstring(r.httpStatus));
        Post(app::WM_APP_SYNC_FAILED);
        return;
    }
    platform::Log::Info(L"Sync", L"ChangedDates completed, dates=" +
                                     std::to_wstring(changed.dates.size()));

    const std::string now = platform::UtcNowIso8601();
    db.SetLastSuccessfulCheckUtc(now);

    // No changes: record the check, advance the (vacuously safe) mark, done.
    if (changed.dates.empty()) {
        if (!changed.resultHighwatermark.empty()) {
            db.SetChangedDatesHighwatermark(changed.resultHighwatermark);
        }
        db.SetLastError("");
        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            status_.status = SyncStatus::Idle;
            status_.message.clear();
            status_.totalDates = 0;
            status_.completedDates = 0;
        }
        SyncWishlists(db, client, key);
        Post(app::WM_APP_SYNC_COMPLETED);
        return;
    }

    // Process oldest -> newest (dates sort lexically = chronologically).
    std::sort(changed.dates.begin(), changed.dates.end());

    // Resumable backfill: during an initial (hwm '0') sync, skip dates already
    // completed on a prior interrupted run. Dates are processed in ascending
    // order, so the single checkpoint cursor marks a contiguous done-prefix.
    const std::string cursor = initial ? db.GetBackfillCursor() : std::string();
    std::vector<std::string> toProcess;
    toProcess.reserve(changed.dates.size());
    for (const std::string& d : changed.dates) {
        if (initial && !cursor.empty() && d <= cursor) {
            continue;  // already synced in an earlier backfill run
        }
        toProcess.push_back(d);
    }
    if (initial && toProcess.size() < changed.dates.size()) {
        platform::Log::Info(
            L"Sync", L"Resuming backfill; skipping " +
                         std::to_wstring(changed.dates.size() -
                                         toProcess.size()) +
                         L" already-synced dates");
    }

    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        status_.totalDates = static_cast<int>(toProcess.size());
        status_.completedDates = 0;
    }
    Post(app::WM_APP_SYNC_PROGRESS);

    // Snapshot lifetime totals before mutating, for notification deltas
    // (skipped on the initial historical sync, §15).
    std::unordered_map<std::int64_t, AppLifetime> before;
    if (notifyEnabled && !initial) {
        before = LifetimeSnapshot(db);
    }

    for (const std::string& date : toProcess) {
        std::vector<steam::DetailedSalesRecord> records;
        steam::FinancialResult pr =
            client.GetAllDetailedSalesForDate(key, date, records);
        if (!pr.ok()) {
            // A failed date must NOT advance the global mark (§8 atomicity):
            // the same dates will be returned next run and retried.
            SetStatus(StatusFromKind(pr.kind), pr.message);
            db.SetLastError(platform::WideToUtf8(pr.message));
            platform::Log::Warn(L"Sync",
                                L"GetDetailedSales failed, httpStatus=" +
                                    std::to_wstring(pr.httpStatus));
            Post(app::WM_APP_SYNC_FAILED);
            return;
        }

        std::vector<storage::DailyProductRow> rows = AggregateDate(records);
        if (!db.ReplaceDate(date, rows, now)) {
            SetStatus(SyncStatus::DatabaseError, L"Failed to save sales data.");
            db.SetLastError("database write failed");
            platform::Log::Error(L"Sync", L"Database write failed for a date");
            Post(app::WM_APP_SYNC_FAILED);
            return;
        }
        // Checkpoint the backfill after each fully-stored date so an
        // interruption resumes here instead of restarting.
        if (initial) {
            db.SetBackfillCursor(date);
        }

        platform::Log::Info(L"Sync",
                            L"Replaced financial date " +
                                platform::Utf8ToWide(date));

        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            ++status_.completedDates;
        }
        Post(app::WM_APP_SYNC_PROGRESS);
    }

    // Every date succeeded: now it is safe to advance the global mark. The
    // backfill is complete, so clear the checkpoint.
    if (!changed.resultHighwatermark.empty()) {
        db.SetChangedDatesHighwatermark(changed.resultHighwatermark);
    }
    if (initial) {
        db.SetBackfillCursor("");
    }
    db.SetLastSuccessfulSyncUtc(now);
    db.SetLastError("");

    // Build a consolidated notification from positive deltas (§15).
    std::wstring notification;
    if (notifyEnabled && !initial) {
        const auto after = LifetimeSnapshot(db);
        notification =
            BuildNotification(before, after, db.SumForDate(platform::SteamToday()));
    }

    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        status_.status = SyncStatus::Idle;
        status_.message.clear();
        status_.lastSuccess = std::chrono::system_clock::now();
        status_.notification = notification;
    }
    platform::Log::Info(L"Sync", L"Sync completed successfully");

    SyncWishlists(db, client, key);

    Post(app::WM_APP_TOTALS_CHANGED);
    Post(app::WM_APP_SYNC_COMPLETED);
}

void SalesSyncService::SyncKnownApps(storage::Database& db,
                                     steam::SteamFinancialClient& client) {
    security::DpapiSecretStore publisherStore(platform::GetPublisherApiKeyPath());
    security::SecureString publisherKey;
    if (!publisherStore.HasStoredKey() || !publisherStore.Load(publisherKey) ||
        publisherKey.Empty()) {
        return;  // no publisher key configured - DLC/Unreleased just stay empty
    }

    std::vector<steam::PartnerApp> apps;
    const steam::FinancialResult r = client.GetAppList(publisherKey, apps);
    if (!r.ok()) {
        platform::Log::Warn(L"Sync", L"GetAppList failed, httpStatus=" +
                                         std::to_wstring(r.httpStatus));
        return;
    }
    const std::string now = platform::UtcNowIso8601();
    for (const steam::PartnerApp& app : apps) {
        db.UpsertKnownApp(app.appId, app.appName, app.appType, now);
    }
}

void SalesSyncService::SyncWishlists(storage::Database& db,
                                     steam::SteamFinancialClient& client,
                                     const security::SecureString& key) {
    SyncKnownApps(db, client);

    const std::string today = platform::SteamToday();
    const std::string yesterday = platform::SteamDateDaysAgo(1);

    for (const storage::AppRef& app : db.GetKnownApps()) {
        // Rolling refresh: today + yesterday are re-checked every cycle since
        // Steam settles wishlist data with roughly a one-day lag.
        for (const std::string& date : {yesterday, today}) {
            steam::AppWishlistReport report;
            const steam::FinancialResult r =
                client.GetAppWishlistReporting(key, app.appId, date, report);
            if (!r.ok()) {
                platform::Log::Warn(
                    L"Sync", L"Wishlist refresh failed for app " +
                                 std::to_wstring(app.appId));
                continue;
            }
            storage::DailyAppWishlistRow row;
            row.appId = app.appId;
            row.adds = report.summary.adds;
            row.deletes = report.summary.deletes;
            row.purchases = report.summary.purchases;
            row.gifts = report.summary.gifts;
            db.UpsertWishlistDay(date, row);
        }

        // Bounded backward backfill, resuming from a per-app cursor. daysAgo
        // starts at backfillDays+2 to skip over today/yesterday, which the
        // rolling refresh above already covers.
        int backfillDays = 0;
        std::string minDate;
        db.GetWishlistSyncState(app.appId, backfillDays, minDate);

        for (int steps = 0; steps < kWishlistBackfillStepsPerCycle; ++steps) {
            const int daysAgo = backfillDays + 2;
            const std::string date = platform::SteamDateDaysAgo(daysAgo);
            if (!minDate.empty() && date < minDate) {
                break;  // already past the app's earliest available date
            }

            steam::AppWishlistReport report;
            const steam::FinancialResult r =
                client.GetAppWishlistReporting(key, app.appId, date, report);
            if (!r.ok()) {
                break;  // retry this app from the same point next cycle
            }

            storage::DailyAppWishlistRow row;
            row.appId = app.appId;
            row.adds = report.summary.adds;
            row.deletes = report.summary.deletes;
            row.purchases = report.summary.purchases;
            row.gifts = report.summary.gifts;
            db.UpsertWishlistDay(date, row);

            backfillDays = daysAgo;
            if (!report.appMinDate.empty()) {
                minDate = report.appMinDate;
            }
            if (!minDate.empty() && date <= minDate) {
                break;  // reached the app's earliest available date
            }
        }

        db.SetWishlistSyncState(app.appId, backfillDays, minDate);
    }
}

}  // namespace sync
