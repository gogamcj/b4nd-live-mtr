#include <cstdio>
#include <optional>

#include <juce_gui_extra/juce_gui_extra.h>

#include "app/AppState.h"
#include "app/MainWindow.h"
#include "ui/AudioSettingsPanel.h"
#include "ui/ModulePanel.h"
#include "ui/PluginPanel.h"
#include "ui/ProjectCommands.h"
#include "ui/B4ndLookAndFeel.h"

namespace b4nd
{

static juce::File getLogDirectory()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("B4ND LIVE MTR")
        .getChildFile ("logs");
}

static void crashHandler (void*)
{
    // Best effort: write the stack trace next to the normal log (Logging & Recovery Engine, docs/01).
    auto f = getLogDirectory().getChildFile ("crash-" + juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S") + ".txt");
    f.replaceWithText (juce::SystemStats::getStackBacktrace());
}

/** Optional verification mode used by CI and development:
      --screenshot <file.png> [--live] [--size WxH] [--song N] [--at SECONDS]
                   [--media <audio file>]... [--with-audio] [--play SECONDS]
    Renders the UI to a PNG and exits. No audio device is opened unless --with-audio.
    --media adds audio tracks (as if dropped on the timeline) and waits until they are decoded.
    --play plays the song for that long first and logs the peak level per output.
    --project <folder> opens a project package first; --save-as <folder> saves into a new one.
    --module <name> adds a sound module profile (no port), --midi <file.mid> adds an external MIDI
    track (as if dropped), --modules captures the MIDI module window instead of the main window.
    --vsti <plugin.vst3> scans the plugin (scanner process) and adds a VSTi track with it,
    --vsti-midi <file.mid> gives that track its notes, --plugins captures the plugin window.
    Without --screenshot, the last opened project is reopened at start-up. */
struct ScreenshotRequest
{
    juce::File file;
    bool live = false;
    bool settings = false;   // capture the audio settings window instead
    bool modulesWindow = false, pluginsWindow = false;
    juce::StringArray midiFiles, moduleNames, vstiPlugins, vstiMidi;
    int width = 1600, height = 960;
    int song = 0;
    double atSeconds = 0.0;
    juce::StringArray media;
    double playSeconds = 0.0;
    juce::File project, saveAs;

    static std::optional<ScreenshotRequest> parse (const juce::StringArray& args)
    {
        const int i = args.indexOf ("--screenshot");
        if (i < 0 || i + 1 >= args.size())
            return std::nullopt;

        ScreenshotRequest r;
        r.file = juce::File::getCurrentWorkingDirectory().getChildFile (args[i + 1]);
        r.live = args.contains ("--live");
        r.settings = args.contains ("--audio-settings");
        r.modulesWindow = args.contains ("--modules");
        r.pluginsWindow = args.contains ("--plugins");
        if (const int s = args.indexOf ("--size"); s >= 0 && s + 1 < args.size())
        {
            r.width = juce::jmax (640, args[s + 1].upToFirstOccurrenceOf ("x", false, true).getIntValue());
            r.height = juce::jmax (480, args[s + 1].fromFirstOccurrenceOf ("x", false, true).getIntValue());
        }
        if (const int s = args.indexOf ("--song"); s >= 0 && s + 1 < args.size())
            r.song = args[s + 1].getIntValue();
        if (const int s = args.indexOf ("--at"); s >= 0 && s + 1 < args.size())
            r.atSeconds = args[s + 1].getDoubleValue();
        if (const int s = args.indexOf ("--play"); s >= 0 && s + 1 < args.size())
            r.playSeconds = args[s + 1].getDoubleValue();
        const auto cwd = juce::File::getCurrentWorkingDirectory();
        if (const int s = args.indexOf ("--project"); s >= 0 && s + 1 < args.size())
            r.project = cwd.getChildFile (args[s + 1].unquoted());
        if (const int s = args.indexOf ("--save-as"); s >= 0 && s + 1 < args.size())
            r.saveAs = cwd.getChildFile (args[s + 1].unquoted());
        for (int k = 0; k + 1 < args.size(); ++k)
        {
            if (args[k] == "--media")
                r.media.add (cwd.getChildFile (args[k + 1].unquoted()).getFullPathName());
            if (args[k] == "--midi")
                r.midiFiles.add (cwd.getChildFile (args[k + 1].unquoted()).getFullPathName());
            if (args[k] == "--module")
                r.moduleNames.add (args[k + 1].unquoted());
            if (args[k] == "--vsti")
                r.vstiPlugins.add (cwd.getChildFile (args[k + 1].unquoted()).getFullPathName());
            if (args[k] == "--vsti-midi")
                r.vstiMidi.add (cwd.getChildFile (args[k + 1].unquoted()).getFullPathName());
        }
        return r;
    }
};

class B4ndApplication : public juce::JUCEApplication,
                        private juce::Timer
{
public:
    const juce::String getApplicationName() override    { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise (const juce::String& commandLine) override
    {
        getLogDirectory().createDirectory();
        logger.reset (juce::FileLogger::createDateStampedLogger ("B4ND LIVE MTR/logs", "b4nd_", ".log",
                                                                 "B4ND LIVE MTR " + getApplicationVersion()));
        juce::Logger::setCurrentLogger (logger.get());
        juce::Logger::writeToLog ("OS: " + juce::SystemStats::getOperatingSystemName()
                                  + "  CPU: " + juce::SystemStats::getCpuModel()
                                  + "  RAM: " + juce::String (juce::SystemStats::getMemorySizeInMegabytes()) + " MB");
        juce::SystemStats::setApplicationCrashHandler (crashHandler);

        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);

        screenshot = ScreenshotRequest::parse (juce::StringArray::fromTokens (commandLine, true));

        engine = std::make_unique<AudioEngine>();
        audioDevice = std::make_unique<AudioDeviceController> (*engine);
        state = std::make_unique<AppState> (*engine, *audioDevice);
        // Screenshot/CI mode never touches the audio hardware.
        audioDevice->initialise (getLogDirectory().getParentDirectory().getChildFile ("audio-device.xml"),
                                 ! screenshot.has_value() || commandLine.contains ("--with-audio"));
        window = std::make_unique<MainWindow> (getApplicationName(), *state);

        if (screenshot && screenshot->project != juce::File())
        {
            const auto err = state->openProject (screenshot->project);
            std::printf ("project open: %s\n", err.isEmpty() ? "ok" : err.toRawUTF8());
            printProjectSummary();
        }
        else if (! screenshot)
        {
            // Reopen the last project so a restart (crash, power cut) comes back where it was.
            const auto last = AppState::getLastProjectRecordFile().loadFileAsString().trim();
            if (last.isNotEmpty())
            {
                if (const auto err = state->openProject (juce::File (last)); err.isEmpty())
                    juce::MessageManager::callAsync ([this] { if (state) projectCommands::reviewOpenedProject (*state); });
                else
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Last project not opened",
                                                            err + "\n\nThe demo setlist is shown instead.");
            }
        }

        if (screenshot)
        {
            state->selectSong (screenshot->song);
            state->locate (screenshot->atSeconds);
            window->setFullScreen (false);
            window->setSize (screenshot->width, screenshot->height);
            for (const auto& m : screenshot->media)
                if (const auto err = state->addAudioFile (juce::File (m)); err.isNotEmpty())
                    std::printf ("media rejected: %s\n", err.toRawUTF8());
            for (const auto& spec : screenshot->moduleNames)
            {
                // "Name" or "Name@port": the first MIDI output whose name contains `port`.
                // A "+clock" suffix also sends MIDI Clock to it.
                ModuleProfile p;
                p.clockOut = spec.endsWith ("+clock");
                const auto bare = spec.upToLastOccurrenceOf ("+clock", false, false);
                p.deviceName = bare.upToFirstOccurrenceOf ("@", false, false).toStdString();
                if (const auto want = bare.fromFirstOccurrenceOf ("@", false, false); want.isNotEmpty())
                {
                    for (const auto& port : state->listMidiOutputs())
                        if (port.name.containsIgnoreCase (want))
                        {
                            p.portName = port.name.toStdString();
                            p.portIdentifier = port.identifier.toStdString();
                            break;
                        }
                    std::printf ("module %s: port %s\n", p.deviceName.c_str(), p.portName.empty() ? "NOT FOUND" : p.portName.c_str());
                }
                p.channel = (int) state->getModules().size() + 1;
                p.bankMsb = 87;
                p.program = 12;
                if (const auto err = state->addModule (p); err.isNotEmpty())
                    std::printf ("module rejected: %s\n", err.toRawUTF8());
            }
            for (const auto& m : screenshot->midiFiles)
                if (const auto err = state->addMidiFile (juce::File (m)); err.isNotEmpty())
                    std::printf ("midi rejected: %s\n", err.toRawUTF8());
            printMidiSummary();
            for (int v = 0; v < screenshot->vstiPlugins.size(); ++v)
                addVstiTrack (screenshot->vstiPlugins[v], screenshot->vstiMidi[v]);
            if (! screenshot->vstiPlugins.isEmpty() || state->getPluginSummary().instruments > 0)
                printPluginSummary();
            if (screenshot->settings)
                showAudioSettingsWindow (*state);
            if (screenshot->live)
            {
                // Show the live view at the requested size instead of going full screen.
                window->getMainComponent().onLiveModeChanged = nullptr;
                window->getMainComponent().toggleLiveMode();
            }
            if (screenshot->modulesWindow)
                showModuleWindow (*state, 0);
            if (screenshot->pluginsWindow)
                showPluginWindow (*state);
            startTimer (100);
        }
    }

    void printProjectSummary()
    {
        std::printf ("project: \"%s\" (%d songs), previous session closed cleanly: %s, backups: %d, missing media: %d\n",
                     state->getProjectName().toRawUTF8(), (int) state->getSongs().size(),
                     state->lastOpenHadStaleLock() ? "NO" : "yes", state->getLastOpenBackups().size(),
                     state->getMissingMediaOnOpen().size());
        for (const auto& s : state->getSongs())
        {
            std::printf ("  song \"%s\" %.1f BPM %.1f s, %d tracks\n", s.title.toRawUTF8(), s.tempo.bpmAtBeat (0.0),
                         s.lengthSeconds, (int) s.tracks.size());
            for (const auto& t : s.tracks)
                std::printf ("    track \"%s\" %s %.1f dB%s%s%s\n", t.name.toRawUTF8(), nameForTrackKind (t.kind).toRawUTF8(),
                             (double) t.volumeDb, t.mute ? " MUTE" : "", t.solo ? " SOLO" : "",
                             t.mediaPath.isNotEmpty() ? (" media=" + juce::File (t.mediaPath).getFileName()).toRawUTF8() : "");
        }
        if (state->getPluginSummary().instruments > 0)
            printPluginSummary();
    }

    void addVstiTrack (const juce::String& pluginFile, const juce::String& midiFile)
    {
        auto& catalog = state->getPluginHost().getCatalog();
        if (catalog.needsScan (juce::File (pluginFile)))
        {
            const auto err = state->getPluginHost().scanFileNow (pluginFile);
            std::printf ("vsti scan %s: %s\n", juce::File (pluginFile).getFileName().toRawUTF8(), err.isEmpty() ? "ok" : err.toRawUTF8());
        }
        juce::String uid;
        for (const auto& r : catalog.getRecords())
            if (r.file == pluginFile && r.status != PluginRecord::Status::Blacklisted)
                uid = r.uid();
        if (uid.isEmpty())
        {
            std::printf ("vsti rejected: %s is blacklisted or has no plugin\n", pluginFile.toRawUTF8());
            return;
        }
        if (const auto err = state->setTrackPlugin (-1, uid); err.isNotEmpty())
            std::printf ("vsti: %s\n", err.toRawUTF8());
        if (midiFile.isNotEmpty())
            if (const auto err = state->addMidiFile (juce::File (midiFile), (int) state->getCurrentSong()->tracks.size() - 1); err.isNotEmpty())
                std::printf ("vsti midi rejected: %s\n", err.toRawUTF8());
    }

    void printPluginSummary()
    {
        const auto ps = state->getPluginSummary();
        std::printf ("vsti: %d instruments (%d ready, %d not certified, %d not available), max plugin DSP load %.1f%%\n",
                     ps.instruments, ps.ready, ps.notCertified, ps.problems, (double) ps.maxLoad * 100.0);
        for (const auto& s : state->getSongs())
            for (const auto& t : s.tracks)
                if (t.kind == TrackKind::MidiVsti && t.pluginUid.isNotEmpty())
                    std::printf ("  vsti track \"%s\" (song \"%s\"): %s [%s]%s%s, %d notes, state %d bytes\n", t.name.toRawUTF8(),
                                 s.title.toRawUTF8(), t.pluginName.toRawUTF8(), AppState::pluginStatusText (t).toRawUTF8(),
                                 t.pluginError.isNotEmpty() ? " " : "", t.pluginError.toRawUTF8(), t.midiNotes,
                                 (int) t.pluginState.getSize());
    }

    void printMidiSummary()
    {
        const auto ms = state->getMidiSummary();
        std::printf ("midi: %d modules (%d ready, %d preparing, %d not ready), %d tracks without module, %d missing files\n",
                     ms.modules, ms.ready, ms.preparing, ms.problems, ms.tracksWithoutModule, ms.missingFiles);
        for (int i = 0; i < (int) state->getModules().size(); ++i)
        {
            const auto& p = state->getModules()[(size_t) i];
            std::printf ("  module \"%s\" port \"%s\" ch %d: %s\n", p.deviceName.c_str(), p.portName.c_str(), p.channel,
                         MidiOutputService::statusText (state->getModuleStatus (i)).toRawUTF8());
        }
        if (const auto* s = state->getCurrentSong())
            for (const auto& t : s->tracks)
                if (t.kind == TrackKind::MidiExternal)
                    std::printf ("  ext midi track \"%s\": %s, %d notes, module %s\n", t.name.toRawUTF8(),
                                 t.midiPath.isEmpty() ? "no file" : juce::File (t.midiPath).getFileName().toRawUTF8(), t.midiNotes,
                                 state->findModule (t.moduleId) >= 0 ? "assigned" : "NONE");
    }

    void shutdown() override
    {
        juce::Logger::writeToLog ("Shutdown");
        if (state != nullptr)
            state->closeProject();
        closeAudioSettingsWindow();
        closeModuleWindow();
        closePluginWindow();
        window = nullptr;
        if (audioDevice != nullptr)
            audioDevice->close();
        state = nullptr;
        audioDevice = nullptr;
        engine = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        juce::Logger::setCurrentLogger (nullptr);
        logger = nullptr;
    }

    void systemRequestedQuit() override
    {
        // Never quit silently in the middle of a show.
        if (state != nullptr && state->isLiveMode() && state->getTransportState() == TransportState::Playing)
        {
            juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::WarningIcon, "Quit during playback?",
                                                "A song is playing in Live Mode. Quit B4ND LIVE MTR anyway?",
                                                "Quit", "Keep playing", nullptr,
                                                juce::ModalCallbackFunction::create ([] (int result)
                                                {
                                                    if (result == 1)
                                                        juce::JUCEApplication::quit();
                                                }));
            return;
        }
        quit();
    }

    void anotherInstanceStarted (const juce::String&) override {}

