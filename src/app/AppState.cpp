#include "app/AppState.h"
#include "ui/Theme.h"

namespace b4nd
{

juce::Colour colourForTrackKind (TrackKind k) noexcept
{
    switch (k)
    {
        case TrackKind::Audio:        return theme::trackAudio;
        case TrackKind::MidiVsti:     return theme::trackVsti;
        case TrackKind::MidiExternal: return theme::trackExt;
        case TrackKind::Click:        return theme::trackClick;
        case TrackKind::Guide:        return theme::trackGuide;
    }
    return theme::trackAudio;
}

juce::String nameForTrackKind (TrackKind k)
{
    switch (k)
    {
        case TrackKind::Audio:        return "AUDIO";
        case TrackKind::MidiVsti:     return "VSTi";
        case TrackKind::MidiExternal: return "EXT MIDI";
        case TrackKind::Click:        return "CLICK";
        case TrackKind::Guide:        return "GUIDE";
    }
    return {};
}

static TrackInfo makeTrack (const juce::String& name, TrackKind kind, const juce::String& dest,
                            std::vector<juce::Range<double>> regions, float volDb = 0.0f, bool mute = false)
{
    TrackInfo t;
    t.name = name;
    t.kind = kind;
    t.destination = dest;
    t.regions = std::move (regions);
    t.volumeDb = volDb;
    t.mute = mute;
    return t;
}

std::vector<SongInfo> AppState::createDemoSetlist()
{
    std::vector<SongInfo> list;

    {   // Steady tempo rock song
        SongInfo s;
        s.title = "Demo 01 - Opening Drive";
        s.artist = "B4ND (demo data)";
        s.key = "E minor";
        s.tempo.setTempoPoints ({ { 0.0, 128.0 } });
        s.tempo.setTimeSigPoints ({ { 0, 4, 4 } });
        s.lengthSeconds = s.tempo.secondsAtBeat (s.tempo.beatAtBar (96));
        const double L = s.lengthSeconds;
        const auto bar = [&s] (int b) { return s.tempo.secondsAtBeat (s.tempo.beatAtBar (b)); };
        s.tracks = {
            makeTrack ("Original Mix", TrackKind::Audio, "Main L/R", { { 0.0, L } }, 0.0f, true),
            makeTrack ("Drums (stem)", TrackKind::Audio, "Monitor L/R", { { 0.0, L } }, 0.0f),
            makeTrack ("Bass (stem)", TrackKind::Audio, "Main L/R", { { 0.0, L } }, 0.0f),
            makeTrack ("Piano", TrackKind::MidiVsti, "Main L/R", { { bar (8), bar (40) }, { bar (56), bar (88) } }, -3.0f),
            makeTrack ("Strings", TrackKind::MidiVsti, "Main L/R", { { bar (24), bar (40) }, { bar (72), bar (92) } }, -6.0f),
            makeTrack ("Synth Pad", TrackKind::MidiExternal, "Module A ch3 / LINE IN 1-2", { { bar (0), bar (8) }, { bar (40), bar (56) } }, -4.0f),
            makeTrack ("Click", TrackKind::Click, "OUT 3", { { 0.0, L } }, -6.0f),
            makeTrack ("Guide", TrackKind::Guide, "OUT 4", { { 0.0, L } }, -3.0f),
        };
        s.cues = { { "Intro", 0.0 }, { "Verse 1", bar (8) }, { "Chorus 1", bar (24) }, { "Verse 2", bar (40) },
                   { "Chorus 2", bar (56) }, { "Solo", bar (72) }, { "Outro", bar (88) } };
        list.push_back (std::move (s));
    }

    {   // Tempo map demo
        SongInfo s;
        s.title = "Demo 02 - Ballad (tempo change)";
        s.artist = "B4ND (demo data)";
        s.key = "D major";
        s.tempo.setTempoPoints ({ { 0.0, 72.0 }, { 128.0, 76.0 } });
        s.tempo.setTimeSigPoints ({ { 0, 4, 4 } });
        s.lengthSeconds = s.tempo.secondsAtBeat (s.tempo.beatAtBar (64));
        const double L = s.lengthSeconds;
        const auto bar = [&s] (int b) { return s.tempo.secondsAtBeat (s.tempo.beatAtBar (b)); };
        s.tracks = {
            makeTrack ("Original Mix", TrackKind::Audio, "Main L/R", { { 0.0, L } }, 0.0f, true),
            makeTrack ("Electric Piano", TrackKind::MidiVsti, "Main L/R", { { 0.0, L } }, -2.0f),
            makeTrack ("Hammond Organ", TrackKind::MidiVsti, "Main L/R", { { bar (16), bar (48) } }, -8.0f),
            makeTrack ("Choir", TrackKind::MidiExternal, "Module A ch5 / LINE IN 1-2", { { bar (32), bar (60) } }, -6.0f),
            makeTrack ("Click", TrackKind::Click, "OUT 3", { { 0.0, L } }, -6.0f),
            makeTrack ("Guide", TrackKind::Guide, "OUT 4", { { 0.0, L } }, -3.0f),
        };
        s.cues = { { "Intro", 0.0 }, { "Verse", bar (4) }, { "Chorus", bar (20) }, { "Bridge", bar (32) },
                   { "Last Chorus", bar (44) }, { "Outro", bar (56) } };
        list.push_back (std::move (s));
    }

    {   // 6/8 shuffle
        SongInfo s;
        s.title = "Demo 03 - Shuffle in 6/8";
        s.artist = "B4ND (demo data)";
        s.key = "A minor";
        s.tempo.setTempoPoints ({ { 0.0, 110.0 } });
        s.tempo.setTimeSigPoints ({ { 0, 6, 8 } });
        s.lengthSeconds = s.tempo.secondsAtBeat (s.tempo.beatAtBar (120));
        const double L = s.lengthSeconds;
        const auto bar = [&s] (int b) { return s.tempo.secondsAtBeat (s.tempo.beatAtBar (b)); };
        s.tracks = {
            makeTrack ("Original Mix", TrackKind::Audio, "Main L/R", { { 0.0, L } }, 0.0f, true),
            makeTrack ("Brass", TrackKind::MidiVsti, "Main L/R", { { bar (16), bar (32) }, { bar (64), bar (80) } }, -4.0f),
            makeTrack ("Synth Lead", TrackKind::MidiVsti, "Main L/R", { { bar (80), bar (104) } }, -5.0f),
            makeTrack ("Click", TrackKind::Click, "OUT 3", { { 0.0, L } }, -6.0f),
        };
        s.cues = { { "Intro", 0.0 }, { "Verse", bar (8) }, { "Chorus", bar (32) }, { "Solo", bar (80) }, { "Outro", bar (104) } };
        list.push_back (std::move (s));
    }

    return list;
}

AppState::AppState (AudioEngine& e, AudioDeviceController& d)
    : songs (createDemoSetlist()), engine (e), device (d)
{
    engine.setRouting (routing);
    device.addChangeListener (this);
    startTimerHz (30);
}

AppState::~AppState()
{
    stopTimer();
    device.removeChangeListener (this);
}

bool AppState::isEngineActive() const noexcept
{
    return device.getStatus() == AudioDeviceController::Status::Running && engine.isPrepared();
}

void AppState::post (EngineCommand::Type type, std::int64_t samples)
{
    EngineCommand c;
    c.type = type;
    c.samples = samples;
    if (! engine.postCommand (c))
        juce::Logger::writeToLog ("Engine command queue full (command dropped)");
}

std::int64_t AppState::secondsToSamples (double seconds) const noexcept
{
    return (std::int64_t) std::llround (seconds * engine.getSampleRate());
}

const SongInfo* AppState::getCurrentSong() const noexcept
{
    return juce::isPositiveAndBelow (currentSong, (int) songs.size()) ? &songs[(size_t) currentSong] : nullptr;
}

SongInfo* AppState::currentSongMutable() noexcept
{
    return juce::isPositiveAndBelow (currentSong, (int) songs.size()) ? &songs[(size_t) currentSong] : nullptr;
}

const SongInfo* AppState::getNextSong() const noexcept
{
    const int n = currentSong + 1;
    return juce::isPositiveAndBelow (n, (int) songs.size()) ? &songs[(size_t) n] : nullptr;
}

double AppState::getSetlistTotalSeconds() const noexcept
{
    double t = 0.0;
    for (const auto& s : songs)
        t += s.lengthSeconds;
    return t;
}

double AppState::getRemainingSeconds() const noexcept
{
    if (auto* s = getCurrentSong())
        return juce::jmax (0.0, s->lengthSeconds - position);
    return 0.0;
}

void AppState::selectSong (int index)
{
    if (! juce::isPositiveAndBelow (index, (int) songs.size()) || index == currentSong)
        return;

    // Song change always stops cleanly first (Note Off / audio tail handled by the engine from Phase 4).
    stop();
    currentSong = index;
    position = 0.0;
    juce::Logger::writeToLog ("Song selected: " + songs[(size_t) index].title);
    sendChangeMessage();
}

void AppState::nextSong()     { selectSong (currentSong + 1); }
void AppState::previousSong() { selectSong (currentSong - 1); }

void AppState::setTransport (TransportState s)
{
    if (transport == s)
        return;
    transport = s;
    lastTickMs = juce::Time::getMillisecondCounterHiRes();
    sendChangeMessage();
}

void AppState::play()
{
    auto* song = getCurrentSong();
    if (song == nullptr)
        return;
    emergency = false;
    if (position >= song->lengthSeconds)
        position = 0.0;
    juce::Logger::writeToLog ("Transport: PLAY at " + juce::String (position, 3)
                              + (isEngineActive() ? "" : " (no audio device: virtual transport)"));
    if (isEngineActive())
    {
        post (EngineCommand::Type::SetSongLength, secondsToSamples (song->lengthSeconds));
        post (EngineCommand::Type::Locate, secondsToSamples (position));
        post (EngineCommand::Type::Play);
    }
    setTransport (TransportState::Playing);
}

void AppState::pause()
{
    if (transport == TransportState::Playing)
    {
        juce::Logger::writeToLog ("Transport: PAUSE at " + juce::String (position, 3));
        if (isEngineActive())
            post (EngineCommand::Type::Pause);
        setTransport (TransportState::Paused);
    }
}

void AppState::togglePlayPause()
{
    if (transport == TransportState::Playing) pause();
    else                                      play();
}

void AppState::stop()
{
    if (transport != TransportState::Stopped)
        juce::Logger::writeToLog ("Transport: STOP at " + juce::String (position, 3));
    if (isEngineActive())
        post (EngineCommand::Type::Stop);
    setTransport (TransportState::Stopped);
    position = 0.0;
    sendChangeMessage();
}

void AppState::restart()
{
    locate (0.0);
}

void AppState::emergencyStop()
{
    juce::Logger::writeToLog ("EMERGENCY STOP at " + juce::String (position, 3));
    // Always sent, even if the transport looks stopped: it also kills output tests.
    if (isEngineActive())
        post (EngineCommand::Type::EmergencyStop);
    emergency = true;
    transport = TransportState::Stopped;
    sendChangeMessage();
}

void AppState::locate (double seconds)
{
    if (auto* s = getCurrentSong())
    {
        position = juce::jlimit (0.0, s->lengthSeconds, seconds);
        if (isEngineActive())
            post (EngineCommand::Type::Locate, secondsToSamples (position));
        lastTickMs = juce::Time::getMillisecondCounterHiRes();
        sendChangeMessage();
    }
}

bool AppState::startOutputTest (TestSignal signal, int output)
{
    if (! isEngineActive() || transport == TransportState::Playing)
        return false;
    EngineCommand c;
    c.type = EngineCommand::Type::StartTest;
    c.intValue = (int) signal;
    c.output = output;
    c.value = -20.0f;
    juce::Logger::writeToLog ("Output test: mode " + juce::String ((int) signal) + " output " + juce::String (output + 1));
    emergency = false;
    return engine.postCommand (c);
}

void AppState::stopOutputTest()
{
    post (EngineCommand::Type::StopTest);
}

void AppState::toggleLoop()
{
    looping = ! looping;
    sendChangeMessage();
}

void AppState::jumpToCue (int cueIndex)
{
    if (auto* s = getCurrentSong())
        if (juce::isPositiveAndBelow (cueIndex, (int) s->cues.size()))
            locate (s->cues[(size_t) cueIndex].seconds);
}

void AppState::jumpToNextCue()
{
    if (auto* s = getCurrentSong())
        for (size_t i = 0; i < s->cues.size(); ++i)
            if (s->cues[i].seconds > position + 0.05)
            {
                locate (s->cues[i].seconds);
                return;
            }
}

void AppState::setTrackVolume (int trackIndex, float db)
{
    if (auto* s = currentSongMutable())
        if (juce::isPositiveAndBelow (trackIndex, (int) s->tracks.size()))
        {
            s->tracks[(size_t) trackIndex].volumeDb = db;
            sendChangeMessage();
        }
}

void AppState::setTrackMute (int trackIndex, bool m)
{
    if (auto* s = currentSongMutable())
        if (juce::isPositiveAndBelow (trackIndex, (int) s->tracks.size()))
        {
            s->tracks[(size_t) trackIndex].mute = m;
            sendChangeMessage();
        }
}

void AppState::setTrackSolo (int trackIndex, bool so)
{
    if (auto* s = currentSongMutable())
        if (juce::isPositiveAndBelow (trackIndex, (int) s->tracks.size()))
        {
            s->tracks[(size_t) trackIndex].solo = so;
            sendChangeMessage();
        }
}

void AppState::setLiveMode (bool shouldBeLive)
{
    if (liveMode == shouldBeLive)
        return;
    liveMode = shouldBeLive;
    routing.setLocked (shouldBeLive);   // no routing edits during a show
    device.setAutoReconnectAllowed (! shouldBeLive);   // on stage, reconnecting is a deliberate action
    juce::Desktop::setScreenSaverEnabled (! shouldBeLive);
    juce::Logger::writeToLog (shouldBeLive ? "Entered LIVE MODE" : "Left LIVE MODE");
    sendChangeMessage();
}

std::uint64_t AppState::getDropoutCount() const noexcept
{
    const auto driver = device.getDriverXrunCount();
    return engine.getLateCallbackCount() + engine.getOverloadCount() + (std::uint64_t) juce::jmax (0, driver) + dropoutsBeforeRestart;
}

void AppState::changeListenerCallback (juce::ChangeBroadcaster*)
{
    const auto st = device.getStatus();
    if (st != lastDeviceStatus)
    {
        if (lastDeviceStatus == AudioDeviceController::Status::Running)
        {
            // Device lost or closed: stop everything. The engine is no longer running.
            dropoutsBeforeRestart += engine.getLateCallbackCount() + engine.getOverloadCount();
            if (transport != TransportState::Stopped)
                juce::Logger::writeToLog ("Transport stopped: audio device not running");
            transport = TransportState::Stopped;
        }
        if (st == AudioDeviceController::Status::Running)
        {
            engine.setRouting (routing);
            transport = TransportState::Stopped;
        }
        lastDeviceStatus = st;
    }
    outputLevels.fill (0.0f);
    sendChangeMessage();
}

void AppState::timerCallback()
{
    if (isEngineActive())
    {
        for (int i = 0; i < (int) outputLevels.size(); ++i)
            outputLevels[(size_t) i] = engine.takeOutputPeak (i);

        EngineEvent ev;
        while (engine.popEvent (ev))
        {
            switch (ev.type)
            {
                case EngineEvent::Type::LateCallback:
                    juce::Logger::writeToLog ("AUDIO DROPOUT (late callback) at sample " + juce::String (ev.samplePosition));
                    break;
                case EngineEvent::Type::DspOverload:
                    juce::Logger::writeToLog ("AUDIO DSP OVERLOAD (load " + juce::String (ev.load * 100.0f, 0) + "%) at sample " + juce::String (ev.samplePosition));
                    break;
                case EngineEvent::Type::EmergencyComplete:
                    juce::Logger::writeToLog ("Emergency stop complete: outputs silent");
                    break;
                case EngineEvent::Type::CommandRejected:
                    juce::Logger::writeToLog ("Engine rejected a command (output test while playing)");
                    break;
                case EngineEvent::Type::SongEnded:
                    if (auto* s = getCurrentSong())
                        juce::Logger::writeToLog ("Song ended: " + s->title);
                    if (looping)
                    {
                        post (EngineCommand::Type::Locate, 0);
                        post (EngineCommand::Type::Play);
                    }
                    break;
            }
        }

        const auto t = engine.getTransport();
        const auto newState = t == EngineTransport::Playing ? TransportState::Playing
                            : t == EngineTransport::Paused  ? TransportState::Paused
                                                            : TransportState::Stopped;
        const double newPos = (double) engine.getPosition() / engine.getSampleRate();
        // Engine is the clock. Ignore the brief window before a posted PLAY is processed.
        const bool pendingPlay = transport == TransportState::Playing && newState == TransportState::Stopped
                                 && juce::Time::getMillisecondCounterHiRes() - lastTickMs < 250.0;
        if (! pendingPlay)
        {
            if (newState != TransportState::Stopped || transport != TransportState::Stopped)
                position = newPos;
            transport = newState;
        }
        if (engine.isEmergencyStopped())
            emergency = true;
        else if (transport == TransportState::Playing)
            emergency = false;
        sendChangeMessage();
        return;
    }

    if (transport != TransportState::Playing)
        return;

    // No audio device: virtual transport so the UI can still be rehearsed.
    const double now = juce::Time::getMillisecondCounterHiRes();
    const double dt = (now - lastTickMs) / 1000.0;
    lastTickMs = now;

    auto* s = getCurrentSong();
    if (s == nullptr)
        return;

    position += dt;
    if (position >= s->lengthSeconds)
    {
        if (looping)
        {
            position = 0.0;
        }
        else
        {
            position = s->lengthSeconds;
            transport = TransportState::Stopped;
            juce::Logger::writeToLog ("Song ended: " + s->title);
        }
    }
    sendChangeMessage();
}

} // namespace b4nd
