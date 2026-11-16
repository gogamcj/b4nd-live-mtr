#pragma once

#include <string>

#include "project/ProjectModel.h"

struct sqlite3;

namespace b4nd
{

struct DbResult
{
    bool ok = true;
    std::string error;

    static DbResult failure (std::string e) { return { false, std::move (e) }; }
    explicit operator bool() const noexcept { return ok; }
};

/**
    project.db access (docs/08). Not thread-safe: use one instance per thread
    (the app's project IO worker). The audio thread never touches it.

    save() replaces the whole project inside one transaction: either the new
    state is stored completely or the previous state stays untouched.
*/
class ProjectDatabase
{
public:
    static constexpr int currentSchemaVersion = 1;

    ProjectDatabase() = default;
    ~ProjectDatabase();
    ProjectDatabase (const ProjectDatabase&) = delete;
    ProjectDatabase& operator= (const ProjectDatabase&) = delete;

    /** Opens (or with create = true, creates) a database file, sets WAL / foreign keys,
        checks integrity and migrates the schema. A newer schema than this build is rejected. */
    DbResult open (const std::string& utf8Path, bool create);
    void close();
    bool isOpen() const noexcept { return db != nullptr; }

    DbResult save (const ProjectData&);
    DbResult load (ProjectData&);

    /** PRAGMA quick_check. */
    DbResult quickCheck();

    /** Online copy with the sqlite3_backup API (safe while the database is in use). */
    DbResult backupTo (const std::string& utf8Path);

    int getSchemaVersion();

private:
    DbResult exec (const char* sql);
    DbResult createSchema();
    DbResult migrate();

    sqlite3* db = nullptr;
};

} // namespace b4nd
