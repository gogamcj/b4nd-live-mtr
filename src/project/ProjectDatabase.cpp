#include "project/ProjectDatabase.h"

#include <sqlite3.h>

#include <chrono>
#include <ctime>
#include <random>

namespace b4nd
{

//==============================================================================
std::string newUuid()
{
    static thread_local std::mt19937_64 rng { std::random_device {}() ^ (std::uint64_t) std::chrono::steady_clock::now().time_since_epoch().count() };
    std::uint64_t a = rng(), b = rng();
    a = (a & 0xffffffffffff0fffULL) | 0x0000000000004000ULL;   // version 4
    b = (b & 0x3fffffffffffffffULL) | 0x8000000000000000ULL;   // variant 10
    char s[37];
    std::snprintf (s, sizeof (s), "%08x-%04x-%04x-%04x-%012llx",
                   (unsigned) (a >> 32), (unsigned) ((a >> 16) & 0xffff), (unsigned) (a & 0xffff),
                   (unsigned) (b >> 48), (unsigned long long) (b & 0xffffffffffffULL));
    return s;
}

std::string nowIso8601()
{
    const std::time_t t = std::time (nullptr);
    std::tm tm {};
#if defined(_WIN32)
    gmtime_s (&tm, &t);
#else
    gmtime_r (&t, &tm);
#endif
    char buf[32];
    std::strftime (buf, sizeof (buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

//==============================================================================
namespace
{
/** RAII prepared statement with 1-based binds and 0-based column reads. */
class Stmt
{
public:
    Stmt (sqlite3* db, const char* sql) { rc = sqlite3_prepare_v2 (db, sql, -1, &s, nullptr); }
    ~Stmt() { sqlite3_finalize (s); }
    Stmt (const Stmt&) = delete;
    Stmt& operator= (const Stmt&) = delete;

    bool valid() const noexcept { return rc == SQLITE_OK && s != nullptr; }

    Stmt& bind (int i, const std::string& v) { sqlite3_bind_text (s, i, v.c_str(), (int) v.size(), SQLITE_TRANSIENT); return *this; }
    Stmt& bindOrNull (int i, const std::string& v) { if (v.empty()) sqlite3_bind_null (s, i); else bind (i, v); return *this; }
    Stmt& bind (int i, double v)              { sqlite3_bind_double (s, i, v); return *this; }
    Stmt& bind (int i, int v)                 { sqlite3_bind_int (s, i, v); return *this; }
    Stmt& bind (int i, bool v)                { sqlite3_bind_int (s, i, v ? 1 : 0); return *this; }
    Stmt& bindNull (int i)                    { sqlite3_bind_null (s, i); return *this; }

    /** For INSERT/UPDATE: true on SQLITE_DONE, then resets for reuse. */
    bool run()
    {
        rc = sqlite3_step (s);
        const bool ok = rc == SQLITE_DONE;
        sqlite3_reset (s);
        sqlite3_clear_bindings (s);
        return ok;
    }
    /** For SELECT: true while a row is available. */
    bool next() { rc = sqlite3_step (s); return rc == SQLITE_ROW; }
    void reset() { sqlite3_reset (s); sqlite3_clear_bindings (s); }

    std::string text (int c) const
    {
        const auto* p = sqlite3_column_text (s, c);
        return p != nullptr ? std::string (reinterpret_cast<const char*> (p), (size_t) sqlite3_column_bytes (s, c)) : std::string();
    }
    double real (int c) const    { return sqlite3_column_double (s, c); }
    int integer (int c) const    { return sqlite3_column_int (s, c); }
    bool isNull (int c) const    { return sqlite3_column_type (s, c) == SQLITE_NULL; }

    int lastCode() const noexcept { return rc; }

private:
    sqlite3_stmt* s = nullptr;
    int rc = SQLITE_ERROR;
};

// Schema v2 = docs/08 plus `clip` (audio regions), track.destination_label (v1)
// and module_profile.midi_port_id (v2).
const char* const schemaV1 = R"SQL(
CREATE TABLE schema_version (version INTEGER NOT NULL);

CREATE TABLE project (
  id TEXT PRIMARY KEY, name TEXT NOT NULL,
  sample_rate INTEGER NOT NULL DEFAULT 48000,
  created_at TEXT NOT NULL, modified_at TEXT NOT NULL,
  app_version TEXT NOT NULL
);

CREATE TABLE media_file (
  id TEXT PRIMARY KEY,
  kind TEXT NOT NULL CHECK (kind IN ('original','stem','cache','guide','click_sample')),
  rel_path TEXT NOT NULL, sha256 TEXT, duration_sec REAL,
  sample_rate INTEGER, channels INTEGER,
  source_media_id TEXT REFERENCES media_file(id)
);

CREATE TABLE song (
  id TEXT PRIMARY KEY, project_id TEXT NOT NULL REFERENCES project(id) ON DELETE CASCADE,
  title TEXT NOT NULL, artist TEXT,
  base_bpm REAL, bpm_confidence REAL,
  song_key TEXT, key_confidence REAL,
  time_sig_num INTEGER DEFAULT 4, time_sig_den INTEGER DEFAULT 4,
  original_media_id TEXT REFERENCES media_file(id),
  length_sec REAL, start_offset_sec REAL DEFAULT 0,
  last_position_sec REAL DEFAULT 0,
  notes TEXT
);

CREATE TABLE tempo_point (
  song_id TEXT NOT NULL REFERENCES song(id) ON DELETE CASCADE,
  beat REAL NOT NULL, bpm REAL NOT NULL CHECK (bpm > 0),
  curve TEXT NOT NULL DEFAULT 'step' CHECK (curve IN ('step','linear')),
  PRIMARY KEY (song_id, beat)
);

CREATE TABLE time_sig_point (
  song_id TEXT NOT NULL REFERENCES song(id) ON DELETE CASCADE,
  bar INTEGER NOT NULL, numerator INTEGER NOT NULL CHECK (numerator > 0),
  denominator INTEGER NOT NULL CHECK (denominator IN (1,2,4,8,16,32)),
  PRIMARY KEY (song_id, bar)
);

CREATE TABLE cue_point (
  id TEXT PRIMARY KEY, song_id TEXT NOT NULL REFERENCES song(id) ON DELETE CASCADE,
  name TEXT NOT NULL,
  section TEXT CHECK (section IN ('intro','verse','pre_chorus','chorus','bridge','solo','outro','custom')),
  position_sec REAL NOT NULL, color TEXT, sort_order INTEGER,
  source TEXT DEFAULT 'user' CHECK (source IN ('user','ai'))
);

CREATE TABLE chord_event (
  song_id TEXT NOT NULL REFERENCES song(id) ON DELETE CASCADE,
  start_sec REAL NOT NULL, end_sec REAL NOT NULL, chord TEXT NOT NULL, confidence REAL,
  PRIMARY KEY (song_id, start_sec)
);

CREATE TABLE bus (
  id TEXT PRIMARY KEY,
  kind TEXT NOT NULL CHECK (kind IN ('main','click','guide','extra','monitor','aux')),
  name TEXT NOT NULL, channels INTEGER NOT NULL CHECK (channels IN (1,2)),
  volume_db REAL DEFAULT 0, limiter_on INTEGER DEFAULT 0
);

CREATE TABLE output_assignment (
  physical_output INTEGER PRIMARY KEY,
  bus_id TEXT REFERENCES bus(id), bus_channel INTEGER,
  is_pa INTEGER NOT NULL DEFAULT 0,
  label TEXT,
  CHECK (NOT (is_pa = 1 AND bus_id IN ('bus-click','bus-guide','bus-monitor')))
);

CREATE TABLE plugin_slot (
  id TEXT PRIMARY KEY, plugin_uid TEXT NOT NULL, plugin_name TEXT NOT NULL,
  vendor TEXT, format TEXT DEFAULT 'VST3', version TEXT,
  state_rel_path TEXT, state_sha256 TEXT, preset_name TEXT
);

CREATE TABLE module_profile (
  id TEXT PRIMARY KEY, device_name TEXT NOT NULL, midi_port TEXT NOT NULL,
  midi_channel INTEGER NOT NULL CHECK (midi_channel BETWEEN 1 AND 16),
  bank_msb INTEGER CHECK (bank_msb BETWEEN 0 AND 127),
  bank_lsb INTEGER CHECK (bank_lsb BETWEEN 0 AND 127),
  program INTEGER CHECK (program BETWEEN 0 AND 127),
  volume_cc INTEGER DEFAULT 7, volume_value INTEGER DEFAULT 100,
  pan_cc INTEGER DEFAULT 10, pan_value INTEGER DEFAULT 64,
  sustain_enabled INTEGER DEFAULT 1, delay_offset_ms REAL DEFAULT 0,
  send_order TEXT DEFAULT 'msb,lsb,pc', gap_ms INTEGER DEFAULT 20, settle_ms INTEGER DEFAULT 200,
  audio_input_channel INTEGER,
  midi_port_id TEXT
);

CREATE TABLE track (
  id TEXT PRIMARY KEY, song_id TEXT NOT NULL REFERENCES song(id) ON DELETE CASCADE,
  name TEXT NOT NULL, sort_order INTEGER NOT NULL,
  kind TEXT NOT NULL CHECK (kind IN ('audio','midi_vsti','midi_external','click','guide','line_in')),
  media_id TEXT REFERENCES media_file(id),
  plugin_slot_id TEXT REFERENCES plugin_slot(id),
  module_profile_id TEXT REFERENCES module_profile(id),
  midi_rel_path TEXT, midi_channel INTEGER,
  volume_db REAL DEFAULT 0, pan REAL DEFAULT 0 CHECK (pan BETWEEN -1 AND 1), gain_db REAL DEFAULT 0,
  mute INTEGER DEFAULT 0, solo INTEGER DEFAULT 0, mono INTEGER DEFAULT 0,
  output_bus_id TEXT REFERENCES bus(id),
  delay_offset_ms REAL DEFAULT 0,
  ai_confidence REAL,
  destination_label TEXT
);

CREATE TABLE clip (
  id TEXT PRIMARY KEY, track_id TEXT NOT NULL REFERENCES track(id) ON DELETE CASCADE,
  media_id TEXT NOT NULL REFERENCES media_file(id),
  start_sec REAL NOT NULL DEFAULT 0, offset_sec REAL NOT NULL DEFAULT 0, length_sec REAL NOT NULL DEFAULT 0
);

CREATE TABLE insert_effect (
  track_id TEXT NOT NULL REFERENCES track(id) ON DELETE CASCADE,
  slot_index INTEGER NOT NULL, plugin_slot_id TEXT NOT NULL REFERENCES plugin_slot(id),
  bypass INTEGER DEFAULT 0, PRIMARY KEY (track_id, slot_index)
);

CREATE TABLE click_settings (
  song_id TEXT PRIMARY KEY REFERENCES song(id) ON DELETE CASCADE,
  enabled INTEGER DEFAULT 1, count_in_bars INTEGER DEFAULT 1 CHECK (count_in_bars IN (0,1,2,4)),
  accent_first INTEGER DEFAULT 1, sound TEXT DEFAULT 'woodblock',
  volume_db REAL DEFAULT -6, subdivision INTEGER DEFAULT 1
);

CREATE TABLE setlist (
  id TEXT PRIMARY KEY, name TEXT NOT NULL, venue TEXT, show_date TEXT
);
CREATE TABLE setlist_entry (
  setlist_id TEXT NOT NULL REFERENCES setlist(id) ON DELETE CASCADE,
  position INTEGER NOT NULL, song_id TEXT NOT NULL REFERENCES song(id) ON DELETE CASCADE,
  auto_advance INTEGER DEFAULT 0, gap_sec REAL DEFAULT 0,
  PRIMARY KEY (setlist_id, position)
);

CREATE TABLE footswitch_mapping (
  id TEXT PRIMARY KEY, midi_port TEXT, message_type TEXT CHECK (message_type IN ('cc','note')),
  channel INTEGER, number INTEGER, action TEXT NOT NULL, double_press INTEGER DEFAULT 0
);

CREATE TABLE analysis_run (
  id TEXT PRIMARY KEY, song_id TEXT NOT NULL REFERENCES song(id) ON DELETE CASCADE,
  started_at TEXT, finished_at TEXT, status TEXT, tasks TEXT,
  result_rel_path TEXT, model_versions TEXT
);

CREATE INDEX idx_track_song ON track(song_id, sort_order);
CREATE INDEX idx_clip_track ON clip(track_id);
CREATE INDEX idx_cue_song ON cue_point(song_id, position_sec);

INSERT INTO bus (id, kind, name, channels) VALUES
  ('bus-main', 'main', 'Main', 2), ('bus-click', 'click', 'Click', 1), ('bus-guide', 'guide', 'Guide', 1),
  ('bus-extra', 'extra', 'Extra', 2), ('bus-monitor', 'monitor', 'Monitor', 2), ('bus-aux', 'aux', 'Aux', 2);

INSERT INTO schema_version (version) VALUES (2);
)SQL";

std::string busId (const std::string& kind) { return "bus-" + kind; }
std::string busKindFromId (const std::string& id) { return id.rfind ("bus-", 0) == 0 ? id.substr (4) : std::string ("main"); }
} // namespace

//==============================================================================
ProjectDatabase::~ProjectDatabase()
{
    close();
}

void ProjectDatabase::close()
{
    if (db != nullptr)
    {
        sqlite3_close_v2 (db);
        db = nullptr;
    }
}

DbResult ProjectDatabase::exec (const char* sql)
{
    char* err = nullptr;
    if (sqlite3_exec (db, sql, nullptr, nullptr, &err) != SQLITE_OK)
    {
        std::string e = err != nullptr ? err : sqlite3_errmsg (db);
        sqlite3_free (err);
        return DbResult::failure (e);
    }
    return {};
}

int ProjectDatabase::getSchemaVersion()
{
    Stmt s (db, "SELECT version FROM schema_version LIMIT 1");
    if (s.valid() && s.next())
        return s.integer (0);
    return 0;
}

DbResult ProjectDatabase::open (const std::string& path, bool create)
{
    close();
    const int flags = SQLITE_OPEN_READWRITE | (create ? SQLITE_OPEN_CREATE : 0) | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2 (path.c_str(), &db, flags, nullptr) != SQLITE_OK)
    {
        std::string e = db != nullptr ? sqlite3_errmsg (db) : "cannot open database";
        close();
        return DbResult::failure ("Cannot open " + path + ": " + e);
    }
    sqlite3_busy_timeout (db, 3000);

    // Reading anything fails here for a file that is not a database.
    if (auto r = exec ("PRAGMA foreign_keys=ON;"); ! r)
    {
        close();
        return DbResult::failure ("Not a project database: " + r.error);
    }
    if (auto r = quickCheck(); ! r)
    {
        close();
        return DbResult::failure ("Project database is damaged (" + r.error + "). Restore a backup.");
    }
    if (auto r = exec ("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;"); ! r)
    {
        close();
        return r;
    }

    bool hasSchema = false;
    {
        Stmt s (db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='schema_version'");
        hasSchema = s.valid() && s.next();
    }
    if (! hasSchema)
    {
        Stmt s (db, "SELECT count(*) FROM sqlite_master WHERE type='table'");
        const bool empty = s.valid() && s.next() && s.integer (0) == 0;
        if (! empty)
        {
            close();
            return DbResult::failure ("This database is not a B4ND LIVE MTR project");
        }
        if (! create)
        {
            close();
            return DbResult::failure ("Project database is empty");
        }
        if (auto r = createSchema(); ! r)
        {
            close();
            return r;
        }
    }
    if (auto r = migrate(); ! r)
    {
        close();
        return r;
    }
    return {};
}

DbResult ProjectDatabase::createSchema()
{
    if (auto r = exec ("BEGIN IMMEDIATE;"); ! r)
        return r;
    if (auto r = exec (schemaV1); ! r)
    {
        exec ("ROLLBACK;");
        return DbResult::failure ("Schema creation failed: " + r.error);
    }
    return exec ("COMMIT;");
}

DbResult ProjectDatabase::migrate()
{
    const int v = getSchemaVersion();
    if (v > currentSchemaVersion)
        return DbResult::failure ("This project was saved by a newer version of B4ND LIVE MTR (schema "
                                  + std::to_string (v) + "). Update the app to open it.");
    if (v < 1)
        return DbResult::failure ("Unknown project schema version " + std::to_string (v));

    // Step-by-step migrations v -> v+1, each in its own transaction.
    struct Step { int from; const char* sql; };
    const Step steps[] = {
        { 1, "ALTER TABLE module_profile ADD COLUMN midi_port_id TEXT; UPDATE schema_version SET version = 2;" },
    };
    for (const auto& st : steps)
    {
        if (getSchemaVersion() != st.from)
            continue;
        if (auto r = exec ("BEGIN IMMEDIATE;"); ! r)
            return r;
        if (auto r = exec (st.sql); ! r)
        {
            exec ("ROLLBACK;");
            return DbResult::failure ("Project upgrade from schema " + std::to_string (st.from) + " failed: " + r.error);
        }
        if (auto r = exec ("COMMIT;"); ! r)
            return r;
    }
    return {};
}

DbResult ProjectDatabase::quickCheck()
{
    Stmt s (db, "PRAGMA quick_check");
    if (! s.valid())
        return DbResult::failure (sqlite3_errmsg (db));
    std::string first;
    if (s.next())
        first = s.text (0);
    else
        return DbResult::failure (sqlite3_errmsg (db));
    if (first != "ok")
        return DbResult::failure (first);
    return {};
}

DbResult ProjectDatabase::backupTo (const std::string& path)
{
    if (db == nullptr)
        return DbResult::failure ("No database open");
    sqlite3* dest = nullptr;
    if (sqlite3_open_v2 (path.c_str(), &dest, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK)
    {
        std::string e = dest != nullptr ? sqlite3_errmsg (dest) : "cannot create backup";
        sqlite3_close_v2 (dest);
        return DbResult::failure ("Backup failed: " + e);
    }
    auto* b = sqlite3_backup_init (dest, "main", db, "main");
    int rc = SQLITE_ERROR;
    if (b != nullptr)
    {
        rc = sqlite3_backup_step (b, -1);
        sqlite3_backup_finish (b);
    }
    std::string e = sqlite3_errmsg (dest);
    sqlite3_close_v2 (dest);
    if (rc != SQLITE_DONE)
        return DbResult::failure ("Backup failed: " + e);
    return {};
}

//==============================================================================
DbResult ProjectDatabase::save (const ProjectData& p)
{
    if (db == nullptr)
        return DbResult::failure ("No database open");

    if (auto r = exec ("BEGIN IMMEDIATE;"); ! r)
        return r;

    auto fail = [this] (const std::string& what) -> DbResult
    {
        std::string e = what + ": " + sqlite3_errmsg (db);
        exec ("ROLLBACK;");
        return DbResult::failure (e);
    };

    // Replace everything (cascades remove songs' children). Buses are fixed rows.
    if (! exec ("DELETE FROM setlist; DELETE FROM output_assignment; DELETE FROM song; DELETE FROM project;"
                " DELETE FROM media_file; DELETE FROM module_profile;"))
        return fail ("Clearing project");

    {
        Stmt s (db, "INSERT INTO project (id, name, sample_rate, created_at, modified_at, app_version) VALUES (?,?,?,?,?,?)");
        if (! s.valid()
            || ! s.bind (1, p.id).bind (2, p.name).bind (3, p.sampleRate)
                  .bind (4, p.createdAt.empty() ? nowIso8601() : p.createdAt)
                  .bind (5, p.modifiedAt.empty() ? nowIso8601() : p.modifiedAt)
                  .bind (6, p.appVersion).run())
            return fail ("Saving project");
    }
    {
        Stmt s (db, "INSERT INTO media_file (id, kind, rel_path, sha256, duration_sec, sample_rate, channels) VALUES (?,?,?,?,?,?,?)");
        for (const auto& m : p.media)
            if (! s.bind (1, m.id).bind (2, m.kind).bind (3, m.path).bindOrNull (4, m.sha256)
                     .bind (5, m.durationSec).bind (6, m.sampleRate).bind (7, m.channels).run())
                return fail ("Saving media " + m.path);
    }

    {
        Stmt m (db, "INSERT INTO module_profile (id, device_name, midi_port, midi_port_id, midi_channel, bank_msb, bank_lsb, program,"
                    " volume_cc, volume_value, pan_cc, pan_value, sustain_enabled, delay_offset_ms, send_order, gap_ms, settle_ms)"
                    " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        auto optional = [&m] (int i, int v) { if (v < 0) m.bindNull (i); else m.bind (i, v); };
        for (const auto& mp : p.modules)
        {
            m.bind (1, mp.id).bind (2, mp.deviceName).bind (3, mp.portName).bindOrNull (4, mp.portIdentifier).bind (5, mp.channel);
            optional (6, mp.bankMsb);
            optional (7, mp.bankLsb);
            optional (8, mp.program);
            m.bind (9, mp.volumeCc);
            optional (10, mp.volume);
            m.bind (11, mp.panCc);
            optional (12, mp.pan);
            if (! m.bind (13, mp.sustainEnabled).bind (14, mp.delayOffsetMs).bind (15, mp.sendOrder)
                     .bind (16, mp.gapMs).bind (17, mp.settleMs).run())
                return fail ("Saving module " + mp.deviceName);
        }
    }

    Stmt song (db, "INSERT INTO song (id, project_id, title, artist, base_bpm, song_key, time_sig_num, time_sig_den,"
                   " length_sec, last_position_sec, notes) VALUES (?,?,?,?,?,?,?,?,?,?,?)");
    Stmt tempo (db, "INSERT INTO tempo_point (song_id, beat, bpm) VALUES (?,?,?)");
    Stmt sig (db, "INSERT INTO time_sig_point (song_id, bar, numerator, denominator) VALUES (?,?,?,?)");
    Stmt cue (db, "INSERT INTO cue_point (id, song_id, name, section, position_sec, color, sort_order, source) VALUES (?,?,?,?,?,?,?,?)");
    Stmt track (db, "INSERT INTO track (id, song_id, name, sort_order, kind, media_id, volume_db, pan, gain_db, mute, solo, mono,"
                    " output_bus_id, delay_offset_ms, destination_label, module_profile_id, midi_channel, midi_rel_path)"
                    " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    Stmt clip (db, "INSERT INTO clip (id, track_id, media_id, start_sec, offset_sec, length_sec) VALUES (?,?,?,?,?,?)");
    Stmt click (db, "INSERT INTO click_settings (song_id, enabled, count_in_bars, accent_first, sound, volume_db, subdivision)"
                    " VALUES (?,?,?,?,?,?,?)");
    if (! song.valid() || ! tempo.valid() || ! sig.valid() || ! cue.valid() || ! track.valid() || ! clip.valid() || ! click.valid())
        return fail ("Preparing statements");

    for (const auto& sd : p.songs)
    {
        const int num = sd.timeSigPoints.empty() ? 4 : sd.timeSigPoints.front().numerator;
        const int den = sd.timeSigPoints.empty() ? 4 : sd.timeSigPoints.front().denominator;
        if (! song.bind (1, sd.id).bind (2, p.id).bind (3, sd.title).bindOrNull (4, sd.artist).bind (5, sd.baseBpm)
                 .bindOrNull (6, sd.key).bind (7, num).bind (8, den).bind (9, sd.lengthSec).bind (10, sd.lastPositionSec)
                 .bindOrNull (11, sd.notes).run())
            return fail ("Saving song " + sd.title);

        for (const auto& t : sd.tempoPoints)
            if (! tempo.bind (1, sd.id).bind (2, t.beat).bind (3, t.bpm).run())
                return fail ("Saving tempo map of " + sd.title);
        for (const auto& t : sd.timeSigPoints)
            if (! sig.bind (1, sd.id).bind (2, t.bar).bind (3, t.numerator).bind (4, t.denominator).run())
                return fail ("Saving time signatures of " + sd.title);

        int order = 0;
        for (const auto& c : sd.cues)
            if (! cue.bind (1, c.id).bind (2, sd.id).bind (3, c.name).bind (4, c.section).bind (5, c.positionSec)
                     .bindOrNull (6, c.color).bind (7, order++).bind (8, c.source).run())
                return fail ("Saving cue " + c.name);

        order = 0;
        for (const auto& t : sd.tracks)
        {
            const std::string firstMedia = t.clips.empty() ? std::string() : t.clips.front().mediaId;
            if (! track.bind (1, t.id).bind (2, sd.id).bind (3, t.name).bind (4, order++).bind (5, t.kind)
                       .bindOrNull (6, firstMedia).bind (7, t.volumeDb).bind (8, t.pan).bind (9, t.gainDb)
                       .bind (10, t.mute).bind (11, t.solo).bind (12, t.mono).bind (13, busId (t.busKind))
                       .bind (14, t.delayOffsetMs).bindOrNull (15, t.destinationLabel).bindOrNull (16, t.moduleProfileId)
                       .bind (17, t.midiChannel).bindOrNull (18, t.midiPath).run())
                return fail ("Saving track " + t.name);
            for (const auto& c : t.clips)
                if (! clip.bind (1, c.id).bind (2, t.id).bind (3, c.mediaId).bind (4, c.startSec)
                         .bind (5, c.offsetSec).bind (6, c.lengthSec).run())
                    return fail ("Saving clip of " + t.name);
        }

        const auto& k = sd.click;
        if (! click.bind (1, sd.id).bind (2, k.enabled).bind (3, k.countInBars).bind (4, k.accentFirst)
                  .bind (5, k.sound).bind (6, k.volumeDb).bind (7, k.subdivision).run())
            return fail ("Saving click settings of " + sd.title);
    }

    {
        Stmt sl (db, "INSERT INTO setlist (id, name, venue, show_date) VALUES (?,?,?,?)");
        Stmt se (db, "INSERT INTO setlist_entry (setlist_id, position, song_id) VALUES (?,?,?)");
        for (const auto& s : p.setlists)
        {
            if (! sl.bind (1, s.id).bind (2, s.name).bindOrNull (3, s.venue).bindOrNull (4, s.showDate).run())
                return fail ("Saving setlist " + s.name);
            int pos = 0;
            for (const auto& id : s.songIds)
                if (! se.bind (1, s.id).bind (2, pos++).bind (3, id).run())
                    return fail ("Saving setlist entry");
        }
    }
    {
        Stmt o (db, "INSERT INTO output_assignment (physical_output, bus_id, bus_channel, is_pa, label) VALUES (?,?,?,?,?)");
        for (const auto& out : p.outputs)
        {
            o.bind (1, out.physicalOutput);
            if (out.hasSource) o.bind (2, busId (out.busKind)).bind (3, out.busChannel);
            else               o.bindNull (2).bindNull (3);
            if (! o.bind (4, out.isPA).bindOrNull (5, out.label).run())
                return fail ("Saving output " + std::to_string (out.physicalOutput + 1)
                             + (out.isPA ? " (click/guide/monitor can not go to a PA output)" : ""));
        }
    }

    if (auto r = exec ("COMMIT;"); ! r)
    {
        exec ("ROLLBACK;");
        return r;
    }
    return {};
}

//==============================================================================
DbResult ProjectDatabase::load (ProjectData& out)
{
    if (db == nullptr)
        return DbResult::failure ("No database open");

    ProjectData p;
    if (auto r = exec ("BEGIN;"); ! r)   // consistent snapshot
        return r;
    struct EndTx { ProjectDatabase& d; ~EndTx() { d.exec ("COMMIT;"); } } endTx { *this };

    {
        Stmt s (db, "SELECT id, name, sample_rate, created_at, modified_at, app_version FROM project LIMIT 1");
        if (! s.valid() || ! s.next())
            return DbResult::failure ("Project database contains no project");
        p.id = s.text (0); p.name = s.text (1); p.sampleRate = s.integer (2);
        p.createdAt = s.text (3); p.modifiedAt = s.text (4); p.appVersion = s.text (5);
    }
    {
        Stmt s (db, "SELECT id, kind, rel_path, sha256, duration_sec, sample_rate, channels FROM media_file ORDER BY rowid");
        while (s.next())
        {
            MediaRef m;
            m.id = s.text (0); m.kind = s.text (1); m.path = s.text (2); m.sha256 = s.text (3);
            m.durationSec = s.real (4); m.sampleRate = s.integer (5); m.channels = s.integer (6);
            p.media.push_back (std::move (m));
        }
    }

    {
        Stmt m (db, "SELECT id, device_name, midi_port, midi_port_id, midi_channel, bank_msb, bank_lsb, program, volume_cc, volume_value,"
                    " pan_cc, pan_value, sustain_enabled, delay_offset_ms, send_order, gap_ms, settle_ms FROM module_profile ORDER BY rowid");
        auto optional = [&m] (int c) { return m.isNull (c) ? -1 : m.integer (c); };
        while (m.next())
        {
            ModuleProfile mp;
            mp.id = m.text (0); mp.deviceName = m.text (1); mp.portName = m.text (2); mp.portIdentifier = m.text (3);
            mp.channel = m.integer (4); mp.bankMsb = optional (5); mp.bankLsb = optional (6); mp.program = optional (7);
            mp.volumeCc = m.integer (8); mp.volume = optional (9); mp.panCc = m.integer (10); mp.pan = optional (11);
            mp.sustainEnabled = m.integer (12) != 0; mp.delayOffsetMs = m.real (13); mp.sendOrder = m.text (14);
            mp.gapMs = m.integer (15); mp.settleMs = m.integer (16);
            p.modules.push_back (std::move (mp));
        }
    }

    Stmt songs (db, "SELECT id, title, artist, base_bpm, song_key, length_sec, last_position_sec, notes FROM song ORDER BY rowid");
    Stmt tempo (db, "SELECT beat, bpm FROM tempo_point WHERE song_id = ? ORDER BY beat");
    Stmt sig (db, "SELECT bar, numerator, denominator FROM time_sig_point WHERE song_id = ? ORDER BY bar");
    Stmt cue (db, "SELECT id, name, section, position_sec, color, source FROM cue_point WHERE song_id = ? ORDER BY sort_order, position_sec");
    Stmt track (db, "SELECT id, name, kind, volume_db, pan, gain_db, mute, solo, mono, output_bus_id, delay_offset_ms, destination_label,"
                    " module_profile_id, midi_channel, midi_rel_path FROM track WHERE song_id = ? ORDER BY sort_order");
    Stmt clip (db, "SELECT id, media_id, start_sec, offset_sec, length_sec FROM clip WHERE track_id = ? ORDER BY start_sec, rowid");
    Stmt click (db, "SELECT enabled, count_in_bars, accent_first, sound, volume_db, subdivision FROM click_settings WHERE song_id = ?");
    if (! songs.valid() || ! tempo.valid() || ! sig.valid() || ! cue.valid() || ! track.valid() || ! clip.valid() || ! click.valid())
        return DbResult::failure (std::string ("Reading project: ") + sqlite3_errmsg (db));

    while (songs.next())
    {
        SongData sd;
        sd.id = songs.text (0); sd.title = songs.text (1); sd.artist = songs.text (2); sd.baseBpm = songs.real (3);
        sd.key = songs.text (4); sd.lengthSec = songs.real (5); sd.lastPositionSec = songs.real (6); sd.notes = songs.text (7);

        tempo.bind (1, sd.id);
        while (tempo.next()) sd.tempoPoints.push_back ({ tempo.real (0), tempo.real (1) });
        tempo.reset();

        sig.bind (1, sd.id);
        while (sig.next()) sd.timeSigPoints.push_back ({ sig.integer (0), sig.integer (1), sig.integer (2) });
        sig.reset();

        cue.bind (1, sd.id);
        while (cue.next())
        {
            CueData c;
            c.id = cue.text (0); c.name = cue.text (1); c.section = cue.text (2); c.positionSec = cue.real (3);
            c.color = cue.text (4); c.source = cue.text (5);
            sd.cues.push_back (std::move (c));
        }
        cue.reset();

        track.bind (1, sd.id);
        while (track.next())
        {
            TrackData t;
            t.id = track.text (0); t.name = track.text (1); t.kind = track.text (2);
            t.volumeDb = track.real (3); t.pan = track.real (4); t.gainDb = track.real (5);
            t.mute = track.integer (6) != 0; t.solo = track.integer (7) != 0; t.mono = track.integer (8) != 0;
            t.busKind = busKindFromId (track.text (9)); t.delayOffsetMs = track.real (10); t.destinationLabel = track.text (11);
            t.moduleProfileId = track.text (12); t.midiChannel = track.integer (13); t.midiPath = track.text (14);
            sd.tracks.push_back (std::move (t));
        }
        track.reset();

        for (auto& t : sd.tracks)
        {
            clip.bind (1, t.id);
            while (clip.next())
                t.clips.push_back ({ clip.text (0), clip.text (1), clip.real (2), clip.real (3), clip.real (4) });
            clip.reset();
        }

        click.bind (1, sd.id);
        if (click.next())
        {
            sd.click.enabled = click.integer (0) != 0; sd.click.countInBars = click.integer (1);
            sd.click.accentFirst = click.integer (2) != 0; sd.click.sound = click.text (3);
            sd.click.volumeDb = click.real (4); sd.click.subdivision = click.integer (5);
        }
        click.reset();

        p.songs.push_back (std::move (sd));
    }

    {
        Stmt sl (db, "SELECT id, name, venue, show_date FROM setlist ORDER BY rowid");
        Stmt se (db, "SELECT song_id FROM setlist_entry WHERE setlist_id = ? ORDER BY position");
        while (sl.next())
        {
            SetlistData s;
            s.id = sl.text (0); s.name = sl.text (1); s.venue = sl.text (2); s.showDate = sl.text (3);
            se.bind (1, s.id);
            while (se.next()) s.songIds.push_back (se.text (0));
            se.reset();
            p.setlists.push_back (std::move (s));
        }
    }
    {
        Stmt o (db, "SELECT physical_output, bus_id, bus_channel, is_pa, label FROM output_assignment ORDER BY physical_output");
        while (o.next())
        {
            OutputData d;
            d.physicalOutput = o.integer (0);
            d.hasSource = ! o.isNull (1);
            if (d.hasSource) { d.busKind = busKindFromId (o.text (1)); d.busChannel = o.integer (2); }
            d.isPA = o.integer (3) != 0;
            d.label = o.text (4);
            p.outputs.push_back (std::move (d));
        }
    }

    out = std::move (p);
    return {};
}

} // namespace b4nd
