// Unit tests for aggregation + SQLite persistence (Phase 4).
// Standalone: compile with Database.cpp, Migrations.cpp, SalesAggregator.cpp,
// StringUtil.cpp, sqlite3.c.
#include <cstdio>
#include <string>
#include <windows.h>

#include "storage/Database.h"
#include "sync/SalesAggregator.h"

static int g_fail = 0;
#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (cond) { std::printf("  PASS: %s\n", msg); }                      \
        else { std::printf("  FAIL: %s\n", msg); ++g_fail; }                 \
    } while (0)

using steam::DetailedSalesRecord;

// A row as GetDetailedSales actually sends it: identity is the package, and
// `primaryApp`/`name` are the revenue-share target (the base game for DLC).
static DetailedSalesRecord Rec(const char* type, const char* sale,
                               std::int64_t primaryApp, const char* name,
                               std::int64_t net, std::int64_t gross = 0,
                               std::int64_t ret = 0,
                               const char* packageName = "") {
    DetailedSalesRecord r;
    r.lineItemType = type;
    r.packageSaleType = sale;
    r.primaryAppId = primaryApp;
    r.primaryAppName = name;
    r.packageName = packageName;
    r.netUnitsSold = net;
    r.grossUnitsSold = gross;
    r.grossUnitsReturned = ret;
    return r;
}

static std::int64_t UnitsFor(const std::vector<storage::ProductUnits>& v,
                             std::int64_t app) {
    for (const auto& p : v)
        if (p.appId == app) return p.units;
    return -999999;  // sentinel: not present
}

static void TestAggregation() {
    std::printf("[Aggregation]\n");
    const sync::AppIdsByName kNoApps;

    std::vector<DetailedSalesRecord> recs = {
        Rec("Package", "Steam", 480, "git gud", 2, 3, 1),
        Rec("Package", "Steam", 480, "", 1, 1, 0),        // 2nd pkg, same app
        Rec("MicroTxn", "Steam", 481, "MTX", 99),          // excluded (MicroTxn)
        Rec("Package", "Retail", 482, "CDKey", 5),         // excluded (Retail)
        Rec("Package", "Steam", 0, "orphan", 7),           // excluded (app 0)
        Rec("Package", "Steam", 490, "Deluxe", -3, 0, 3),  // negative net
    };
    auto rows = sync::AggregateDate(recs, kNoApps);
    CHECK(rows.size() == 2, "only Steam packages with app id kept (2 groups)");

    std::int64_t net480 = 0, net490 = 0;
    std::string name480;
    for (auto& r : rows) {
        if (r.appId == 480) { net480 = r.netUnitsSold; name480 = r.displayName; }
        if (r.appId == 490) net490 = r.netUnitsSold;
    }
    CHECK(net480 == 3, "two packages for app 480 summed (2+1=3)");
    CHECK(name480 == "git gud", "kept non-empty name across records");
    CHECK(net490 == -3, "negative net preserved");

    // The bug this attribution exists for: a DLC package and its base game
    // arrive with the SAME primary_appid, distinguishable only by package
    // name. Without the split, all 3 units would land on the base game.
    const sync::AppIdsByName known = {
        {"git gud", 1490570},
        {"git gud - Branch Strategy Bootcamp", 5118140},
    };
    std::vector<DetailedSalesRecord> mixed = {
        Rec("Package", "Steam", 1490570, "git gud", 2, 2, 0, "git gud"),
        Rec("Package", "Steam", 1490570, "git gud", 1, 1, 0,
            "git gud - Branch Strategy Bootcamp"),
    };
    auto split = sync::AggregateDate(mixed, known);
    CHECK(split.size() == 2, "DLC and base game split into separate rows");
    std::int64_t baseNet = -1, dlcNet = -1;
    std::string dlcName;
    for (auto& r : split) {
        if (r.appId == 1490570) baseNet = r.netUnitsSold;
        if (r.appId == 5118140) { dlcNet = r.netUnitsSold; dlcName = r.displayName; }
    }
    CHECK(baseNet == 2, "base game keeps only its own 2 units");
    CHECK(dlcNet == 1, "DLC units credited to the DLC's own appid");
    CHECK(dlcName == "git gud - Branch Strategy Bootcamp",
          "DLC row shows the DLC's name");

    // A package name that matches no known app (bundle, CD-key pack, or no
    // publisher key configured) falls back to primary_appid, and must NOT
    // rename the app it pays out to.
    std::vector<DetailedSalesRecord> bundle = {
        Rec("Package", "Steam", 1490570, "git gud", 4, 4, 0,
            "git gud Deluxe Bundle"),
    };
    auto fell = sync::AggregateDate(bundle, known);
    CHECK(fell.size() == 1 && fell[0].appId == 1490570 &&
              fell[0].netUnitsSold == 4,
          "unknown package name falls back to primary_appid");
    if (fell.size() == 1) {
        CHECK(fell[0].displayName == "git gud",
              "fallback keeps the app's own name, not the package name");
    }
}

