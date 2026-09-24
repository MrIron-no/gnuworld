/**
 * MigrationChecker.cc
 * Database schema migration tracking utility for GNUWorld
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#include <iostream>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <filesystem>
#include <regex>

#include "MigrationChecker.h"
#include "dbHandle.h"
#include "logger.h"

namespace gnuworld {

MigrationChecker::MigrationChecker(const std::string& moduleName, dbHandle* db, Logger* logger,
                                   const std::string& migrationsDir)
    : moduleName(moduleName), db(db), logger(logger), migrationsDir(migrationsDir) {}

bool MigrationChecker::check() {
    if (!db) {
        LOG_TO(logger, ERROR, "Database handle is null");
        return false;
    }

    // Ensure the gnuworld_migrations table exists
    if (!ensureMigrationsTableExists()) {
        LOG_TO(logger, ERROR, "Failed to create/access gnuworld_migrations table");
        return false;
    }

    // Scan for migration files
    auto onDisk = scanMigrationFiles();
    if (onDisk.empty()) {
        LOG_TO(logger, INFO, "Module '{}' has no migrations to check", moduleName);
        return true;
    }

    // Get list of already-applied migrations
    auto applied = getAppliedMigrations();

    // Find unapplied migrations
    std::vector<std::string> unapplied;
    for (const auto& file : onDisk) {
        if (std::find(applied.begin(), applied.end(), file) == applied.end()) {
            unapplied.push_back(file);
        }
    }

    if (!unapplied.empty()) {
        LOG_TO(logger, INFO, "Module '{}' has {} unapplied migration(s). Applying now...",
               moduleName, unapplied.size());

        // Apply the unapplied migrations
        if (!applyMigrations(unapplied)) {
            return false; // Error already logged by applyMigrations
        }
    }

    LOG_TO(logger, INFO, "Module '{}' migrations verified - all applied", moduleName);
    return true;
}

bool MigrationChecker::ensureMigrationsTableExists() {
    // Check if gnuworld_migrations table already exists
    const char* checkTableSQL = R"(
        SELECT EXISTS (
            SELECT 1 FROM information_schema.tables
            WHERE table_name = 'gnuworld_migrations'
        );
    )";

    if (!db->Exec(checkTableSQL, true)) {
        LOG_TO(logger, ERROR, "Failed to check for gnuworld_migrations table");
        return false;
    }

    // Check if table exists (GetValue returns "t" for true in PostgreSQL)
    std::string exists = db->GetValue(0, 0);
    if (exists == "t" || exists == "true") {
        return true; // Table already exists
    }

    // Table doesn't exist, create it
    const char* createTableSQL = R"(
        CREATE TABLE IF NOT EXISTS gnuworld_migrations (
            id SERIAL PRIMARY KEY,
            module VARCHAR(50) NOT NULL,
            file VARCHAR(255) NOT NULL,
            applied_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
            UNIQUE(module, file)
        );
    )";

    if (!db->Exec(createTableSQL)) {
        LOG_TO(logger, ERROR, "Failed to create gnuworld_migrations table");
        return false;
    }

    LOG_TO(logger, INFO, "Created gnuworld_migrations table for module '{}'", moduleName);
    return true;
}

std::vector<std::string> MigrationChecker::scanMigrationFiles() {
    std::vector<std::string> files;

    try {
        if (!std::filesystem::exists(migrationsDir)) {
            // Directory doesn't exist yet, which is fine
            return files;
        }

        // Pattern: NNN_*.sql where NNN are digits
        std::regex migrationPattern(R"(^(\d+)[-_].*\.sql$)");

        for (const auto& entry : std::filesystem::directory_iterator(migrationsDir)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                if (std::regex_match(filename, migrationPattern)) {
                    files.push_back(filename);
                }
            }
        }

        // Sort numerically by the NNN prefix
        std::sort(files.begin(), files.end(), [](const std::string& a, const std::string& b) {
            int aNum = std::stoi(a.substr(0, 3));
            int bNum = std::stoi(b.substr(0, 3));
            return aNum < bNum;
        });

    } catch (const std::exception& e) {
        LOG_TO(logger, ERROR, "Failed to scan migrations directory '{}': {}", migrationsDir,
               e.what());
    }

    return files;
}

std::vector<std::string> MigrationChecker::getAppliedMigrations() {
    std::vector<std::string> applied;

    std::stringstream query;
    query << "SELECT file FROM gnuworld_migrations WHERE module = '" << moduleName
          << "' ORDER BY id;";

    if (!db->Exec(query.str(), true)) {
        LOG_TO(logger, WARN, "Failed to query applied migrations for '{}'", moduleName);
        return applied;
    }

    // Parse results
    unsigned int numRows = db->countTuples();
    for (unsigned int i = 0; i < numRows; ++i) {
        std::string file = db->GetValue(i, 0);
        if (!file.empty()) {
            applied.push_back(file);
        }
    }

    return applied;
}

bool MigrationChecker::applyMigrations(const std::vector<std::string>& unappliedFiles) {
    for (const auto& filename : unappliedFiles) {
        // Build the full path to the migration file
        std::filesystem::path migrationPath = std::filesystem::path(migrationsDir) / filename;

        // Read the SQL file
        std::ifstream sqlFile(migrationPath);
        if (!sqlFile.is_open()) {
            LOG_TO(logger, ERROR, "Failed to open migration file '{}' at {}", filename,
                   migrationPath.string());
            return false;
        }

        // Read the entire file content
        std::stringstream sqlContent;
        sqlContent << sqlFile.rdbuf();
        sqlFile.close();

        // Execute the migration SQL
        if (!db->Exec(sqlContent.str())) {
            LOG_TO(logger, ERROR, "Failed to apply migration '{}' for module '{}'", filename,
                   moduleName);
            return false;
        }

        // Record the migration as applied
        if (!recordMigration(filename)) {
            return false; // Error already logged by recordMigration
        }

        LOG_TO(logger, INFO, "Successfully applied migration '{}' to module '{}'", filename,
               moduleName);
    }

    return true;
}

bool MigrationChecker::recordMigration(const std::string& filename) {
    std::stringstream insertQuery;
    insertQuery << "INSERT INTO gnuworld_migrations (module, file) VALUES ('" << moduleName
                << "', '" << filename << "');";

    if (!db->Exec(insertQuery.str())) {
        LOG_TO(logger, ERROR, "Failed to record migration '{}' in database", filename);
        return false;
    }

    return true;
}

} // namespace gnuworld
