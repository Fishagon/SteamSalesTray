#include "storage/Database.h"

#include "platform/StringUtil.h"
#include "storage/Migrations.h"
#include "sqlite3.h"

namespace storage {

namespace {

bool ExecSimple(sqlite3* db, const char* sql) {
    return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

void BindText(sqlite3_stmt* stmt, int index, const std::string& value) {
    sqlite3_bind_text(stmt, index, value.c_str(),
                      static_cast<int>(value.size()), SQLITE_TRANSIENT);
}

std::string ColumnText(sqlite3_stmt* stmt, int col) {
    const auto* text = sqlite3_column_text(stmt, col);
    if (!text) {
        return {};
    }
    const int bytes = sqlite3_column_bytes(stmt, col);
    return std::string(reinterpret_cast<const char*>(text),
                       static_cast<size_t>(bytes));
}

// Runs a query that returns (app_id, display_name, units) rows.
std::vector<ProductUnits> RunProductQuery(sqlite3_stmt* stmt) {
    std::vector<ProductUnits> rows;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ProductUnits row;
        row.appId = sqlite3_column_int64(stmt, 0);
        row.displayName = platform::Utf8ToWide(ColumnText(stmt, 1));
        row.units = sqlite3_column_int64(stmt, 2);
        rows.push_back(std::move(row));
    }
    return rows;
}

// Runs a query that returns (app_id, display_name, balance) rows.
std::vector<ProductWishlistBalance> RunWishlistBalanceQuery(sqlite3_stmt* stmt) {
    std::vector<ProductWishlistBalance> rows;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ProductWishlistBalance row;
        row.appId = sqlite3_column_int64(stmt, 0);
        row.displayName = platform::Utf8ToWide(ColumnText(stmt, 1));
        row.balance = sqlite3_column_int64(stmt, 2);
        rows.push_back(std::move(row));
    }
    return rows;
}

std::int64_t RunScalarSum(sqlite3* db, const char* sql,
                          const std::string* a = nullptr,
                          const std::string* b = nullptr) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    int idx = 1;
    if (a) BindText(stmt, idx++, *a);
    if (b) BindText(stmt, idx++, *b);
    std::int64_t total = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        total = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return total;
}

// Sets a single sync_state text column.
bool SetSyncStateColumn(sqlite3* db, const char* column,
                        const std::string& value) {
    const std::string sql =
        std::string("UPDATE sync_state SET ") + column + " = ? WHERE id = 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    BindText(stmt, 1, value);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

}  // namespace

Database::~Database() {
    Close();
}

bool Database::Open(const std::wstring& path) {
    Close();
    const std::string utf8Path = platform::WideToUtf8(path);
    if (sqlite3_open_v2(utf8Path.c_str(), &db_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                        nullptr) != SQLITE_OK) {
        Close();
        return false;
    }
    ExecSimple(db_, "PRAGMA foreign_keys = ON;");
    ExecSimple(db_, "PRAGMA journal_mode = WAL;");
    ExecSimple(db_, "PRAGMA synchronous = NORMAL;");
    ExecSimple(db_, "PRAGMA busy_timeout = 5000;");

    if (!ApplyMigrations(db_)) {
        Close();
        return false;
    }
    return true;
}

void Database::Close() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

std::string Database::GetChangedDatesHighwatermark() {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_, "SELECT changed_dates_highwatermark FROM sync_state WHERE id=1;",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return "0";
    }
    std::string value = "0";
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        value = ColumnText(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return value.empty() ? "0" : value;
}

bool Database::SetChangedDatesHighwatermark(const std::string& highwatermark) {
    return SetSyncStateColumn(db_, "changed_dates_highwatermark", highwatermark);
}

bool Database::SetLastSuccessfulCheckUtc(const std::string& iso8601) {
    return SetSyncStateColumn(db_, "last_successful_check_utc", iso8601);
}

bool Database::SetLastSuccessfulSyncUtc(const std::string& iso8601) {
    return SetSyncStateColumn(db_, "last_successful_sync_utc", iso8601);
}

std::string Database::GetLastSuccessfulSyncUtc() {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_, "SELECT last_successful_sync_utc FROM sync_state WHERE id=1;",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return {};
    }
    std::string value;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        value = ColumnText(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return value;
}

bool Database::SetLastError(const std::string& message) {
    return SetSyncStateColumn(db_, "last_error", message);
}

std::string Database::GetLastError() {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT last_error FROM sync_state WHERE id=1;",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        return {};
    }
    std::string value;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        value = ColumnText(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return value;
}

