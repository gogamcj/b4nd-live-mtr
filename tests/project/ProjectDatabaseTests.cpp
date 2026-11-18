// Project storage tests (docs/08, docs/10): round trip, atomic save, damage, backups, locks.

#include "TestHarness.h"
#include "project/ProjectPackage.h"

#include <sqlite3.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <set>

using namespace b4nd;
namespace fs = std::filesystem;

namespace
{
struct TempDir
{
    TempDir()
    {
        path = fs::temp_directory_path() / ("b4nd-proj-test-" + newUuid());
        fs::create_directories (path);
    }
    ~TempDir() { std::error_code ec; fs::remove_all (path, ec); }
    std::string sub (const std::string& name) const
    {
        const auto s = (path / fs::u8path (name)).u8string();
        return std::string (s.begin(), s.end());
    }
    fs::path path;
};

ProjectData sampleProject()
{
    ProjectData p;
    p.id = newUuid();
    p.name = "주말 공연 2026";   // Korean title must survive the round trip
    p.sampleRate = 48000;
    p.appVersion = "0.1.0";

    MediaRef mr;
    mr.id = newUuid(); mr.path = "media/backing.wav"; mr.durationSec = 180.5; mr.sampleRate = 44100; mr.channels = 2;
    p.media.push_back (mr);
    MediaRef gd;
    gd.id = newUuid(); gd.kind = "guide"; gd.path = "C:/Shows/guide 보컬.wav"; gd.durationSec = 180.0; gd.sampleRate = 48000; gd.channels = 1;
    p.media.push_back (gd);

    for (int i = 0; i < 3; ++i)
    {
        SongData s;
        s.id = newUuid();
        s.title = "곡 " + std::to_string (i + 1);
        s.artist = "B4ND";
        s.key = i == 1 ? "D major" : "E minor";
        s.baseBpm = 120.0 + i;
        s.lengthSec = 200.0 + i;
        s.lastPositionSec = 12.5 * i;
        s.tempoPoints = { { 0.0, 120.0 + i }, { 64.0, 90.0 } };
        s.timeSigPoints = { { 0, i == 2 ? 6 : 4, i == 2 ? 8 : 4 }, { 16, 3, 4 } };
        s.cues = { { newUuid(), "Intro", "intro", 0.0, "#3366ff", "user" }, { newUuid(), "후렴", "chorus", 45.25, "", "ai" } };

        TrackData a;
        a.id = newUuid(); a.name = "Original Mix"; a.kind = "audio"; a.busKind = "main";
        a.volumeDb = -3.5; a.pan = -0.25; a.mute = true;
        a.clips.push_back ({ newUuid(), mr.id, 1.5, 0.25, 0.0 });
        TrackData g;
        g.id = newUuid(); g.name = "Guide"; g.kind = "guide"; g.busKind = "guide"; g.solo = true;
        g.clips.push_back ({ newUuid(), gd.id, 0.0, 0.0, 0.0 });
        TrackData c;
        c.id = newUuid(); c.name = "Click"; c.kind = "click"; c.busKind = "click"; c.volumeDb = -6.0;
        TrackData m;
        m.id = newUuid(); m.name = "Piano"; m.kind = "midi_vsti"; m.destinationLabel = "Main L/R";
        TrackData x;
        x.id = newUuid(); x.name = "Strings"; x.kind = "midi_external"; x.moduleProfileId = "mod-integra";
        x.midiChannel = i == 0 ? 0 : 5; x.midiPath = "media/strings 현악.mid";
        m.pluginSlotId = "slot-piano-" + std::to_string (i);
        s.tracks = { a, g, c, m, x };
        s.click.countInBars = 2;
        s.click.volumeDb = -9.0;
        p.songs.push_back (s);
    }
    ModuleProfile mod;
    mod.id = "mod-integra"; mod.deviceName = "INTEGRA-7 현악"; mod.portName = "UM-ONE"; mod.portIdentifier = "\\\\?\\usb#um-one";
    mod.channel = 3; mod.bankMsb = 87; mod.bankLsb = -1; mod.program = 41; mod.volume = 90; mod.pan = -1;
    mod.delayOffsetMs = 4.5; mod.sendOrder = "lsb,msb,pc"; mod.gapMs = 30; mod.settleMs = 400; mod.sustainEnabled = false;
    ModuleProfile plain;
    plain.id = "mod-plain"; plain.deviceName = "GM"; plain.portName = "Port 2";
    p.modules = { mod, plain };
    for (int i = 0; i < 3; ++i)
    {
        PluginSlotData ps;
        ps.id = "slot-piano-" + std::to_string (i);
        ps.pluginUid = "VST3-B4ND Test Synth-1a2b3c4d-5e6f7a8b";
        ps.pluginName = "B4ND Test Synth";
        ps.vendor = "B4ND Test";
        ps.version = "1.0.0";
        ps.descriptionXml = "<PLUGIN name=\"B4ND Test Synth\"/>";
        for (int k = 0; k < 70000 + i; ++k)   // binary, with zero bytes
            ps.state.push_back ((std::uint8_t) ((k * 31 + i) & 0xff));
        p.plugins.push_back (ps);
    }

    SetlistData sl;
    sl.id = newUuid(); sl.name = "Main set"; sl.venue = "홍대";
    sl.songIds = { p.songs[2].id, p.songs[0].id, p.songs[1].id };
    p.setlists.push_back (sl);

    p.outputs = { { 0, true, "main", 2, true, "MAIN MONO" }, { 1, true, "monitor", 2, false, "IN-EAR" }, { 2, false, "", 0, false, "OUT 3" } };
    return p;
}

bool sameProject (const ProjectData& a, const ProjectData& b)
{
    if (a.id != b.id || a.name != b.name || a.sampleRate != b.sampleRate || a.media.size() != b.media.size()
        || a.songs.size() != b.songs.size() || a.setlists.size() != b.setlists.size() || a.outputs.size() != b.outputs.size())
        return false;
    for (size_t i = 0; i < a.media.size(); ++i)
        if (a.media[i].id != b.media[i].id || a.media[i].path != b.media[i].path || a.media[i].kind != b.media[i].kind
            || a.media[i].channels != b.media[i].channels)
            return false;
    for (size_t i = 0; i < a.songs.size(); ++i)
    {
        const auto &x = a.songs[i], &y = b.songs[i];
        if (x.id != y.id || x.title != y.title || x.key != y.key || x.lengthSec != y.lengthSec   // NOLINT exact: stored as REAL
            || x.lastPositionSec != y.lastPositionSec || x.tempoPoints.size() != y.tempoPoints.size()
            || x.timeSigPoints.size() != y.timeSigPoints.size() || x.cues.size() != y.cues.size()
            || x.tracks.size() != y.tracks.size() || x.click.countInBars != y.click.countInBars)
            return false;
        for (size_t k = 0; k < x.tempoPoints.size(); ++k)
            if (x.tempoPoints[k].beat != y.tempoPoints[k].beat || x.tempoPoints[k].bpm != y.tempoPoints[k].bpm)
                return false;
        for (size_t k = 0; k < x.timeSigPoints.size(); ++k)
            if (x.timeSigPoints[k].bar != y.timeSigPoints[k].bar || x.timeSigPoints[k].numerator != y.timeSigPoints[k].numerator
                || x.timeSigPoints[k].denominator != y.timeSigPoints[k].denominator)
                return false;
        for (size_t k = 0; k < x.cues.size(); ++k)
            if (x.cues[k].name != y.cues[k].name || x.cues[k].positionSec != y.cues[k].positionSec || x.cues[k].source != y.cues[k].source)
                return false;
        for (size_t k = 0; k < x.tracks.size(); ++k)
        {
            const auto &t = x.tracks[k], &u = y.tracks[k];
            if (t.id != u.id || t.name != u.name || t.kind != u.kind || t.busKind != u.busKind || t.volumeDb != u.volumeDb
                || t.pan != u.pan || t.mute != u.mute || t.solo != u.solo || t.destinationLabel != u.destinationLabel
                || t.clips.size() != u.clips.size() || t.moduleProfileId != u.moduleProfileId || t.midiChannel != u.midiChannel
            || t.midiPath != u.midiPath || t.pluginSlotId != u.pluginSlotId)
                return false;
            for (size_t c = 0; c < t.clips.size(); ++c)
                if (t.clips[c].mediaId != u.clips[c].mediaId || t.clips[c].startSec != u.clips[c].startSec
                    || t.clips[c].offsetSec != u.clips[c].offsetSec)
                    return false;
        }
    }
    if (a.plugins.size() != b.plugins.size())
        return false;
    for (size_t i = 0; i < a.plugins.size(); ++i)
        if (a.plugins[i].id != b.plugins[i].id || a.plugins[i].pluginUid != b.plugins[i].pluginUid
            || a.plugins[i].state != b.plugins[i].state || a.plugins[i].descriptionXml != b.plugins[i].descriptionXml
            || a.plugins[i].vendor != b.plugins[i].vendor)
            return false;
    if (a.modules.size() != b.modules.size())
        return false;
    for (size_t i = 0; i < a.modules.size(); ++i)
    {
        const auto &m = a.modules[i], &n = b.modules[i];
        if (m.id != n.id || m.deviceName != n.deviceName || m.portName != n.portName || m.portIdentifier != n.portIdentifier
            || m.channel != n.channel || m.bankMsb != n.bankMsb || m.bankLsb != n.bankLsb || m.program != n.program
            || m.volumeCc != n.volumeCc || m.volume != n.volume || m.panCc != n.panCc || m.pan != n.pan
            || m.sustainEnabled != n.sustainEnabled || m.delayOffsetMs != n.delayOffsetMs || m.sendOrder != n.sendOrder
            || m.gapMs != n.gapMs || m.settleMs != n.settleMs)
            return false;
    }
    if (a.setlists[0].songIds != b.setlists[0].songIds || a.setlists[0].venue != b.setlists[0].venue)
        return false;
    for (size_t i = 0; i < a.outputs.size(); ++i)
        if (a.outputs[i].physicalOutput != b.outputs[i].physicalOutput || a.outputs[i].hasSource != b.outputs[i].hasSource
            || a.outputs[i].busKind != b.outputs[i].busKind || a.outputs[i].busChannel != b.outputs[i].busChannel
            || a.outputs[i].isPA != b.outputs[i].isPA || a.outputs[i].label != b.outputs[i].label)
            return false;
    return true;
}
} // namespace

