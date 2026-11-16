#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "core/OutputRouting.h"
#include "core/TempoMap.h"

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
    juce::String name;
    TrackKind kind = TrackKind::Audio;
    juce::String destination;          // "Main L/R", "OUT 3", plugin or module name
    float volumeDb = 0.0f;
    float pan = 0.0f;
    bool mute = false;
    bool solo = false;
    std::vector<juce::Range<double>> regions;   // seconds
};

struct CuePoint
{
    juce::String name;
    double seconds = 0.0;
};

struct SongInfo
{
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
    Phase 1 UI-side application model.

    The transport here is *virtual* (advanced by a UI timer) so the layout and
    controls can be exercised before the audio engine exists. From Phase 2 the
    same commands are forwarded to the audio engine's command queue and the
    position comes from engine telemetry instead (docs/01, docs/03).
*/
class AppState : public juce::ChangeBroadcaster,
                 private juce::Timer
{
public:
    AppState();
    ~AppState() override;

    static std::vector<SongInfo> createDemoSetlist();

    // --- project / setlist ---
    const juce::String& getProjectName() const noexcept { return projectName; }
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

    // --- mixer (UI model only in Phase 1) ---
    void setTrackVolume (int trackIndex, float db);
    void setTrackMute (int trackIndex, bool m);
    void setTrackSolo (int trackIndex, bool s);

    // --- modes ---
    bool isLiveMode() const noexcept { return liveMode; }
    void setLiveMode (bool shouldBeLive);

    const OutputRouting& getRouting() const noexcept { return routing; }

    /** Per physical output peak level (0..1). Zero until the audio engine exists. */
    float getOutputLevel (int /*output*/) const noexcept { return 0.0f; }
    float getTrackLevel (int /*track*/) const noexcept { return 0.0f; }

private:
    void timerCallback() override;
    SongInfo* currentSongMutable() noexcept;
    void setTransport (TransportState s);

    juce::String projectName { "B4ND Demo Project" };
    std::vector<SongInfo> songs;
    int currentSong = 0;

    TransportState transport = TransportState::Stopped;
    double position = 0.0;
    double lastTickMs = 0.0;
    bool looping = false;
    bool emergency = false;
    bool liveMode = false;

    OutputRouting routing = OutputRouting::createDefault (8);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AppState)
};

juce::Colour colourForTrackKind (TrackKind) noexcept;
juce::String nameForTrackKind (TrackKind);

} // namespace b4nd