static void TestDatabase() {
    std::printf("[Database]\n");
    wchar_t tmp[MAX_PATH]; ::GetTempPathW(MAX_PATH, tmp);
    std::wstring path = std::wstring(tmp) + L"sst_test_sales.db";
    ::DeleteFileW(path.c_str());
    ::DeleteFileW((path + L"-wal").c_str());
    ::DeleteFileW((path + L"-shm").c_str());

    storage::Database db;
    CHECK(db.Open(path), "Open() creates + migrates");
    CHECK(db.GetChangedDatesHighwatermark() == "0", "fresh hwm == 0");

    const std::string now = "2026-07-20T00:00:00Z";
    const std::string d1 = "2026/07/18";
    const std::string d2 = "2026/07/19";

    // Initial fill for d1.
    std::vector<storage::DailyProductRow> a = {
        {480, "git gud", 3, 4, 1}, {490, "Deluxe", -3, 0, 3}};
    CHECK(db.ReplaceDate(d1, a, now), "ReplaceDate d1");
    CHECK(db.SumLifetime() == 0, "lifetime sum 3 + (-3) = 0");
    CHECK(UnitsFor(db.QueryLifetime(), 490) == -3, "negative persisted");

    // Re-sync d1 (recalculated): must REPLACE, not accumulate.
    std::vector<storage::DailyProductRow> a2 = {{480, "git gud", 10, 10, 0}};
    CHECK(db.ReplaceDate(d1, a2, now), "ReplaceDate d1 again (recalc)");
    CHECK(db.SumForDate(d1) == 10, "date fully replaced (10, not 3+10 or 7)");
    CHECK(UnitsFor(db.QueryLifetime(), 490) == -999999,
          "app 490's d1 row removed by replacement");

    // Second date.
    std::vector<storage::DailyProductRow> b = {{480, "git gud", 5, 5, 0}};
    CHECK(db.ReplaceDate(d2, b, now), "ReplaceDate d2");
    CHECK(db.SumForRange(d1, d2) == 15, "range sum 10 + 5 = 15");
    CHECK(UnitsFor(db.QueryRange(d1, d2), 480) == 15, "range groups per app");

    // Today view (LEFT JOIN): product with no sales that date shows 0.
    auto today = db.QueryDate(d2);
    CHECK(UnitsFor(today, 480) == 5, "QueryDate shows units for d2");
    CHECK(UnitsFor(today, 490) == 0,
          "QueryDate lists product with 0 units for the date");

    // Huge unsigned high-water mark round-trips as text.
    CHECK(db.SetChangedDatesHighwatermark("18446744073709551615"),
          "set huge hwm");
    CHECK(db.GetChangedDatesHighwatermark() == "18446744073709551615",
          "huge hwm round-trips (beyond signed 64-bit)");

    // Timestamps.
    CHECK(db.SetLastSuccessfulSyncUtc(now), "set last sync utc");
    CHECK(db.GetLastSuccessfulSyncUtc() == now, "get last sync utc");

    // Backfill checkpoint (schema v2).
    CHECK(db.GetBackfillCursor().empty(), "fresh backfill cursor empty");
    CHECK(db.SetBackfillCursor("2026/07/10"), "set backfill cursor");
    CHECK(db.GetBackfillCursor() == "2026/07/10", "backfill cursor round-trips");
    CHECK(db.SetBackfillCursor(""), "clear backfill cursor");
    CHECK(db.GetBackfillCursor().empty(), "backfill cursor cleared");

    // Clear cache resets sales + hwm + cursor, keeps sync_state row.
    db.SetBackfillCursor("2026/07/11");
    CHECK(db.ClearAllSales(), "ClearAllSales");
    CHECK(db.SumLifetime() == 0 && db.QueryLifetime().empty(),
          "sales cleared");
    CHECK(db.GetChangedDatesHighwatermark() == "0", "hwm reset to 0");
    CHECK(db.GetBackfillCursor().empty(), "ClearAllSales clears backfill cursor");

    db.Close();
    ::DeleteFileW(path.c_str());
    ::DeleteFileW((path + L"-wal").c_str());
    ::DeleteFileW((path + L"-shm").c_str());
}