TEST_CASE ("Project: UUIDs are v4 and unique")
{
    std::set<std::string> ids;
    for (int i = 0; i < 10000; ++i)
    {
        const auto id = newUuid();
        CHECK (id.size() == 36 && id[14] == '4' && (id[19] == '8' || id[19] == '9' || id[19] == 'a' || id[19] == 'b'));
        ids.insert (id);
    }
    CHECK (ids.size() == 10000);
}

TEST_CASE ("Project: new database gets the full schema, WAL and the current version")
{
    TempDir t;
    ProjectDatabase db;
    CHECK (db.open (t.sub ("p.db"), true).ok);
    CHECK (db.getSchemaVersion() == ProjectDatabase::currentSchemaVersion);
    CHECK (db.quickCheck().ok);
    db.close();

    sqlite3* raw = nullptr;
    sqlite3_open (t.sub ("p.db").c_str(), &raw);
    int tables = 0;
    sqlite3_exec (raw, "SELECT count(*) FROM sqlite_master WHERE type='table'",
                  [] (void* n, int, char** v, char**) { *(int*) n = std::atoi (v[0]); return 0; }, &tables, nullptr);
    std::string mode;
    sqlite3_exec (raw, "PRAGMA journal_mode",
                  [] (void* s, int, char** v, char**) { *(std::string*) s = v[0]; return 0; }, &mode, nullptr);
    sqlite3_close (raw);
    CHECK (tables >= 20);
    CHECK (mode == "wal");
}

