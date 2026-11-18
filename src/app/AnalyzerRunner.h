#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <memory>
#include <mutex>

#include "core/TempoMap.h"
#include "engine/PlaybackPlan.h"
#include "project/ProjectModel.h"

namespace b4nd
{

/** result.json of one analyzer run (docs/07 §4), parsed. Everything is a draft. */
struct AnalysisResult
{
    bool valid = false;
    juce::String status;                 // done | partial | failed
    juce::StringArray errors;
    double durationSec = 0.0;

    bool hasTempo = false;
    double bpm = 0.0, bpmConfidence = -1.0;
    std::vector<double> bpmAlternatives;
    double gridStartSec = 0.0;           // bar 1 beat 1
    std::vector<TempoPoint> tempoMap;    // beats from bar 1
    bool variableTempo = false;
    int numerator = 4, denominator = 4;
    double timeSigConfidence = -1.0;

    bool hasKey = false;
    juce::String key;                    // "E minor"
    double keyConfidence = -1.0;
    juce::StringArray keyCandidates;

    bool hasChords = false;
    std::vector<ChordData> chords;

    struct Section
    {
        juce::String label;              // "Verse 1"
        juce::String section;            // intro | verse | ... (docs/08 cue_point.section)
        double startSec = 0.0;
        int bar = 0;                     // 0-based bar from the grid start
        double confidence = -1.0;
    };
    bool hasSections = false;
    std::vector<Section> sections;

    struct Stem
    {
        juce::String name;               // vocals | drums | bass | guitar | piano | other
        juce::String relPath;            // relative to the result folder
        double energyDb = 0.0;           // stem RMS against the mix RMS
        bool empty = false;              // nearly silent: nothing of this instrument in the song
        bool clipped = false;
    };
    bool hasStems = false;
    std::vector<Stem> stems;
    double stemResidualDb = 0.0;         // mix minus the sum of the stems, against the mix
    juce::StringArray stemWarnings;
    juce::String stemModel;
    double stemConfidence = -1.0;        // 0..1 from the residual (docs/07 quality check)

    struct MidiNote { double startSec, endSec; int pitch, velocity; double confidence; };
    struct MidiPart
    {
        juce::String name;               // bass | drums | piano | ... (the stem it came from)
        juce::String sourceId;           // track id of the stem, or "stems/<name>" (separated in the same run)
        int program = 0;                 // GM program suggestion (not written to the file)
        bool drums = false;              // GM drum map, channel 10
        double confidence = -1.0;        // mean note confidence
        std::vector<MidiNote> notes;     // in the source file's time
    };
    bool hasMidi = false;
    std::vector<MidiPart> midi;

    juce::String sourceTrackId;          // the track that was analysed (from the job)
    double sourceStartSec = 0.0;         // where its audio starts on the song timeline

    juce::String versionsJson;

    /** Parses result.json text. Never throws; `valid` is false with a reason in errors. */
    static AnalysisResult parse (const juce::String& json);
};

/**
    Runs the Python analyzer as a separate process (docs/07: offline, never inside the MTR).

    The analyzer is found, in this order:
      1. an explicit command (setCommand: the standalone b4nd-analyzer executable, or a Python
         interpreter that can import numpy and scipy),
      2. B4ND_ANALYZER (path of the standalone executable) or B4ND_ANALYZER_PYTHON,
      3. b4nd-analyzer/b4nd-analyzer(.exe) next to the app (standalone build, no Python needed),
      4. Python ("py -3" / "python" on Windows, "python3" elsewhere) with the analyzer package
         copied next to the app (analyzer/b4nd_analyzer).

    One run at a time. The audio to analyse is written as a mono WAV in song time (silence up to
    the track start), so all result times are song times. Progress comes from the analyzer's
    JSON lines; cancel() kills the process.
*/
class AnalyzerRunner : private juce::Thread
{
public:
    AnalyzerRunner();
    ~AnalyzerRunner() override;

    enum class State { Idle, Preparing, Running, Done, Failed, Cancelled };
    struct Status
    {
        State state = State::Idle;
        juce::String songId;
        juce::String task;          // current analyzer task
        float progress = 0.0f;      // 0..1 over all tasks
        juce::String message;       // failure reason / partial-result note
        double elapsedSec = 0.0;
        int exitCode = -1;
    };

    struct Job
    {
        juce::String songId;
        juce::File outDir;                              // analysis/<song-id>/ in the package
        juce::StringArray tasks { "tempo", "key", "chords", "structure" };
        std::shared_ptr<const AudioData> media;         // decoded source (shared, read-only)
        double mediaRate = 0.0;
        double mediaStartSec = 0.0;                     // where the media starts on the song timeline
        juce::String sourceTrackId;                     // written to the result, for stems
        double timeoutSec = 0.0;                        // 0: the runner's default
        struct MidiInput { juce::String name, path, id; };
        std::vector<MidiInput> midiInputs;              // audio to MIDI: stem files (absolute paths)
        /** Analysis and stems need the decoded media; MIDI alone reads its input files. */
        bool needsMedia() const;
    };

    /** Explicit analyzer: an executable path, or a Python interpreter. Empty: search. */
    void setCommand (const juce::String& pathOrEmpty);
    juce::String getCommand() const;

    /** The command line prefix that would be used (empty if nothing is found) and a
        human-readable description of where it came from. */
    juce::StringArray resolveCommand (juce::String& description) const { return resolveCommand (getCommand(), description); }
    static juce::StringArray resolveCommand (const juce::String& explicitCommand, juce::String& description);

    /** Blocking (up to ~20 s): runs "info" and returns its JSON line, or "" with error set.
        Static so it can run on a background thread that does not depend on this object. */
    static juce::String probe (const juce::String& explicitCommand, juce::String& error);

    /** Starts a run. Returns "" or the reason it cannot start. */
    juce::String start (Job job);
    void cancel();
    bool isBusy() const;
    Status getStatus() const;

    /** The parsed result of the last finished run (valid only after Done). */
    AnalysisResult getResult() const;

    /** Seconds after which a run is cancelled (a hung analyzer must not block the next one).
        A job can ask for more (stem separation on a CPU is slow). */
    void setTimeoutSeconds (double s) { timeoutSec = s; }

    static constexpr int maxSeconds = 20 * 60;   // longest audio sent to the analyzer

private:
    void run() override;
    void setState (State s, const juce::String& message = {});
    bool writeInputWav (const Job& job, const juce::File& wav, juce::String& error);
    bool writeStemsInputWav (const Job& job, const juce::File& wav, juce::String& error);

    mutable std::mutex lock;
    Status status;
    AnalysisResult result;
    Job job;
    juce::String command;
    std::unique_ptr<juce::ChildProcess> process;
    mutable std::atomic<bool> cancelRequested { false };
    juce::uint32 startedMs = 0;
    double timeoutSec = 30.0 * 60.0;
    double jobTimeoutSec = 30.0 * 60.0;   // of the running job
};

} // namespace b4nd
