#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "storage/Models.h"

struct sqlite3;

namespace storage {

// One SQLite connection to sales.db. Not thread-safe: each thread that touches
// the database should own its own Database instance (WAL mode allows a writer
// and readers to run concurrently across connections).
class Database {
public:
    Database() = default;
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Opens (creating if needed), applies pragmas, and runs migrations.
    bool Open(const std::wstring& path);
    void Close();
    bool IsOpen() const { return db_ != nullptr; }

    // --- sync_state ---------------------------------------------------------
    std::string GetChangedDatesHighwatermark();
    bool SetChangedDatesHighwatermark(const std::string& highwatermark);
    bool SetLastSuccessfulCheckUtc(const std::string& iso8601);
    bool SetLastSuccessfulSyncUtc(const std::string& iso8601);
    std::string GetLastSuccessfulSyncUtc();
    bool SetLastError(const std::string& message);  // empty clears it
    std::string GetLastError();

    // Backfill checkpoint: the newest financial date fully synced during an
    // in-progress full sync. Empty means "no checkpoint" (fresh/complete).
    std::string GetBackfillCursor();
    bool SetBackfillCursor(const std::string& date);  // empty clears it

    // --- writes -------------------------------------------------------------
    // Atomically replaces all of `date`'s rows with `rows` (plan §8): delete
    // the date, upsert products, insert the new daily rows - one transaction,
    // rolled back on any error.
    bool ReplaceDate(const std::string& date,
                     const std::vector<DailyProductRow>& rows,
                     const std::string& nowUtc);

    // Deletes all sales + products and resets the high-water mark to '0'
    // (Clear Local Sales Cache). Keeps sync_state row and timestamps.
    bool ClearAllSales();

    // --- reads --------------------------------------------------------------
    // Today view (§10): every known product with its units for `date` (0 if
    // none), ordered by units desc then name.
    std::vector<ProductUnits> QueryDate(const std::string& date);
    // Range/lifetime views: products with sales in the window, grouped.
    std::vector<ProductUnits> QueryRange(const std::string& startDate,
                                         const std::string& endDate);
    std::vector<ProductUnits> QueryLifetime();

    std::int64_t SumForDate(const std::string& date);
    std::int64_t SumForRange(const std::string& startDate,
                             const std::string& endDate);
    std::int64_t SumLifetime();

    // --- wishlist ------------------------------------------------------------
    // Every known product (source of app ids for GetAppWishlistReporting,
    // which is queried per app rather than paginated across all apps).
    std::vector<AppRef> GetKnownApps();

    // Upserts one (date, app) wishlist row; wishlist_balance is computed as
    // adds - deletes - purchases - gifts before storing.
    bool UpsertWishlistDay(const std::string& date,
                           const DailyAppWishlistRow& row);

    // Per-app wishlist backfill cursor. Returns false (leaving outputs at
    // their defaults) if no state row exists yet for this app.
    bool GetWishlistSyncState(std::int64_t appId, int& backfillDays,
                              std::string& minDate);
    bool SetWishlistSyncState(std::int64_t appId, int backfillDays,
                              const std::string& minDate);

    // Every known app with enough to classify it (type, ever-sold), for the
    // Product Sales window's Released/DLC/Unreleased grouping.
    std::vector<ProductInfo> GetAllProducts();

    // Upserts app_type/display_name for one app, discovered independently of
    // sales via GetPartnerAppListForWebAPIKey (the only source for DLC and
    // unreleased titles, which never appear in daily_product_sales).
    bool UpsertKnownApp(std::int64_t appId, const std::string& displayNameUtf8,
                       const std::string& appType, const std::string& nowUtc);

    // Per-app wishlist balance rows for a period, for the per-product "WL
    // Balance" column (as opposed to the SumWishlistBalance* scalars above,
    // used for the aggregate label).
    std::vector<ProductWishlistBalance> QueryWishlistBalanceRowsForDate(
        const std::string& date);
    std::vector<ProductWishlistBalance> QueryWishlistBalanceRowsForRange(
        const std::string& startDate, const std::string& endDate);
    std::vector<ProductWishlistBalance> QueryWishlistBalanceRowsLifetime();

private:
    sqlite3* db_ = nullptr;
};

}  // namespace storage
