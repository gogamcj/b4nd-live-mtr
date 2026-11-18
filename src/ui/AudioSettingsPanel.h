#pragma once

#include "app/AppState.h"

namespace b4nd
{

/** Audio device selection (ASIO first), sample rate / buffer, driver status, and the
    pre-show output test (docs/03 §2, docs/06 §5). */
class AudioSettingsPanel : public juce::Component,
                           private juce::ChangeListener,
                           private juce::Timer
{
public:
    explicit AudioSettingsPanel (AppState&);
    ~AudioSettingsPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void refreshTypes();
    void refreshDevices();
    void refreshDeviceDetails();
    void refreshEnablement();
    juce::String lockReason() const;

    AppState& state;
    AudioDeviceController& device;

    juce::ComboBox typeBox, deviceBox, rateBox, bufferBox, outputBox, inputBox;
    juce::TextButton measureButton { "Measure Round Trip" }, cancelMeasureButton { "Cancel" };
    juce::TextButton openButton { "Open" }, panelButton { "Driver Panel" }, reconnectButton { "Reconnect" },
                     closeButton { "Close Device" };
    juce::TextButton toneButton { "Tone 1 kHz" }, identifyButton { "Identify (N beeps)" },
                     busCheckButton { "Click / Guide Check" }, walkButton { "Walk All Outputs" },
                     stopTestButton { "STOP TEST" };

    juce::Rectangle<int> deviceHeader, infoArea, testHeader, testInfoArea, latencyHeader, latencyInfoArea, lockArea;
    bool updating = false;
};

/** Opens (or brings to front) the audio settings window. */
void showAudioSettingsWindow (AppState&);
/** Destroys the window (call before AppState is destroyed). */
void closeAudioSettingsWindow();

} // namespace b4nd
