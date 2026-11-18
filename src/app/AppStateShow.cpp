// Show check, sleep blocking, crash recovery (Phase 14, docs/10 §4-5).

#include "app/AppState.h"
#include "app/SystemGuard.h"

namespace b4nd
{

void AppState::updateKeepAwake()
{
    const bool want = liveMode || transport == TransportState::Playing;
    if (want == systemguard::isKeepingAwake())
        return;
    if (systemguard::keepAwake (want))
        juce::Logger::writeToLog (want ? "Power: sleep and display off blocked (show running)" : "Power: sleep allowed again");
}

bool AppState::hasBlockers (const std::vector<ShowCheckItem>& items)
{
    for (const auto& i : items)
        if (i.level == ShowCheckItem::Level::Block)
            return true;
    return false;
}

std::vector<AppState::ShowCheckItem> AppState::runShowCheck()
{
    using L = ShowCheckItem::Level;
    std::vector<ShowCheckItem> r;
    auto add = [&r] (L level, const juce::String& title, const juce::String& detail) { r.push_back ({ level, title, detail }); };

    // Audio device
    if (isEngineActive())
    {
        const int buf = device.getBufferSize();
        add (buf > 512 ? L::Warn : L::Ok, "Audio device",
             device.getDeviceName() + ", " + juce::String (device.getSampleRate(), 0) + " Hz, buffer " + juce::String (buf)
                 + (buf > 512 ? " (large: MIDI and VSTi feel late)" : ""));
    }
    else
        add (L::Block, "Audio device", "Not running: open the audio settings (Ctrl+,) and choose the interface");
    const auto drops = getDropoutCount();
    add (drops > 0 ? L::Warn : L::Ok, "Dropouts since start",
         drops > 0 ? juce::String ((juce::int64) drops) + " (larger buffer, close other programs, check the power plan)" : juce::String ("none"));
    add (outputTestRun ? L::Ok : L::Warn, "Output test",
         outputTestRun ? juce::String ("done in this session") : juce::String ("not done yet: Ctrl+, OUTPUT TEST, check every output (click only in the in-ears)"));

    // Media of every song
    int missing = 0, songsMissing = 0;
    juce::StringArray missingSongs;
    for (int i = 0; i < (int) songs.size(); ++i)
    {
        const auto m = getSongMedia (i);
        if (m.missing + m.errors > 0)
        {
            missing += m.missing + m.errors;
            ++songsMissing;
            if (missingSongs.size() < 3)
                missingSongs.add (songs[(size_t) i].title);
        }
    }
    if (missing > 0)
        add (L::Block, "Audio files", juce::String (missing) + " missing in " + juce::String (songsMissing) + " song(s): "
                                          + missingSongs.joinIntoString (", ") + (songsMissing > 3 ? ", ..." : ""));
    else
        add (L::Ok, "Audio files", "all found");
    const auto ms = getMediaSummary();
    if (! isSongReady (currentSong))
        add (L::Warn, "Current song", "audio still loading");
    add (ms.windowed ? L::Info : L::Ok, "RAM",
         juce::String (ms.setlistBytes >> 20) + " MB of audio, budget " + juce::String (ms.budgetBytes >> 20) + " MB: "
             + (ms.windowed ? "previous / current / next song in RAM (the next loads while you play)" : "every song in RAM"));

    // MIDI modules, plugins, footswitch
    const auto mid = getMidiSummary();
    if (mid.modules > 0)
        add (mid.problems > 0 ? L::Block : mid.preparing > 0 ? L::Warn : L::Ok, "MIDI modules",
             juce::String (mid.ready) + " of " + juce::String (mid.modules) + " ready"
                 + (mid.problems > 0 ? ": " + juce::String (mid.problems) + " disconnected / not opened" : juce::String()));
    if (mid.tracksWithoutModule > 0 || mid.missingFiles > 0)
        add (L::Warn, "MIDI tracks", juce::String (mid.tracksWithoutModule) + " without a module, " + juce::String (mid.missingFiles) + " missing .mid files");
    const auto ps = getPluginSummary();
    if (ps.instruments > 0)
        add (ps.problems > 0 ? L::Block : ps.notCertified > 0 ? L::Warn : L::Ok, "VSTi",
             juce::String (ps.ready) + " ready" + (ps.problems > 0 ? ", " + juce::String (ps.problems) + " NOT LOADED" : juce::String())
                 + (ps.notCertified > 0 ? ", " + juce::String (ps.notCertified) + " not Live Certified" : juce::String()));
    int lost = 0;
    for (const auto& p : footswitch->getPorts())
        lost += p.status != FootswitchInput::PortState::Status::Open ? 1 : 0;
    if (! footswitch->getPorts().empty())
        add (lost > 0 ? L::Warn : L::Ok, "Footswitch", lost > 0 ? juce::String ("not connected") : footswitch->getPorts().front().port.name);

    // Project and PC
    if (! session.isOpen())
        add (L::Warn, "Project", "not saved to a file (File > Save As)");
    else if (dirty || session.getLastError().isNotEmpty())
        add (session.getLastError().isNotEmpty() ? L::Warn : L::Info, "Project", getSaveStatusText());
    else
        add (L::Ok, "Project", "saved");
#if JUCE_WINDOWS
    const auto powerPlan = systemguard::activePowerPlan();   // a cheap API call: re-read on every check
    add (powerPlan.highPerformance ? L::Ok : L::Warn, "Windows power plan",
         (powerPlan.name.isEmpty() ? juce::String ("unknown") : powerPlan.name)
             + (powerPlan.highPerformance ? "" : " (choose High performance in the Windows power options)"));
#endif
    add (L::Info, "Sleep / screen saver", "blocked automatically in Live Mode and while playing"
                                              + juce::String (systemguard::isKeepingAwake() ? " (now: blocked)" : ""));
    return r;
}

//==============================================================================
juce::File AppState::getPositionRecordFile()
{
    return getLastProjectRecordFile().getParentDirectory().getChildFile ("last-position.txt");
}

void AppState::writePositionRecord (bool force)
{
    if (! session.isOpen())
        return;
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (! force && now - lastPositionWriteMs < 10000.0)
        return;
    const auto* s = getCurrentSong();
    if (s == nullptr)
        return;
    const auto text = session.getFolder().getFullPathName() + "\n" + s->id + "\n" + juce::String (position, 1) + "\n";
    if (text == lastPositionWritten)
        return;
    lastPositionWriteMs = now;
    lastPositionWritten = text;
    // A few bytes; a failure here only loses the recovery offer.
    getPositionRecordFile().replaceWithText (text);
}

void AppState::readRecoveryRecord (const juce::StringArray& lines)
{
    if (lines.size() < 3 || lines[0] != session.getFolder().getFullPathName())
        return;
    for (int i = 0; i < (int) songs.size(); ++i)
        if (songs[(size_t) i].id == lines[1])
        {
            recovery.available = true;
            recovery.song = i;
            recovery.seconds = juce::jlimit (0.0, songs[(size_t) i].lengthSeconds, lines[2].getDoubleValue());
            recovery.title = songs[(size_t) i].title;
            if (i == currentSong && recovery.seconds < 1.0)
            {
                recovery = {};   // stopped at the top of the song that opens anyway: nothing to offer
                return;
            }
            juce::Logger::writeToLog ("Recovery: the last run stopped in " + recovery.title + " at " + juce::String (recovery.seconds, 1) + " s");
        }
}

void AppState::restoreRecovery()
{
    if (! recovery.available)
        return;
    const auto r = recovery;
    recovery = {};
    selectSong (r.song);
    locate (r.seconds);
    juce::Logger::writeToLog ("Recovery: back to " + r.title + " at " + juce::String (r.seconds, 1) + " s");
}

} // namespace b4nd
