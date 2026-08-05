#include "storage/Migrations.h"

#include "sqlite3.h"

namespace storage {

namespace {

// Schema version 1 - the full schema from plan §9. High-water marks are TEXT
// because Steam documents them as unsigned 64-bit ids returned as strings.
constexpr const char* kSchemaV1 = R"SQL(
CREATE TABLE schema_version (
    version INTEGER NOT NULL
);

CREATE TABLE sync_state (
    id INTEGER PRIMARY KEY CHECK (id = 1),
    changed_dates_highwatermark TEXT NOT NULL DEFAULT '0',
    last_successful_check_utc TEXT,
    last_successful_sync_utc TEXT,
    last_error TEXT
);

CREATE TABLE products (
    app_id INTEGER PRIMARY KEY,
    display_name TEXT NOT NULL,
    last_seen_utc TEXT NOT NULL
);

CREATE TABLE daily_product_sales (
    financial_date TEXT NOT NULL,
    app_id INTEGER NOT NULL,
    gross_units_sold INTEGER NOT NULL DEFAULT 0,
    gross_units_returned INTEGER NOT NULL DEFAULT 0,
    net_units_sold INTEGER NOT NULL DEFAULT 0,

    PRIMARY KEY (financial_date, app_id),
    FOREIGN KEY (app_id) REFERENCES products(app_id)
);

CREATE INDEX idx_daily_product_sales_app
    ON daily_product_sales(app_id);

CREATE INDEX idx_daily_product_sales_date
    ON daily_product_sales(financial_date);

INSERT INTO sync_state (id, changed_dates_highwatermark) VALUES (1, '0');
INSERT INTO schema_version (version) VALUES (1);
)SQL";

// Schema version 2 - resumable backfill. Adds a checkpoint recording the newest
// financial date fully synced during an initial (high-water mark == '0') sync,
// so an interrupted full backfill resumes instead of restarting. If an existing
// database is mid-backfill, seed the cursor from the newest stored date so
// current progress is salvaged (per-date writes are atomic, so MAX is complete).
constexpr const char* kSchemaV2 = R"SQL(
ALTER TABLE sync_state ADD COLUMN backfill_cursor TEXT;

UPDATE sync_state
   SET backfill_cursor = (SELECT MAX(financial_date) FROM daily_product_sales)
 WHERE id = 1
   AND changed_dates_highwatermark = '0'
   AND EXISTS (SELECT 1 FROM daily_product_sales);

UPDATE schema_version SET version = 2;
)SQL";

// Schema version 3 - wishlist balance tracking (adds/deletes/purchases/gifts
// from GetAppWishlistReporting). One row per (financial_date, app_id), plus a
// per-app sync cursor since that API is queried per app rather than paginated
// across all apps like GetDetailedSales.
constexpr const char* kSchemaV3 = R"SQL(
CREATE TABLE daily_app_wishlist (
    financial_date TEXT NOT NULL,
    app_id INTEGER NOT NULL,
    wishlist_adds INTEGER NOT NULL DEFAULT 0,
    wishlist_deletes INTEGER NOT NULL DEFAULT 0,
    wishlist_purchases INTEGER NOT NULL DEFAULT 0,
    wishlist_gifts INTEGER NOT NULL DEFAULT 0,
    wishlist_balance INTEGER NOT NULL DEFAULT 0,

    PRIMARY KEY (financial_date, app_id),
    FOREIGN KEY (app_id) REFERENCES products(app_id)
);

CREATE INDEX idx_daily_app_wishlist_date
    ON daily_app_wishlist(financial_date);

CREATE TABLE wishlist_sync_state (
    app_id INTEGER PRIMARY KEY,
    backfill_days_covered INTEGER NOT NULL DEFAULT 0,
    min_available_date TEXT,

    FOREIGN KEY (app_id) REFERENCES products(app_id)
);

UPDATE schema_version SET version = 3;
)SQL";

// Schema version 4 - product classification. GetDetailedSales only ever
// surfaces apps that have sold something, so DLC and unreleased titles are
// discovered separately (GetPartnerAppListForWebAPIKey) and need a type to
// tell them apart in the UI. Defaults to 'game' for rows created by the
// sales path, which never carries a type of its own.
constexpr const char* kSchemaV4 = R"SQL(
ALTER TABLE products ADD COLUMN app_type TEXT NOT NULL DEFAULT 'game';

UPDATE schema_version SET version = 4;
)SQL";

bool Exec(sqlite3* db, const char* sql) {
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (err) {
        sqlite3_free(err);
    }
    return rc == SQLITE_OK;
}

// Runs `sql` wrapped in a transaction; rolls back on failure.
bool ApplyStep(sqlite3* db, const char* sql) {
    if (!Exec(db, "BEGIN IMMEDIATE;")) {
        return false;
    }
    if (!Exec(db, sql)) {
        Exec(db, "ROLLBACK;");
        return false;
    }
    return Exec(db, "COMMIT;");
}

}  // namespace

int GetSchemaVersion(sqlite3* db) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT version FROM schema_version LIMIT 1;", -1,
                           &stmt, nullptr) != SQLITE_OK) {
        return 0;  // table absent => fresh database
    }
    int version = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        version = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return version;
}

bool ApplyMigrations(sqlite3* db) {
    int version = GetSchemaVersion(db);

    if (version < 1) {
        if (!ApplyStep(db, kSchemaV1)) {
            return false;
        }
        version = 1;
    }
    if (version < 2) {
        if (!ApplyStep(db, kSchemaV2)) {
            return false;
        }
        version = 2;
    }
    if (version < 3) {
        if (!ApplyStep(db, kSchemaV3)) {
            return false;
        }
        version = 3;
    }
    if (version < 4) {
        if (!ApplyStep(db, kSchemaV4)) {
            return false;
        }
        version = 4;
    }

    return version >= kTargetSchemaVersion;
}

}  // namespace storage
