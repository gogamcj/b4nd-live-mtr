#include <cstdio>
#include <optional>

#include <juce_gui_extra/juce_gui_extra.h>

#include "app/AppState.h"
#include "app/MainWindow.h"
#include "ui/AudioSettingsPanel.h"
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
    --play plays the song for that long first and logs the peak level per output. */
struct ScreenshotRequest
{
    juce::File file;
    bool live = false;
    bool settings = false;   // capture the audio settings window instead
    int width = 1600, height = 960;
    int song = 0;
    double atSeconds = 0.0;
    juce::StringArray media;
    double playSeconds = 0.0;

    static std::optional<ScreenshotRequest> parse (const juce::StringArray& args)
    {
        const int i = args.indexOf ("--screenshot");
        if (i < 0 || i + 1 >= args.size())
            return std::nullopt;

        ScreenshotRequest r;
        r.file = juce::File::getCurrentWorkingDirectory().getChildFile (args[i + 1]);
        r.live = args.contains ("--live");
        r.settings = args.contains ("--audio-settings");
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
        for (int k = 0; k + 1 < args.size(); ++k)
            if (args[k] == "--media")
                r.media.add (juce::File::getCurrentWorkingDirectory().getChildFile (args[k + 1].unquoted()).getFullPathName());
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

        if (screenshot)
        {
            state->selectSong (screenshot->song);
            state->locate (screenshot->atSeconds);
            window->setFullScreen (false);
            window->setSize (screenshot->width, screenshot->height);
            if (screenshot->settings)
                showAudioSettingsWindow (*state);
            if (screenshot->live)
            {
                // Show the live view at the requested size instead of going full screen.
                window->getMainComponent().onLiveModeChanged = nullptr;
                window->getMainComponent().toggleLiveMode();
            }
            for (const auto& m : screenshot->media)
                if (const auto err = state->addAudioFile (juce::File (m)); err.isNotEmpty())
                    std::printf ("media rejected: %s\n", err.toRawUTF8());
            startTimer (100);
        }
    }

    void shutdown() override
    {
        juce::Logger::writeToLog ("Shutdown");
        closeAudioSettingsWindow();
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
                const auto names = audioDevice->getOutputChannelNames();
                for (int o = 0; o < juce::jmin ((int) outputPeaks.size(), state->getRouting().getNumOutputs()); ++o)
                    std::printf ("output %d [%s]: peak %.4f\n", o + 1, state->getRouting().getOutput (o).label.c_str(),
                                 (double) outputPeaks[(size_t) o]);
                std::printf ("position after play: %.3f s, dropouts %llu (late callbacks %llu, DSP overloads %llu, "
                             "peak DSP load %.1f%%)\n", state->getPositionSeconds(),
                             (unsigned long long) state->getDropoutCount(),
                             (unsigned long long) engine->getLateCallbackCount(),
                             (unsigned long long) engine->getOverloadCount(), engine->getPeakLoad() * 100.0);
            }
            phase = 2;
            phaseStart = now;
            startTimer (100);
            return;
        }
        if (elapsed < 1.4)   // settle: let layout and meters update
            return;

        stopTimer();
        juce::Component* comp = &window->getMainComponent();
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
