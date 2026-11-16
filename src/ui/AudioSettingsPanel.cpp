#include "ui/AudioSettingsPanel.h"
#include "ui/Theme.h"

namespace b4nd
{

AudioSettingsPanel::AudioSettingsPanel (AppState& s) : state (s), device (s.getDevice())
{
    for (auto* c : { &typeBox, &deviceBox, &rateBox, &bufferBox, &outputBox })
        addAndMakeVisible (*c);
    for (auto* b : { &openButton, &panelButton, &reconnectButton, &closeButton, &toneButton, &identifyButton,
                     &busCheckButton, &walkButton, &stopTestButton })
    {
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (*b);
    }

    stopTestButton.setColour (juce::TextButton::buttonColourId, theme::danger.darker (0.35f));
    openButton.setColour (juce::TextButton::buttonColourId, theme::accent.darker (0.4f));

    typeBox.onChange = [this] { if (! updating) refreshDevices(); };

    openButton.onClick = [this]
    {
        if (lockReason().isNotEmpty())
            return;
        device.open (typeBox.getText(), deviceBox.getText());
    };
    closeButton.onClick     = [this] { if (lockReason().isEmpty()) device.close(); };
    reconnectButton.onClick = [this] { device.reconnect(); };
    panelButton.onClick     = [this] { if (lockReason().isEmpty()) device.showControlPanel(); };

    rateBox.onChange = [this]
    {
        if (! updating && lockReason().isEmpty())
            device.setSampleRate (rateBox.getText().getDoubleValue());
    };
    bufferBox.onChange = [this]
    {
        if (! updating && lockReason().isEmpty())
            device.setBufferSize (bufferBox.getText().getIntValue());
    };

    auto test = [this] (TestSignal sig)
    {
        if (! state.startOutputTest (sig, outputBox.getSelectedItemIndex()))
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Output test",
                                                    "Output tests need a running audio device and a stopped transport.");
    };
    toneButton.onClick     = [test] { test (TestSignal::Tone); };
    identifyButton.onClick = [test] { test (TestSignal::Identify); };
    busCheckButton.onClick = [test] { test (TestSignal::BusCheck); };
    walkButton.onClick     = [test] { test (TestSignal::WalkOutputs); };
    stopTestButton.onClick = [this] { state.stopOutputTest(); };

    toneButton.setTooltip ("1 kHz sine at -20 dBFS on the selected output only");
    identifyButton.setTooltip ("Output N plays N beeps, so you can tell cables apart at the desk");
    busCheckButton.setTooltip ("Click pattern on CLICK outputs, noise bursts on GUIDE outputs. PA outputs stay silent.");
    walkButton.setTooltip ("Tone steps through every output, 1.5 s each");

    device.addChangeListener (this);
    state.addChangeListener (this);
    refreshTypes();
    refreshDeviceDetails();
    startTimerHz (4);
    setSize (680, 600);
}

AudioSettingsPanel::~AudioSettingsPanel()
{
    device.removeChangeListener (this);
    state.removeChangeListener (this);
}

juce::String AudioSettingsPanel::lockReason() const
{
    if (state.isLiveMode())
        return "Locked in Live Mode: leave Live Mode to change the audio device.";
    if (state.getTransportState() == TransportState::Playing)
        return "Stop playback to change the audio device.";
    return {};
}

void AudioSettingsPanel::refreshTypes()
{
    updating = true;
    typeBox.clear (juce::dontSendNotification);
    const auto types = device.getTypeNames();
    for (int i = 0; i < types.size(); ++i)
        typeBox.addItem (types[i], i + 1);

    const auto current = device.getTypeName().isNotEmpty() ? device.getTypeName() : juce::String();
    const int idx = types.indexOf (current);
    typeBox.setSelectedItemIndex (idx >= 0 ? idx : 0, juce::dontSendNotification);
    updating = false;
    refreshDevices();
}

void AudioSettingsPanel::refreshDevices()
{
    updating = true;
    deviceBox.clear (juce::dontSendNotification);
    const auto names = device.getDeviceNames (typeBox.getText());
    for (int i = 0; i < names.size(); ++i)
        deviceBox.addItem (names[i], i + 1);
    const int idx = names.indexOf (device.getDeviceName().isNotEmpty() ? device.getDeviceName() : device.getSavedDeviceName());
    deviceBox.setSelectedItemIndex (idx >= 0 ? idx : 0, juce::dontSendNotification);
    if (names.isEmpty())
        deviceBox.setTextWhenNothingSelected ("(no devices for this driver)");
    updating = false;
}