static std::int64_t BalanceFor(
    const std::vector<storage::ProductWishlistBalance>& v,
    std::int64_t appId) {
    for (const auto& p : v) {
        if (p.appId == appId) return p.balance;
    }
    return -999999;  // sentinel: not present
}

static void TestWishlist() {
    std::printf("[Wishlist]\n");
    wchar_t tmp[MAX_PATH]; ::GetTempPathW(MAX_PATH, tmp);
    std::wstring path = std::wstring(tmp) + L"sst_test_wishlist.db";
    ::DeleteFileW(path.c_str());
    ::DeleteFileW((path + L"-wal").c_str());
    ::DeleteFileW((path + L"-shm").c_str());

    storage::Database db;
    CHECK(db.Open(path), "Open() creates + migrates (incl. schema v3)");

    const std::string now = "2026-07-20T00:00:00Z";
    const std::string d1 = "2026/07/18";
    const std::string d2 = "2026/07/19";

    // Wishlist rows FK-reference products, so seed them via a sales replace
    // first (mirrors how GetKnownApps() is populated in the real sync).
    std::vector<storage::DailyProductRow> sales = {{480, "git gud", 1, 1, 0},
                                                   {490, "Deluxe", 1, 1, 0}};
    CHECK(db.ReplaceDate(d1, sales, now), "seed products via ReplaceDate");

    auto apps = db.GetKnownApps();
    CHECK(apps.size() == 2, "GetKnownApps returns both seeded products");

    storage::DailyAppWishlistRow row1{480, 100, 20, 10, 5};  // balance = 65
    CHECK(db.UpsertWishlistDay(d1, row1), "UpsertWishlistDay d1/480");
    CHECK(BalanceFor(db.QueryWishlistBalanceRowsForDate(d1), 480) == 65,
          "balance = adds-deletes-purchases-gifts (100-20-10-5)");

    storage::DailyAppWishlistRow row2{490, 10, 50, 0, 0};  // balance = -40
    CHECK(db.UpsertWishlistDay(d1, row2), "UpsertWishlistDay d1/490");
    auto dateRows1 = db.QueryWishlistBalanceRowsForDate(d1);
    CHECK(BalanceFor(dateRows1, 480) == 65 && BalanceFor(dateRows1, 490) == -40,
          "both apps' date balances present (65 and -40)");

    // Re-upsert (same date/app) must replace, not accumulate.
    storage::DailyAppWishlistRow row1b{480, 1, 1, 1, 1};  // balance = -2
    CHECK(db.UpsertWishlistDay(d1, row1b), "UpsertWishlistDay overwrites");
    CHECK(BalanceFor(db.QueryWishlistBalanceRowsForDate(d1), 480) == -2,
          "overwritten, not accumulated");

    storage::DailyAppWishlistRow row1c{480, 5, 0, 0, 0};  // balance = 5
    CHECK(db.UpsertWishlistDay(d2, row1c), "UpsertWishlistDay d2/480");
    auto rangeRows = db.QueryWishlistBalanceRowsForRange(d1, d2);
    CHECK(BalanceFor(rangeRows, 480) == 3,
          "range sum across dates (-2+5=3)");
    auto lifetimeRows = db.QueryWishlistBalanceRowsLifetime();
    CHECK(BalanceFor(lifetimeRows, 480) == 3, "lifetime matches range");

    // Per-app sync cursor round-trips; absent until first set.
    int days = -1;
    std::string minDate = "unset";
    CHECK(!db.GetWishlistSyncState(480, days, minDate),
          "no sync state yet for a known app");
    CHECK(days == 0 && minDate.empty(), "outputs reset to defaults");
    CHECK(db.SetWishlistSyncState(480, 12, "2020/01/01"), "set sync state");
    CHECK(db.GetWishlistSyncState(480, days, minDate) && days == 12 &&
              minDate == "2020/01/01",
          "sync state round-trips");
    CHECK(db.SetWishlistSyncState(480, 40, ""), "clear min date");
    CHECK(db.GetWishlistSyncState(480, days, minDate) && minDate.empty(),
          "cleared min date persists as empty");

    // ClearAllSales must not hit an FK violation now that wishlist tables
    // reference products, and must wipe wishlist data + cursors too.
    CHECK(db.ClearAllSales(), "ClearAllSales (wishlist + products, FK-safe)");
    CHECK(db.QueryWishlistBalanceRowsLifetime().empty(),
          "wishlist balances cleared");
    CHECK(db.GetKnownApps().empty(), "products cleared");
    CHECK(!db.GetWishlistSyncState(480, days, minDate),
          "wishlist sync state cleared");

    db.Close();
    ::DeleteFileW(path.c_str());
    ::DeleteFileW((path + L"-wal").c_str());
    ::DeleteFileW((path + L"-shm").c_str());
}

