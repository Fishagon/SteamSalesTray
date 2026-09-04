#include "sync/SalesAggregator.h"

#include <unordered_map>

namespace sync {

namespace {
constexpr char kLineItemPackage[] = "Package";
constexpr char kPackageSaleSteam[] = "Steam";

// The app a row's units belong to, plus the name to show for it. Prefers the
// package's own app (so a DLC keeps its own units); falls back to the
// revenue-share target when the package name matches no known app.
struct Attribution {
    std::int64_t appId = 0;
    std::string displayName;
};

Attribution Attribute(const steam::DetailedSalesRecord& rec,
                      const AppIdsByName& appIdsByName) {
    if (!rec.packageName.empty()) {
        if (auto it = appIdsByName.find(rec.packageName);
            it != appIdsByName.end()) {
            return {it->second, rec.packageName};
        }
    }
    // Unknown package: credit the revenue-share target under its own name, so
    // a bundle or CD-key pack never renames the app it pays out to.
    return {rec.primaryAppId, rec.primaryAppName};
}
}  // namespace

std::vector<storage::DailyProductRow> AggregateDate(
    const std::vector<steam::DetailedSalesRecord>& records,
    const AppIdsByName& appIdsByName) {
    // Preserve first-seen order of app ids for stable, testable output.
    std::unordered_map<std::int64_t, size_t> indexByApp;
    std::vector<storage::DailyProductRow> rows;

    for (const steam::DetailedSalesRecord& rec : records) {
        // Category filter: only Steam store package sales count.
        if (rec.lineItemType != kLineItemPackage ||
            rec.packageSaleType != kPackageSaleSteam) {
            continue;
        }
        const Attribution attribution = Attribute(rec, appIdsByName);
        if (attribution.appId == 0) {
            continue;  // no stable product identity
        }

        auto it = indexByApp.find(attribution.appId);
        if (it == indexByApp.end()) {
            storage::DailyProductRow row;
            row.appId = attribution.appId;
            row.displayName = attribution.displayName;
            row.netUnitsSold = rec.netUnitsSold;
            row.grossUnitsSold = rec.grossUnitsSold;
            row.grossUnitsReturned = rec.grossUnitsReturned;
            indexByApp.emplace(attribution.appId, rows.size());
            rows.push_back(std::move(row));
        } else {
            storage::DailyProductRow& row = rows[it->second];
            row.netUnitsSold += rec.netUnitsSold;
            row.grossUnitsSold += rec.grossUnitsSold;
            row.grossUnitsReturned += rec.grossUnitsReturned;
            // Keep the last non-empty name we encounter.
            if (!attribution.displayName.empty()) {
                row.displayName = attribution.displayName;
            }
        }
    }

    return rows;
}

}  // namespace sync
