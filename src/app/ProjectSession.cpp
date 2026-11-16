#include "app/ProjectSession.h"

namespace b4nd
{

ProjectSession::ProjectSession() : juce::Thread ("B4ND project IO")
{
    startThread (juce::Thread::Priority::low);
}

ProjectSession::~ProjectSession()
{
    close();
    signalThreadShouldExit();
    wake.signal();
    stopThread (10000);
}

juce::File ProjectSession::folderFor (const juce::File& f)
{
    return f.getFileName() == ProjectPackage::dbFileName ? f.getParentDirectory() : f;
}

void ProjectSession::post (Job j)
{
    {
        const juce::ScopedLock sl (lock);
        jobs.push_back (std::move (j));
    }
    wake.signal();
}

void ProjectSession::runAndWait (Job j)
{
    juce::WaitableEvent done;
    post ([&] { j(); done.signal(); });
    done.wait (-1);
}

void ProjectSession::run()
{
    while (! threadShouldExit())
    {
        Job job;
        std::unique_ptr<ProjectData> snapshot;
        {
            const juce::ScopedLock sl (lock);
            if (! jobs.empty())
            {
                job = std::move (jobs.front());
                jobs.pop_front();
            }
            else if (pendingSnapshot != nullptr)
            {
                snapshot = std::move (pendingSnapshot);
            }
            busy = job != nullptr || snapshot != nullptr;
        }

        if (job)
            job();
        else if (snapshot != nullptr)
            writeSnapshot (*snapshot);
        else
            wake.wait (1000);

        const juce::ScopedLock sl (lock);
        busy = false;
    }
}

DbResult ProjectSession::writeSnapshot (const ProjectData& data)
{
    if (! db.isOpen())
        return DbResult::failure ("No project open");
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    auto r = db.save (data);
    const juce::ScopedLock sl (lock);
    if (r)
    {
        lastSave = juce::Time::getCurrentTime();
        lastError.clear();
        juce::Logger::writeToLog ("Project saved (" + juce::String (juce::roundToInt (juce::Time::getMillisecondCounterHiRes() - t0)) + " ms)");
    }
    else
    {
        lastError = juce::String::fromUTF8 (r.error.c_str());
        juce::Logger::writeToLog ("PROJECT SAVE FAILED: " + lastError);
    }
    return r;
}

ProjectSession::OpenResult ProjectSession::open (const juce::File& folderOrDb)
{
    close();
    const auto dir = folderFor (folderOrDb);
    OpenResult out;
    runAndWait ([&]
    {
        auto pkg = std::make_unique<ProjectPackage> (dir.getFullPathName().toStdString());
        if (! pkg->exists())
        {
            out.result = DbResult::failure ("No project found in " + dir.getFullPathName().toStdString());
            return;
        }
        out.hadStaleLock = pkg->hasStaleLock();
        out.result = db.open (pkg->getDatabasePath(), false);
        if (! out.result)
            return;
        out.result = db.load (out.data);
        if (! out.result)
        {
            db.close();
            return;
        }
        pkg->writeLock();
        if (auto b = pkg->makeBackup (db); ! b)
            juce::Logger::writeToLog ("WARNING: backup on open failed: " + juce::String (b.error));
        out.backups = pkg->listBackups();
        for (const auto* m : pkg->findMissingMedia (out.data))
            out.missingMedia.push_back (m->path);
        package = std::move (pkg);
    });

    if (out.result)
    {
        const juce::ScopedLock sl (lock);
        folder = dir;
        lastError.clear();
        lastSave = juce::Time::getCurrentTime();
    }
    juce::Logger::writeToLog (out.result ? "Project opened: " + dir.getFullPathName()
                                               + (out.hadStaleLock ? " (previous session did not close cleanly)" : "")
                                         : "Project open failed: " + juce::String (out.result.error));
    return out;
}

DbResult ProjectSession::create (const juce::File& dir, const ProjectData& data)
{
    close();
    DbResult r;
    runAndWait ([&]
    {
        auto pkg = std::make_unique<ProjectPackage> (dir.getFullPathName().toStdString());
        if (pkg->exists())
        {
            r = DbResult::failure ("A project already exists in " + dir.getFullPathName().toStdString());
            return;
        }
        if (r = pkg->create(); ! r)
            return;
        if (r = db.open (pkg->getDatabasePath(), true); ! r)
            return;
        if (r = db.save (data); ! r)
        {
            db.close();
            return;
        }
        pkg->writeLock();
        package = std::move (pkg);
    });
    if (r)
    {
        const juce::ScopedLock sl (lock);
        folder = dir;
        lastSave = juce::Time::getCurrentTime();
        lastError.clear();
    }
    juce::Logger::writeToLog (r ? "Project created: " + dir.getFullPathName() : "Project create failed: " + juce::String (r.error));
    return r;
}

ProjectSession::OpenResult ProjectSession::restoreBackup (const std::string& backupPath)
{
    OpenResult out;
    runAndWait ([&]
    {
        if (package == nullptr || ! db.isOpen())
        {
            out.result = DbResult::failure ("No project open");
            return;
        }
        ProjectDatabase src;
        if (out.result = src.open (backupPath, false); ! out.result)
            return;
        if (out.result = src.load (out.data); ! out.result)
            return;
        package->makeBackup (db);   // keep the state we are replacing
        out.result = db.save (out.data);
        out.backups = package->listBackups();
        for (const auto* m : package->findMissingMedia (out.data))
            out.missingMedia.push_back (m->path);
    });
    juce::Logger::writeToLog (out.result ? "Project restored from backup " + juce::String (backupPath)
                                         : "Restore failed: " + juce::String (out.result.error));
    return out;
}

void ProjectSession::saveAsync (ProjectData snapshot)
{
    {
        const juce::ScopedLock sl (lock);
        pendingSnapshot = std::make_unique<ProjectData> (std::move (snapshot));
    }
    wake.signal();
}

DbResult ProjectSession::saveNow (ProjectData snapshot)
{
    {
        const juce::ScopedLock sl (lock);
        pendingSnapshot.reset();   // superseded
    }
    DbResult r;
    runAndWait ([&] { r = writeSnapshot (snapshot); });
    return r;
}

void ProjectSession::backupAsync()
{
    post ([this]
    {
        if (package != nullptr && db.isOpen())
            if (auto r = package->makeBackup (db); ! r)
                juce::Logger::writeToLog ("WARNING: backup failed: " + juce::String (r.error));
    });
}

void ProjectSession::close()
{
    if (! isThreadRunning())
        return;
    runAndWait ([this]
    {
        std::unique_ptr<ProjectData> last;
        {
            const juce::ScopedLock sl (lock);
            last = std::move (pendingSnapshot);
        }
        if (last != nullptr)
            writeSnapshot (*last);
        db.close();
        if (package != nullptr)
            package->removeLock();
        package.reset();
    });
    const juce::ScopedLock sl (lock);
    folder = juce::File();
}

bool ProjectSession::isOpen() const
{
    const juce::ScopedLock sl (lock);
    return folder != juce::File();
}

juce::File ProjectSession::getFolder() const
{
    const juce::ScopedLock sl (lock);
    return folder;
}

ProjectPackage ProjectSession::getPackage() const
{
    return ProjectPackage (getFolder().getFullPathName().toStdString());
}

juce::Time ProjectSession::getLastSaveTime() const
{
    const juce::ScopedLock sl (lock);
    return lastSave;
}

juce::String ProjectSession::getLastError() const
{
    const juce::ScopedLock sl (lock);
    return lastError;
}

bool ProjectSession::isSaving() const
{
    const juce::ScopedLock sl (lock);
    return busy || pendingSnapshot != nullptr;
}

} // namespace b4nd