TEST_CASE ("Project: save then load returns exactly the same project")
{
    TempDir t;
    const auto p = sampleProject();
    {
        ProjectDatabase db;
        CHECK (db.open (t.sub ("p.db"), true).ok);
        const auto r = db.save (p);
        CHECK (r.ok);
        if (! r.ok) std::printf ("    %s\n", r.error.c_str());
    }
    ProjectDatabase db;
    CHECK (db.open (t.sub ("p.db"), false).ok);
    ProjectData q;
    CHECK (db.load (q).ok);
    CHECK (sameProject (p, q));
    CHECK (q.name == "주말 공연 2026");
    CHECK (q.songs.size() == 3 && q.songs[1].cues[1].name == "후렴");
}

TEST_CASE ("Project: saving again replaces the previous state (no leftovers)")
{
    TempDir t;
    ProjectDatabase db;
    CHECK (db.open (t.sub ("p.db"), true).ok);
    auto p = sampleProject();
    CHECK (db.save (p).ok);
    p.songs.pop_back();
    p.songs[0].tracks.pop_back();
    p.setlists[0].songIds = { p.songs[1].id, p.songs[0].id };
    CHECK (db.save (p).ok);
    ProjectData q;
    CHECK (db.load (q).ok);
    CHECK (sameProject (p, q));
}

