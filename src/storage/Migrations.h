#pragma once

struct sqlite3;

namespace storage {

// The schema version this build targets.
inline constexpr int kTargetSchemaVersion = 4;

// Reads the current schema version (0 if the schema_version table is absent).
int GetSchemaVersion(sqlite3* db);

// Applies any pending migrations up to kTargetSchemaVersion. Each migration
// runs in its own transaction. Returns false on failure (schema left at the
// last successfully applied version).
bool ApplyMigrations(sqlite3* db);

}  // namespace storage
