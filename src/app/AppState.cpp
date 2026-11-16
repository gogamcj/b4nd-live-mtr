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

static std::shared_ptr<const std::vector<float>> makeOverview (const AudioData& d, int buckets = 2048)
{
    auto v = std::make_shared<std::vector<float>> ((size_t) buckets, 0.0f);
    if (d.numFrames <= 0)
        return v;
    for (int c = 0; c < d.numChannels; ++c)
    {
        const float* x = d.channel (c);
        for (std::int64_t i = 0; i < d.numFrames; ++i)
        {
            auto& p = (*v)[(size_t) (i * buckets / d.numFrames)];
            p = std::max (p, std::abs (x[i]));
        }
    }
    return v;
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
    t.bus = dest.startsWithIgnoreCase ("Monitor") ? BusKind::Monitor
          : dest.startsWithIgnoreCase ("Extra")   ? BusKind::Extra
          : dest.startsWithIgnoreCase ("Aux")     ? BusKind::Aux
                                                  : BusKind::Main;
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
    requestAllMedia();
    rebuildPlan();
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
    trackLevels.fill (0.0f);
    juce::Logger::writeToLog ("Song selected: " + songs[(size_t) index].title);
    rebuildPlan();   // the engine swaps it in once the stop fade has finished
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
            engine.setTrackGain (trackIndex, juce::Decibels::decibelsToGain (db, -60.0f));
            sendChangeMessage();
        }
}

void AppState::setTrackMute (int trackIndex, bool m)
{
    if (auto* s = currentSongMutable())
        if (juce::isPositiveAndBelow (trackIndex, (int) s->tracks.size()))
        {
            s->tracks[(size_t) trackIndex].mute = m;
            engine.setTrackMute (trackIndex, m);
            sendChangeMessage();
        }
}

void AppState::setTrackSolo (int trackIndex, bool so)
{
    if (auto* s = currentSongMutable())
        if (juce::isPositiveAndBelow (trackIndex, (int) s->tracks.size()))
        {
            s->tracks[(size_t) trackIndex].solo = so;
            engine.setTrackSolo (trackIndex, so);
            sendChangeMessage();
        }
}

static juce::String formatOutputs (std::vector<int> outs)
{
    std::sort (outs.begin(), outs.end());
    outs.erase (std::unique (outs.begin(), outs.end()), outs.end());
    juce::StringArray parts;
    for (size_t i = 0; i < outs.size();)
    {
        size_t j = i;
        while (j + 1 < outs.size() && outs[j + 1] == outs[j] + 1) ++j;
        parts.add (j > i ? juce::String (outs[i] + 1) + "-" + juce::String (outs[j] + 1) : juce::String (outs[i] + 1));
        i = j + 1;
    }
    return parts.joinIntoString (",");
}

juce::String AppState::getTrackDestination (int trackIndex) const
{
    const auto* s = getCurrentSong();
    if (s == nullptr || ! juce::isPositiveAndBelow (trackIndex, (int) s->tracks.size()))
        return {};
    const auto& t = s->tracks[(size_t) trackIndex];
    if (t.kind == TrackKind::MidiVsti || t.kind == TrackKind::MidiExternal)
        return t.destination;

    // Click and guide are also mixed into the in-ear monitor bus by the engine.
    const BusKind bus = t.kind == TrackKind::Click ? BusKind::Click : t.kind == TrackKind::Guide ? BusKind::Guide : t.bus;
    const bool alsoInEar = bus == BusKind::Click || bus == BusKind::Guide;
    std::vector<int> direct, inEar;
    for (int o = 0; o < routing.getNumOutputs(); ++o)
    {
        const auto& src = routing.getOutput (o).source;
        if (! src) continue;
        if (src->kind == bus)                                   direct.push_back (o);
        else if (alsoInEar && src->kind == BusKind::Monitor)    inEar.push_back (o);
    }
    juce::String r;
    if (! direct.empty())
        r = "OUT " + formatOutputs (direct);
    if (! inEar.empty())
        r += (r.isEmpty() ? "" : " + ") + juce::String ("IN-EAR ") + formatOutputs (inEar);
    return r.isEmpty() ? juce::String ("NOT ROUTED") : r;
}

void AppState::setMasterVolume (float db)
{
    masterDb = juce::jlimit (-60.0f, 6.0f, db);
    engine.setMasterGain (juce::Decibels::decibelsToGain (masterDb, -60.0f));
    sendChangeMessage();
}

void AppState::setTrackPan (int trackIndex, float pan)
{
    if (auto* s = currentSongMutable())
        if (juce::isPositiveAndBelow (trackIndex, (int) s->tracks.size()))
        {
            s->tracks[(size_t) trackIndex].pan = juce::jlimit (-1.0f, 1.0f, pan);
            engine.setTrackPan (trackIndex, s->tracks[(size_t) trackIndex].pan);
            sendChangeMessage();
        }
}

