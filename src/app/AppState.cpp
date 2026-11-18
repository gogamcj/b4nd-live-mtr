#include "app/AppState.h"

#include <map>
#include "midi/MidiFileImport.h"
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
    t.id = newUuid();
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

    for (auto& s : list)
        s.id = newUuid();
    return list;
}

AppState::AppState (AudioEngine& e, AudioDeviceController& d)
    : songs (createDemoSetlist()), engine (e), device (d), midi (&e, createJuceMidiBackend())
{
    engine.setRouting (routing);
    midi.onStatusChanged = [this] { sendChangeMessage(); };
    midi.start();
    device.addChangeListener (this);
    requestAllMedia();
    rebuildPlan();
    startTimerHz (30);
}

AppState::~AppState()
{
    stopTimer();
    closeProject();
    midi.onStatusChanged = nullptr;
    midi.shutdown();   // releases every sounding note first
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
    midi.panic();   // external modules: Note Off + pedal up + All Notes Off, now
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
            if (s->tracks[(size_t) trackIndex].kind == TrackKind::MidiExternal)
                sendTrackVolumeToModule (s->tracks[(size_t) trackIndex]);
            markDirty();
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
            markDirty();
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
            markDirty();
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
    if (t.kind == TrackKind::MidiExternal)
    {
        const int m = findModule (t.moduleId);
        if (m < 0)
            return "NO MODULE";
        const auto& mp = modules[(size_t) m];
        const int ch = t.midiChannel > 0 ? t.midiChannel : mp.channel;
        return juce::String::fromUTF8 (mp.deviceName.c_str()) + " ch" + juce::String (ch) + "  ["
               + MidiOutputService::statusText (midi.getStatus (m)) + "]";
    }
    if (t.kind == TrackKind::MidiVsti)
    {
        if (t.pluginUid.isEmpty())
            return t.destination.isNotEmpty() ? t.destination : juce::String ("NO PLUGIN");
        std::vector<int> outs;
        for (int o = 0; o < routing.getNumOutputs(); ++o)
            if (const auto& src = routing.getOutput (o).source; src && src->kind == t.bus)
                outs.push_back (o);
        return t.pluginName + " -> " + (outs.empty() ? juce::String ("NOT ROUTED") : "OUT " + formatOutputs (outs))
               + "  [" + pluginStatusText (t) + "]";
    }

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
            markDirty();
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
        nt.id = newUuid();
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
    markDirty();
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

//==============================================================================
// External MIDI modules
int AppState::findModule (const juce::String& id) const
{
    if (id.isEmpty())
        return -1;
    for (size_t i = 0; i < modules.size(); ++i)
        if (juce::String::fromUTF8 (modules[i].id.c_str()) == id)
            return (int) i;
    return -1;
}

juce::String AppState::guardModuleChange() const
{
    if (liveMode)
        return "Live Mode: modules cannot be changed during a show";
    if (transport == TransportState::Playing)
        return "Stop playback first";
    return {};
}

static ModuleProfile sanitise (ModuleProfile p)
{
    p.channel = juce::jlimit (1, 16, p.channel);
    auto opt = [] (int v) { return v < 0 ? -1 : juce::jlimit (0, 127, v); };
    p.bankMsb = opt (p.bankMsb);
    p.bankLsb = opt (p.bankLsb);
    p.program = opt (p.program);
    p.volume = opt (p.volume);
    p.pan = opt (p.pan);
    p.volumeCc = juce::jlimit (0, 127, p.volumeCc);
    p.panCc = juce::jlimit (0, 127, p.panCc);
    p.delayOffsetMs = juce::jlimit (0.0, 200.0, p.delayOffsetMs);
    p.gapMs = juce::jlimit (0, 500, p.gapMs);
    p.settleMs = juce::jlimit (0, 5000, p.settleMs);
    if (p.deviceName.empty())
        p.deviceName = p.portName.empty() ? "Module" : p.portName;
    return p;
}

juce::String AppState::addModule (ModuleProfile p)
{
    if (auto g = guardModuleChange(); g.isNotEmpty())
        return g;
    if ((int) modules.size() >= MidiOutputService::maxSlots)
        return "At most " + juce::String (MidiOutputService::maxSlots) + " modules";
    if (p.id.empty())
        p.id = newUuid();
    modules.push_back (sanitise (std::move (p)));
    midi.setModules (modules);
    juce::Logger::writeToLog ("MIDI module added: " + juce::String::fromUTF8 (modules.back().deviceName.c_str()));
    markDirty();
    rebuildPlan();
    sendChangeMessage();
    return {};
}

juce::String AppState::updateModule (int index, const ModuleProfile& p)
{
    if (auto g = guardModuleChange(); g.isNotEmpty())
        return g;
    if (! juce::isPositiveAndBelow (index, (int) modules.size()))
        return "No such module";
    auto np = sanitise (p);
    np.id = modules[(size_t) index].id;
    modules[(size_t) index] = np;
    midi.setModules (modules);   // reopens on a port change, sends the preset again on a preset change
    markDirty();
    rebuildPlan();               // channel or delay may have changed
    sendChangeMessage();
    return {};
}

juce::String AppState::removeModule (int index)
{
    if (auto g = guardModuleChange(); g.isNotEmpty())
        return g;
    if (! juce::isPositiveAndBelow (index, (int) modules.size()))
        return "No such module";
    const auto id = juce::String::fromUTF8 (modules[(size_t) index].id.c_str());
    midi.panic();
    modules.erase (modules.begin() + index);
    for (auto& song : songs)
        for (auto& t : song.tracks)
            if (t.moduleId == id)
                t.moduleId.clear();
    midi.setModules (modules);
    markDirty();
    rebuildPlan();
    sendChangeMessage();
    return {};
}

void AppState::sendTrackVolumeToModule (const TrackInfo& t)
{
    // GM volume curve: dB = 40 log10 (cc / 127), relative to the module's stored volume.
    const int m = findModule (t.moduleId);
    if (m < 0 || modules[(size_t) m].volume < 0)
        return;
    const auto& mp = modules[(size_t) m];
    const int ch = juce::jlimit (1, 16, t.midiChannel > 0 ? t.midiChannel : mp.channel) - 1;
    const double scale = t.volumeDb <= -59.9f ? 0.0 : std::pow (10.0, (double) t.volumeDb / 40.0);
    const int value = juce::jlimit (0, 127, juce::roundToInt (mp.volume * scale));
    midi.sendLater (m, MidiBytes::make ((std::uint8_t) (0xb0 | ch), (std::uint8_t) mp.volumeCc, (std::uint8_t) value), 0.0);
}

juce::String AppState::setTrackModule (int trackIndex, const juce::String& moduleId, int channel)
{
    if (auto g = guardModuleChange(); g.isNotEmpty())
        return g;
    auto* s = currentSongMutable();
    if (s == nullptr || ! juce::isPositiveAndBelow (trackIndex, (int) s->tracks.size()))
        return "No such track";
    auto& t = s->tracks[(size_t) trackIndex];
    if (t.kind != TrackKind::MidiExternal)
        return "Not an external MIDI track";
    if (moduleId.isNotEmpty() && findModule (moduleId) < 0)
        return "No such module";
    midi.panic();   // notes started on the old module must not hang
    t.moduleId = moduleId;
    t.midiChannel = juce::jlimit (0, 16, channel);
    markDirty();
    rebuildPlan();
    sendChangeMessage();
    return {};
}

void AppState::loadMidiFile (TrackInfo& t, const SongInfo& song)
{
    t.midiEvents.reset();
    t.midiNotes = 0;
    t.midiError.clear();
    if (t.midiPath.isEmpty())
    {
        t.midiStatus = TrackInfo::MidiStatus::None;
        return;
    }
    const juce::File f (t.midiPath);
    if (! f.existsAsFile())
    {
        t.midiStatus = TrackInfo::MidiStatus::Missing;
        juce::Logger::writeToLog ("MIDI FILE MISSING: " + t.midiPath);
        return;
    }
    auto r = importMidiFile (f);
    if (! r.ok)
    {
        t.midiStatus = TrackInfo::MidiStatus::Error;
        t.midiError = r.error;
        juce::Logger::writeToLog ("MIDI file error (" + t.midiPath + "): " + r.error);
        return;
    }
    t.midiNotes = r.noteCount;
    const double first = song.tempo.secondsAtBeat ((double) r.events.front().tick / midiPpq);
    const double last = song.tempo.secondsAtBeat ((double) r.lastTick / midiPpq);
    t.regions = { { first, juce::jmax (first + 0.1, last) } };
    t.midiEvents = std::make_shared<const std::vector<MidiTickEvent>> (std::move (r.events));
    t.midiStatus = TrackInfo::MidiStatus::Ready;
    if (r.fileBpm > 0.0 && std::abs (r.fileBpm - song.tempo.bpmAtBeat (0.0)) > 0.5)
        juce::Logger::writeToLog ("Note: " + f.getFileName() + " was written at " + juce::String (r.fileBpm, 1)
                                  + " BPM; it plays on the song's tempo map (" + juce::String (song.tempo.bpmAtBeat (0.0), 1) + " BPM)");
}

juce::String AppState::addMidiFile (const juce::File& file, int trackIndex)
{
    if (auto g = guardModuleChange(); g.isNotEmpty())
        return g;
    if (! file.existsAsFile())
        return "File not found: " + file.getFullPathName();
    auto* s = currentSongMutable();
    if (s == nullptr)
        return "No song selected";
    const auto check = importMidiFile (file);
    if (! check.ok)
        return file.getFileName() + ": " + check.error;

    TrackInfo* t = nullptr;
    if (juce::isPositiveAndBelow (trackIndex, (int) s->tracks.size())
        && (s->tracks[(size_t) trackIndex].kind == TrackKind::MidiExternal || s->tracks[(size_t) trackIndex].kind == TrackKind::MidiVsti))
    {
        t = &s->tracks[(size_t) trackIndex];
    }
    else
    {
        if ((int) s->tracks.size() >= AudioEngine::maxTracks)
            return "Too many tracks in this song";
        TrackInfo nt;
        nt.id = newUuid();
        nt.name = file.getFileNameWithoutExtension();
        nt.kind = TrackKind::MidiExternal;
        if (! modules.empty())
            nt.moduleId = juce::String::fromUTF8 (modules.front().id.c_str());
        s->tracks.push_back (nt);
        t = &s->tracks.back();
        pushMixerToEngine();
    }
    t->midiPath = file.getFullPathName();
    loadMidiFile (*t, *s);
    juce::Logger::writeToLog ("MIDI file assigned: " + t->name + " <- " + t->midiPath + " (" + juce::String (t->midiNotes) + " notes)");
    markDirty();
    rebuildPlan();
    sendChangeMessage();
    return {};
}

AppState::MidiSummary AppState::getMidiSummary() const
{
    MidiSummary m;
    m.modules = (int) modules.size();
    for (int i = 0; i < m.modules; ++i)
    {
        const auto st = midi.getStatus (i);
        if (st == MidiOutputService::PortStatus::Ready)          ++m.ready;
        else if (st == MidiOutputService::PortStatus::Preparing) ++m.preparing;
        else                                                     ++m.problems;   // no port / disconnected / busy
    }
    if (const auto* s = getCurrentSong())
        for (const auto& t : s->tracks)
            if (t.kind == TrackKind::MidiExternal)
            {
                if (t.midiEvents != nullptr && findModule (t.moduleId) < 0) ++m.tracksWithoutModule;
                if (t.midiStatus == TrackInfo::MidiStatus::Missing) ++m.missingFiles;
            }
    return m;
}

//==============================================================================
// VST3 instruments (docs/05)

double AppState::pluginSampleRate() const
{
    return mediaSampleRate();   // the running device rate, else 48 kHz
}

int AppState::pluginBlockSize() const
{
    // Some drivers deliver blocks larger than the size they report: leave headroom.
    return juce::jmax (1024, device.getBufferSize());
}

juce::String AppState::pluginStatusText (const TrackInfo& t)
{
    using S = TrackInfo::PluginStatus;
    switch (t.pluginStatus)
    {
        case S::None:        return t.pluginUid.isEmpty() ? "NO PLUGIN" : "NOT LOADED";
        case S::Ready:       return t.pluginCertified ? "READY" : "READY - NOT CERTIFIED";
        case S::Missing:     return "PLUGIN MISSING";
        case S::NotScanned:  return "NOT SCANNED";
        case S::Blacklisted: return "BLACKLISTED";
        case S::LoadError:   return "LOAD ERROR";
        case S::Failed:      return "PLUGIN FAILED";
    }
    return {};
}

void AppState::loadInstrument (TrackInfo& t)
{
    if (t.instrument != nullptr)
        plugins.closeEditor (t.instrument.get());
    t.instrument.reset();
    t.pluginError.clear();
    t.pluginCertified = false;
    if (t.pluginUid.isEmpty())
    {
        t.pluginStatus = TrackInfo::PluginStatus::None;
        return;
    }
    const auto* rec = plugins.resolve (t.pluginUid, t.pluginDescXml);
    if (rec == nullptr)
    {
        // Not in the catalog: either not installed here, or installed but never scanned.
        // Never load an unscanned plugin into the app (it could crash it): the scan comes first.
        juce::PluginDescription d;
        if (auto xml = juce::parseXML (t.pluginDescXml))
            d.loadFromXml (*xml);
        const bool fileThere = d.fileOrIdentifier.isNotEmpty() && juce::File (d.fileOrIdentifier).exists();
        t.pluginStatus = fileThere ? TrackInfo::PluginStatus::NotScanned : TrackInfo::PluginStatus::Missing;
        t.pluginError = fileThere ? "scan the plugin folder first (Plugins window)" : "not installed on this computer";
        juce::Logger::writeToLog ("PLUGIN " + juce::String (fileThere ? "NOT SCANNED" : "MISSING") + ": " + t.pluginName
                                  + " (" + t.pluginVendor + ") on track " + t.name + " - its saved settings are kept");
        return;
    }
    if (! juce::File (rec->file).exists())
    {
        t.pluginStatus = TrackInfo::PluginStatus::Missing;
        t.pluginError = "not installed on this computer (" + rec->file + ")";
        juce::Logger::writeToLog ("PLUGIN MISSING: " + t.pluginName + " on track " + t.name + " - its saved settings are kept");
        return;
    }
    if (rec->status == PluginRecord::Status::Blacklisted)
    {
        t.pluginStatus = TrackInfo::PluginStatus::Blacklisted;
        t.pluginError = rec->reason;
        return;
    }
    auto r = plugins.load (*rec, pluginSampleRate(), pluginBlockSize());
    if (r.instrument == nullptr)
    {
        t.pluginStatus = TrackInfo::PluginStatus::LoadError;
        t.pluginError = r.error;
        juce::Logger::writeToLog ("PLUGIN LOAD ERROR: " + t.pluginName + " on track " + t.name + ": " + r.error);
        return;
    }
    if (rec->uid() != t.pluginUid)
        juce::Logger::writeToLog ("Plugin " + t.pluginName + " found as " + rec->desc.name + " " + rec->desc.version + " (another version or computer)");
    if (! t.pluginState.isEmpty() && ! r.instrument->setState (t.pluginState))
        juce::Logger::writeToLog ("WARNING: " + t.pluginName + " did not accept its saved settings (track " + t.name + ")");
    r.instrument->takeChanged();   // restoring the state is not a change
    t.pluginUid = rec->uid();
    t.pluginName = rec->desc.name;
    t.pluginVendor = rec->desc.manufacturerName;
    if (auto xml = rec->desc.createXml())
        t.pluginDescXml = xml->toString (juce::XmlElement::TextFormat().singleLine().withoutHeader());
    t.instrument = std::move (r.instrument);
    t.pluginStatus = TrackInfo::PluginStatus::Ready;
    t.pluginCertified = rec->status == PluginRecord::Status::Certified;
    if (! t.pluginCertified)
        juce::Logger::writeToLog ("Note: " + t.pluginName + " is not Live Certified (" + rec->reason + ")");
}

void AppState::loadAllInstruments()
{
    int loaded = 0, problems = 0;
    for (auto& s : songs)
        for (auto& t : s.tracks)
            if (t.kind == TrackKind::MidiVsti && t.pluginUid.isNotEmpty())
            {
                loadInstrument (t);
                (t.pluginStatus == TrackInfo::PluginStatus::Ready ? loaded : problems)++;
            }
    if (loaded + problems > 0)
        juce::Logger::writeToLog ("Plugins: " + juce::String (loaded) + " loaded, " + juce::String (problems) + " not available");
}

void AppState::prepareInstruments()
{
    const double sr = pluginSampleRate();
    const int block = pluginBlockSize();
    for (auto& s : songs)
        for (auto& t : s.tracks)
            if (t.instrument != nullptr && ! t.instrument->isPreparedFor (sr, block))
            {
                juce::String error;
                if (! t.instrument->prepare (sr, block, error))
                {
                    t.pluginStatus = TrackInfo::PluginStatus::LoadError;
                    t.pluginError = "prepare failed at " + juce::String (sr, 0) + " Hz: " + error;
                    juce::Logger::writeToLog ("PLUGIN ERROR: " + t.pluginName + ": " + t.pluginError);
                }
            }
}

void AppState::captureInstrumentStates()
{
    for (auto& s : songs)
        for (auto& t : s.tracks)
            if (t.instrument != nullptr && ! t.instrument->hasFailed())
            {
                auto st = t.instrument->getState();
                if (! st.isEmpty())
                    t.pluginState = std::move (st);
            }
}

juce::String AppState::setTrackPlugin (int trackIndex, const juce::String& pluginUid)
{
    if (auto g = guardModuleChange(); g.isNotEmpty())
        return g;
    auto* s = currentSongMutable();
    if (s == nullptr)
        return "No song selected";
    const auto* rec = plugins.getCatalog().findByUid (pluginUid);
    if (rec == nullptr)
        return "Plugin not in the scanned list";
    TrackInfo* t = nullptr;
    if (juce::isPositiveAndBelow (trackIndex, (int) s->tracks.size()) && s->tracks[(size_t) trackIndex].kind == TrackKind::MidiVsti)
    {
        t = &s->tracks[(size_t) trackIndex];
    }
    else
    {
        if ((int) s->tracks.size() >= AudioEngine::maxTracks)
            return "Too many tracks in this song";
        TrackInfo nt;
        nt.id = newUuid();
        nt.name = rec->desc.name;
        nt.kind = TrackKind::MidiVsti;
        s->tracks.push_back (nt);
        t = &s->tracks.back();
        pushMixerToEngine();
    }
    if (t->pluginSlotId.isEmpty())
        t->pluginSlotId = newUuid();
    t->pluginUid = rec->uid();
    t->pluginName = rec->desc.name;
    t->pluginVendor = rec->desc.manufacturerName;
    t->pluginDescXml.clear();
    t->pluginState.reset();   // a new plugin starts from its defaults
    loadInstrument (*t);
    juce::Logger::writeToLog ("Plugin assigned: " + t->name + " <- " + t->pluginName + " [" + pluginStatusText (*t) + "]");
    markDirty();
    rebuildPlan();
    sendChangeMessage();
    return t->pluginStatus == TrackInfo::PluginStatus::Ready ? juce::String() : t->pluginName + ": " + t->pluginError;
}

juce::String AppState::clearTrackPlugin (int trackIndex)
{
    if (auto g = guardModuleChange(); g.isNotEmpty())
        return g;
    auto* s = currentSongMutable();
    if (s == nullptr || ! juce::isPositiveAndBelow (trackIndex, (int) s->tracks.size()))
        return "No such track";
    auto& t = s->tracks[(size_t) trackIndex];
    if (t.instrument != nullptr)
        plugins.closeEditor (t.instrument.get());
    t.instrument.reset();
    t.pluginUid.clear();
    t.pluginName.clear();
    t.pluginVendor.clear();
    t.pluginDescXml.clear();
    t.pluginState.reset();
    t.pluginStatus = TrackInfo::PluginStatus::None;
    t.pluginError.clear();
    markDirty();
    rebuildPlan();
    sendChangeMessage();
    return {};
}

juce::String AppState::showPluginEditor (int trackIndex)
{
    if (liveMode)
        return "Live Mode: plugin editors are closed during a show";
    auto* s = currentSongMutable();
    if (s == nullptr || ! juce::isPositiveAndBelow (trackIndex, (int) s->tracks.size()))
        return "No such track";
    auto& t = s->tracks[(size_t) trackIndex];
    if (t.instrument == nullptr || t.instrument->hasFailed())
        return "The plugin is not loaded: " + pluginStatusText (t);
    plugins.showEditor (t.instrument, t.name + " - " + t.pluginName);
    return {};
}

void AppState::reloadPlugins()
{
    if (guardModuleChange().isNotEmpty())
        return;
    captureInstrumentStates();
    for (auto& s : songs)
        for (auto& t : s.tracks)
            if (t.kind == TrackKind::MidiVsti && t.pluginUid.isNotEmpty() && t.pluginStatus != TrackInfo::PluginStatus::Ready)
                loadInstrument (t);
    rebuildPlan();
    sendChangeMessage();
}

AppState::PluginSummary AppState::getPluginSummary() const
{
    PluginSummary p;
    for (const auto& s : songs)
        for (const auto& t : s.tracks)
        {
            if (t.kind != TrackKind::MidiVsti || t.pluginUid.isEmpty())
                continue;
            ++p.instruments;
            if (t.pluginStatus == TrackInfo::PluginStatus::Ready && t.instrument != nullptr && ! t.instrument->hasFailed())
            {
                ++p.ready;
                if (! t.pluginCertified) ++p.notCertified;
                p.maxLoad = juce::jmax (p.maxLoad, t.instrument->getLoad());
            }
            else
            {
                ++p.problems;
            }
        }
    return p;
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
        if (t.kind == TrackKind::MidiExternal)
        {
            const int m = findModule (t.moduleId);
            if (t.midiEvents == nullptr || m < 0 || m >= AudioEngine::maxMidiPorts)
                continue;
            const auto& mp = modules[(size_t) m];
            MidiTrackRuntime mt;
            mt.slot = i;
            mt.port = m;
            mt.sendEarlyMs = mp.delayOffsetMs;
            const int ch = juce::jlimit (1, 16, t.midiChannel > 0 ? t.midiChannel : mp.channel) - 1;
            mt.events = toSampleEvents (*t.midiEvents, song->tempo, sr, ch);
            if (! mp.sustainEnabled)   // some modules hang notes on CC64: the profile can strip it
                mt.events.erase (std::remove_if (mt.events.begin(), mt.events.end(),
                                                 [] (const MidiSampleEvent& e) { return e.msg.isController (64); }),
                                 mt.events.end());
            plan->midiTracks.push_back (std::move (mt));
            continue;
        }
        if (t.kind == TrackKind::MidiVsti)
        {
            if (t.instrument == nullptr || t.instrument->hasFailed() || t.midiEvents == nullptr)
                continue;
            InstrumentTrackRuntime it;
            it.slot = i;
            it.bus = t.bus;
            it.instrument = t.instrument;
            const int ch = juce::jlimit (1, 16, t.midiChannel > 0 ? t.midiChannel : 1) - 1;
            it.events = toSampleEvents (*t.midiEvents, song->tempo, sr, ch);
            plan->instrumentTracks.push_back (std::move (it));
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
    if (shouldBeLive)
    {
        plugins.closeAllEditors();   // no plugin windows on stage
        const auto ps = getPluginSummary();
        if (ps.problems > 0 || ps.notCertified > 0)
            juce::Logger::writeToLog ("LIVE MODE WARNING: " + juce::String (ps.problems) + " plugin(s) not available, "
                                      + juce::String (ps.notCertified) + " not Live Certified");
    }
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
            // The engine can no longer release its notes: the MIDI thread does it from what it sent.
            midi.panic();
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
            engine.setOutputLatencyMs (device.getOutputLatencyMs());
            juce::Logger::writeToLog ("MIDI timing: output latency " + juce::String (device.getOutputLatencyMs(), 1) + " ms");
            transport = TransportState::Stopped;

            const double sr = engine.getSampleRate();
            if (std::abs (sr - lastDeviceRate) >= 0.5)
            {
                lastDeviceRate = sr;
                requestAllMedia();   // re-decode at the new rate in the background
                mediaPool.purgeUnused();
            }
            prepareInstruments();   // new rate / block size (the engine plays them silent meanwhile)
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

    // Plugins: unsaved changes (editor / presets) and failures in the audio thread.
    bool pluginEvent = false;
    for (auto& s : songs)
        for (auto& t : s.tracks)
        {
            if (t.instrument == nullptr)
                continue;
            if (t.instrument->takeChanged())
            {
                instrumentStateStale = true;
                markDirty();
            }
            if (t.instrument->hasFailed() && t.pluginStatus != TrackInfo::PluginStatus::Failed)
            {
                t.pluginStatus = TrackInfo::PluginStatus::Failed;
                t.pluginError = t.instrument->getFailure();
                plugins.closeEditor (t.instrument.get());
                juce::Logger::writeToLog ("PLUGIN FAILED: " + t.pluginName + " on track " + t.name + ": " + t.pluginError
                                          + " - it is silent now, the rest keeps playing");
                pluginEvent = true;
            }
        }
    if (pluginEvent)
        sendChangeMessage();
    // Plugin state is read only when stopped: some plugins glitch if asked for it while playing.
    if (instrumentStateStale && transport != TransportState::Playing)
    {
        captureInstrumentStates();
        instrumentStateStale = false;
        markDirty();
    }

    // Autosave (5 s after the last change) and periodic backups, all on the project IO thread.
    if (session.isOpen())
    {
        const double now = juce::Time::getMillisecondCounterHiRes();
        if (dirty && now - lastChangeMs >= autosaveDelayMs)
        {
            session.saveAsync (toProjectData());
            dirty = false;
            sendChangeMessage();
        }
        if (now - lastBackupMs >= backupIntervalMs)
        {
            lastBackupMs = now;
            session.backupAsync();
        }
    }

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

//==============================================================================
// Project files
namespace b4nd
{

static std::string kindToDb (TrackKind k)
{
    switch (k)
    {
        case TrackKind::Audio:        return "audio";
        case TrackKind::MidiVsti:     return "midi_vsti";
        case TrackKind::MidiExternal: return "midi_external";
        case TrackKind::Click:        return "click";
        case TrackKind::Guide:        return "guide";
    }
    return "audio";
}

static TrackKind kindFromDb (const std::string& k)
{
    if (k == "midi_vsti")     return TrackKind::MidiVsti;
    if (k == "midi_external") return TrackKind::MidiExternal;
    if (k == "click")         return TrackKind::Click;
    if (k == "guide")         return TrackKind::Guide;
    return TrackKind::Audio;
}

static std::string busToDb (BusKind b)
{
    switch (b)
    {
        case BusKind::Main:    return "main";
        case BusKind::Click:   return "click";
        case BusKind::Guide:   return "guide";
        case BusKind::Extra:   return "extra";
        case BusKind::Monitor: return "monitor";
        case BusKind::Aux:     return "aux";
    }
    return "main";
}

static BusKind busFromDb (const std::string& b)
{
    if (b == "click")   return BusKind::Click;
    if (b == "guide")   return BusKind::Guide;
    if (b == "extra")   return BusKind::Extra;
    if (b == "monitor") return BusKind::Monitor;
    if (b == "aux")     return BusKind::Aux;
    return BusKind::Main;
}

static std::string u8 (const juce::String& s) { return s.toStdString(); }
static juce::String fromU8 (const std::string& s) { return juce::String::fromUTF8 (s.c_str(), (int) s.size()); }

juce::File AppState::getLastProjectRecordFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("B4ND LIVE MTR").getChildFile ("last-project.txt");
}

juce::File AppState::getDefaultProjectsFolder()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("B4ND Projects");
}

void AppState::markDirty()
{
    dirty = true;
    lastChangeMs = juce::Time::getMillisecondCounterHiRes();
}

juce::String AppState::getSaveStatusText() const
{
    if (! session.isOpen())
        return "NOT SAVED (no project file)";
    if (const auto err = session.getLastError(); err.isNotEmpty())
        return "SAVE FAILED: " + err;
    if (dirty || session.isSaving())
        return "SAVING...";
    return "SAVED " + session.getLastSaveTime().formatted ("%H:%M:%S");
}

juce::String AppState::guardProjectChange() const
{
    if (liveMode)
        return "Live Mode: projects cannot be changed during a show";
    if (transport == TransportState::Playing)
        return "Stop playback first";
    return {};
}

ProjectData AppState::toProjectData() const
{
    ProjectData p;
    p.id = u8 (projectId.isNotEmpty() ? projectId : juce::String (newUuid()));
    p.name = u8 (projectName);
    p.sampleRate = juce::roundToInt (mediaSampleRate());
    p.createdAt = projectCreatedAt.isNotEmpty() ? u8 (projectCreatedAt) : nowIso8601();
    p.modifiedAt = nowIso8601();
    p.appVersion = JUCE_APPLICATION_VERSION_STRING;

    const auto pkg = session.getPackage();
    std::map<juce::String, std::string> mediaIds;   // absolute path -> media id

    SetlistData setlist;
    setlist.id = "setlist-main";
    setlist.name = "Main";

    for (const auto& s : songs)
    {
        SongData sd;
        sd.id = u8 (s.id);
        sd.title = u8 (s.title);
        sd.artist = u8 (s.artist);
        sd.key = u8 (s.key);
        sd.baseBpm = s.tempo.bpmAtBeat (0.0);
        sd.lengthSec = s.lengthSeconds;
        sd.tempoPoints = s.tempo.getTempoPoints();
        sd.timeSigPoints = s.tempo.getTimeSigPoints();
        for (const auto& c : s.cues)
        {
            CueData cd;
            cd.id = newUuid();
            cd.name = u8 (c.name);
            cd.positionSec = c.seconds;
            sd.cues.push_back (cd);
        }
        for (const auto& t : s.tracks)
        {
            TrackData td;
            td.id = u8 (t.id);
            td.name = u8 (t.name);
            td.kind = kindToDb (t.kind);
            td.busKind = t.kind == TrackKind::Click ? "click" : t.kind == TrackKind::Guide ? "guide" : busToDb (t.bus);
            td.destinationLabel = u8 (t.destination);
            td.volumeDb = t.volumeDb;
            td.pan = t.pan;
            td.mute = t.mute;
            td.solo = t.solo;
            if (t.kind == TrackKind::MidiExternal || t.kind == TrackKind::MidiVsti)
            {
                td.moduleProfileId = t.kind == TrackKind::MidiExternal ? u8 (t.moduleId) : std::string();
                td.midiChannel = t.midiChannel;
                if (t.midiPath.isNotEmpty())
                    td.midiPath = session.isOpen() ? pkg.toStoredPath (u8 (t.midiPath)) : u8 (t.midiPath);
            }
            if (t.kind == TrackKind::MidiVsti && t.pluginUid.isNotEmpty())
            {
                // The saved state is the last one captured while stopped (or the loaded one when the
                // plugin is missing here, so opening on another PC never loses the sound settings).
                PluginSlotData ps;
                ps.id = u8 (t.pluginSlotId.isNotEmpty() ? t.pluginSlotId : t.id + "-plugin");
                ps.pluginUid = u8 (t.pluginUid);
                ps.pluginName = u8 (t.pluginName);
                ps.vendor = u8 (t.pluginVendor);
                ps.descriptionXml = u8 (t.pluginDescXml);
                if (auto xml = juce::parseXML (t.pluginDescXml))
                    ps.version = u8 (xml->getStringAttribute ("version"));
                const auto* bytes = static_cast<const std::uint8_t*> (t.pluginState.getData());
                ps.state.assign (bytes, bytes + t.pluginState.getSize());
                td.pluginSlotId = ps.id;
                p.plugins.push_back (std::move (ps));
            }
            if (t.mediaPath.isNotEmpty())
            {
                auto it = mediaIds.find (t.mediaPath);
                if (it == mediaIds.end())
                {
                    MediaRef m;
                    m.id = newUuid();
                    m.kind = t.kind == TrackKind::Guide ? "guide" : "original";
                    m.path = session.isOpen() ? pkg.toStoredPath (u8 (t.mediaPath)) : u8 (t.mediaPath);
                    if (t.media != nullptr && t.mediaRate > 0.0)
                    {
                        m.durationSec = (double) t.media->numFrames / t.mediaRate;
                        m.channels = t.media->numChannels;
                    }
                    p.media.push_back (m);
                    it = mediaIds.emplace (t.mediaPath, m.id).first;
                }
                ClipData c;
                c.id = newUuid();
                c.mediaId = it->second;
                c.startSec = t.mediaStartSeconds;
                td.clips.push_back (c);
            }
            sd.tracks.push_back (td);
        }
        if (s.tracks.empty() || std::none_of (s.tracks.begin(), s.tracks.end(), [] (const TrackInfo& t) { return t.kind == TrackKind::Click; }))
            sd.click.enabled = false;
        setlist.songIds.push_back (sd.id);
        p.songs.push_back (std::move (sd));
    }
    p.setlists.push_back (setlist);
    p.modules = modules;
    return p;
}

void AppState::applyProjectData (const ProjectData& p, const juce::File& folder)
{
    const ProjectPackage pkg (folder.getFullPathName().toStdString());

    std::vector<SongInfo> loaded;
    std::map<std::string, const SongData*> byId;
    for (const auto& sd : p.songs)
        byId[sd.id] = &sd;

    // Show order from the first setlist; songs not in it follow in stored order.
    std::vector<const SongData*> order;
    if (! p.setlists.empty())
        for (const auto& id : p.setlists.front().songIds)
            if (auto it = byId.find (id); it != byId.end())
                order.push_back (it->second);
    for (const auto& sd : p.songs)
        if (std::find (order.begin(), order.end(), &sd) == order.end())
            order.push_back (&sd);

    for (const auto* sdp : order)
    {
        const auto& sd = *sdp;
        SongInfo s;
        s.id = fromU8 (sd.id);
        s.title = fromU8 (sd.title);
        s.artist = fromU8 (sd.artist);
        s.key = fromU8 (sd.key);
        s.lengthSeconds = sd.lengthSec > 0.0 ? sd.lengthSec : 180.0;
        s.tempo.setTempoPoints (sd.tempoPoints.empty() ? std::vector<TempoPoint> { { 0.0, sd.baseBpm > 0.0 ? sd.baseBpm : 120.0 } } : sd.tempoPoints);
        if (! sd.timeSigPoints.empty())
            s.tempo.setTimeSigPoints (sd.timeSigPoints);
        for (const auto& c : sd.cues)
            s.cues.push_back ({ fromU8 (c.name), c.positionSec });
        for (const auto& td : sd.tracks)
        {
            TrackInfo t;
            t.id = fromU8 (td.id);
            t.name = fromU8 (td.name);
            t.kind = kindFromDb (td.kind);
            t.bus = busFromDb (td.busKind);
            t.destination = fromU8 (td.destinationLabel);
            t.volumeDb = (float) td.volumeDb;
            t.pan = (float) td.pan;
            t.mute = td.mute;
            t.solo = td.solo;
            if (t.kind == TrackKind::Click)
                t.regions = { { 0.0, s.lengthSeconds } };
            if (t.kind == TrackKind::MidiVsti && ! td.pluginSlotId.empty())
                for (const auto& ps : p.plugins)
                    if (ps.id == td.pluginSlotId)
                    {
                        t.pluginSlotId = fromU8 (ps.id);
                        t.pluginUid = fromU8 (ps.pluginUid);
                        t.pluginName = fromU8 (ps.pluginName);
                        t.pluginVendor = fromU8 (ps.vendor);
                        t.pluginDescXml = fromU8 (ps.descriptionXml);
                        t.pluginState.replaceAll (ps.state.data(), ps.state.size());
                        break;
                    }
            if (t.kind == TrackKind::MidiExternal || t.kind == TrackKind::MidiVsti)
            {
                if (t.kind == TrackKind::MidiExternal)
                    t.moduleId = fromU8 (td.moduleProfileId);
                t.midiChannel = juce::jlimit (0, 16, td.midiChannel);
                if (! td.midiPath.empty())
                    t.midiPath = fromU8 (pkg.toAbsolutePath (td.midiPath));
                loadMidiFile (t, s);
            }
            if (! td.clips.empty())
                if (const auto* m = p.findMedia (td.clips.front().mediaId))
                {
                    t.mediaPath = fromU8 (pkg.toAbsolutePath (m->path));
                    t.mediaStartSeconds = td.clips.front().startSec;
                    t.mediaStatus = TrackInfo::MediaStatus::Loading;
                }
            s.tracks.push_back (std::move (t));
        }
        loaded.push_back (std::move (s));
    }

    // Swap the model in. The engine keeps playing nothing: we are stopped (guardProjectChange).
    stop();
    midi.panic();
    plugins.closeAllEditors();
    instrumentStateStale = false;
    modules.clear();
    for (const auto& mp : p.modules)
        if ((int) modules.size() < MidiOutputService::maxSlots)
            modules.push_back (sanitise (mp));
    midi.setModules (modules);
    songs = std::move (loaded);
    currentSong = 0;
    position = 0.0;
    trackLevels.fill (0.0f);
    projectId = fromU8 (p.id);
    projectCreatedAt = fromU8 (p.createdAt);
    projectName = fromU8 (p.name).isNotEmpty() ? fromU8 (p.name) : folder.getFileNameWithoutExtension();
    dirty = false;
    for (const auto& song : songs)
        for (const auto& t : song.tracks)
            if (t.midiStatus == TrackInfo::MidiStatus::Missing && ! missingOnOpen.contains (t.midiPath))
                missingOnOpen.add (t.midiPath);
    loadAllInstruments();
    for (const auto& song : songs)
        for (const auto& t : song.tracks)
            if (t.kind == TrackKind::MidiVsti && t.pluginUid.isNotEmpty() && t.pluginStatus != TrackInfo::PluginStatus::Ready)
            {
                const auto what = "Plugin " + t.pluginName + " (" + pluginStatusText (t) + ")";
                if (! missingOnOpen.contains (what))
                    missingOnOpen.add (what);
            }
    mediaPool.purgeUnused();
    requestAllMedia();
    rebuildPlan();
    sendChangeMessage();
}

void AppState::rememberLastProject() const
{
    auto f = getLastProjectRecordFile();
    f.getParentDirectory().createDirectory();
    f.replaceWithText (session.isOpen() ? session.getFolder().getFullPathName() : juce::String());
}

juce::String AppState::newProject (const juce::File& folder, const juce::String& name)
{
    if (auto g = guardProjectChange(); g.isNotEmpty())
        return g;
    ProjectData p;
    p.id = newUuid();
    p.name = u8 (name);
    p.createdAt = p.modifiedAt = nowIso8601();
    p.appVersion = JUCE_APPLICATION_VERSION_STRING;
    SongData s;
    s.id = newUuid();
    s.title = "Song 1";
    s.lengthSec = 180.0;
    s.tempoPoints = { { 0.0, 120.0 } };
    s.timeSigPoints = { { 0, 4, 4 } };
    TrackData click;
    click.id = newUuid();
    click.name = "Click";
    click.kind = "click";
    click.busKind = "click";
    click.volumeDb = -6.0;
    s.tracks.push_back (click);
    p.songs.push_back (s);
    p.setlists.push_back ({ "setlist-main", "Main", "", "", { s.id } });

    if (auto r = session.create (folder, p); ! r)
        return fromU8 (r.error);
    applyProjectData (p, folder);
    staleLockOnOpen = false;
    backupsOnOpen.clear();
    missingOnOpen.clear();
    lastBackupMs = juce::Time::getMillisecondCounterHiRes();
    rememberLastProject();
    return {};
}

juce::String AppState::openProject (const juce::File& folderOrDb)
{
    if (auto g = guardProjectChange(); g.isNotEmpty())
        return g;
    if (session.isOpen() && dirty)
    {
        captureInstrumentStates();
        session.saveNow (toProjectData());
    }
    dirty = false;

    auto r = session.open (folderOrDb);
    if (! r.result)
        return fromU8 (r.result.error);

    staleLockOnOpen = r.hadStaleLock;
    backupsOnOpen.clear();
    for (const auto& b : r.backups) backupsOnOpen.add (fromU8 (b));
    missingOnOpen.clear();
    for (const auto& m : r.missingMedia) missingOnOpen.add (fromU8 (m));
    if (! missingOnOpen.isEmpty())
        juce::Logger::writeToLog ("MEDIA MISSING on open: " + missingOnOpen.joinIntoString (", "));

    applyProjectData (r.data, session.getFolder());
    lastBackupMs = juce::Time::getMillisecondCounterHiRes();
    rememberLastProject();
    return {};
}

juce::String AppState::saveProject()
{
    if (! session.isOpen())
        return "No project file yet: use Save As";
    if (transport != TransportState::Playing)
        captureInstrumentStates();
    auto r = session.saveNow (toProjectData());
    if (! r)
        return fromU8 (r.error);
    dirty = false;
    sendChangeMessage();
    return {};
}

juce::String AppState::saveProjectAs (const juce::File& folder)
{
    if (auto g = guardProjectChange(); g.isNotEmpty())
        return g;
    // The new package gets its own identity; media keep their absolute paths (outside the package).
    const auto oldId = projectId;
    projectId = newUuid();
    projectName = folder.getFileNameWithoutExtension();
    session.close();
    captureInstrumentStates();
    auto p = toProjectData();
    if (auto r = session.create (folder, p); ! r)
    {
        projectId = oldId;
        return fromU8 (r.error);
    }
    dirty = false;
    staleLockOnOpen = false;
    lastBackupMs = juce::Time::getMillisecondCounterHiRes();
    rememberLastProject();
    sendChangeMessage();
    return {};
}

juce::String AppState::restoreBackup (const juce::String& backupPath)
{
    if (auto g = guardProjectChange(); g.isNotEmpty())
        return g;
    auto r = session.restoreBackup (backupPath.toStdString());
    if (! r.result)
        return fromU8 (r.result.error);
    staleLockOnOpen = false;
    missingOnOpen.clear();
    for (const auto& m : r.missingMedia) missingOnOpen.add (fromU8 (m));
    applyProjectData (r.data, session.getFolder());
    return {};
}

void AppState::closeProject()
{
    plugins.closeAllEditors();
    if (session.isOpen() && (dirty || instrumentStateStale))
    {
        captureInstrumentStates();
        session.saveAsync (toProjectData());
    }
    dirty = false;
    session.close();   // writes the pending snapshot, removes session.lock
}

} // namespace b4nd
