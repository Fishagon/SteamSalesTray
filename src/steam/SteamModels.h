#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace steam {

// Result of GetChangedDatesForPartner.
struct ChangedDates {
    // Steam financial dates ("YYYY/MM/DD", Pacific time) whose data changed.
    std::vector<std::string> dates;
    // Global high-water mark. Documented as an unsigned 64-bit id, sometimes
    // returned as a string, so we keep it as text (see plan §9).
    std::string resultHighwatermark;
};

// One raw line-item record from GetDetailedSales, before category filtering.
// Filtering/aggregation (Package + Steam, resolved to an app) happens in the
// aggregator (Phase 4) so this stays a faithful representation.
//
// What Steam actually sends per row, verified against live responses: the
// thing sold is a *package* (packageid, named via response.package_info).
// There is no per-row appid. primary_appid is only "the appid that this
// package associates to for purpose of additional revenue share accrual",
// which for a DLC package is the base game - so it cannot identify the
// product sold. The package name is the only per-row DLC identity available.
struct DetailedSalesRecord {
    std::string lineItemType;      // e.g. "Package", "MicroTxn"
    std::string packageSaleType;   // e.g. "Steam", "Retail"
    // The package actually sold, and its name from the package_info lookup.
    std::int64_t packageId = 0;
    std::string packageName;
    // Revenue-share attribution target (base game for a DLC package), plus
    // its name from the app_info lookup. Used only as a fallback identity.
    std::int64_t primaryAppId = 0;
    std::string primaryAppName;
    std::int64_t netUnitsSold = 0;
    std::int64_t grossUnitsSold = 0;
    std::int64_t grossUnitsReturned = 0;
};

// One page of GetDetailedSales.
struct DetailedSalesPage {
    std::vector<DetailedSalesRecord> records;
    // Pagination cursor. Requesting with highwatermark_id == this value's
    // previous request terminates the loop (see plan §6).
    std::string maxId;
    bool hasMaxId = false;
};

// wishlist_summary block from GetAppWishlistReporting.
struct WishlistActionSummary {
    std::int64_t adds = 0;
    std::int64_t deletes = 0;
    std::int64_t purchases = 0;
    std::int64_t gifts = 0;
};

// Result of GetAppWishlistReporting for one (appid, date).
struct AppWishlistReport {
    std::int64_t appId = 0;
    std::string date;
    WishlistActionSummary summary;
    // Earliest date Steam has wishlist data for this app; empty if unknown.
    std::string appMinDate;
};

// One entry from ISteamApps/GetPartnerAppListForWebAPIKey - every app tied to
// the account, regardless of sales history (so it's the only way to discover
// DLC and unreleased titles).
struct PartnerApp {
    std::int64_t appId = 0;
    std::string appType;  // "game", "dlc", "application", "tool", "demo", "music"
    std::string appName;  // UTF-8
};

}  // namespace steam
