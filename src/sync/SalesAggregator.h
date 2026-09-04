#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "steam/SteamModels.h"
#include "storage/Models.h"

namespace sync {

// Maps a package name (UTF-8, as sent in package_info) to the appid of the
// product that package sells. Built from the known-apps list, which is the
// only place DLC appids are available - GetDetailedSales never sends them.
using AppIdsByName = std::unordered_map<std::string, std::int64_t>;

// Filters and aggregates raw GetDetailedSales records into per-product rows for
// one financial date (plan §7):
//   - keeps only line_item_type == "Package" && package_sale_type == "Steam"
//   - resolves each row to an app and sums net/gross/returned units per app
//
// Attribution: a row identifies only the *package* sold. `appIdsByName` maps
// the package's name to its app, which is what separates a DLC sale from its
// base game - both carry the same primary_appid (the DLC's revenue-share
// target), so primary_appid alone would credit DLC units to the base game.
// Rows whose package name is unknown fall back to primary_appid, which is
// correct for base-game packages (and for bundles/CD-key packs named
// differently from any single app).
//
// Negative net units are preserved (refunds/corrections). Rows that resolve to
// no app at all are dropped (no stable product identity).
std::vector<storage::DailyProductRow> AggregateDate(
    const std::vector<steam::DetailedSalesRecord>& records,
    const AppIdsByName& appIdsByName);

}  // namespace sync