private:
    void timerCallback() override
    {
        // Small state machine: wait for media -> optional playback -> settle -> capture.
        const double now = juce::Time::getMillisecondCounterHiRes();
        if (phaseStart <= 0.0)
            phaseStart = now;
        const double elapsed = (now - phaseStart) / 1000.0;

        if (phase == 0)   // waiting for media decoding (max 60 s)
        {
            if (state->getMediaSummary().loading > 0 && elapsed < 60.0)
                return;
            const auto m = state->getMediaSummary();
            std::printf ("media: %d ready, %d loading, %d missing, %d errors, %.1f MB\n", m.ready, m.loading, m.missing,
                         m.errors, (double) m.ramBytes / (1024.0 * 1024.0));
            if (screenshot->saveAs != juce::File())
            {
                const auto err = state->saveProjectAs (screenshot->saveAs);
                std::printf ("project save-as: %s\n", err.isEmpty() ? "ok" : err.toRawUTF8());
            }
            if (screenshot->playSeconds > 0.0)
            {
                state->locate (screenshot->atSeconds);
                state->play();
                std::printf ("play: engine %s\n", state->isEngineActive() ? "active" : "NOT active (virtual transport)");
                startTimer (10);   // sample the 30 Hz meters often enough to catch every click
            }
            phase = 1;
            phaseStart = now;
            return;
        }
        if (phase == 1)   // playback
        {
            for (int o = 0; o < (int) outputPeaks.size(); ++o)
                outputPeaks[(size_t) o] = juce::jmax (outputPeaks[(size_t) o], state->getOutputLevel (o));
            if (elapsed < screenshot->playSeconds)
                return;
            if (screenshot->playSeconds > 0.0)
            {
                // Stop first: the engine releases its MIDI notes, and they must be counted too.
                state->stop();
                phase = 3;
                phaseStart = now;
                return;
            }
            phase = 2;
            phaseStart = now;
            startTimer (100);
            return;
        }
        if (phase == 3)   // after stop: let the release messages go out
        {
            if (elapsed < 0.5)
                return;
            {
                const auto names = audioDevice->getOutputChannelNames();
                for (int o = 0; o < juce::jmin ((int) outputPeaks.size(), state->getRouting().getNumOutputs()); ++o)
                    std::printf ("output %d [%s]: peak %.4f\n", o + 1, state->getRouting().getOutput (o).label.c_str(),
                                 (double) outputPeaks[(size_t) o]);
                std::printf ("position after play: %.3f s, dropouts %llu (late callbacks %llu, DSP overloads %llu, "
                             "peak DSP load %.1f%%)\n", state->getPositionSeconds(),
                             (unsigned long long) state->getDropoutCount(),
                             (unsigned long long) engine->getLateCallbackCount(),
                             (unsigned long long) engine->getOverloadCount(), engine->getPeakLoad() * 100.0);
                auto& mo = state->getMidiOutput();
                int sounding = 0;
                for (int p = 0; p < AudioEngine::maxMidiPorts; ++p)
                    sounding += engine->getActiveMidiNotes (p);
                std::printf ("midi out: sent %llu, dropped %llu (no open port), late %llu (>2 ms), max lateness %.2f ms, "
                             "engine queue full %llu, notes still on after stop %d\n",
                             (unsigned long long) mo.getSentCount(), (unsigned long long) mo.getDroppedCount(),
                             (unsigned long long) mo.getLateCount(), mo.getMaxLatenessMs(),
                             (unsigned long long) engine->getMidiDroppedCount(), sounding);
                if (state->getPluginSummary().instruments > 0)
                {
                    int instNotes = 0;
                    for (int k = 0; k < AudioEngine::maxTracks; ++k)
                        instNotes += engine->getActiveInstrumentNotes (k);
                    std::printf ("vsti play: instrument events dropped %llu, notes still on after stop %d\n",
                                 (unsigned long long) engine->getInstrumentEventsDropped(), instNotes);
                    printPluginSummary();
                }
            }
            phase = 2;
            phaseStart = now;
            startTimer (100);
            return;
        }
        if (elapsed < 1.4)   // settle: let layout and meters update
            return;

        stopTimer();
        if (! screenshot->moduleNames.isEmpty() || ! screenshot->midiFiles.isEmpty())
            printMidiSummary();
        juce::Component* comp = &window->getMainComponent();
        if (screenshot->modulesWindow && getModuleWindowContent() != nullptr)
            comp = getModuleWindowContent();
        if (screenshot->pluginsWindow && getPluginWindowContent() != nullptr)
            comp = getPluginWindowContent();
        if (screenshot->settings)
            for (int i = juce::TopLevelWindow::getNumTopLevelWindows(); --i >= 0;)
                if (auto* w = dynamic_cast<juce::DocumentWindow*> (juce::TopLevelWindow::getTopLevelWindow (i)))
                    if (w != window.get() && w->getContentComponent() != nullptr)
                        comp = w->getContentComponent();
        const auto img = comp->createComponentSnapshot (comp->getLocalBounds(), true, 1.0f);
        screenshot->file.deleteFile();
        juce::FileOutputStream out (screenshot->file);
        const bool ok = out.openedOk() && juce::PNGImageFormat().writeImageToStream (img, out);
        out.flush();
        std::printf ("screenshot %s: %s (%dx%d)\n", ok ? "written" : "FAILED",
                     screenshot->file.getFullPathName().toRawUTF8(), img.getWidth(), img.getHeight());
        setApplicationReturnValue (ok ? 0 : 1);
        quit();
    }

    B4ndLookAndFeel lookAndFeel;
    std::unique_ptr<juce::FileLogger> logger;
    std::unique_ptr<AudioEngine> engine;
    std::unique_ptr<AudioDeviceController> audioDevice;
    std::unique_ptr<AppState> state;
    std::unique_ptr<MainWindow> window;
    std::optional<ScreenshotRequest> screenshot;
    int phase = 0;
    double phaseStart = 0.0;
    std::array<float, 16> outputPeaks {};
};

} // namespace b4nd

START_JUCE_APPLICATION (b4nd::B4ndApplication)