//==============================================================================
double AppState::mediaSampleRate() const noexcept
{
    // Decode at the running device rate. Without a device, 48 kHz (the usual live rate);
    // everything is re-decoded if the device later opens at another rate.
    return isEngineActive() ? engine.getSampleRate() : 48000.0;
}

void AppState::requestAllMedia()
{
    const double sr = mediaSampleRate();
    for (auto& song : songs)
        for (auto& t : song.tracks)
        {
            if (t.mediaPath.isEmpty())
                continue;
            if (t.media != nullptr && std::abs (t.mediaRate - sr) < 0.5)
                continue;
            const juce::File f (t.mediaPath);
            if (auto data = mediaPool.request (f, sr))
            {
                t.media = std::move (data);
                t.mediaRate = sr;
                t.mediaStatus = TrackInfo::MediaStatus::Ready;
            }
            else
            {
                t.mediaStatus = f.existsAsFile() ? TrackInfo::MediaStatus::Loading : TrackInfo::MediaStatus::Missing;
                if (t.mediaStatus == TrackInfo::MediaStatus::Missing)
                {
                    t.mediaError = "File not found: " + t.mediaPath;
                    juce::Logger::writeToLog ("MEDIA MISSING (" + song.title + " / " + t.name + "): " + t.mediaPath);
                }
            }
        }
}

void AppState::applyFinishedMedia()
{
    auto finished = mediaPool.takeFinished();
    if (finished.empty())
        return;

    const double sr = mediaSampleRate();
    bool currentChanged = false;
    for (auto& f : finished)
    {
        if (std::abs (f.sampleRate - sr) >= 0.5)
            continue;   // decoded for a previous device rate: a new request is already queued
        for (size_t si = 0; si < songs.size(); ++si)
            for (auto& t : songs[si].tracks)
            {
                if (t.mediaPath != f.path
                    || (t.mediaStatus == TrackInfo::MediaStatus::Ready && std::abs (t.mediaRate - sr) < 0.5))
                    continue;
                if (f.result.data != nullptr)
                {
                    t.media = f.result.data;
                    t.mediaRate = f.sampleRate;
                    t.mediaStatus = TrackInfo::MediaStatus::Ready;
                    t.mediaError.clear();
                    if (t.overview == nullptr)
                        t.overview = makeOverview (*t.media);
                    const double len = t.mediaStartSeconds + (double) t.media->numFrames / f.sampleRate;
                    t.regions = { { t.mediaStartSeconds, len } };
                    if (len > songs[si].lengthSeconds)
                        songs[si].lengthSeconds = len;
                }
                else
                {
                    t.mediaStatus = f.result.missing ? TrackInfo::MediaStatus::Missing : TrackInfo::MediaStatus::Error;
                    t.mediaError = f.result.error;
                }
                if ((int) si == currentSong)
                    currentChanged = true;
            }
    }

    const auto summary = getMediaSummary();
    if (summary.ramBytes > ramWarningBytes && ! ramWarningLogged)
    {
        ramWarningLogged = true;
        juce::Logger::writeToLog ("WARNING: media uses " + juce::String ((double) summary.ramBytes / (1024.0 * 1024.0 * 1024.0), 2, false)
                                  + " GB of RAM. Consider fewer stems or a lower sample rate.");
    }

    if (currentChanged)
        rebuildPlan();
    sendChangeMessage();
}

juce::String AppState::addAudioFile (const juce::File& file, int trackIndex)
{
    if (liveMode)
        return "Live Mode: media cannot be changed during a show";
    if (! file.existsAsFile())
        return "File not found: " + file.getFullPathName();
    if (! mediaPool.canRead (file))
        return "Unsupported audio format: " + file.getFileName();
    auto* s = currentSongMutable();
    if (s == nullptr)
        return "No song selected";

    TrackInfo* t = nullptr;
    if (juce::isPositiveAndBelow (trackIndex, (int) s->tracks.size())
        && (s->tracks[(size_t) trackIndex].kind == TrackKind::Audio || s->tracks[(size_t) trackIndex].kind == TrackKind::Guide))
    {
        t = &s->tracks[(size_t) trackIndex];
    }
    else
    {
        if ((int) s->tracks.size() >= AudioEngine::maxTracks)
            return "Too many tracks in this song";
        TrackInfo nt;
        nt.name = file.getFileNameWithoutExtension();
        nt.kind = TrackKind::Audio;
        nt.destination = "Main L/R";
        nt.bus = BusKind::Main;
        s->tracks.push_back (nt);
        t = &s->tracks.back();
        pushMixerToEngine();   // new slot starts at its stored volume
    }

    t->mediaPath = file.getFullPathName();
    t->media.reset();
    t->overview.reset();
    t->mediaRate = 0.0;
    t->mediaError.clear();
    t->mediaStatus = TrackInfo::MediaStatus::Loading;
    juce::Logger::writeToLog ("Media assigned: " + t->name + " <- " + t->mediaPath);
    requestAllMedia();
    rebuildPlan();
    sendChangeMessage();
    return {};
}