void AudioSettingsPanel::refreshDeviceDetails()
{
    updating = true;
    rateBox.clear (juce::dontSendNotification);
    bufferBox.clear (juce::dontSendNotification);

    const auto rates = device.getSupportedSampleRates();
    for (int i = 0; i < rates.size(); ++i)
        rateBox.addItem (juce::String (rates[i], 0), i + 1);
    rateBox.setSelectedItemIndex (rates.indexOf (device.getSampleRate()), juce::dontSendNotification);

    const auto sizes = device.getSupportedBufferSizes();
    for (int i = 0; i < sizes.size(); ++i)
        bufferBox.addItem (juce::String (sizes[i]), i + 1);
    bufferBox.setSelectedItemIndex (sizes.indexOf (device.getBufferSize()), juce::dontSendNotification);

    const int prevOut = outputBox.getSelectedItemIndex();
    outputBox.clear (juce::dontSendNotification);
    const auto& routing = state.getRouting();
    const auto channelNames = device.getOutputChannelNames();
    const int numOut = device.getNumOutputs() > 0 ? device.getNumOutputs() : routing.getNumOutputs();
    for (int i = 0; i < numOut; ++i)
    {
        juce::String label = "OUT " + juce::String (i + 1);
        if (i < routing.getNumOutputs())
            label << "  " << routing.getOutput (i).label << (routing.getOutput (i).isPA ? "  [PA]" : "");
        if (i < channelNames.size())
            label << "   (" << channelNames[i] << ")";
        outputBox.addItem (label, i + 1);
    }
    outputBox.setSelectedItemIndex (juce::jlimit (0, juce::jmax (0, numOut - 1), prevOut < 0 ? 2 : prevOut), juce::dontSendNotification);
    updating = false;

    refreshEnablement();
    repaint();
}

void AudioSettingsPanel::refreshEnablement()
{
    const bool locked = lockReason().isNotEmpty();
    const bool running = device.getStatus() == AudioDeviceController::Status::Running;
    for (auto* c : { (juce::Component*) &typeBox, (juce::Component*) &deviceBox, (juce::Component*) &openButton,
                     (juce::Component*) &closeButton })
        c->setEnabled (! locked);
    rateBox.setEnabled (! locked && running);
    bufferBox.setEnabled (! locked && running);
    panelButton.setEnabled (! locked && device.hasControlPanel());
    reconnectButton.setEnabled (device.getStatus() != AudioDeviceController::Status::Running && device.canReconnect());

    const bool canTest = running && state.getTransportState() != TransportState::Playing;
    for (auto* b : { &toneButton, &identifyButton, &busCheckButton, &walkButton })
        b->setEnabled (canTest);
    stopTestButton.setEnabled (running);
}

void AudioSettingsPanel::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &device)
    {
        refreshTypes();
        refreshDeviceDetails();
    }
    else
    {
        refreshEnablement();
    }
}

void AudioSettingsPanel::timerCallback()
{
    refreshEnablement();
    repaint (infoArea);
    repaint (testInfoArea);
    repaint (lockArea);
}

void AudioSettingsPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);

    auto header = [&g] (juce::Rectangle<int> r, const juce::String& t)
    {
        g.setColour (theme::accent);
        g.setFont (theme::font (12.0f, true));
        g.drawText (t, r, juce::Justification::centredLeft);
        g.setColour (theme::outline);
        g.drawHorizontalLine (r.getBottom() - 1, (float) r.getX(), (float) r.getRight());
    };
    header (deviceHeader, "AUDIO DEVICE  (ASIO preferred)");
    header (testHeader, "OUTPUT TEST  (before the show)");

    auto label = [&g] (int x, int y, const juce::String& t)
    {
        g.setColour (theme::textDim);
        g.setFont (theme::font (12.0f, true));
        g.drawText (t, x, y, 110, 26, juce::Justification::centredLeft);
    };
    label (16, typeBox.getY(), "Driver");
    label (16, deviceBox.getY(), "Device");
    label (16, rateBox.getY(), "Sample rate");
    label (16, bufferBox.getY(), "Buffer size");
    label (16, outputBox.getY(), "Output");

    // Device info
    auto info = infoArea;
    g.setColour (theme::panelInset);
    g.fillRoundedRectangle (info.toFloat(), 4.0f);
    info.reduce (10, 6);
    const bool running = device.getStatus() == AudioDeviceController::Status::Running;
    const bool lost = device.getStatus() == AudioDeviceController::Status::Lost
                   || device.getStatus() == AudioDeviceController::Status::SavedDeviceMissing;
    g.setFont (theme::font (12.0f, true));
    g.setColour (running ? theme::ok : lost ? theme::danger : theme::textDim);
    g.drawFittedText ("Status: " + device.getStatusText(), info.removeFromTop (20), juce::Justification::centredLeft, 1);
    g.setFont (theme::mono (12.0f));
    g.setColour (theme::text);
    if (running)
    {
        const int xr = device.getDriverXrunCount();
        g.drawText ("Outputs " + juce::String (device.getNumOutputs()) + "   Inputs " + juce::String (device.getNumInputs())
                        + "   " + juce::String (device.getSampleRate(), 0) + " Hz / " + juce::String (device.getBufferSize()) + " samples",
                    info.removeFromTop (18), juce::Justification::centredLeft);
        g.drawText ("Latency  out " + juce::String (device.getOutputLatencyMs(), 1) + " ms   in "
                        + juce::String (device.getInputLatencyMs(), 1) + " ms   DSP load "
                        + juce::String (juce::roundToInt (state.getEngine().getLoad() * 100.0f)) + "%",
                    info.removeFromTop (18), juce::Justification::centredLeft);
        g.drawText ("Dropouts  detected " + juce::String ((juce::int64) state.getDropoutCount())
                        + "   driver-reported " + (xr >= 0 ? juce::String (xr) : juce::String ("n/a")),
                    info.removeFromTop (18), juce::Justification::centredLeft);
        if (device.getNumOutputs() < 8)
        {
            g.setColour (theme::warn);
            g.setFont (theme::font (11.0f, true));
            g.drawFittedText ("This device has " + juce::String (device.getNumOutputs())
                                  + " outputs. The 8-output layout needs a larger interface; outputs above "
                                  + juce::String (device.getNumOutputs()) + " are inactive.",
                              info, juce::Justification::topLeft, 2);
        }
    }
    else if (device.getLastError().isNotEmpty())
    {
        g.drawFittedText (device.getLastError(), info, juce::Justification::topLeft, 3);
    }

    // Test info
    auto ti = testInfoArea;
    g.setFont (theme::font (11.5f));
    g.setColour (theme::textDim);
    const auto sig = state.getEngine().getTestSignal();
    juce::String now = "No test running.";
    if (running && sig != TestSignal::None)
    {
        const char* names[] = { "", "Tone", "Identify", "Click / Guide check", "Walk all outputs" };
        now = juce::String ("Running: ") + names[(int) sig];
        if (sig != TestSignal::BusCheck && state.getEngine().getTestOutput() >= 0)
            now << "  on OUT " << (state.getEngine().getTestOutput() + 1);
        g.setColour (theme::warn);
    }
    g.drawText (now, ti.removeFromTop (18), juce::Justification::centredLeft);
    g.setColour (theme::textDim);
    g.drawFittedText ("Test signals are at -20 dBFS (never above -6 dBFS). Turn the PA down before testing PA outputs. "
                      "Click / Guide check never sends to outputs marked PA. Tests are blocked while playing.",
                      ti, juce::Justification::topLeft, 3);

    const auto reason = lockReason();
    if (reason.isNotEmpty())
    {
        g.setColour (theme::warn);
        g.setFont (theme::font (12.0f, true));
        g.drawText (reason, lockArea, juce::Justification::centredLeft);
    }
}

