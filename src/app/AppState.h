#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "audio/AudioDeviceController.h"
#include "core/OutputRouting.h"
#include "core/TempoMap.h"
#include "app/ProjectSession.h"
#include "media/MediaLoader.h"
#include "midi/MidiOutputService.h"
#include "plugins/PluginHost.h"

#include <set>

namespace b4nd
{

enum class TrackKind
{
    Audio,
    MidiVsti,
    MidiExternal,
    Click,
    Guide
};

struct TrackInfo
{
    juce::String id;                   // UUID, kept across saves
    juce::String name;
    TrackKind kind = TrackKind::Audio;
    juce::String destination;          // "Main L/R", "OUT 3", plugin or module name
    float volumeDb = 0.0f;
    float pan = 0.0f;
    bool mute = false;
    bool solo = false;
    std::vector<juce::Range<double>> regions;   // seconds

    // --- audio media (Audio / Guide tracks) ---
    BusKind bus = BusKind::Main;       // Audio tracks: Main / Extra / Monitor / Aux
    juce::String mediaPath;            // empty: no media assigned
    double mediaStartSeconds = 0.0;

    enum class MediaStatus { None, Loading, Ready, Missing, Error };
    MediaStatus mediaStatus = MediaStatus::None;
    juce::String mediaError;
    std::shared_ptr<const AudioData> media;   // decoded at mediaRate
    double mediaRate = 0.0;
    std::shared_ptr<const std::vector<float>> overview;   // peak per bucket over the whole file, for drawing

    // --- external MIDI (MidiExternal tracks) ---
    juce::String midiPath;             // .mid file; empty: none
    juce::String moduleId;             // ModuleProfile::id; empty: not assigned
    int midiChannel = 0;               // 1-16; 0 = the module's channel. Every event is sent on this channel.
    enum class MidiStatus { None, Ready, Missing, Error };
    MidiStatus midiStatus = MidiStatus::None;
    juce::String midiError;
    int midiNotes = 0;
    std::shared_ptr<const std::vector<MidiTickEvent>> midiEvents;   // 960 PPQ, song beats
    // (MidiVsti tracks use midiPath / midiChannel / midiEvents too)

