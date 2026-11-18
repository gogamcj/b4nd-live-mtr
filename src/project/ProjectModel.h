#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/Midi.h"
#include "core/TempoMap.h"

namespace b4nd
{

/**
    Plain, JUCE-free project model: what is stored in project.db (docs/08).
    Strings are UTF-8. Times are seconds (independent of the sample rate).
    The app converts to and from its UI model; the audio thread never sees this.
*/

struct MediaRef
{
    std::string id;
    std::string kind = "original";   // original | stem | cache | guide | click_sample
    std::string path;                // relative to the package folder when inside it, else absolute
    std::string sha256;
    double durationSec = 0.0;
    int sampleRate = 0;
    int channels = 0;
};

struct CueData
{
    std::string id;
    std::string name;
    std::string section = "custom";   // intro | verse | pre_chorus | chorus | bridge | solo | outro | custom
    double positionSec = 0.0;
    std::string color;
    std::string source = "user";      // user | ai
};

struct ClipData
{
    std::string id;
    std::string mediaId;
    double startSec = 0.0;    // position on the song timeline
    double offsetSec = 0.0;   // offset into the media file
    double lengthSec = 0.0;   // 0 = until the end of the file
};

struct TrackData
{
    std::string id;
    std::string name;
    std::string kind = "audio";         // audio | midi_vsti | midi_external | click | guide | line_in
    std::string busKind = "main";       // main | click | guide | extra | monitor | aux
    std::string destinationLabel;       // free text for MIDI destinations until Phase 4/5
    double volumeDb = 0.0, pan = 0.0, gainDb = 0.0;
    bool mute = false, solo = false, mono = false;
    double delayOffsetMs = 0.0;
    std::vector<ClipData> clips;

    // MIDI tracks
    std::string pluginSlotId;      // software instrument (midi_vsti)
    std::string moduleProfileId;   // external module (midi_external)
    int midiChannel = 0;           // 1-16 overrides the profile's channel, 0 = use the profile's
    std::string midiPath;          // .mid file, stored like media paths
};

/** A plugin instance and its saved state (docs/05 §6). The state is stored in the database
    (not a side file) so that backups and the save transaction always include it. */
struct PluginSlotData
{
    std::string id;
    std::string pluginUid;       // format-specific identifier (JUCE PluginDescription::createIdentifierString)
    std::string pluginName;
    std::string vendor;
    std::string format = "VST3";
    std::string version;
    std::string presetName;
    std::string descriptionXml;  // full description, so the plugin can be found again on another PC
    std::vector<std::uint8_t> state;
};

struct ClickSettingsData
{
    bool enabled = true;
    int countInBars = 1;
    bool accentFirst = true;
    std::string sound = "default";
    double volumeDb = -6.0;
    int subdivision = 1;
};

struct SongData
{
    std::string id;
    std::string title;
    std::string artist;
    std::string key;
    double baseBpm = 120.0;
    double lengthSec = 0.0;
    double lastPositionSec = 0.0;
    std::string notes;
    std::vector<TempoPoint> tempoPoints;
    std::vector<TimeSigPoint> timeSigPoints;
    std::vector<CueData> cues;
    std::vector<TrackData> tracks;
    ClickSettingsData click;
};

struct OutputData
{
    int physicalOutput = 0;
    bool hasSource = false;
    std::string busKind;   // main | click | guide | extra | monitor | aux
    int busChannel = 0;    // 0 L/mono, 1 R, 2 mono sum
    bool isPA = false;
    std::string label;
};

struct SetlistData
{
    std::string id;
    std::string name;
    std::string venue;
    std::string showDate;
    std::vector<std::string> songIds;   // in show order
};

struct ProjectData
{
    std::string id;
    std::string name;
    int sampleRate = 48000;
    std::string createdAt, modifiedAt;
    std::string appVersion;
    std::vector<MediaRef> media;
    std::vector<SongData> songs;
    std::vector<SetlistData> setlists;   // setlists[0] is the active show order
    std::vector<OutputData> outputs;     // empty: use the device default preset
    std::vector<ModuleProfile> modules;  // external sound modules
    std::vector<PluginSlotData> plugins; // software instrument instances

    const MediaRef* findMedia (const std::string& mediaId) const
    {
        for (const auto& m : media)
            if (m.id == mediaId)
                return &m;
        return nullptr;
    }
};

/** Random RFC 4122 version 4 UUID, lower case. */
std::string newUuid();

/** Current UTC time, ISO 8601 ("2026-11-16T13:51:02Z"). */
std::string nowIso8601();

} // namespace b4nd