static const storage::ProductInfo* FindProduct(
    const std::vector<storage::ProductInfo>& v, std::int64_t appId) {
    for (const auto& p : v) {
        if (p.appId == appId) return &p;
    }
    return nullptr;
}

static void TestProductClassification() {
    std::printf("[ProductClassification]\n");
    wchar_t tmp[MAX_PATH]; ::GetTempPathW(MAX_PATH, tmp);
    std::wstring path = std::wstring(tmp) + L"sst_test_classification.db";
    ::DeleteFileW(path.c_str());
    ::DeleteFileW((path + L"-wal").c_str());
    ::DeleteFileW((path + L"-shm").c_str());

    storage::Database db;
    CHECK(db.Open(path), "Open() creates + migrates (incl. schema v4)");

    const std::string now = "2026-07-20T00:00:00Z";
    const std::string d1 = "2026/07/18";
    const std::string d2 = "2026/07/19";

    // 480: a real released game, discovered via the sales path.
    std::vector<storage::DailyProductRow> sales = {{480, "git gud", 1, 1, 0}};
    CHECK(db.ReplaceDate(d1, sales, now), "seed released app via ReplaceDate");

    // Discovered only via GetPartnerAppListForWebAPIKey - no sales ever.
    CHECK(db.UpsertKnownApp(222850, "Some DLC", "dlc", now),
          "UpsertKnownApp: dlc");
    CHECK(db.UpsertKnownApp(999999, "Upcoming Game", "game", now),
          "UpsertKnownApp: unreleased game");

    auto products = db.GetAllProducts();
    CHECK(products.size() == 3, "all three apps known");

    const auto* released = FindProduct(products, 480);
    CHECK(released && released->appType == "game" && released->everSold,
          "sales-path app: type game, everSold true");

    const auto* dlc = FindProduct(products, 222850);
    CHECK(dlc && dlc->appType == "dlc" && !dlc->everSold,
          "app-list-only dlc: type dlc, everSold false");

    const auto* unreleased = FindProduct(products, 999999);
    CHECK(unreleased && unreleased->appType == "game" && !unreleased->everSold,
          "app-list-only game with no sales: everSold false (unreleased)");

    // UpsertKnownApp is authoritative for type/name but must not run for the
    // sales path - ReplaceDate must never clobber a type UpsertKnownApp set.
    CHECK(db.UpsertKnownApp(480, "git gud", "application", now),
          "UpsertKnownApp can override an existing app's type");
    std::vector<storage::DailyProductRow> sales2 = {{480, "git gud", 2, 2, 0}};
    CHECK(db.ReplaceDate(d2, sales2, now), "ReplaceDate again for app 480");
    auto productsAfterResync = db.GetAllProducts();
    const auto* afterResync = FindProduct(productsAfterResync, 480);
    CHECK(afterResync && afterResync->appType == "application",
          "ReplaceDate (sales path) does not clobber app_type");

    // Name -> appid lookup that sale attribution resolves package names
    // against. DLC discovered only via the app list must be in it, otherwise
    // DLC sales cannot be told apart from their base game.
    const auto byName = db.GetAppIdsByName();
    CHECK(byName.size() == 3, "every known app is in the name lookup");
    CHECK(byName.count("Some DLC") == 1 && byName.at("Some DLC") == 222850,
          "app-list-only DLC is resolvable by name");
    CHECK(byName.count("Upcoming Game") == 1 &&
              byName.at("Upcoming Game") == 999999,
          "unreleased app is resolvable by name");
    CHECK(byName.count("No Such Product") == 0,
          "unknown name is absent (caller falls back to primary_appid)");

    // Per-app wishlist balance rows, including an app with wishlist data but
    // no sales at all (the unreleased title).
    storage::DailyAppWishlistRow w1{480, 100, 10, 5, 0};    // balance 85
    storage::DailyAppWishlistRow w2{999999, 50, 0, 0, 0};   // balance 50
    CHECK(db.UpsertWishlistDay(d1, w1), "wishlist day for released app");
    CHECK(db.UpsertWishlistDay(d1, w2), "wishlist day for unreleased app");
    storage::DailyAppWishlistRow w1b{480, 10, 0, 0, 0};     // balance 10
    CHECK(db.UpsertWishlistDay(d2, w1b), "second wishlist day for app 480");

    auto dateRows = db.QueryWishlistBalanceRowsForDate(d1);
    CHECK(BalanceFor(dateRows, 480) == 85, "date rows: app 480 balance");
    CHECK(BalanceFor(dateRows, 999999) == 50,
          "date rows: unreleased app balance");
    CHECK(BalanceFor(dateRows, 222850) == 0,
          "date rows: app with no wishlist data defaults to 0 (LEFT JOIN)");

    auto rangeRows = db.QueryWishlistBalanceRowsForRange(d1, d2);
    CHECK(BalanceFor(rangeRows, 480) == 95,
          "range rows: app 480 summed across dates (85+10)");
    CHECK(BalanceFor(rangeRows, 999999) == 50,
          "range rows: unreleased app's single-date balance included");
    CHECK(BalanceFor(rangeRows, 222850) == -999999,
          "range rows: app with zero wishlist rows ever is absent (INNER JOIN, "
          "unlike QueryWishlistBalanceRowsForDate's LEFT JOIN default-0)");

    auto lifetimeRows = db.QueryWishlistBalanceRowsLifetime();
    CHECK(BalanceFor(lifetimeRows, 480) == 95, "lifetime rows: app 480");
    CHECK(BalanceFor(lifetimeRows, 999999) == 50,
          "lifetime rows: unreleased app");
    CHECK(BalanceFor(lifetimeRows, 222850) == -999999,
          "lifetime rows: app with zero wishlist rows ever is absent");

    db.Close();
    ::DeleteFileW(path.c_str());
    ::DeleteFileW((path + L"-wal").c_str());
    ::DeleteFileW((path + L"-shm").c_str());
}

int main() {
    TestAggregation();
    TestDatabase();
    TestWishlist();
    TestProductClassification();
    std::printf("\n%s (failures=%d)\n",
                g_fail ? "TESTS FAILED" : "ALL TESTS PASSED", g_fail);
    return g_fail ? 1 : 0;
}
