#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "audio/AudioDeviceController.h"
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

    /** Per physical output peak level (0..1) from the engine, refreshed at 30 Hz. */
    float getOutputLevel (int output) const noexcept
    {
        return juce::isPositiveAndBelow (output, (int) outputLevels.size()) ? outputLevels[(size_t) output] : 0.0f;
    }
    /** Per track level: tracks are rendered from Phase 3. */
    float getTrackLevel (int /*track*/) const noexcept { return 0.0f; }

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

    AudioEngine& engine;
    AudioDeviceController& device;
    AudioDeviceController::Status lastDeviceStatus = AudioDeviceController::Status::NoDevice;
    std::array<float, AudioEngine::maxOutputs> outputLevels {};
    std::uint64_t dropoutsBeforeRestart = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AppState)
};

juce::Colour colourForTrackKind (TrackKind) noexcept;
juce::String nameForTrackKind (TrackKind);

} // namespace b4nd
