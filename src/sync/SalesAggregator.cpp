#include "sync/SalesAggregator.h"

#include <unordered_map>

namespace sync {

namespace {
constexpr char kLineItemPackage[] = "Package";
constexpr char kPackageSaleSteam[] = "Steam";
}  // namespace

std::vector<storage::DailyProductRow> AggregateDate(
    const std::vector<steam::DetailedSalesRecord>& records) {
    // Preserve first-seen order of app ids for stable, testable output.
    std::unordered_map<std::int64_t, size_t> indexByApp;
    std::vector<storage::DailyProductRow> rows;

    for (const steam::DetailedSalesRecord& rec : records) {
        // Category filter: only Steam store package sales count.
        if (rec.lineItemType != kLineItemPackage ||
            rec.packageSaleType != kPackageSaleSteam) {
            continue;
        }
        if (rec.appId == 0) {
            continue;  // no stable product identity
        }

        auto it = indexByApp.find(rec.appId);
        if (it == indexByApp.end()) {
            storage::DailyProductRow row;
            row.appId = rec.appId;
            row.displayName = rec.primaryAppName;
            row.netUnitsSold = rec.netUnitsSold;
            row.grossUnitsSold = rec.grossUnitsSold;
            row.grossUnitsReturned = rec.grossUnitsReturned;
            indexByApp.emplace(rec.appId, rows.size());
            rows.push_back(std::move(row));
        } else {
            storage::DailyProductRow& row = rows[it->second];
            row.netUnitsSold += rec.netUnitsSold;
            row.grossUnitsSold += rec.grossUnitsSold;
            row.grossUnitsReturned += rec.grossUnitsReturned;
            // Keep the last non-empty name we encounter.
            if (!rec.primaryAppName.empty()) {
                row.displayName = rec.primaryAppName;
            }
        }
    }

    return rows;
}

}  // namespace sync