std::string Database::GetBackfillCursor() {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_, "SELECT backfill_cursor FROM sync_state WHERE id=1;", -1,
            &stmt, nullptr) != SQLITE_OK) {
        return {};
    }
    std::string value;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        value = ColumnText(stmt, 0);  // NULL -> empty
    }
    sqlite3_finalize(stmt);
    return value;
}

bool Database::SetBackfillCursor(const std::string& date) {
    if (date.empty()) {
        return ExecSimple(
            db_, "UPDATE sync_state SET backfill_cursor=NULL WHERE id=1;");
    }
    return SetSyncStateColumn(db_, "backfill_cursor", date);
}

bool Database::ReplaceDate(const std::string& date,
                           const std::vector<DailyProductRow>& rows,
                           const std::string& nowUtc) {
    if (!db_) {
        return false;
    }
    if (!ExecSimple(db_, "BEGIN IMMEDIATE;")) {
        return false;
    }

    auto fail = [&]() {
        ExecSimple(db_, "ROLLBACK;");
        return false;
    };

    // 1) Remove the date's existing rows.
    {
        sqlite3_stmt* del = nullptr;
        if (sqlite3_prepare_v2(
                db_, "DELETE FROM daily_product_sales WHERE financial_date=?;",
                -1, &del, nullptr) != SQLITE_OK) {
            return fail();
        }
        BindText(del, 1, date);
        const bool ok = sqlite3_step(del) == SQLITE_DONE;
        sqlite3_finalize(del);
        if (!ok) {
            return fail();
        }
    }

    // 2) Upsert products + insert new daily rows (reuse prepared statements).
    sqlite3_stmt* upsertProduct = nullptr;
    sqlite3_stmt* insertDaily = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO products (app_id, display_name, last_seen_utc) "
            "VALUES (?,?,?) "
            "ON CONFLICT(app_id) DO UPDATE SET "
            "display_name=excluded.display_name, "
            "last_seen_utc=excluded.last_seen_utc;",
            -1, &upsertProduct, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(
            db_,
            "INSERT INTO daily_product_sales "
            "(financial_date, app_id, gross_units_sold, gross_units_returned, "
            "net_units_sold) VALUES (?,?,?,?,?);",
            -1, &insertDaily, nullptr) != SQLITE_OK) {
        if (upsertProduct) sqlite3_finalize(upsertProduct);
        if (insertDaily) sqlite3_finalize(insertDaily);
        return fail();
    }

    bool ok = true;
    for (const DailyProductRow& row : rows) {
        const std::string name =
            row.displayName.empty() ? ("App " + std::to_string(row.appId))
                                     : row.displayName;

        sqlite3_reset(upsertProduct);
        sqlite3_bind_int64(upsertProduct, 1, row.appId);
        BindText(upsertProduct, 2, name);
        BindText(upsertProduct, 3, nowUtc);
        if (sqlite3_step(upsertProduct) != SQLITE_DONE) {
            ok = false;
            break;
        }

        sqlite3_reset(insertDaily);
        BindText(insertDaily, 1, date);
        sqlite3_bind_int64(insertDaily, 2, row.appId);
        sqlite3_bind_int64(insertDaily, 3, row.grossUnitsSold);
        sqlite3_bind_int64(insertDaily, 4, row.grossUnitsReturned);
        sqlite3_bind_int64(insertDaily, 5, row.netUnitsSold);
        if (sqlite3_step(insertDaily) != SQLITE_DONE) {
            ok = false;
            break;
        }
    }

    sqlite3_finalize(upsertProduct);
    sqlite3_finalize(insertDaily);

    if (!ok) {
        return fail();
    }
    return ExecSimple(db_, "COMMIT;");
}

bool Database::ClearAllSales() {
    if (!db_) {
        return false;
    }
    if (!ExecSimple(db_, "BEGIN IMMEDIATE;")) {
        return false;
    }
    // Wishlist rows/cursors reference products via FK, so they must go before
    // the products delete (foreign_keys=ON would otherwise reject it).
    if (!ExecSimple(db_, "DELETE FROM daily_app_wishlist;") ||
        !ExecSimple(db_, "DELETE FROM wishlist_sync_state;") ||
        !ExecSimple(db_, "DELETE FROM daily_product_sales;") ||
        !ExecSimple(db_, "DELETE FROM products;") ||
        !ExecSimple(db_,
                    "UPDATE sync_state SET changed_dates_highwatermark='0', "
                    "last_successful_sync_utc=NULL, backfill_cursor=NULL "
                    "WHERE id=1;")) {
        ExecSimple(db_, "ROLLBACK;");
        return false;
    }
    return ExecSimple(db_, "COMMIT;");
}

