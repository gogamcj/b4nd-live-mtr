// AppState: song analysis (docs/07). The analyzer is a separate process; nothing here runs on
// the audio thread, and nothing is applied to a song without the user's Apply.

#include "app/AppState.h"

#include <algorithm>
#include <cmath>

namespace b4nd
{

namespace
{
juce::String nowIso()
{
    return juce::String::fromUTF8 (nowIso8601().c_str());
}

juce::File analysisDir (const juce::File& package, const juce::String& songId)
{
    return package.getChildFile ("analysis").getChildFile (songId);
}
} // namespace

int AppState::findAnalysisSourceTrack() const
{
    const auto* s = getCurrentSong();
    if (s == nullptr)
        return -1;
    int fallback = -1;
    for (int i = 0; i < (int) s->tracks.size(); ++i)
    {
        const auto& t = s->tracks[(size_t) i];
        if (t.kind != TrackKind::Audio || t.media == nullptr || t.mediaStatus != TrackInfo::MediaStatus::Ready)
            continue;
        const auto n = t.name.toLowerCase();
        if (t.bus == BusKind::Main && (n.contains ("original") || n.contains ("mix") || n.contains ("mr")))
            return i;
        if (fallback < 0 || (t.bus == BusKind::Main && s->tracks[(size_t) fallback].bus != BusKind::Main))
            fallback = i;
    }
    return fallback;
}

juce::String AppState::startAnalysis (int trackIndex, juce::StringArray tasks)
{
    if (liveMode)
        return "Live Mode: analysis is for preparation, not during a show";
    if (transport == TransportState::Playing)
        return "Stop playback first (the analysis uses a lot of CPU)";
    if (! session.isOpen())
        return "Save the project first: results are stored in its analysis folder";
    if (analyzer.isBusy())
        return "An analysis is already running";
    const auto* s = getCurrentSong();
    if (s == nullptr)
        return "No song";
    if (trackIndex < 0)
        trackIndex = findAnalysisSourceTrack();
    if (! juce::isPositiveAndBelow (trackIndex, (int) s->tracks.size()))
        return "No audio track with loaded audio in this song";
    const auto& t = s->tracks[(size_t) trackIndex];
    if (t.media == nullptr || t.mediaStatus != TrackInfo::MediaStatus::Ready)
        return "The track's audio is not loaded";

    AnalyzerRunner::Job job;
    job.songId = s->id;
    job.outDir = analysisDir (session.getFolder(), s->id);
    if (! tasks.isEmpty())
        job.tasks = tasks;
    job.media = t.media;
    job.mediaRate = t.mediaRate;
    job.mediaStartSec = t.mediaStartSeconds;
    if (auto err = analyzer.start (job); err.isNotEmpty())
        return err;
    analysisStartedAt = nowIso();
    analysisTasks = job.tasks.joinIntoString (",");
    lastAnalysisState = AnalyzerRunner::State::Preparing;
    juce::String where;
    analyzer.resolveCommand (where);
    juce::Logger::writeToLog ("Analysis started: " + s->title + " / " + t.name + " [" + analysisTasks + "] using " + where);
    sendChangeMessage();
    return {};
}

void AppState::cancelAnalysis()
{
    if (analyzer.isBusy())
    {
        analyzer.cancel();
        juce::Logger::writeToLog ("Analysis cancelled by the user");
    }
}

void AppState::pollAnalysis()
{
    const auto st = analyzer.getStatus();
    if (st.state == lastAnalysisState)
    {
        if (st.state == AnalyzerRunner::State::Running || st.state == AnalyzerRunner::State::Preparing)
            sendChangeMessage();   // progress
        return;
    }
    lastAnalysisState = st.state;
    if (st.state != AnalyzerRunner::State::Done && st.state != AnalyzerRunner::State::Failed
        && st.state != AnalyzerRunner::State::Cancelled)
    {
        sendChangeMessage();
        return;
    }

    SongInfo* song = nullptr;
    for (auto& s : songs)
        if (s.id == st.songId)
            song = &s;

    AnalysisRunData run;
    run.id = newUuid();
    run.startedAt = analysisStartedAt.toStdString();
    run.finishedAt = nowIso8601();
    run.tasks = analysisTasks.toStdString();
    run.resultRelPath = ("analysis/" + st.songId + "/result.json").toStdString();
    if (st.state == AnalyzerRunner::State::Done)
    {
        auto r = analyzer.getResult();
        run.status = r.status.toStdString();
        run.modelVersions = r.versionsJson.toStdString();
        juce::String summary;
        if (r.hasTempo)
            summary << juce::String (r.bpm, 2) << " BPM (" << juce::roundToInt (r.bpmConfidence * 100) << "%), bar 1 at "
                    << juce::String (r.gridStartSec, 3) << " s, " << r.numerator << "/" << r.denominator
                    << (r.variableTempo ? ", " + juce::String ((int) r.tempoMap.size()) + " tempo sections" : juce::String());
        if (r.hasKey)
            summary << ", " << r.key << " (" << juce::roundToInt (r.keyConfidence * 100) << "%)";
        if (r.hasChords)
            summary << ", " << (int) r.chords.size() << " chords";
        if (r.hasSections)
        {
            juce::StringArray names;
            for (const auto& x : r.sections)
                names.add (x.label);
            summary << ", sections: " << names.joinIntoString (" / ");
        }
        juce::Logger::writeToLog ("Analysis " + r.status.toUpperCase() + " in " + juce::String (st.elapsedSec, 1) + " s: " + summary
                                  + (st.message.isNotEmpty() ? " | " + st.message : juce::String()));
        analysisResults[st.songId] = std::move (r);
    }
    else
    {
        run.status = st.state == AnalyzerRunner::State::Cancelled ? "cancelled" : "failed";
        juce::Logger::writeToLog ("Analysis " + juce::String (run.status).toUpperCase() + ": " + st.message);
    }
    if (song != nullptr)
    {
        song->analysisRuns.push_back (run);
        if (song->analysisRuns.size() > 20)   // history, not an archive
            song->analysisRuns.erase (song->analysisRuns.begin());
        markDirty();
    }
    sendChangeMessage();
}

const AnalysisResult* AppState::getAnalysisResult()
{
    const auto* s = getCurrentSong();
    if (s == nullptr)
        return nullptr;
    if (auto it = analysisResults.find (s->id); it != analysisResults.end())
        return &it->second;
    if (! session.isOpen())
        return nullptr;
    const auto f = analysisDir (session.getFolder(), s->id).getChildFile ("result.json");
    if (! f.existsAsFile())
        return nullptr;
    auto r = AnalysisResult::parse (f.loadFileAsString());
    if (! r.valid)
        return nullptr;
    return &(analysisResults[s->id] = std::move (r));
}

juce::String AppState::applyAnalysis (const AnalysisApply& a)
{
    if (liveMode)
        return "Live Mode: songs cannot be changed during a show";
    if (transport != TransportState::Stopped)
        return "Stop playback first";
    auto* s = currentSongMutable();
    if (s == nullptr)
        return "No song";
    const auto* rp = getAnalysisResult();
    if (rp == nullptr)
        return "No analysis result for this song";
    const auto& r = *rp;

    AnalysisUndo undo;
    undo.songId = s->id;
    undo.tempo = s->tempo;
    undo.key = s->key;
    undo.bpmConfidence = s->bpmConfidence;
    undo.keyConfidence = s->keyConfidence;
    undo.cues = s->cues;
    undo.chords = s->chords;

    TempoMap tempo = s->tempo;
    double gridShift = 0.0;
    juce::StringArray applied;
    if (a.withTempo && r.hasTempo)
    {
        std::vector<TempoPoint> pts;
        if (a.bpm > 0.0)
            pts = { { 0.0, juce::jlimit (20.0, 400.0, a.bpm) } };
        else
        {
            const double k = std::abs (a.tempoScale - 2.0) < 1e-9 || std::abs (a.tempoScale - 0.5) < 1e-9 ? a.tempoScale : 1.0;
            for (const auto& p : r.tempoMap)
                pts.push_back ({ p.beat * k, juce::jlimit (20.0, 400.0, p.bpm * k) });
        }
        if (! tempo.setTempoPoints (pts))
            return "Invalid tempo";
        const double start = a.gridStartSec >= 0.0 ? a.gridStartSec : r.gridStartSec;
        gridShift = start - r.gridStartSec;
        if (! tempo.setOffsetSeconds (std::max (0.0, start)))
            return "Invalid bar 1 position";
        s->bpmConfidence = a.bpm > 0.0 || std::abs (a.tempoScale - 1.0) > 1e-9 ? -1.0 : r.bpmConfidence;   // corrected by hand
        applied.add (juce::String (tempo.bpmAtBeat (0.0), 2) + " BPM, bar 1 at " + juce::String (tempo.getOffsetSeconds(), 3) + " s");
    }
    if (a.withTimeSig && r.hasTempo)
    {
        const int n = a.numerator > 0 ? a.numerator : r.numerator;
        if (! tempo.setTimeSigPoints ({ { 0, juce::jlimit (1, 16, n), r.denominator } }))
            return "Invalid time signature";
        applied.add (juce::String (n) + "/" + juce::String (r.denominator));
    }
    s->tempo = tempo;
    if (a.withKey && (r.hasKey || a.key.isNotEmpty()))
    {
        s->key = a.key.isNotEmpty() ? a.key : r.key;
        s->keyConfidence = a.key.isNotEmpty() && a.key != r.key ? -1.0 : r.keyConfidence;
        applied.add (s->key);
    }
    if (a.withCues && r.hasSections)
    {
        std::vector<CuePoint> cues;
        for (const auto& c : s->cues)
            if (c.source != "ai")
                cues.push_back (c);   // the user's own cues stay
        const auto& secs = a.useEditedSections ? a.sections : r.sections;
        for (const auto& x : secs)
        {
            const double at = juce::jlimit (0.0, std::max (0.0, s->lengthSeconds), x.startSec + gridShift);
            cues.push_back ({ x.label, at, x.section, "ai" });
        }
        std::stable_sort (cues.begin(), cues.end(), [] (const CuePoint& p, const CuePoint& q) { return p.seconds < q.seconds; });
        s->cues = std::move (cues);
        applied.add (juce::String ((int) secs.size()) + " cues");
    }
    if (a.withChords && r.hasChords)
    {
        std::vector<ChordData> ch;
        for (const auto& c : r.chords)
            if (ch.empty() || c.startSec > ch.back().startSec)   // unique start times (primary key)
                ch.push_back ({ c.startSec + gridShift, c.endSec + gridShift, c.chord, c.confidence });
        s->chords = std::move (ch);
        applied.add (juce::String ((int) s->chords.size()) + " chords");
    }
    if (applied.isEmpty())
        return "Nothing selected to apply";

    analysisUndo = std::move (undo);
    rebuildPlan();
    markDirty();
    juce::Logger::writeToLog ("Analysis applied to " + s->title + ": " + applied.joinIntoString (", "));
    sendChangeMessage();
    return {};
}

bool AppState::canUndoAnalysisApply() const
{
    const auto* s = getCurrentSong();
    return analysisUndo.has_value() && s != nullptr && s->id == analysisUndo->songId;
}

juce::String AppState::undoAnalysisApply()
{
    if (liveMode)
        return "Live Mode: songs cannot be changed during a show";
    if (transport != TransportState::Stopped)
        return "Stop playback first";
    if (! canUndoAnalysisApply())
        return "Nothing to undo";
    auto* s = currentSongMutable();
    s->tempo = analysisUndo->tempo;
    s->key = analysisUndo->key;
    s->bpmConfidence = analysisUndo->bpmConfidence;
    s->keyConfidence = analysisUndo->keyConfidence;
    s->cues = analysisUndo->cues;
    s->chords = analysisUndo->chords;
    analysisUndo.reset();
    rebuildPlan();
    markDirty();
    juce::Logger::writeToLog ("Analysis apply undone: " + s->title);
    sendChangeMessage();
    return {};
}

} // namespace b4nd
