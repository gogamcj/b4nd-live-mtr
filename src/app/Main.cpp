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
    Renders the UI to a PNG and exits (no audio device is opened). */
struct ScreenshotRequest
{
    juce::File file;
    bool live = false;
    bool settings = false;   // capture the audio settings window instead
    int width = 1600, height = 960;
    int song = 0;
    double atSeconds = 0.0;

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
            startTimer (1500);
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
};

} // namespace b4nd

START_JUCE_APPLICATION (b4nd::B4ndApplication)