std::vector<ProductUnits> Database::QueryDate(const std::string& date) {
    // Today view: list every product, LEFT JOIN this date's sales.
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT p.app_id, p.display_name, "
            "COALESCE(s.net_units_sold, 0) AS units "
            "FROM products p "
            "LEFT JOIN daily_product_sales s "
            "  ON s.app_id = p.app_id AND s.financial_date = ? "
            "ORDER BY units DESC, p.display_name COLLATE NOCASE;",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return {};
    }
    BindText(stmt, 1, date);
    auto rows = RunProductQuery(stmt);
    sqlite3_finalize(stmt);
    return rows;
}

std::vector<ProductUnits> Database::QueryRange(const std::string& startDate,
                                               const std::string& endDate) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT p.app_id, p.display_name, SUM(s.net_units_sold) AS units "
            "FROM daily_product_sales s "
            "JOIN products p ON p.app_id = s.app_id "
            "WHERE s.financial_date BETWEEN ? AND ? "
            "GROUP BY p.app_id, p.display_name "
            "ORDER BY units DESC, p.display_name COLLATE NOCASE;",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return {};
    }
    BindText(stmt, 1, startDate);
    BindText(stmt, 2, endDate);
    auto rows = RunProductQuery(stmt);
    sqlite3_finalize(stmt);
    return rows;
}

std::vector<ProductUnits> Database::QueryLifetime() {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT p.app_id, p.display_name, SUM(s.net_units_sold) AS units "
            "FROM daily_product_sales s "
            "JOIN products p ON p.app_id = s.app_id "
            "GROUP BY p.app_id, p.display_name "
            "ORDER BY units DESC, p.display_name COLLATE NOCASE;",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return {};
    }
    auto rows = RunProductQuery(stmt);
    sqlite3_finalize(stmt);
    return rows;
}

std::int64_t Database::SumForDate(const std::string& date) {
    return RunScalarSum(
        db_,
        "SELECT COALESCE(SUM(net_units_sold),0) FROM daily_product_sales "
        "WHERE financial_date = ?;",
        &date);
}

std::int64_t Database::SumForRange(const std::string& startDate,
                                   const std::string& endDate) {
    return RunScalarSum(
        db_,
        "SELECT COALESCE(SUM(net_units_sold),0) FROM daily_product_sales "
        "WHERE financial_date BETWEEN ? AND ?;",
        &startDate, &endDate);
}

std::int64_t Database::SumLifetime() {
    return RunScalarSum(
        db_,
        "SELECT COALESCE(SUM(net_units_sold),0) FROM daily_product_sales;");
}

std::vector<AppRef> Database::GetKnownApps() {
    std::vector<AppRef> apps;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT app_id, display_name FROM products;",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        return apps;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        AppRef app;
        app.appId = sqlite3_column_int64(stmt, 0);
        app.displayName = platform::Utf8ToWide(ColumnText(stmt, 1));
        apps.push_back(std::move(app));
    }
    sqlite3_finalize(stmt);
    return apps;
}

bool Database::UpsertWishlistDay(const std::string& date,
                                 const DailyAppWishlistRow& row) {
    if (!db_) {
        return false;
    }
    const std::int64_t balance =
        row.adds - row.deletes - row.purchases - row.gifts;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO daily_app_wishlist "
            "(financial_date, app_id, wishlist_adds, wishlist_deletes, "
            "wishlist_purchases, wishlist_gifts, wishlist_balance) "
            "VALUES (?,?,?,?,?,?,?) "
            "ON CONFLICT(financial_date, app_id) DO UPDATE SET "
            "wishlist_adds=excluded.wishlist_adds, "
            "wishlist_deletes=excluded.wishlist_deletes, "
            "wishlist_purchases=excluded.wishlist_purchases, "
            "wishlist_gifts=excluded.wishlist_gifts, "
            "wishlist_balance=excluded.wishlist_balance;",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    BindText(stmt, 1, date);
    sqlite3_bind_int64(stmt, 2, row.appId);
    sqlite3_bind_int64(stmt, 3, row.adds);
    sqlite3_bind_int64(stmt, 4, row.deletes);
    sqlite3_bind_int64(stmt, 5, row.purchases);
    sqlite3_bind_int64(stmt, 6, row.gifts);
    sqlite3_bind_int64(stmt, 7, balance);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool Database::GetWishlistSyncState(std::int64_t appId, int& backfillDays,
                                    std::string& minDate) {
    backfillDays = 0;
    minDate.clear();
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT backfill_days_covered, min_available_date "
            "FROM wishlist_sync_state WHERE app_id = ?;",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, appId);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        backfillDays = sqlite3_column_int(stmt, 0);
        minDate = ColumnText(stmt, 1);
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

bool Database::SetWishlistSyncState(std::int64_t appId, int backfillDays,
                                    const std::string& minDate) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO wishlist_sync_state "
            "(app_id, backfill_days_covered, min_available_date) "
            "VALUES (?,?,?) "
            "ON CONFLICT(app_id) DO UPDATE SET "
            "backfill_days_covered=excluded.backfill_days_covered, "
            "min_available_date=excluded.min_available_date;",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, appId);
    sqlite3_bind_int(stmt, 2, backfillDays);
    if (minDate.empty()) {
        sqlite3_bind_null(stmt, 3);
    } else {
        BindText(stmt, 3, minDate);
    }
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<ProductInfo> Database::GetAllProducts() {
    std::vector<ProductInfo> products;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT p.app_id, p.display_name, p.app_type, "
            "EXISTS(SELECT 1 FROM daily_product_sales d "
            "       WHERE d.app_id = p.app_id) AS ever_sold "
            "FROM products p "
            "ORDER BY p.display_name COLLATE NOCASE;",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return products;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ProductInfo info;
        info.appId = sqlite3_column_int64(stmt, 0);
        info.displayName = platform::Utf8ToWide(ColumnText(stmt, 1));
        info.appType = ColumnText(stmt, 2);
        info.everSold = sqlite3_column_int(stmt, 3) != 0;
        products.push_back(std::move(info));
    }
    sqlite3_finalize(stmt);
    return products;
}

