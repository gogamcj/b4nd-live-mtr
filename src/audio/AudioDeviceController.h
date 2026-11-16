#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include "engine/AudioEngine.h"

namespace b4nd
{

/**
    Owns the audio device and feeds AudioEngine (docs/03 §2).

    Deliberately does NOT use juce::AudioDeviceManager: when the current device
    disappears, AudioDeviceManager re-initialises and silently opens another
    (default) device. On stage that could send the show to the laptop speakers or
    a different interface. Here a lost device stays lost until the *same* device
    comes back and is reconnected.

    All public functions are message-thread only.
*/
class AudioDeviceController : public juce::ChangeBroadcaster,
                              private juce::AudioIODeviceCallback,
                              private juce::AudioIODeviceType::Listener,
                              private juce::Timer
{
public:
    enum class Status
    {
        NoDevice,             // nothing configured / nothing available
        Running,
        Lost,                 // was running, then disconnected or the driver failed
        SavedDeviceMissing,   // the saved device is not connected at startup
        OpenFailed
    };

    explicit AudioDeviceController (AudioEngine&);
    ~AudioDeviceController() override;

    /** Restores the saved device, or on first run picks the best available
        backend (ASIO first). Does nothing when audio is disabled. */
    void initialise (const juce::File& settingsFile, bool enableAudio);

    // --- discovery ---
    juce::StringArray getTypeNames() const;                       // priority order
    juce::StringArray getDeviceNames (const juce::String& typeName) const;

    // --- control ---
    bool open (const juce::String& typeName, const juce::String& deviceName,
               double sampleRate = 0.0, int bufferSize = 0);
    void close();
    bool reconnect();
    bool canReconnect() const;
    void setAutoReconnectAllowed (bool allowed) noexcept { autoReconnect = allowed; }

    /** Sample rates / buffer sizes offered to the user: the spec values the open device supports. */
    juce::Array<double> getSupportedSampleRates() const;
    juce::Array<int> getSupportedBufferSizes() const;
    bool setSampleRate (double);
    bool setBufferSize (int);

    bool hasControlPanel() const;
    void showControlPanel();

    // --- state ---
    Status getStatus() const noexcept { return status; }
    juce::String getStatusText() const;
    juce::String getTypeName() const      { return currentType; }
    juce::String getDeviceName() const    { return currentName; }
    juce::String getSavedDeviceName() const { return savedName; }
    double getSampleRate() const;
    int getBufferSize() const;
    int getNumOutputs() const;
    int getNumInputs() const;
    juce::StringArray getOutputChannelNames() const;
    double getOutputLatencyMs() const;
    double getInputLatencyMs() const;
    int getDriverXrunCount() const;        // -1 = not reported by this driver
    juce::String getLastError() const     { return lastError; }

    static const juce::Array<double>& specSampleRates();
    static const juce::Array<int>& specBufferSizes();

private:
    // AudioIODeviceCallback (audio thread except AboutToStart/Stopped)
    void audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs,
                                           float* const* outputs, int numOutputs, int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;
    void audioDeviceError (const juce::String& message) override;

    void audioDeviceListChanged() override;
    void timerCallback() override;

    juce::AudioIODeviceType* findType (const juce::String& name) const;
    void handleLost (const juce::String& reason);
    void setStatus (Status, const juce::String& error = {});
    void saveSettings();
    bool autoPick();

    AudioEngine& engine;
    juce::OwnedArray<juce::AudioIODeviceType> types;
    std::unique_ptr<juce::AudioIODevice> device;
    juce::File settingsFile;

    juce::String currentType, currentName, savedType, savedName;
    double savedRate = 0.0;
    int savedBuffer = 0;

    Status status = Status::NoDevice;
    juce::String lastError;
    bool autoReconnect = true;
    bool stoppingOnPurpose = false;
    int suspectTicks = 0;

    std::atomic<bool> errorFlag { false };
    std::atomic<bool> unexpectedStop { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioDeviceController)
};

} // namespace b4nd