void AudioSettingsPanel::resized()
{
    auto r = getLocalBounds().reduced (16, 12);
    deviceHeader = r.removeFromTop (26);
    r.removeFromTop (8);

    auto row = [&r] { auto x = r.removeFromTop (28); r.removeFromTop (6); return x.withTrimmedLeft (110); };

    auto typeRow = row();
    typeBox.setBounds (typeRow.removeFromLeft (260));
    auto devRow = row();
    openButton.setBounds (devRow.removeFromRight (80));
    devRow.removeFromRight (6);
    deviceBox.setBounds (devRow);

    auto rateRow = row();
    rateBox.setBounds (rateRow.removeFromLeft (120));
    rateRow.removeFromLeft (12);
    panelButton.setBounds (rateRow.removeFromLeft (110));
    rateRow.removeFromLeft (6);
    reconnectButton.setBounds (rateRow.removeFromLeft (100));
    rateRow.removeFromLeft (6);
    closeButton.setBounds (rateRow.removeFromLeft (110));

    auto bufRow = row();
    bufferBox.setBounds (bufRow.removeFromLeft (120));

    infoArea = r.removeFromTop (112);
    r.removeFromTop (14);

    testHeader = r.removeFromTop (26);
    r.removeFromTop (8);
    auto outRow = row();
    outputBox.setBounds (outRow);

    auto buttons = r.removeFromTop (34);
    const int bw = (buttons.getWidth() - 4 * 6) / 5;
    for (auto* b : { &toneButton, &identifyButton, &busCheckButton, &walkButton, &stopTestButton })
    {
        b->setBounds (buttons.removeFromLeft (bw));
        buttons.removeFromLeft (6);
    }
    r.removeFromTop (8);
    testInfoArea = r.removeFromTop (64);
    lockArea = r.removeFromBottom (24);
}

//==============================================================================
namespace
{
class AudioSettingsWindow : public juce::DocumentWindow
{
public:
    explicit AudioSettingsWindow (AppState& s)
        : DocumentWindow ("Audio Settings - B4ND LIVE MTR", theme::panel, DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new AudioSettingsPanel (s), true);
        setResizable (false, false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    void closeButtonPressed() override { setVisible (false); }
};

std::unique_ptr<AudioSettingsWindow> settingsWindowOwner;
} // namespace

void showAudioSettingsWindow (AppState& state)
{
    if (settingsWindowOwner == nullptr)
    {
        settingsWindowOwner = std::make_unique<AudioSettingsWindow> (state);
    }
    settingsWindowOwner->setVisible (true);
    settingsWindowOwner->toFront (true);
}

void closeAudioSettingsWindow()
{
    settingsWindowOwner.reset();
}

} // namespace b4nd