TEST_CASE ("Project: a failing save rolls back and keeps the last good state")
{
    TempDir t;
    ProjectDatabase db;
    CHECK (db.open (t.sub ("p.db"), true).ok);
    const auto good = sampleProject();
    CHECK (db.save (good).ok);

    auto bad = good;
    bad.name = "should not be stored";
    bad.songs[2].tracks[0].kind = "not-a-kind";   // CHECK constraint fails halfway through
    const auto r = db.save (bad);
    CHECK (! r.ok);
    CHECK (! r.error.empty());

    ProjectData q;
    CHECK (db.load (q).ok);
    CHECK (sameProject (good, q));

    // Click protection is also enforced by the database itself.
    auto unsafe = good;
    unsafe.outputs[0] = { 0, true, "click", 0, true, "PA with click" };
    CHECK (! db.save (unsafe).ok);
    CHECK (db.load (q).ok && sameProject (good, q));
}

TEST_CASE ("Project: damaged, foreign and newer-version files are refused with a reason")
{
    TempDir t;
    {
        std::ofstream f (t.sub ("junk.db"), std::ios::binary);
        f << "this is not a database at all, just some text that is long enough to look like a header...";
    }
    ProjectDatabase db;
    auto r = db.open (t.sub ("junk.db"), false);
    CHECK (! r.ok && ! db.isOpen());

    sqlite3* raw = nullptr;
    sqlite3_open (t.sub ("other.db").c_str(), &raw);
    sqlite3_exec (raw, "CREATE TABLE something (x INTEGER);", nullptr, nullptr, nullptr);
    sqlite3_close (raw);
    r = db.open (t.sub ("other.db"), true);
    CHECK (! r.ok);
    CHECK (r.error.find ("not a B4ND") != std::string::npos);

    CHECK (db.open (t.sub ("new.db"), true).ok);
    db.close();
    sqlite3_open (t.sub ("new.db").c_str(), &raw);
    sqlite3_exec (raw, "UPDATE schema_version SET version = 99;", nullptr, nullptr, nullptr);
    sqlite3_close (raw);
    r = db.open (t.sub ("new.db"), false);
    CHECK (! r.ok);
    CHECK (r.error.find ("newer version") != std::string::npos);

    r = db.open (t.sub ("does-not-exist.db"), false);
    CHECK (! r.ok);
}

TEST_CASE ("Project: corrupted pages are detected on open")
{
    TempDir t;
    {
        ProjectDatabase db;
        CHECK (db.open (t.sub ("p.db"), true).ok);
        auto p = sampleProject();
        for (int i = 0; i < 40; ++i)   // make the file a few pages long
        {
            auto s = p.songs[0];
            s.id = newUuid();
            for (auto& tr : s.tracks) { tr.id = newUuid(); for (auto& c : tr.clips) c.id = newUuid(); }
            for (auto& c : s.cues) c.id = newUuid();
            s.notes = std::string (2000, 'x');
            p.songs.push_back (s);
        }
        CHECK (db.save (p).ok);
        // Checkpoint the WAL into the main file so the corruption below hits real data.
        db.close();
    }
    {
        std::fstream f (t.sub ("p.db"), std::ios::in | std::ios::out | std::ios::binary);
        // Overwrite the b-tree page headers of several pages (a bad sector / torn write).
        std::mt19937 rng (1);
        for (int page = 2; page < 8; ++page)
        {
            f.seekp (4096 * page);
            for (int i = 0; i < 64; ++i)
                f.put ((char) (rng() & 0xff));
        }
    }
    ProjectDatabase db;
    const auto r = db.open (t.sub ("p.db"), false);
    bool detected = ! r.ok;
    if (r.ok)
    {
        ProjectData q;
        detected = ! db.load (q).ok;   // corruption in a non-checked area must still fail loudly, never silently
    }
    CHECK (detected);
}

