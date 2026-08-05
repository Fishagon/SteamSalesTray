#pragma once

#include <string>

#include "steam/SteamModels.h"

namespace steam {

enum class ParseStatus {
    Ok,
    InvalidJson,       // not parseable as JSON
    MissingResponse,   // no top-level "response" object
    ApiError,          // Steam returned a recognizable error payload
    StructureError,    // response present but required fields malformed/absent
};

struct ParseResult {
    ParseStatus status = ParseStatus::StructureError;
    std::wstring message;  // sanitized; safe to log/display
    bool ok() const { return status == ParseStatus::Ok; }
};

// Parses a GetChangedDatesForPartner response body. An empty "dates" array is
// valid (no changes). Requires a top-level "response" object.
ParseResult ParseChangedDates(const std::string& json, ChangedDates& out);

// Parses one GetDetailedSales page. Records are returned unfiltered.
ParseResult ParseDetailedSalesPage(const std::string& json,
                                   DetailedSalesPage& out);

// Parses a GetAppWishlistReporting response. A response for an out-of-range
// date is valid and carries only app_min_date (wishlist_summary absent).
ParseResult ParseAppWishlistReporting(const std::string& json,
                                      AppWishlistReport& out);

// Parses a GetPartnerAppListForWebAPIKey response. Unlike every other call in
// this app, the payload is the classic Steam Web API shape -
// {"applist":{"apps":{"app":[...]}}} - not {"response": {...}}. A missing or
// empty "app" array is valid (e.g. an account with no DLC).
ParseResult ParseGetPartnerAppList(const std::string& json,
                                   std::vector<PartnerApp>& out);

}  // namespace steam
