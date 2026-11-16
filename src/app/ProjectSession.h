#pragma once

#include <juce_core/juce_core.h>

#include <deque>
#include <functional>
#include <memory>

#include "project/ProjectPackage.h"

namespace b4nd
{

/**
    The project IO worker (docs/08 §1): the only thread that touches project.db.

    Synchronous calls (open / create / flush) are for moments when nothing is playing
    or the user explicitly waits (open dialog, quit). Autosave and backups are posted
    and never block the message thread. The audio thread is never involved.
*/
class ProjectSession : private juce::Thread
{
public:
    ProjectSession();
    ~ProjectSession() override;

    struct OpenResult
    {
        DbResult result;
        ProjectData data;
        bool hadStaleLock = false;            // last session crashed or was killed
        std::vector<std::string> backups;     // newest first
        std::vector<std::string> missingMedia; // stored paths that do not exist
    };

    /** Opens an existing package (folder or its project.db). Writes session.lock and a backup. */
    OpenResult open (const juce::File& folderOrDb);

    /** Creates a new package folder and saves `data` into it. */
    DbResult create (const juce::File& folder, const ProjectData& data);

    /** Replaces the open project with a backup file's content (the current state is backed up first). */
    OpenResult restoreBackup (const std::string& backupPath);

    /** Queues a save. Only the newest pending snapshot is written (coalesced). */
    void saveAsync (ProjectData snapshot);
    /** Writes now and waits (used for Ctrl+S and on quit). */
    DbResult saveNow (ProjectData snapshot);
    void backupAsync();

    /** Waits for queued work, removes session.lock and closes the database. */
    void close();

    bool isOpen() const;
    juce::File getFolder() const;
    ProjectPackage getPackage() const;

    // Status for the UI (message thread).
    juce::Time getLastSaveTime() const;
    juce::String getLastError() const;
    bool isSaving() const;

    static juce::File folderFor (const juce::File& folderOrDb);

private:
    using Job = std::function<void()>;
    void run() override;
    void post (Job);
    void runAndWait (Job);
    DbResult writeSnapshot (const ProjectData&);   // worker only

    // Worker-owned.
    ProjectDatabase db;
    std::unique_ptr<ProjectPackage> package;

    mutable juce::CriticalSection lock;
    std::deque<Job> jobs;
    std::unique_ptr<ProjectData> pendingSnapshot;
    bool busy = false;
    juce::File folder;
    juce::Time lastSave;
    juce::String lastError;
    juce::WaitableEvent wake;
};

} // namespace b4nd
