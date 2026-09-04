#pragma once

#include <vector>

#include "steam/SteamModels.h"
#include "storage/Models.h"

namespace sync {

// Filters and aggregates raw GetDetailedSales records into per-product rows for
// one financial date (plan §7):
//   - keeps only line_item_type == "Package" && package_sale_type == "Steam"
//   - groups by appid (the actual product sold, not primary_appid - that's
//     only the revenue-share attribution target and for a DLC package is
//     typically the base game), summing net/gross/returned units
//   - keeps the last non-empty primary_app_name seen for each app
// Negative net units are preserved (refunds/corrections). Rows with a zero
// appid are dropped (no stable product identity).
std::vector<storage::DailyProductRow> AggregateDate(
    const std::vector<steam::DetailedSalesRecord>& records);

}  // namespace sync