    // --- software instrument (MidiVsti tracks, docs/05) ---
    juce::String pluginSlotId;         // PluginSlotData::id in the project
    juce::String pluginUid, pluginName, pluginVendor, pluginDescXml;
    juce::MemoryBlock pluginState;     // last captured state: saved even when the plugin is missing
    enum class PluginStatus { None, Ready, Missing, NotScanned, Blacklisted, LoadError, Failed };
    PluginStatus pluginStatus = PluginStatus::None;
    juce::String pluginError;
    bool pluginCertified = false;
    std::shared_ptr<PluginInstrument> instrument;
};

struct CuePoint
{
    juce::String name;
    double seconds = 0.0;
};

struct SongInfo
{
    juce::String id;                   // UUID, kept across saves
    juce::String title;
    juce::String artist;
    juce::String key;
    double lengthSeconds = 0.0;
    TempoMap tempo;
    std::vector<TrackInfo> tracks;
    std::vector<CuePoint> cues;
};

enum class TransportState
{
    Stopped,
    Playing,
    Paused
};

/**
    UI-side application model.

    With a running audio device, transport commands go to the AudioEngine's
    lock-free command queue and the play position comes from engine telemetry
    (the audio sample clock is the master clock, docs/03). Without a device the
    transport is *virtual* (UI timer) so the screens can still be rehearsed.
*/
class AppState : public juce::ChangeBroadcaster,
                 private juce::ChangeListener,
                 private juce::Timer
{
public:
    AppState (AudioEngine&, AudioDeviceController&);
    ~AppState() override;

    static std::vector<SongInfo> createDemoSetlist();

    // --- project / setlist ---
    const juce::String& getProjectName() const noexcept { return projectName; }

    // --- project file (SQLite package, docs/08) ---
    /** True when the project lives in a package folder (otherwise: unsaved demo / new). */
    bool hasProjectFile() const { return session.isOpen(); }
    juce::File getProjectFolder() const { return session.getFolder(); }
    bool isDirty() const noexcept { return dirty; }
    /** "Saved 14:02", "Saving...", "NOT SAVED: <error>", "Unsaved" for the status bar. */
    juce::String getSaveStatusText() const;
    bool hasSaveError() const { return session.getLastError().isNotEmpty(); }

    /** Empty string on success, else the reason. All are rejected in Live Mode and while playing. */
    juce::String newProject (const juce::File& folder, const juce::String& name);
    juce::String openProject (const juce::File& folderOrDb);
    juce::String saveProject();                          // requires hasProjectFile()
    juce::String saveProjectAs (const juce::File& folder);
    juce::String restoreBackup (const juce::String& backupPath);
    /** Flushes pending changes and closes the project (quit). */
    void closeProject();

    /** Set after openProject() when the previous session crashed: newest backups first. */
    bool lastOpenHadStaleLock() const noexcept { return staleLockOnOpen; }
    const juce::StringArray& getLastOpenBackups() const noexcept { return backupsOnOpen; }
    const juce::StringArray& getMissingMediaOnOpen() const noexcept { return missingOnOpen; }

    static juce::File getLastProjectRecordFile();
    static juce::File getDefaultProjectsFolder();
    const std::vector<SongInfo>& getSongs() const noexcept { return songs; }
    int getCurrentSongIndex() const noexcept { return currentSong; }
    const SongInfo* getCurrentSong() const noexcept;
    const SongInfo* getNextSong() const noexcept;
    void selectSong (int index);
    void nextSong();
    void previousSong();
    double getSetlistTotalSeconds() const noexcept;

    // --- transport ---
    TransportState getTransportState() const noexcept { return transport; }
    double getPositionSeconds() const noexcept { return position; }
    double getRemainingSeconds() const noexcept;
    bool isLooping() const noexcept { return looping; }
    bool isEmergencyStopped() const noexcept { return emergency; }

    void play();
    void pause();
    void togglePlayPause();
    void stop();
    void restart();
    void emergencyStop();
    void locate (double seconds);
    void toggleLoop();
    void jumpToCue (int cueIndex);
    void jumpToNextCue();

    // --- mixer (pushed to the engine in real time) ---
    void setTrackVolume (int trackIndex, float db);
    void setTrackPan (int trackIndex, float pan);
    void setTrackMute (int trackIndex, bool m);
    void setTrackSolo (int trackIndex, bool s);
    /** Master fader: Main (PA) bus only. */
    void setMasterVolume (float db);
    float getMasterVolumeDb() const noexcept { return masterDb; }
    /** Main (PA) bus limiter, -0.3 dBFS. On by default; saved with the project. Locked in Live Mode. */
    void setLimiterEnabled (bool on);
    bool isLimiterEnabled() const noexcept { return limiterOn; }
    /** Current limiter gain reduction in dB (0 = none, negative = limiting), decays on the UI side. */
    float getLimiterReductionDb() const noexcept { return limiterGrDb; }
    /** Some PA output (or the Main mix) went over 0 dBFS since the last clear. */
    bool isPAClipLatched() const;
    void clearClipLatches() { engine.clearClipLatches(); sendChangeMessage(); }

    // --- media ---
    /** Assigns an audio file to an Audio/Guide track, or adds a new Audio track (trackIndex < 0).
        Rejected in Live Mode. Returns an empty string on success, else the reason. */
    juce::String addAudioFile (const juce::File& file, int trackIndex = -1);
    bool canReadAudioFile (const juce::File& f) const { return mediaPool.canRead (f); }

    struct MediaSummary
    {
        int loading = 0, missing = 0, errors = 0, ready = 0;
        std::int64_t ramBytes = 0;
    };
    MediaSummary getMediaSummary() const;

    /** RAM above this is reported as a warning (docs/03 §6). */
    static constexpr std::int64_t ramWarningBytes = (std::int64_t) 4 * 1024 * 1024 * 1024;

    // --- external MIDI sound modules (docs/04) ---
    const std::vector<ModuleProfile>& getModules() const noexcept { return modules; }
    int findModule (const juce::String& id) const;
    /** All return an empty string on success, else the reason. Rejected in Live Mode and while playing. */
    juce::String addModule (ModuleProfile);
    juce::String updateModule (int index, const ModuleProfile&);
    juce::String removeModule (int index);
    MidiOutputService::PortStatus getModuleStatus (int index) const noexcept { return midi.getStatus (index); }
    void sendModuleTestNote (int index) { midi.sendTestNote (index); }
    void recallModulePreset (int index) { midi.recallPreset (index); }
    std::vector<MidiPortInfo> listMidiOutputs() { return midi.listOutputs(); }
    MidiOutputService& getMidiOutput() noexcept { return midi; }

    /** Assigns a .mid file to an external MIDI track, or adds a new one (trackIndex < 0). */
    juce::String addMidiFile (const juce::File&, int trackIndex = -1);
    juce::String setTrackModule (int trackIndex, const juce::String& moduleId, int channel = 0);

    struct MidiSummary
    {
        int modules = 0, ready = 0, preparing = 0, problems = 0;   // problems: disconnected / busy / no port
        int tracksWithoutModule = 0, missingFiles = 0;
    };
    MidiSummary getMidiSummary() const;

    // --- latency measurement (docs/04 §4) ---
    struct Measurement
    {
        bool running = false, finished = false, ok = false, midi = false;
        int moduleIndex = -1, input = -1, output = -1;
        int pingsDone = 0, pings = 5;
        double resultMs = 0.0;     // round trip, or the module's MIDI-to-sound delay
        juce::String message;      // what happened, for the UI and the log
    };
    /** Interface round trip: a short tick on `output` (default: the first in-ear output; never
        a PA output) must be cabled back into `input`. Stopped, outside Live Mode only. */
    juce::String startLoopbackMeasurement (int input, int output = -1);
    /** Sends test notes to module `index` and listens for its sound on `input`. The delay
        (minus the round trip) is written into the module's MIDI delay offset. */
    juce::String startModuleMeasurement (int moduleIndex, int input);
    void cancelMeasurement();
    const Measurement& getMeasurement() const noexcept { return measurement; }
    /** Measured round trip for the current device settings, or < 0 if not measured. */
    double getRoundTripMs() const;
    /** The output a loopback test uses by default, or -1. */
    int defaultLoopbackOutput() const;

    // --- VST3 instruments (docs/05) ---
    PluginHost& getPluginHost() noexcept { return plugins; }
    /** Puts a plugin from the catalog on a VSTi track, or adds a new VSTi track (trackIndex < 0).
        Rejected in Live Mode and while playing. */
    juce::String setTrackPlugin (int trackIndex, const juce::String& pluginUid);
    juce::String clearTrackPlugin (int trackIndex);
    /** Opens the plugin's own editor (rejected in Live Mode). */
    juce::String showPluginEditor (int trackIndex);
    /** Tries again to load missing / failed plugins (after a scan, or after installing them). */
    void reloadPlugins();

    struct PluginSummary
    {
        int instruments = 0, ready = 0, notCertified = 0, problems = 0;   // problems: missing / failed / not loaded
        float maxLoad = 0.0f;                                             // highest plugin DSP load (0..1)
    };
    PluginSummary getPluginSummary() const;
    static juce::String pluginStatusText (const TrackInfo&);

    // --- modes ---
    bool isLiveMode() const noexcept { return liveMode; }
    void setLiveMode (bool shouldBeLive);

    const OutputRouting& getRouting() const noexcept { return routing; }

    /** Where a track is actually heard, from the current routing (e.g. "OUT 3 + IN-EAR 7-8").
        MIDI tracks return their configured destination text. */
    juce::String getTrackDestination (int trackIndex) const;

    /** Per physical output peak level (0..1) from the engine, refreshed at 30 Hz. */
    float getOutputLevel (int output) const noexcept
    {
        return juce::isPositiveAndBelow (output, (int) outputLevels.size()) ? outputLevels[(size_t) output] : 0.0f;
    }
    /** Per track peak level (0..1) of the current song, refreshed at 30 Hz. */
    float getTrackLevel (int track) const noexcept
    {
        return juce::isPositiveAndBelow (track, (int) trackLevels.size()) ? trackLevels[(size_t) track] : 0.0f;
    }

    // --- audio engine / device ---
    bool isEngineActive() const noexcept;
    AudioEngine& getEngine() noexcept { return engine; }
    AudioDeviceController& getDevice() noexcept { return device; }
    bool startOutputTest (TestSignal, int output);
    void stopOutputTest();
    std::uint64_t getDropoutCount() const noexcept;

private:
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void post (EngineCommand::Type, std::int64_t samples = 0);
    std::int64_t secondsToSamples (double) const noexcept;
    SongInfo* currentSongMutable() noexcept;
    void setTransport (TransportState s);

    double mediaSampleRate() const noexcept;
    void requestAllMedia();
    void applyFinishedMedia();
    void rebuildPlan();
    void pushMixerToEngine();
    void loadMidiFile (TrackInfo&, const SongInfo&);
    void sendTrackVolumeToModule (const TrackInfo&);
    juce::String guardModuleChange() const;
    void loadInstrument (TrackInfo&);
    void loadAllInstruments();
    void prepareInstruments();
    void captureInstrumentStates();
    double pluginSampleRate() const;
    int pluginBlockSize() const;

    void pollMeasurement();
    juce::String guardMeasurement() const;
    juce::String deviceKey() const;

    void markDirty();
    ProjectData toProjectData() const;
    void applyProjectData (const ProjectData&, const juce::File& folder);
    juce::String guardProjectChange() const;
    void rememberLastProject() const;

    juce::String projectName { "B4ND Demo Project" };
    std::vector<SongInfo> songs;
    int currentSong = 0;

    TransportState transport = TransportState::Stopped;
    double position = 0.0;
    double lastTickMs = 0.0;
    bool looping = false;
    bool emergency = false;
    bool liveMode = false;
    float masterDb = 0.0f;
    bool limiterOn = true;
    float limiterGrDb = 0.0f;

    OutputRouting routing = OutputRouting::createDefault (8);

    AudioEngine& engine;
    AudioDeviceController& device;
    AudioDeviceController::Status lastDeviceStatus = AudioDeviceController::Status::NoDevice;
    std::array<float, AudioEngine::maxOutputs> outputLevels {};
    std::uint64_t dropoutsBeforeRestart = 0;
    std::array<float, AudioEngine::maxTracks> trackLevels {};
    double lastDeviceRate = 0.0;
    int lastDeviceOutputs = -1;
    bool ramWarningLogged = false;
    // project file state
    ProjectSession session;
    juce::String projectId, projectCreatedAt;
    bool dirty = false;
    double lastChangeMs = 0.0, lastBackupMs = 0.0;
    bool staleLockOnOpen = false;
    juce::StringArray backupsOnOpen, missingOnOpen;
    static constexpr double autosaveDelayMs = 5000.0;
    static constexpr double backupIntervalMs = 30.0 * 60.0 * 1000.0;

    Measurement measurement;
    std::uint32_t measurementRun = 0;
    double roundTripMs = -1.0;
    juce::String roundTripKey;   // device / rate / buffer it was measured with

    std::vector<ModuleProfile> modules;   // module i = MIDI port slot i
    MidiOutputService midi;
    PluginHost plugins;
    bool instrumentStateStale = false;   // a plugin changed: capture its state when stopped
    std::set<const PluginInstrument*> failureLogged;
    MediaPool mediaPool;   // last: its thread stops first on destruction

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AppState)
};

juce::Colour colourForTrackKind (TrackKind) noexcept;
juce::String nameForTrackKind (TrackKind);

} // namespace b4nd
