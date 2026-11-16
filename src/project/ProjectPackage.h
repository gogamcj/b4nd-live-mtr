#pragma once

#include <string>
#include <vector>

#include "project/ProjectDatabase.h"

namespace b4nd
{

/**
    A project is a folder ("My Show.b4nd/"):

        project.db        SQLite project (docs/08)
        media/            audio files copied or recorded into the project (optional)
        backups/          project-YYYYMMDD-HHMMSS.db, newest 20 kept
        session.lock      exists while the project is open; left behind by a crash

    All paths are UTF-8. Media paths inside the package are stored relative to it,
    so the folder can be moved to another computer or drive letter.
*/
class ProjectPackage
{
public:
    static constexpr const char* extension = ".b4nd";
    static constexpr const char* dbFileName = "project.db";
    static constexpr int maxBackups = 20;

    explicit ProjectPackage (std::string folderUtf8);

    const std::string& getFolder() const noexcept { return folder; }
    std::string getDatabasePath() const;
    std::string getBackupFolder() const;
    std::string getMediaFolder() const;

    bool exists() const;
    /** Creates the folder structure (not the database). */
    DbResult create() const;

    // --- crash detection ---
    /** True if session.lock exists: the last session did not close the project. */
    bool hasStaleLock() const;
    DbResult writeLock() const;
    void removeLock() const;

    // --- backups ---
    /** Copies the open database into backups/ and deletes the oldest beyond maxBackups.
        Returns the new file's path in `created` on success. */
    DbResult makeBackup (ProjectDatabase& db, std::string* created = nullptr) const;
    /** Newest first. */
    std::vector<std::string> listBackups() const;

    // --- media paths ---
    /** Relative to the package if the file is inside it, otherwise the absolute path. */
    std::string toStoredPath (const std::string& absoluteUtf8) const;
    std::string toAbsolutePath (const std::string& storedUtf8) const;

    /** Media entries whose file is missing or empty. */
    std::vector<const MediaRef*> findMissingMedia (const ProjectData&) const;

private:
    std::string folder;
};

} // namespace b4nd