bool Database::UpsertKnownApp(std::int64_t appId,
                              const std::string& displayNameUtf8,
                              const std::string& appType,
                              const std::string& nowUtc) {
    if (!db_) {
        return false;
    }
    const std::string name =
        displayNameUtf8.empty() ? ("App " + std::to_string(appId))
                                 : displayNameUtf8;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO products (app_id, display_name, app_type, "
            "last_seen_utc) VALUES (?,?,?,?) "
            "ON CONFLICT(app_id) DO UPDATE SET "
            "display_name=excluded.display_name, "
            "app_type=excluded.app_type, "
            "last_seen_utc=excluded.last_seen_utc;",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, appId);
    BindText(stmt, 2, name);
    BindText(stmt, 3, appType.empty() ? "game" : appType);
    BindText(stmt, 4, nowUtc);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::unordered_map<std::string, std::int64_t> Database::GetAppIdsByName() {
    std::unordered_map<std::string, std::int64_t> byName;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "SELECT display_name, app_id FROM products "
                           "WHERE display_name <> '';",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        return byName;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        byName[ColumnText(stmt, 0)] = sqlite3_column_int64(stmt, 1);
    }
    sqlite3_finalize(stmt);
    return byName;
}

std::vector<ProductWishlistBalance> Database::QueryWishlistBalanceRowsForDate(
    const std::string& date) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT p.app_id, p.display_name, "
            "COALESCE(w.wishlist_balance, 0) AS balance "
            "FROM products p "
            "LEFT JOIN daily_app_wishlist w "
            "  ON w.app_id = p.app_id AND w.financial_date = ? "
            "ORDER BY balance DESC, p.display_name COLLATE NOCASE;",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return {};
    }
    BindText(stmt, 1, date);
    auto rows = RunWishlistBalanceQuery(stmt);
    sqlite3_finalize(stmt);
    return rows;
}

std::vector<ProductWishlistBalance> Database::QueryWishlistBalanceRowsForRange(
    const std::string& startDate, const std::string& endDate) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT p.app_id, p.display_name, "
            "SUM(w.wishlist_balance) AS balance "
            "FROM daily_app_wishlist w "
            "JOIN products p ON p.app_id = w.app_id "
            "WHERE w.financial_date BETWEEN ? AND ? "
            "GROUP BY p.app_id, p.display_name "
            "ORDER BY balance DESC, p.display_name COLLATE NOCASE;",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return {};
    }
    BindText(stmt, 1, startDate);
    BindText(stmt, 2, endDate);
    auto rows = RunWishlistBalanceQuery(stmt);
    sqlite3_finalize(stmt);
    return rows;
}

std::vector<ProductWishlistBalance> Database::QueryWishlistBalanceRowsLifetime() {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT p.app_id, p.display_name, "
            "SUM(w.wishlist_balance) AS balance "
            "FROM daily_app_wishlist w "
            "JOIN products p ON p.app_id = w.app_id "
            "GROUP BY p.app_id, p.display_name "
            "ORDER BY balance DESC, p.display_name COLLATE NOCASE;",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return {};
    }
    auto rows = RunWishlistBalanceQuery(stmt);
    sqlite3_finalize(stmt);
    return rows;
}

}  // namespace storage