TEST_CASE ("Project: package folder, lock file, backups (newest 20 kept), restore from backup")
{
    TempDir t;
    ProjectPackage pkg (t.sub ("주말 공연.b4nd"));
    CHECK (pkg.create().ok);
    CHECK (! pkg.exists());
    CHECK (! pkg.hasStaleLock());
    CHECK (pkg.writeLock().ok);
    CHECK (pkg.hasStaleLock());

    ProjectDatabase db;
    CHECK (db.open (pkg.getDatabasePath(), true).ok);
    CHECK (pkg.exists());
    const auto p = sampleProject();
    CHECK (db.save (p).ok);

    std::string first;
    CHECK (pkg.makeBackup (db, &first).ok);
    for (int i = 0; i < 24; ++i)
        CHECK (pkg.makeBackup (db).ok);
    const auto backups = pkg.listBackups();
    CHECK (backups.size() == (size_t) ProjectPackage::maxBackups);
    CHECK (std::find (backups.begin(), backups.end(), first) == backups.end());   // oldest removed

    ProjectDatabase restored;
    CHECK (restored.open (backups.front(), false).ok);
    ProjectData q;
    CHECK (restored.load (q).ok);
    CHECK (sameProject (p, q));

    pkg.removeLock();
    CHECK (! pkg.hasStaleLock());
}

TEST_CASE ("Project: media paths are stored relative to the package when inside it")
{
    TempDir t;
    ProjectPackage pkg (t.sub ("Show.b4nd"));
    CHECK (pkg.create().ok);
    const auto inside = t.sub ("Show.b4nd/media/드럼 stem.wav");
    const auto outside = t.sub ("elsewhere/mr.wav");
    CHECK (pkg.toStoredPath (inside) == "media/드럼 stem.wav");
    CHECK (pkg.toStoredPath (outside) == outside);
    CHECK (fs::u8path (pkg.toAbsolutePath ("media/드럼 stem.wav")) == fs::u8path (inside).lexically_normal());
    CHECK (pkg.toAbsolutePath (outside) == outside);

    { std::ofstream f (fs::u8path (inside), std::ios::binary); f << "RIFF....WAVE"; }
    ProjectData p;
    p.media = { { "a", "original", "media/드럼 stem.wav", "", 0, 0, 0 }, { "b", "original", outside, "", 0, 0, 0 } };
    const auto missing = pkg.findMissingMedia (p);
    CHECK (missing.size() == 1 && missing[0]->id == "b");
}

TEST_CASE ("Project: a schema v1 project is upgraded step by step on open and keeps its data")
{
    TempDir t;
    const auto file = t.sub ("old.db");
    auto p = sampleProject();
    {
        ProjectDatabase db;
        CHECK (db.open (file, true).ok);
        p.modules.clear();
        p.plugins.clear();
        for (auto& s : p.songs)
            for (auto& tr : s.tracks)
            {
                tr.moduleProfileId.clear();
                tr.pluginSlotId.clear();
            }
        CHECK (db.save (p).ok);
    }
    // Turn it back into a v1 file (as written by the Phase 3b build).
    sqlite3* raw = nullptr;
    sqlite3_open (file.c_str(), &raw);
    CHECK (sqlite3_exec (raw, "ALTER TABLE module_profile DROP COLUMN midi_port_id; ALTER TABLE plugin_slot DROP COLUMN state_blob;"
                              " ALTER TABLE plugin_slot DROP COLUMN description_xml; UPDATE schema_version SET version = 1;",
                         nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close (raw);

    ProjectDatabase db;
    CHECK (db.open (file, false).ok);
    CHECK (db.getSchemaVersion() == ProjectDatabase::currentSchemaVersion);
    ProjectData q;
    CHECK (db.load (q).ok);
    CHECK (sameProject (p, q));

    // and the new column works after the upgrade
    auto withModules = sampleProject();
    CHECK (db.save (withModules).ok);
    ProjectData r;
    CHECK (db.load (r).ok);
    CHECK (sameProject (withModules, r));
}
