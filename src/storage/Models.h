#pragma once

#include <cstdint>
#include <string>

namespace storage {

// One aggregated per-product row for a single financial date, ready to persist.
// Produced by the aggregator (Phase 5 filters/groups raw Steam records into
// these); consumed by Database::ReplaceDate.
struct DailyProductRow {
    std::int64_t appId = 0;
    std::string displayName;  // UTF-8
    std::int64_t netUnitsSold = 0;
    std::int64_t grossUnitsSold = 0;
    std::int64_t grossUnitsReturned = 0;
};

// A product + a unit total, for display in the UI. Name is UTF-16 for Win32.
struct ProductUnits {
    std::int64_t appId = 0;
    std::wstring displayName;
    std::int64_t units = 0;
};

// Net-unit summary totals across standard windows.
struct SummaryTotals {
    std::int64_t today = 0;
    std::int64_t week = 0;
    std::int64_t month = 0;
    std::int64_t lifetime = 0;
};

// A known product's app id + display name (from the products table).
struct AppRef {
    std::int64_t appId = 0;
    std::wstring displayName;
};

// One aggregated per-app wishlist row for a single financial date, ready to
// persist. Produced from steam::AppWishlistReport by the wishlist sync.
struct DailyAppWishlistRow {
    std::int64_t appId = 0;
    std::int64_t adds = 0;
    std::int64_t deletes = 0;
    std::int64_t purchases = 0;
    std::int64_t gifts = 0;
};

// A known app plus enough to classify it for the Product Sales window's
// Released/DLC/Unreleased grouping. everSold is lifetime, not period-scoped -
// a released game with zero sales today must still count as Released.
struct ProductInfo {
    std::int64_t appId = 0;
    std::wstring displayName;
    std::string appType;  // "game", "dlc", "application", "tool", "demo", "music"
    bool everSold = false;
};

// A product + its net wishlist balance for a period, for display in the UI.
struct ProductWishlistBalance {
    std::int64_t appId = 0;
    std::wstring displayName;
    std::int64_t balance = 0;
};

}  // namespace storage
