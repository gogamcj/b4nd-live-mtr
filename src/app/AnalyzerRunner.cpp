#include "app/AnalyzerRunner.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace b4nd
{

namespace
{
double num (const juce::var& v, double fallback = -1.0)
{
    if (v.isDouble() || v.isInt() || v.isInt64())
    {
        const double d = (double) v;
        return std::isfinite (d) ? d : fallback;
    }
    return fallback;
}

bool validSection (const juce::String& s)
{
    static const juce::StringArray ok { "intro", "verse", "pre_chorus", "chorus", "bridge", "solo", "outro", "custom" };
    return ok.contains (s);
}

// Share of the whole run per analyzer task, for one progress bar. The analyzer runs stems, then
// MIDI, then the analysis (measured on a 4 min song: stems take most of the time).
float overallProgress (const juce::String& task, float p, bool withStems, bool withMidi, bool withAnalysis)
{
    static const std::map<juce::String, std::pair<float, float>> w {
        { "load", { 0.00f, 0.05f } }, { "features", { 0.05f, 0.35f } }, { "tempo", { 0.40f, 0.25f } },
        { "key", { 0.65f, 0.05f } }, { "chords", { 0.70f, 0.15f } }, { "structure", { 0.85f, 0.15f } } };
    p = juce::jlimit (0.0f, 1.0f, p);
    const float ws = withStems ? 8.0f : 0.0f, wm = withMidi ? 2.0f : 0.0f, wa = withAnalysis ? 1.0f : 0.0f;
    const float total = std::max (1e-6f, ws + wm + wa);
    if (task == "stems")
        return ws / total * p;
    if (task == "midi")
        return (ws + wm * p) / total;
    const auto it = w.find (task);
    if (it == w.end())
        return -1.0f;
    return (ws + wm + wa * (it->second.first + it->second.second * p)) / total;
}

bool stemNameOk (const juce::String& n)
{
    return n.isNotEmpty() && n.length() <= 32 && n.containsOnly ("abcdefghijklmnopqrstuvwxyz0123456789_");
}

const char* bootstrap =
    "import sys; sys.path.insert(0, sys.argv.pop(1)); from b4nd_analyzer.cli import main; sys.exit(main())";

juce::File appDir()
{
    return juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();
}

juce::File packageDir()
{
    if (auto env = juce::SystemStats::getEnvironmentVariable ("B4ND_ANALYZER_DIR", {}); env.isNotEmpty())
        return juce::File (env);
    const auto next = appDir().getChildFile ("analyzer");
    if (next.getChildFile ("b4nd_analyzer").isDirectory())
        return next;
   #ifdef B4ND_SOURCE_ANALYZER_DIR
    return juce::File (B4ND_SOURCE_ANALYZER_DIR);   // development build run from the build tree
   #else
    return next;
   #endif
}

bool looksLikePython (const juce::File& f)
{
    return f.getFileNameWithoutExtension().startsWithIgnoreCase ("python") || f.getFileNameWithoutExtension() == "py";
}

juce::StringArray pythonCommand (const juce::String& python)
{
    juce::StringArray a;
    a.add (python);
    if (juce::File::isAbsolutePath (python) ? juce::File (python).getFileNameWithoutExtension() == "py" : python == "py")
        a.add ("-3");
    a.add ("-c");
    a.add (bootstrap);
    a.add (packageDir().getFullPathName());
    return a;
}

/** Path of a program on PATH, or "" (ChildProcess needs a real file on Windows). */
juce::String onPath (const juce::String& name)
{
    const auto path = juce::SystemStats::getEnvironmentVariable ("PATH", {});
   #if JUCE_WINDOWS
    const auto dirs = juce::StringArray::fromTokens (path, ";", "\"");
    const juce::StringArray exts { ".exe", ".bat", "" };
   #else
    const auto dirs = juce::StringArray::fromTokens (path, ":", "");
    const juce::StringArray exts { "" };
   #endif
    for (const auto& d : dirs)
    {
        if (d.isEmpty() || ! juce::File::isAbsolutePath (d))
            continue;
        for (const auto& e : exts)
        {
            const auto f = juce::File (d).getChildFile (name + e);
            if (f.existsAsFile())
            {
               #if JUCE_WINDOWS
                // The Microsoft Store stub opens the Store instead of running Python.
                if (f.getFullPathName().containsIgnoreCase ("WindowsApps"))
                    continue;
               #endif
                return f.getFullPathName();
            }
        }
    }
    return {};
}
} // namespace

AnalysisResult AnalysisResult::parse (const juce::String& json)
{
    AnalysisResult r;
    juce::var v;
    const auto parsed = juce::JSON::parse (json, v);
    if (parsed.failed() || ! v.isObject())
    {
        r.errors.add ("result.json is not valid JSON: " + parsed.getErrorMessage());
        return r;
    }
    r.status = v["status"].toString();
    if (auto* e = v["errors"].getArray())
        for (const auto& x : *e)
            r.errors.add (x.toString());
    r.durationSec = num (v["durationSec"], 0.0);

    if (const auto& b = v["bpm"]; b.isObject())
    {
        r.bpm = num (b["value"], 0.0);
        r.bpmConfidence = juce::jlimit (-1.0, 1.0, num (b["confidence"]));
        if (auto* alt = b["alternatives"].getArray())
            for (const auto& x : *alt)
                if (const double d = num (x, 0.0); d >= 20.0 && d <= 400.0)
                    r.bpmAlternatives.push_back (d);
        r.gridStartSec = std::max (0.0, num (v["gridStartSec"], 0.0));
        if (auto* m = v["tempoMap"].getArray())
            for (const auto& p : *m)
            {
                const double beat = num (p["beat"], -1.0), bpm = num (p["bpm"], 0.0);
                if (beat >= 0.0 && bpm >= 20.0 && bpm <= 400.0)
                    r.tempoMap.push_back ({ beat, bpm });
            }
        r.variableTempo = (bool) v["variableTempo"];
        const auto ts = juce::StringArray::fromTokens (v["timeSig"].toString(), "/", "");
        if (ts.size() == 2 && ts[0].getIntValue() >= 1 && ts[0].getIntValue() <= 16)
        {
            r.numerator = ts[0].getIntValue();
            r.denominator = juce::jlimit (1, 16, ts[1].getIntValue());
        }
        r.timeSigConfidence = num (v["timeSigConfidence"]);
        r.hasTempo = r.bpm >= 20.0 && r.bpm <= 400.0 && ! r.tempoMap.empty();
    }

    if (const auto& k = v["key"]; k.isObject())
    {
        r.key = k["value"].toString();
        r.keyConfidence = num (k["confidence"]);
        if (auto* c = k["candidates"].getArray())
            for (const auto& x : *c)
                r.keyCandidates.add (x["value"].toString());
        r.hasKey = r.key.isNotEmpty();
    }

    if (auto* c = v["chords"].getArray())
    {
        for (const auto& x : *c)
        {
            ChordData d;
            d.startSec = num (x["startSec"], -1.0);
            d.endSec = num (x["endSec"], -1.0);
            d.chord = x["chord"].toString().substring (0, 16).toStdString();
            d.confidence = num (x["confidence"]);
            if (d.startSec >= 0.0 && d.endSec > d.startSec && ! d.chord.empty())
                r.chords.push_back (d);
        }
        std::sort (r.chords.begin(), r.chords.end(), [] (const ChordData& a, const ChordData& b) { return a.startSec < b.startSec; });
        r.hasChords = true;
    }

    if (auto* s = v["sections"].getArray())
    {
        for (const auto& x : *s)
        {
            Section sec;
            sec.label = x["label"].toString().substring (0, 40);
            sec.section = x["section"].toString();
            if (! validSection (sec.section))
                sec.section = "custom";
            sec.startSec = num (x["startSec"], -1.0);
            sec.bar = (int) num (x["bar"], 0.0);
            sec.confidence = num (x["confidence"]);
            if (sec.startSec >= 0.0 && sec.label.isNotEmpty())
                r.sections.push_back (sec);
        }
        r.hasSections = true;
    }

    if (const auto& st = v["stems"]; st.isObject())
    {
        if (auto* files = st["files"].getArray())
            for (const auto& f : *files)
            {
                Stem x;
                x.name = f["name"].toString();
                x.relPath = f["path"].toString();
                x.energyDb = num (f["energyDb"], -120.0);
                x.empty = (bool) f["empty"];
                x.clipped = (bool) f["clipped"];
                // Only plain names under stems/: the path is joined to the package folder.
                if (stemNameOk (x.name) && x.relPath == "stems/" + x.name + ".wav")
                    r.stems.push_back (x);
                else
                    r.errors.add ("stems: ignored a bad stem entry");
            }
        r.stemResidualDb = num (st["residualDb"], 0.0);
        if (auto* w = st["warnings"].getArray())
            for (const auto& x : *w)
                r.stemWarnings.add (x.toString().substring (0, 200));
        r.stemModel = st["model"].toString().substring (0, 40);
        // -40 dB residual or better: the stems add up to the mix (0.9); -10 dB: a lot is missing (0.3).
        r.stemConfidence = juce::jlimit (0.3, 0.9, 0.3 + (-10.0 - r.stemResidualDb) / 30.0 * 0.6);
        r.hasStems = ! r.stems.empty();
    }
    if (const auto& m = v["midi"]; m.isObject())
    {
        if (auto* tracks = m["tracks"].getArray())
            for (const auto& t : *tracks)
            {
                MidiPart part;
                part.name = t["name"].toString().substring (0, 32);
                part.sourceId = t["sourceId"].toString().substring (0, 64);
                part.program = juce::jlimit (0, 127, (int) num (t["program"], 0.0));
                part.drums = (bool) t["drums"];
                part.confidence = juce::jlimit (-1.0, 1.0, num (t["confidence"]));
                if (! stemNameOk (part.name))
                {
                    r.errors.add ("midi: ignored a bad part name");
                    continue;
                }
                if (auto* notes = t["notes"].getArray())
                    for (const auto& n : *notes)
                    {
                        auto* a = n.getArray();
                        if (a == nullptr || a->size() < 5 || part.notes.size() >= 200000)
                            continue;
                        MidiNote x { num ((*a)[0], -1.0), num ((*a)[1], -1.0), (int) num ((*a)[2], -1.0),
                                     (int) num ((*a)[3], -1.0), juce::jlimit (0.0, 1.0, num ((*a)[4], 0.0)) };
                        if (x.startSec >= 0.0 && x.endSec > x.startSec && x.endSec - x.startSec < 60.0
                            && x.pitch >= 0 && x.pitch <= 127 && x.velocity >= 1 && x.velocity <= 127)
                            part.notes.push_back (x);
                    }
                r.midi.push_back (std::move (part));
            }
        r.hasMidi = ! r.midi.empty();
    }
    if (const auto& j = v["job"]; j.isObject())
    {
        r.sourceTrackId = j["sourceTrackId"].toString().substring (0, 64);
        r.sourceStartSec = std::max (0.0, num (j["sourceStartSec"], 0.0));
    }

    if (v["versions"].isObject())
        r.versionsJson = juce::JSON::toString (v["versions"], true);
    r.valid = r.status == "done" || r.status == "partial";
    return r;
}

bool AnalyzerRunner::Job::needsMedia() const
{
    for (const auto* t : { "tempo", "key", "chords", "structure", "stems" })
        if (tasks.contains (t))
            return true;
    return false;
}

AnalyzerRunner::AnalyzerRunner() : juce::Thread ("B4ND analyzer") {}

AnalyzerRunner::~AnalyzerRunner()
{
    cancel();
    stopThread (10000);
}

void AnalyzerRunner::setCommand (const juce::String& pathOrEmpty)
{
    std::lock_guard<std::mutex> g (lock);
    command = pathOrEmpty.trim();
}

juce::String AnalyzerRunner::getCommand() const
{
    std::lock_guard<std::mutex> g (lock);
    return command;
}

juce::StringArray AnalyzerRunner::resolveCommand (const juce::String& explicitCmd, juce::String& description)
{
    auto fromPath = [&] (const juce::String& p, const juce::String& source) -> juce::StringArray {
        const juce::File f (p);
        if (! f.existsAsFile())
            return {};
        description = source + ": " + p;
        if (looksLikePython (f))
            return pythonCommand (p);
        return { p };
    };
    if (explicitCmd.isNotEmpty())
    {
        if (auto c = fromPath (explicitCmd, "Settings"); ! c.isEmpty())
            return c;
        description = "Settings: " + explicitCmd + " (NOT FOUND)";
        return {};
    }
    if (auto e = juce::SystemStats::getEnvironmentVariable ("B4ND_ANALYZER", {}); e.isNotEmpty())
        if (auto c = fromPath (e, "B4ND_ANALYZER"); ! c.isEmpty())
            return c;
    if (auto e = juce::SystemStats::getEnvironmentVariable ("B4ND_ANALYZER_PYTHON", {}); e.isNotEmpty())
        if (juce::File (e).existsAsFile())
        {
            description = "B4ND_ANALYZER_PYTHON: " + e;
            return pythonCommand (e);
        }
   #if JUCE_WINDOWS
    const auto standalone = appDir().getChildFile ("b4nd-analyzer").getChildFile ("b4nd-analyzer.exe");
   #else
    const auto standalone = appDir().getChildFile ("b4nd-analyzer").getChildFile ("b4nd-analyzer");
   #endif
    if (standalone.existsAsFile())
    {
        description = "Standalone: " + standalone.getFullPathName();
        return { standalone.getFullPathName() };
    }
    if (! packageDir().getChildFile ("b4nd_analyzer").isDirectory())
    {
        description = "No analyzer found next to the app";
        return {};
    }
   #if JUCE_WINDOWS
    for (const auto* name : { "python", "py", "python3" })   // "py" may pick a newer Python without numpy
   #else
    for (const auto* name : { "python3", "python" })
   #endif
        if (const auto p = onPath (name); p.isNotEmpty())
        {
            description = "Python: " + p + " + " + packageDir().getFullPathName();
            return pythonCommand (p);
        }
    description = "Python not found (install Python 3 with numpy and scipy, or the standalone analyzer)";
    return {};
}

juce::String AnalyzerRunner::probe (const juce::String& explicitCommand, juce::String& error)
{
    juce::String where;
    auto cmd = resolveCommand (explicitCommand, where);
    if (cmd.isEmpty())
    {
        error = where;
        return {};
    }
    cmd.add ("info");
    juce::ChildProcess p;
    if (! p.start (cmd, juce::ChildProcess::wantStdOut))
    {
        error = "Cannot start " + where;
        return {};
    }
    const auto out = p.readAllProcessOutput();
    if (! p.waitForProcessToFinish (20000))
    {
        p.kill();
        error = "The analyzer did not answer (" + where + ")";
        return {};
    }
    for (const auto& line : juce::StringArray::fromLines (out))
        if (line.trim().startsWith ("{") && line.contains ("b4nd_analyzer"))
            return line.trim();
    error = "The analyzer cannot run (" + where + "): " + out.trim().substring (0, 300);
    return {};
}

bool AnalyzerRunner::isBusy() const
{
    std::lock_guard<std::mutex> g (lock);
    return status.state == State::Preparing || status.state == State::Running;
}

juce::String AnalyzerRunner::start (Job j)
{
    if (isBusy() || isThreadRunning())
        return "An analysis is already running";
    if (j.needsMedia())
    {
        if (j.media == nullptr || j.media->numFrames <= 0 || j.mediaRate <= 0.0)
            return "The track's audio is not loaded";
        if ((double) j.media->numFrames / j.mediaRate + j.mediaStartSec > maxSeconds)
            return "Longer than 20 minutes: not analysed";
    }
    if (j.tasks.contains ("midi") && j.midiInputs.empty() && ! j.tasks.contains ("stems"))
        return "Nothing to convert to MIDI: separate stems first";
    if (! j.outDir.createDirectory())
        return "Cannot create " + j.outDir.getFullPathName();
    stopThread (1000);
    {
        std::lock_guard<std::mutex> g (lock);
        job = std::move (j);
        status = {};
        status.state = State::Preparing;
        status.songId = job.songId;
        result = {};
        jobTimeoutSec = std::max (timeoutSec, job.timeoutSec);
    }
    cancelRequested = false;
    startedMs = juce::Time::getMillisecondCounter();
    startThread (juce::Thread::Priority::low);
    return {};
}

void AnalyzerRunner::cancel()
{
    cancelRequested = true;
    std::lock_guard<std::mutex> g (lock);
    if (process != nullptr)
        process->kill();
}

AnalyzerRunner::Status AnalyzerRunner::getStatus() const
{
    std::lock_guard<std::mutex> g (lock);
    auto s = status;
    if (s.state == State::Preparing || s.state == State::Running)
    {
        s.elapsedSec = (juce::Time::getMillisecondCounter() - startedMs) / 1000.0;
        if (s.elapsedSec > jobTimeoutSec && process != nullptr)
        {
            // A hung analyzer: kill it (the run thread then reports the timeout).
            cancelRequested = true;
            process->kill();
        }
    }
    return s;
}

AnalysisResult AnalyzerRunner::getResult() const
{
    std::lock_guard<std::mutex> g (lock);
    return result;
}

void AnalyzerRunner::setState (State s, const juce::String& message)
{
    std::lock_guard<std::mutex> g (lock);
    status.state = s;
    status.message = message;
    status.elapsedSec = (juce::Time::getMillisecondCounter() - startedMs) / 1000.0;
}

bool AnalyzerRunner::writeInputWav (const Job& j, const juce::File& wav, juce::String& error)
{
    // Mono, 16-bit, at the decoded rate, in song time: silence before the track starts.
    wav.deleteFile();
    auto stream = wav.createOutputStream();
    if (stream == nullptr)
    {
        error = "Cannot write " + wav.getFullPathName();
        return false;
    }
    juce::WavAudioFormat fmt;
    std::unique_ptr<juce::OutputStream> os (stream.release());
    auto w = fmt.createWriterFor (os, juce::AudioFormatWriterOptions{}.withSampleRate (j.mediaRate).withNumChannels (1)
                                                                      .withBitsPerSample (16));
    if (w == nullptr)
    {
        error = "Cannot create the analysis WAV";
        return false;
    }
    const auto& m = *j.media;
    const int block = 65536;
    juce::AudioBuffer<float> buf (1, block);
    auto lead = (std::int64_t) std::llround (std::max (0.0, j.mediaStartSec) * j.mediaRate);
    while (lead > 0)
    {
        const int n = (int) std::min<std::int64_t> (block, lead);
        buf.clear();
        if (! w->writeFromAudioSampleBuffer (buf, 0, n))
            return error = "Writing the analysis WAV failed", false;
        lead -= n;
    }
    const float g = m.numChannels > 1 ? 0.5f : 1.0f;
    for (std::int64_t pos = 0; pos < m.numFrames; pos += block)
    {
        if (cancelRequested || threadShouldExit())
            return error = "Cancelled", false;
        const int n = (int) std::min<std::int64_t> (block, m.numFrames - pos);
        auto* d = buf.getWritePointer (0);
        for (int i = 0; i < n; ++i)
        {
            float s = m.channel (0)[pos + i];
            if (m.numChannels > 1)
                s += m.channel (1)[pos + i];
            d[i] = s * g;
        }
        if (! w->writeFromAudioSampleBuffer (buf, 0, n))
            return error = "Writing the analysis WAV failed (disk full?)", false;
    }
    w.reset();
    return true;
}

bool AnalyzerRunner::writeStemsInputWav (const Job& j, const juce::File& wav, juce::String& error)
{
    // Stereo, 24-bit, in media time (the stems get the source track's start): separation needs
    // the stereo image and the full resolution, unlike the analysis.
    wav.deleteFile();
    auto stream = wav.createOutputStream();
    if (stream == nullptr)
        return error = "Cannot write " + wav.getFullPathName(), false;
    juce::WavAudioFormat fmt;
    std::unique_ptr<juce::OutputStream> os (stream.release());
    auto w = fmt.createWriterFor (os, juce::AudioFormatWriterOptions{}.withSampleRate (j.mediaRate).withNumChannels (2)
                                                                      .withBitsPerSample (24));
    if (w == nullptr)
        return error = "Cannot create the stem input WAV", false;
    const auto& m = *j.media;
    const int block = 65536;
    juce::AudioBuffer<float> buf (2, block);
    for (std::int64_t pos = 0; pos < m.numFrames; pos += block)
    {
        if (cancelRequested || threadShouldExit())
            return error = "Cancelled", false;
        const int n = (int) std::min<std::int64_t> (block, m.numFrames - pos);
        for (int c = 0; c < 2; ++c)
        {
            const float* src = m.channel (std::min (c, m.numChannels - 1)) + pos;
            std::copy (src, src + n, buf.getWritePointer (c));
        }
        if (! w->writeFromAudioSampleBuffer (buf, 0, n))
            return error = "Writing the stem input WAV failed (disk full?)", false;
    }
    w.reset();
    return true;
}

void AnalyzerRunner::run()
{
    Job j;
    {
        std::lock_guard<std::mutex> g (lock);
        j = job;
    }
    const auto wav = j.outDir.getChildFile ("input.wav");
    const auto jobFile = j.outDir.getChildFile ("job.json");
    const auto resultFile = j.outDir.getChildFile ("result.json");
    juce::String error;
    const bool needsMedia = j.needsMedia();
    if (needsMedia && ! writeInputWav (j, wav, error))
    {
        wav.deleteFile();
        setState (cancelRequested ? State::Cancelled : State::Failed, error);
        return;
    }
    const bool withStems = j.tasks.contains ("stems");
    const bool withMidi = j.tasks.contains ("midi");
    bool withAnalysis = false;
    for (const auto* t : { "tempo", "key", "chords", "structure" })
        withAnalysis = withAnalysis || j.tasks.contains (t);
    const auto stemsWav = j.outDir.getChildFile ("stems_input.wav");
    if (withStems)
    {
        // Old stem files of this song's previous run: the new run replaces them all.
        j.outDir.getChildFile ("stems").deleteRecursively();
        if (! writeStemsInputWav (j, stemsWav, error))
        {
            wav.deleteFile();
            stemsWav.deleteFile();
            setState (cancelRequested ? State::Cancelled : State::Failed, error);
            return;
        }
    }
    j.media.reset();   // the WAVs are written: do not hold the decoded audio longer than needed

    auto* o = new juce::DynamicObject();
    o->setProperty ("input", needsMedia ? wav.getFileName() : juce::String());
    if (withMidi)
    {
        juce::Array<juce::var> inputs;
        for (const auto& m : j.midiInputs)
        {
            auto* x = new juce::DynamicObject();
            x->setProperty ("name", m.name);
            x->setProperty ("path", m.path);
            x->setProperty ("id", m.id);
            inputs.add (juce::var (x));
        }
        o->setProperty ("midiInputs", inputs);
    }
    juce::Array<juce::var> tasks;
    for (const auto& t : j.tasks)
        tasks.add (t);
    o->setProperty ("tasks", tasks);
    o->setProperty ("device", "cpu");
    o->setProperty ("out", ".");
    if (withStems)
        o->setProperty ("stemsInput", stemsWav.getFileName());
    auto* meta = new juce::DynamicObject();
    meta->setProperty ("sourceTrackId", j.sourceTrackId);
    meta->setProperty ("sourceStartSec", j.mediaStartSec);
    o->setProperty ("meta", juce::var (meta));
    resultFile.deleteFile();
    if (! jobFile.replaceWithText (juce::JSON::toString (juce::var (o))))
    {
        wav.deleteFile();
        stemsWav.deleteFile();
        setState (State::Failed, "Cannot write " + jobFile.getFullPathName());
        return;
    }

    juce::String where;
    auto cmd = resolveCommand (where);
    if (cmd.isEmpty())
    {
        wav.deleteFile();
        stemsWav.deleteFile();
        setState (State::Failed, where);
        return;
    }
    cmd.add ("run");
    cmd.add ("--job");
    cmd.add (jobFile.getFullPathName());

    {
        std::lock_guard<std::mutex> g (lock);
        process = std::make_unique<juce::ChildProcess>();
        if (cancelRequested || ! process->start (cmd, juce::ChildProcess::wantStdOut))
        {
            process.reset();
            wav.deleteFile();
            stemsWav.deleteFile();
            status.state = cancelRequested ? State::Cancelled : State::Failed;
            status.message = cancelRequested ? "Cancelled" : "Cannot start the analyzer (" + where + ")";
            return;
        }
        status.state = State::Running;
        status.task = "load";
    }

    juce::String pending;
    juce::String lastError;
    char buf[4096];
    for (;;)
    {
        const int n = process->readProcessOutput (buf, (int) sizeof (buf));
        if (n <= 0)
        {
            if (! process->isRunning())
                break;
            juce::Thread::sleep (20);
            continue;
        }
        pending += juce::String::fromUTF8 (buf, n);
        int nl;
        while ((nl = pending.indexOfChar ('\n')) >= 0)
        {
            const auto line = pending.substring (0, nl).trim();
            pending = pending.substring (nl + 1);
            juce::var v;
            if (! line.startsWith ("{") || juce::JSON::parse (line, v).failed())
                continue;
            if (v["task"].isString())
            {
                const auto p = overallProgress (v["task"].toString(), (float) num (v["progress"], 0.0), withStems, withMidi, withAnalysis);
                std::lock_guard<std::mutex> g (lock);
                status.task = v["task"].toString();
                if (p >= 0.0f)
                    status.progress = std::max (status.progress, p);
            }
            if (v["error"].isString())
                lastError = v["error"].toString();
        }
    }
    // The loop above ends when isRunning() has reaped the process, which stores the exit code.
    // Asking isRunning() again (waitForProcessToFinish) would overwrite it with 0 on POSIX.
    const int code = (int) process->getExitCode();
    {
        std::lock_guard<std::mutex> g (lock);
        process.reset();
        status.exitCode = code;
    }
    wav.deleteFile();   // ~10 MB per song; the result keeps everything that is needed
    stemsWav.deleteFile();

    if (cancelRequested)
    {
        const bool timedOut = (juce::Time::getMillisecondCounter() - startedMs) / 1000.0 > jobTimeoutSec;
        setState (State::Cancelled, timedOut ? "Timed out" : "Cancelled");
        return;
    }
    if (! resultFile.existsAsFile())
    {
        setState (State::Failed, lastError.isNotEmpty() ? lastError
                                                        : "The analyzer stopped without a result (exit code " + juce::String (code) + ")");
        return;
    }
    auto r = AnalysisResult::parse (resultFile.loadFileAsString());
    const bool ok = r.valid && (code == 0 || code == 3);
    juce::String msg;
    if (! ok)
        msg = r.errors.isEmpty() ? "Analysis failed (exit code " + juce::String (code) + ")" : r.errors.joinIntoString ("; ");
    else if (! r.errors.isEmpty())
        msg = "Partial: " + r.errors.joinIntoString ("; ");
    {
        std::lock_guard<std::mutex> g (lock);
        result = std::move (r);
        status.progress = ok ? 1.0f : status.progress;
    }
    setState (ok ? State::Done : State::Failed, msg);
}

} // namespace b4nd
