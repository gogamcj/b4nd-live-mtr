#include "project/ProjectPackage.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace b4nd
{

namespace
{
// UTF-8 <-> path (std::filesystem uses wide strings on Windows).
fs::path u8 (const std::string& s)
{
#if defined(__cpp_char8_t)
    return fs::path (std::u8string (s.begin(), s.end()));
#else
    return fs::u8path (s);
#endif
}

std::string str (const fs::path& p)
{
    const auto s = p.u8string();
    return std::string (s.begin(), s.end());
}

std::string timestampForFile()
{
    const std::time_t t = std::time (nullptr);
    std::tm tm {};
#if defined(_WIN32)
    localtime_s (&tm, &t);
#else
    localtime_r (&t, &tm);
#endif
    char buf[32];
    std::strftime (buf, sizeof (buf), "%Y%m%d-%H%M%S", &tm);
    return buf;
}
} // namespace

ProjectPackage::ProjectPackage (std::string f) : folder (std::move (f)) {}

std::string ProjectPackage::getDatabasePath() const { return str (u8 (folder) / dbFileName); }
std::string ProjectPackage::getBackupFolder() const { return str (u8 (folder) / "backups"); }
std::string ProjectPackage::getMediaFolder() const  { return str (u8 (folder) / "media"); }

bool ProjectPackage::exists() const
{
    std::error_code ec;
    return fs::is_regular_file (u8 (getDatabasePath()), ec);
}

DbResult ProjectPackage::create() const
{
    std::error_code ec;
    fs::create_directories (u8 (folder) / "media", ec);
    if (! ec)
        fs::create_directories (u8 (folder) / "backups", ec);
    if (ec)
        return DbResult::failure ("Cannot create project folder " + folder + ": " + ec.message());
    return {};
}

bool ProjectPackage::hasStaleLock() const
{
    std::error_code ec;
    return fs::exists (u8 (folder) / "session.lock", ec);
}

DbResult ProjectPackage::writeLock() const
{
    std::ofstream f (u8 (folder) / "session.lock", std::ios::trunc);
    if (! f)
        return DbResult::failure ("Cannot write session.lock in " + folder);
    f << "B4ND LIVE MTR session " << std::time (nullptr) << "\n";
    return {};
}

void ProjectPackage::removeLock() const
{
    std::error_code ec;
    fs::remove (u8 (folder) / "session.lock", ec);
}

std::vector<std::string> ProjectPackage::listBackups() const
{
    std::vector<std::string> files;
    std::error_code ec;
    for (fs::directory_iterator it (u8 (getBackupFolder()), ec), end; ! ec && it != end; it.increment (ec))
    {
        const auto name = str (it->path().filename());
        if (it->is_regular_file (ec) && name.rfind ("project-", 0) == 0 && it->path().extension() == ".db")
            files.push_back (str (it->path()));
    }
    // Names embed the timestamp, so lexical order is chronological.
    std::sort (files.rbegin(), files.rend());
    return files;
}

DbResult ProjectPackage::makeBackup (ProjectDatabase& db, std::string* created) const
{
    std::error_code ec;
    fs::create_directories (u8 (getBackupFolder()), ec);

    // project-YYYYMMDD-HHMMSS-NNN.db: NNN separates backups made within the same second,
    // and the fixed width keeps lexical order == chronological order.
    fs::path target;
    for (int n = 0; n < 1000; ++n)
    {
        char seq[8];
        std::snprintf (seq, sizeof (seq), "-%03d", n);
        target = u8 (getBackupFolder()) / ("project-" + timestampForFile() + seq + ".db");
        if (! fs::exists (target, ec))
            break;
    }

    if (auto r = db.backupTo (str (target)); ! r)
        return r;
    if (created != nullptr)
        *created = str (target);

    auto all = listBackups();
    for (size_t i = (size_t) maxBackups; i < all.size(); ++i)
        fs::remove (u8 (all[i]), ec);
    return {};
}

std::string ProjectPackage::toStoredPath (const std::string& absolute) const
{
    std::error_code ec;
    const auto root = fs::weakly_canonical (u8 (folder), ec);
    const auto file = fs::weakly_canonical (u8 (absolute), ec);
    if (! ec)
    {
        const auto rel = file.lexically_relative (root);
        if (! rel.empty() && *rel.begin() != ".." && ! rel.is_absolute())
        {
            const auto g = rel.generic_u8string();   // forward slashes on every OS
            return std::string (g.begin(), g.end());
        }
    }
    return absolute;
}

std::string ProjectPackage::toAbsolutePath (const std::string& stored) const
{
    const auto p = u8 (stored);
    if (p.is_absolute())
        return stored;
    return str ((u8 (folder) / p).lexically_normal());
}

std::vector<const MediaRef*> ProjectPackage::findMissingMedia (const ProjectData& p) const
{
    std::vector<const MediaRef*> missing;
    for (const auto& m : p.media)
    {
        std::error_code ec;
        const auto path = u8 (toAbsolutePath (m.path));
        if (! fs::is_regular_file (path, ec) || fs::file_size (path, ec) == 0 || ec)
            missing.push_back (&m);
    }
    return missing;
}

} // namespace b4nd
