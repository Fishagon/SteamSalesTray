#include "steam/SteamFinancialClient.h"

#include <winhttp.h>

#include "network/UrlEncoder.h"
#include "steam/SteamJsonParser.h"

namespace steam {

namespace {
constexpr wchar_t kHost[] = L"partner.steam-api.com";
constexpr INTERNET_PORT kPort = 443;

constexpr wchar_t kPathChangedDates[] =
    L"/IPartnerFinancialsService/GetChangedDatesForPartner/v001/";
constexpr wchar_t kPathDetailedSales[] =
    L"/IPartnerFinancialsService/GetDetailedSales/v001/";
constexpr wchar_t kPathWishlistReporting[] =
    L"/IPartnerFinancialsService/GetAppWishlistReporting/v001/";
constexpr wchar_t kPathAppList[] =
    L"/ISteamApps/GetPartnerAppListForWebAPIKey/v2/";

// Response-size caps (§16).
constexpr size_t kMaxChangedDates = 8u * 1024 * 1024;
constexpr size_t kMaxDetailedSales = 32u * 1024 * 1024;
// One app/day of summary counters - tiny compared to a sales page.
constexpr size_t kMaxWishlistReporting = 2u * 1024 * 1024;
// One row per app in the account's portfolio - same tier as changed dates.
constexpr size_t kMaxAppList = 8u * 1024 * 1024;

// Safety cap so a misbehaving server can't loop forever over one date.
constexpr int kMaxPagesPerDate = 10'000;

// Builds "key=...&a=b&..." with every value percent-encoded. The key is passed
// as raw bytes and never logged. Returns a wide query string (never surfaced in
// errors/logs by callers).
std::wstring BuildQuery(const security::SecureString& keyUtf8,
                        const std::wstring& extra) {
    std::wstring q = L"key=";
    q += network::EncodeComponent(
        std::string(keyUtf8.Data(), keyUtf8.Size()));
    q += L"&include_view_grants=false";
    if (!extra.empty()) {
        q += L"&";
        q += extra;
    }
    return q;
}

// Maps a transport/HTTP outcome to a sanitized FinancialResult. On 2xx, leaves
// kind=Success for the caller to refine after parsing.
FinancialResult Classify(const network::HttpResult& http) {
    FinancialResult r;
    r.httpStatus = http.statusCode;

    if (http.exceededLimit) {
        r.kind = ResultKind::TooLarge;
        r.message = L"The response was larger than the allowed limit.";
        return r;
    }
    if (!http.transportOk) {
        const DWORD e = http.win32Error;
        switch (e) {
            case ERROR_WINHTTP_NAME_NOT_RESOLVED:
            case ERROR_WINHTTP_CANNOT_CONNECT:
            case ERROR_WINHTTP_TIMEOUT:
            case ERROR_WINHTTP_CONNECTION_ERROR:
                r.kind = ResultKind::Offline;
                break;
            default:
                r.kind = ResultKind::Other;
                break;
        }
        r.message = network::DescribeWinHttpError(e);
        return r;
    }

    const int s = http.statusCode;
    if (s >= 200 && s < 300) {
        r.kind = ResultKind::Success;
    } else if (s == 401 || s == 403) {
        r.kind = ResultKind::Auth;
        r.message = L"The API key was rejected (check the key and its "
                    L"Financial API Group access).";
    } else if (s == 429) {
        r.kind = ResultKind::RateLimited;
        r.message = L"Rate limited by Steam; will retry later.";
    } else if (s >= 500) {
        r.kind = ResultKind::ServerError;
        r.message = L"Steam reported a server error (HTTP " +
                    std::to_wstring(s) + L").";
    } else {
        r.kind = ResultKind::Other;
        r.message = L"Unexpected HTTP status " + std::to_wstring(s) + L".";
    }
    return r;
}

std::wstring ParseMessage(const ParseResult& p) {
    return p.message.empty() ? L"Could not read Steam's response." : p.message;
}

}  // namespace

FinancialResult SteamFinancialClient::GetChangedDates(
    const security::SecureString& keyUtf8, const std::string& highwatermark,
    ChangedDates& out) {
    out = {};
    if (!http_.IsValid()) {
        FinancialResult r;
        r.kind = ResultKind::Offline;
        r.message = L"Could not initialize the network session.";
        return r;
    }

    std::wstring extra =
        L"highwatermark=" +
        network::EncodeComponent(highwatermark.empty() ? std::string("0")
                                                       : highwatermark);
    std::wstring query = BuildQuery(keyUtf8, extra);

    network::HttpResult http =
        http_.Get(kHost, kPort, kPathChangedDates, query, kMaxChangedDates);
    // The query held the key; do not retain it.
    ::SecureZeroMemory(query.data(), query.size() * sizeof(wchar_t));

    FinancialResult result = Classify(http);
    if (!result.ok()) {
        return result;
    }

    ParseResult parsed = ParseChangedDates(http.body, out);
    if (!parsed.ok()) {
        result.kind = (parsed.status == ParseStatus::ApiError)
                          ? ResultKind::Auth
                          : ResultKind::Parse;
        result.message = ParseMessage(parsed);
    }
    return result;
}

FinancialResult SteamFinancialClient::GetDetailedSales(
    const security::SecureString& keyUtf8, const std::string& date,
    const std::string& highwatermarkId, DetailedSalesPage& out) {
    out = {};
    if (!http_.IsValid()) {
        FinancialResult r;
        r.kind = ResultKind::Offline;
        r.message = L"Could not initialize the network session.";
        return r;
    }

    std::wstring extra = L"date=" + network::EncodeComponent(date) +
                         L"&highwatermark_id=" +
                         network::EncodeComponent(
                             highwatermarkId.empty() ? std::string("0")
                                                     : highwatermarkId);
    std::wstring query = BuildQuery(keyUtf8, extra);

    network::HttpResult http =
        http_.Get(kHost, kPort, kPathDetailedSales, query, kMaxDetailedSales);
    ::SecureZeroMemory(query.data(), query.size() * sizeof(wchar_t));

    FinancialResult result = Classify(http);
    if (!result.ok()) {
        return result;
    }

    ParseResult parsed = ParseDetailedSalesPage(http.body, out);
    if (!parsed.ok()) {
        result.kind = (parsed.status == ParseStatus::ApiError)
                          ? ResultKind::Auth
                          : ResultKind::Parse;
        result.message = ParseMessage(parsed);
    }
    return result;
}

FinancialResult SteamFinancialClient::GetAllDetailedSalesForDate(
    const security::SecureString& keyUtf8, const std::string& date,
    std::vector<DetailedSalesRecord>& out) {
    out.clear();
    std::string requested = "0";

    for (int page = 0; page < kMaxPagesPerDate; ++page) {
        DetailedSalesPage pageResult;
        FinancialResult r =
            GetDetailedSales(keyUtf8, date, requested, pageResult);
        if (!r.ok()) {
            return r;
        }

        out.insert(out.end(),
                   std::make_move_iterator(pageResult.records.begin()),
                   std::make_move_iterator(pageResult.records.end()));

        // Termination: no cursor, or the cursor stopped advancing (§6).
        if (!pageResult.hasMaxId || pageResult.maxId == requested) {
            break;
        }
        requested = pageResult.maxId;
    }

    FinancialResult ok;
    ok.kind = ResultKind::Success;
    return ok;
}

FinancialResult SteamFinancialClient::GetAppWishlistReporting(
    const security::SecureString& keyUtf8, std::int64_t appId,
    const std::string& date, AppWishlistReport& out) {
    out = {};
    if (!http_.IsValid()) {
        FinancialResult r;
        r.kind = ResultKind::Offline;
        r.message = L"Could not initialize the network session.";
        return r;
    }

    std::wstring extra = L"appid=" + std::to_wstring(appId) +
                         L"&date=" + network::EncodeComponent(date);
    std::wstring query = BuildQuery(keyUtf8, extra);

    network::HttpResult http = http_.Get(kHost, kPort, kPathWishlistReporting,
                                        query, kMaxWishlistReporting);
    ::SecureZeroMemory(query.data(), query.size() * sizeof(wchar_t));

    FinancialResult result = Classify(http);
    if (!result.ok()) {
        return result;
    }

    ParseResult parsed = ParseAppWishlistReporting(http.body, out);
    if (!parsed.ok()) {
        result.kind = (parsed.status == ParseStatus::ApiError)
                          ? ResultKind::Auth
                          : ResultKind::Parse;
        result.message = ParseMessage(parsed);
    }
    return result;
}

FinancialResult SteamFinancialClient::GetAppList(
    const security::SecureString& keyUtf8, std::vector<PartnerApp>& out) {
    out.clear();
    if (!http_.IsValid()) {
        FinancialResult r;
        r.kind = ResultKind::Offline;
        r.message = L"Could not initialize the network session.";
        return r;
    }

    // ISteamApps is a different interface than IPartnerFinancialsService, so
    // it takes just the key - no include_view_grants (that's Financial-only).
    std::wstring query =
        L"key=" + network::EncodeComponent(
                     std::string(keyUtf8.Data(), keyUtf8.Size()));

    network::HttpResult http =
        http_.Get(kHost, kPort, kPathAppList, query, kMaxAppList);
    ::SecureZeroMemory(query.data(), query.size() * sizeof(wchar_t));

    FinancialResult result = Classify(http);
    if (!result.ok()) {
        return result;
    }

    ParseResult parsed = ParseGetPartnerAppList(http.body, out);
    if (!parsed.ok()) {
        result.kind = (parsed.status == ParseStatus::ApiError)
                          ? ResultKind::Auth
                          : ResultKind::Parse;
        result.message = ParseMessage(parsed);
    }
    return result;
}

}  // namespace steam
