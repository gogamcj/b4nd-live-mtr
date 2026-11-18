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

juce::String stemTitle (const juce::String& name)
{
    return name.substring (0, 1).toUpperCase() + name.substring (1) + " (stem)";
}

/** One line for the track's warning icon, or "" (docs/07: low quality -> warning). */
juce::String stemWarning (const AnalysisResult& r, const AnalysisResult::Stem& st)
{
    juce::StringArray w;
    if (r.stemConfidence < 0.5)
        w.add ("the stems do not add up to the mix (" + juce::String (r.stemResidualDb, 1) + " dB)");
    if (st.clipped)
        w.add ("clipped");
    if (st.name == "piano" || st.name == "guitar")
        w.add ("piano / guitar separation is the weakest part of the model: check by ear");
    return w.joinIntoString ("; ");
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
    job.sourceTrackId = t.id;
    if (job.tasks.contains ("stems"))
    {
        if (t.isStem())
            return "This track is a stem: separate the original instead";
        job.timeoutSec = 120.0 * 60.0;   // Demucs on a CPU: several minutes per song, more on old PCs
    }
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
        if (r.hasStems)
            summary << (summary.isEmpty() ? "" : ", ") << (int) r.stems.size() << " stems (" << r.stemModel << ", residual "
                    << juce::String (r.stemResidualDb, 1) << " dB, " << juce::roundToInt (r.stemConfidence * 100) << "%)";
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
    const bool withStems = a.withStems && r.hasStems;
    if (withStems)
        if (auto err = applyStems (*s, r, undo, applied, true); err.isNotEmpty())
            return err;   // checked before anything is written to the song
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
    if (withStems)
        if (auto err = applyStems (*s, r, undo, applied, false); err.isNotEmpty())
        {
            // Moving the files failed (disk, permissions): put the song back as it was.
            s->tempo = undo.tempo;
            s->key = undo.key;
            s->bpmConfidence = undo.bpmConfidence;
            s->keyConfidence = undo.keyConfidence;
            s->cues = undo.cues;
            s->chords = undo.chords;
            return err;
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

juce::String AppState::applyStems (SongInfo& s, const AnalysisResult& r, AnalysisUndo& undo, juce::StringArray& applied, bool checkOnly)
{
    const auto from = analysisDir (session.getFolder(), s.id).getChildFile ("stems");
    const TrackInfo* source = nullptr;
    for (const auto& t : s.tracks)
        if (t.id == r.sourceTrackId)
            source = &t;
    if (source == nullptr || source->mediaPath.isEmpty())
        return "Stems: the separated track is no longer in this song";
    std::vector<const AnalysisResult::Stem*> use;
    int added = 0;
    for (const auto& st : r.stems)
    {
        if (! from.getChildFile (st.relPath.fromFirstOccurrenceOf ("stems/", false, false)).existsAsFile())
            return "Stems: the files are no longer in the analysis folder (already added? run the separation again)";
        if (st.empty)
            continue;   // nothing of this instrument in the song: no track
        use.push_back (&st);
        const bool exists = std::any_of (s.tracks.begin(), s.tracks.end(), [&] (const TrackInfo& t) {
            return t.stemName == st.name && t.stemSourcePath == source->mediaPath; });
        if (! exists)
            ++added;
    }
    if (use.empty())
        return "Stems: every stem is empty";
    if ((int) s.tracks.size() + added > AudioEngine::maxTracks)
        return "Stems: too many tracks in this song";
    if (checkOnly)
        return {};

    const auto runFolder = session.getFolder().getChildFile ("stems").getChildFile (s.id)
                               .getChildFile (juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S"));
    if (runFolder.exists() || ! runFolder.getParentDirectory().createDirectory() || ! from.moveFileTo (runFolder))
        return "Stems: cannot move the files to " + runFolder.getFullPathName();
    undo.tracks = s.tracks;
    undo.stemsFrom = from;
    undo.stemsTo = runFolder;

    const juce::String sourcePath = source->mediaPath;
    const double start = source->mediaStartSeconds;
    juce::StringArray names, skipped;
    for (const auto& st : r.stems)
        if (st.empty)
            skipped.add (st.name);
    for (const auto* st : use)
    {
        const auto file = runFolder.getChildFile (st->name + ".wav");
        TrackInfo* t = nullptr;
        for (auto& x : s.tracks)
            if (x.stemName == st->name && x.stemSourcePath == sourcePath)
                t = &x;   // separated again: keep its name, fader, mute and solo
        if (t == nullptr)
        {
            TrackInfo nt;
            nt.id = newUuid();
            nt.name = stemTitle (st->name);
            nt.kind = TrackKind::Audio;
            nt.destination = "Main L/R";
            nt.bus = BusKind::Main;
            nt.mute = true;   // nothing changes in the mix until the user unmutes a stem
            nt.stemName = st->name;
            nt.stemSourcePath = sourcePath;
            s.tracks.push_back (nt);
            t = &s.tracks.back();
        }
        t->mediaPath = file.getFullPathName();
        t->mediaStartSeconds = start;
        t->media.reset();
        t->overview.reset();
        t->mediaRate = 0.0;
        t->mediaError.clear();
        t->mediaStatus = TrackInfo::MediaStatus::Loading;
        t->aiConfidence = (float) r.stemConfidence;
        t->aiWarning = stemWarning (r, *st);
        names.add (st->name);
    }
    applied.add (juce::String ((int) names.size()) + " stems (" + names.joinIntoString (", ") + ", muted)"
                 + (skipped.isEmpty() ? juce::String() : ", empty: " + skipped.joinIntoString (", ")));
    juce::Logger::writeToLog ("Stems from " + juce::File (sourcePath).getFileName() + " -> " + runFolder.getFullPathName()
                              + " residual " + juce::String (r.stemResidualDb, 1) + " dB, confidence "
                              + juce::String (juce::roundToInt (r.stemConfidence * 100)) + "%");
    pushMixerToEngine();
    requestAllMedia();
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
    if (analysisUndo->tracks.has_value())
    {
        s->tracks = std::move (*analysisUndo->tracks);
        // Back to the analysis folder, so that the stems can be added again.
        if (analysisUndo->stemsTo.isDirectory() && ! analysisUndo->stemsFrom.exists())
            analysisUndo->stemsTo.moveFileTo (analysisUndo->stemsFrom);
        pushMixerToEngine();
        requestAllMedia();
    }
    analysisUndo.reset();
    rebuildPlan();
    markDirty();
    juce::Logger::writeToLog ("Analysis apply undone: " + s->title);
    sendChangeMessage();
    return {};
}

} // namespace b4nd