AppState::MediaSummary AppState::getMediaSummary() const
{
    MediaSummary m;
    for (const auto& song : songs)
        for (const auto& t : song.tracks)
            switch (t.mediaStatus)
            {
                case TrackInfo::MediaStatus::Loading: ++m.loading; break;
                case TrackInfo::MediaStatus::Missing: ++m.missing; break;
                case TrackInfo::MediaStatus::Error:   ++m.errors;  break;
                case TrackInfo::MediaStatus::Ready:   ++m.ready;   break;
                case TrackInfo::MediaStatus::None:    break;
            }
    m.ramBytes = mediaPool.getCachedBytes();
    return m;
}

void AppState::pushMixerToEngine()
{
    auto* s = getCurrentSong();
    for (int i = 0; i < AudioEngine::maxTracks; ++i)
    {
        const TrackInfo* t = (s != nullptr && i < (int) s->tracks.size()) ? &s->tracks[(size_t) i] : nullptr;
        engine.setTrackGain (i, t != nullptr ? juce::Decibels::decibelsToGain (t->volumeDb, -60.0f) : 1.0f);
        engine.setTrackPan (i, t != nullptr ? t->pan : 0.0f);
        engine.setTrackMute (i, t != nullptr && t->mute);
        engine.setTrackSolo (i, t != nullptr && t->solo);
    }
}

void AppState::rebuildPlan()
{
    auto* song = getCurrentSong();
    if (song == nullptr)
        return;

    const double sr = mediaSampleRate();
    auto plan = std::make_unique<PlaybackPlan>();
    plan->lengthSamples = (std::int64_t) std::llround (song->lengthSeconds * sr);
    plan->tempo = song->tempo;

    for (int i = 0; i < (int) song->tracks.size() && i < AudioEngine::maxTracks; ++i)
    {
        const auto& t = song->tracks[(size_t) i];
        if (t.kind == TrackKind::Click)
        {
            if (! plan->hasClick)
            {
                plan->hasClick = true;
                plan->clickSlot = i;
                plan->click = PlaybackPlan::makeDefaultClick (sr);
            }
            continue;
        }
        if ((t.kind != TrackKind::Audio && t.kind != TrackKind::Guide) || t.media == nullptr
            || std::abs (t.mediaRate - sr) >= 0.5)
            continue;

        TrackRuntime rt;
        rt.slot = i;
        rt.role = t.kind == TrackKind::Guide ? TrackRole::Guide : TrackRole::Audio;
        rt.bus = t.bus;
        rt.clips.push_back ({ t.media, (std::int64_t) std::llround (t.mediaStartSeconds * sr) });
        plan->tracks.push_back (std::move (rt));
    }

    pushMixerToEngine();
    engine.collectGarbage();
    engine.setPlan (std::move (plan), sr);
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
            const int outs = device.getOutputChannelNames().size();
            if (outs != lastDeviceOutputs)
            {
                // New device layout: 2-3 outputs get the split preset (OUT1 PA mono, OUT2 in-ear).
                const bool wasLocked = routing.isLocked();
                routing = OutputRouting::createForDevice (outs);
                routing.setLocked (wasLocked);
                lastDeviceOutputs = outs;
                juce::Logger::writeToLog ("Output routing preset for " + juce::String (outs) + " outputs"
                                          + (outs < 4 ? " (2-out split: OUT1 Main mono to PA, OUT2 click/guide in-ear)" : ""));
            }
            engine.setRouting (routing);
            transport = TransportState::Stopped;

            const double sr = engine.getSampleRate();
            if (std::abs (sr - lastDeviceRate) >= 0.5)
            {
                lastDeviceRate = sr;
                requestAllMedia();   // re-decode at the new rate in the background
                mediaPool.purgeUnused();
            }
            rebuildPlan();
        }
        lastDeviceStatus = st;
    }
    outputLevels.fill (0.0f);
    sendChangeMessage();
}

void AppState::timerCallback()
{
    applyFinishedMedia();
    engine.collectGarbage();

    if (isEngineActive())
    {
        for (int i = 0; i < (int) outputLevels.size(); ++i)
            outputLevels[(size_t) i] = engine.takeOutputPeak (i);
        for (int i = 0; i < (int) trackLevels.size(); ++i)
            trackLevels[(size_t) i] = engine.takeTrackPeak (i);

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
