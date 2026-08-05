#pragma once

#include <string>

namespace platform {

// All mutable application data lives under %LOCALAPPDATA%\SteamSalesTray\.
// These helpers resolve (and, for the data directory, create) those paths.

// Returns the application data directory, creating it if necessary. Returns an
// empty string on failure.
std::wstring GetDataDirectory();

// Full paths to individual files (each rooted at GetDataDirectory()).
std::wstring GetApiKeyPath();           // api-key.dat
std::wstring GetPublisherApiKeyPath();  // publisher-api-key.dat
std::wstring GetDatabasePath();         // sales.db
std::wstring GetLogPath();              // app.log

// Absolute path to the running executable (for the startup Run entry).
std::wstring GetExecutablePath();

}  // namespace platform
