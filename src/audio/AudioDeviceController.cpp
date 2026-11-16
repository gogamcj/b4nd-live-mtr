#include "audio/AudioDeviceController.h"

namespace b4nd
{

const juce::Array<double>& AudioDeviceController::specSampleRates()
{
    static const juce::Array<double> r { 44100.0, 48000.0, 88200.0, 96000.0 };
    return r;
}

const juce::Array<int>& AudioDeviceController::specBufferSizes()
{
    static const juce::Array<int> b { 64, 128, 256, 512, 1024 };
    return b;
}

AudioDeviceController::AudioDeviceController (AudioEngine& e) : engine (e)
{
    // Backend priority (docs/03 §2): ASIO -> WASAPI exclusive -> WASAPI shared -> DirectSound.
    auto add = [this] (juce::AudioIODeviceType* t) { if (t != nullptr) types.add (t); };
   #if JUCE_WINDOWS
    #if JUCE_ASIO
    add (juce::AudioIODeviceType::createAudioIODeviceType_ASIO());
    #endif
    add (juce::AudioIODeviceType::createAudioIODeviceType_WASAPI (juce::WASAPIDeviceMode::exclusive));
    add (juce::AudioIODeviceType::createAudioIODeviceType_WASAPI (juce::WASAPIDeviceMode::shared));
    add (juce::AudioIODeviceType::createAudioIODeviceType_DirectSound());
   #elif JUCE_MAC
    add (juce::AudioIODeviceType::createAudioIODeviceType_CoreAudio());
   #elif JUCE_LINUX
    add (juce::AudioIODeviceType::createAudioIODeviceType_ALSA());
    add (juce::AudioIODeviceType::createAudioIODeviceType_JACK());
   #endif
}

AudioDeviceController::~AudioDeviceController()
{
    stopTimer();
    for (auto* t : types)
        t->removeListener (this);
    close();
}

void AudioDeviceController::initialise (const juce::File& file, bool enableAudio)
{
    settingsFile = file;
    if (! enableAudio)
        return;

    for (auto* t : types)
    {
        t->scanForDevices();
        t->addListener (this);
    }

    if (auto xml = juce::XmlDocument::parse (settingsFile))
    {
        savedType = xml->getStringAttribute ("type");
        savedName = xml->getStringAttribute ("device");
        savedRate = xml->getDoubleAttribute ("sampleRate", 0.0);
        savedBuffer = xml->getIntAttribute ("bufferSize", 0);
    }

    if (savedName.isNotEmpty())
    {
        if (getDeviceNames (savedType).contains (savedName))
            open (savedType, savedName, savedRate, savedBuffer);
        else
        {
            // Never substitute a different device: show what is missing instead.
            juce::Logger::writeToLog ("Audio: saved device not found: " + savedType + " / " + savedName);
            setStatus (Status::SavedDeviceMissing);
        }
    }
    else
    {
        autoPick();
    }

    startTimer (1000);
}

bool AudioDeviceController::autoPick()
{
    for (auto* t : types)
    {
        const auto names = t->getDeviceNames (false);
        if (names.isEmpty())
            continue;
        const int def = juce::jlimit (0, names.size() - 1, t->getDefaultDeviceIndex (false));
        juce::Logger::writeToLog ("Audio: first run, trying " + t->getTypeName() + " / " + names[def]);
        if (open (t->getTypeName(), names[def], 48000.0, 256))
            return true;
    }
    setStatus (Status::NoDevice);
    return false;
}

juce::StringArray AudioDeviceController::getTypeNames() const
{
    juce::StringArray names;
    for (auto* t : types)
        names.add (t->getTypeName());
    return names;
}

juce::AudioIODeviceType* AudioDeviceController::findType (const juce::String& name) const
{
    for (auto* t : types)
        if (t->getTypeName() == name)
            return t;
    return nullptr;
}

juce::StringArray AudioDeviceController::getDeviceNames (const juce::String& typeName) const
{
    if (auto* t = findType (typeName))
        return t->getDeviceNames (false);
    return {};
}

bool AudioDeviceController::open (const juce::String& typeName, const juce::String& deviceName,
                                  double sampleRate, int bufferSize)
{
    close();

    auto* type = findType (typeName);
    if (type == nullptr)
    {
        setStatus (Status::OpenFailed, "Unknown driver type: " + typeName);
        return false;
    }

    // ASIO devices are full duplex by name; other backends open output only for now
    // (line inputs for external modules are added in Phase 4/7).
    const bool isAsio = typeName.containsIgnoreCase ("ASIO");
    std::unique_ptr<juce::AudioIODevice> d (type->createDevice (deviceName, isAsio ? deviceName : juce::String()));
    if (d == nullptr)
    {
        setStatus (Status::OpenFailed, "Could not create device: " + deviceName);
        return false;
    }

    // Sample rate: requested if supported, else 48k, else the device's first spec rate.
    const auto rates = d->getAvailableSampleRates();
    double sr = rates.contains (sampleRate) ? sampleRate : rates.contains (48000.0) ? 48000.0 : 0.0;
    if (sr <= 0.0)
        for (auto r : specSampleRates())
            if (rates.contains (r)) { sr = r; break; }
    if (sr <= 0.0 && ! rates.isEmpty())
        sr = rates[0];

    const auto sizes = d->getAvailableBufferSizes();
    int bs = sizes.contains (bufferSize) ? bufferSize : sizes.contains (256) ? 256 : d->getDefaultBufferSize();

    juce::BigInteger outs, ins;
    outs.setRange (0, d->getOutputChannelNames().size(), true);
    if (isAsio)
        ins.setRange (0, d->getInputChannelNames().size(), true);

    const auto err = d->open (ins, outs, sr, bs);
    if (err.isNotEmpty())
    {
        juce::Logger::writeToLog ("Audio: open failed: " + typeName + " / " + deviceName + ": " + err);
        setStatus (Status::OpenFailed, err);
        return false;
    }

    device = std::move (d);
    suspectTicks = 0;
    currentType = typeName;
    currentName = deviceName;
    errorFlag = false;
    unexpectedStop = false;
    device->start (this);

    if (! device->isPlaying())
    {
        const auto e = device->getLastError();
        device->close();
        device.reset();
        setStatus (Status::OpenFailed, e.isNotEmpty() ? e : juce::String ("Device did not start"));
        return false;
    }

    savedType = currentType;
    savedName = currentName;
    savedRate = device->getCurrentSampleRate();
    savedBuffer = device->getCurrentBufferSizeSamples();
    saveSettings();

    juce::Logger::writeToLog ("Audio: running " + currentType + " / " + currentName + " @ "
                              + juce::String (savedRate, 0) + " Hz, " + juce::String (savedBuffer) + " samples, "
                              + juce::String (getNumOutputs()) + " outs, " + juce::String (getNumInputs()) + " ins");
    setStatus (Status::Running);
    return true;
}

void AudioDeviceController::close()
{
    if (device != nullptr)
    {
        stoppingOnPurpose = true;
        device->stop();
        device->close();
        device.reset();
        stoppingOnPurpose = false;
        engine.release();
    }
    currentType.clear();
    currentName.clear();
    if (status == Status::Running)
        setStatus (Status::NoDevice);
}

bool AudioDeviceController::canReconnect() const
{
    return savedName.isNotEmpty() && getDeviceNames (savedType).contains (savedName);
}

bool AudioDeviceController::reconnect()
{
    if (! canReconnect())
        return false;
    juce::Logger::writeToLog ("Audio: reconnecting " + savedType + " / " + savedName);
    return open (savedType, savedName, savedRate, savedBuffer);
}

juce::Array<double> AudioDeviceController::getSupportedSampleRates() const
{
    juce::Array<double> result;
    if (device == nullptr)
        return result;
    const auto rates = device->getAvailableSampleRates();
    for (auto r : specSampleRates())
        if (rates.contains (r))
            result.add (r);
    return result;
}

juce::Array<int> AudioDeviceController::getSupportedBufferSizes() const
{
    juce::Array<int> result;
    if (device == nullptr)
        return result;
    const auto sizes = device->getAvailableBufferSizes();
    for (auto s : specBufferSizes())
        if (sizes.contains (s))
            result.add (s);
    if (result.isEmpty())   // e.g. an ASIO driver with a fixed, non-standard size: show what it offers
        result = sizes;
    return result;
}

bool AudioDeviceController::setSampleRate (double sr)
{
    if (device == nullptr || juce::approximatelyEqual (sr, getSampleRate()))
        return false;
    return open (currentType, currentName, sr, getBufferSize());
}

bool AudioDeviceController::setBufferSize (int bs)
{
    if (device == nullptr || bs == getBufferSize())
        return false;
    return open (currentType, currentName, getSampleRate(), bs);
}

bool AudioDeviceController::hasControlPanel() const { return device != nullptr && device->hasControlPanel(); }

void AudioDeviceController::showControlPanel()
{
    if (device != nullptr && device->hasControlPanel())
    {
        // ASIO control panels may change the buffer size; JUCE restarts the device itself.
        device->showControlPanel();
        sendChangeMessage();
    }
}

double AudioDeviceController::getSampleRate() const { return device != nullptr ? device->getCurrentSampleRate() : 0.0; }
int AudioDeviceController::getBufferSize() const    { return device != nullptr ? device->getCurrentBufferSizeSamples() : 0; }
int AudioDeviceController::getNumOutputs() const    { return device != nullptr ? device->getActiveOutputChannels().countNumberOfSetBits() : 0; }
int AudioDeviceController::getNumInputs() const     { return device != nullptr ? device->getActiveInputChannels().countNumberOfSetBits() : 0; }
int AudioDeviceController::getDriverXrunCount() const { return device != nullptr ? device->getXRunCount() : -1; }

juce::StringArray AudioDeviceController::getOutputChannelNames() const
{
    return device != nullptr ? device->getOutputChannelNames() : juce::StringArray();
}

double AudioDeviceController::getOutputLatencyMs() const
{
    if (device == nullptr || getSampleRate() <= 0.0) return 0.0;
    return (device->getOutputLatencyInSamples() + getBufferSize()) * 1000.0 / getSampleRate();
}

double AudioDeviceController::getInputLatencyMs() const
{
    if (device == nullptr || getSampleRate() <= 0.0) return 0.0;
    return device->getInputLatencyInSamples() * 1000.0 / getSampleRate();
}

juce::String AudioDeviceController::getStatusText() const
{
    switch (status)
    {
        case Status::NoDevice:           return "No audio device";
        case Status::Running:            return currentName + " (" + currentType + ")";
        case Status::Lost:               return "DEVICE LOST: " + savedName + (canReconnect() ? "  - reconnect available" : "");
        case Status::SavedDeviceMissing: return "Not connected: " + savedName;
        case Status::OpenFailed:         return "Open failed: " + lastError;
    }
    return {};
}

void AudioDeviceController::setStatus (Status s, const juce::String& error)
{
    status = s;
    lastError = error;
    sendChangeMessage();
}

void AudioDeviceController::saveSettings()
{
    if (settingsFile == juce::File())
        return;
    juce::XmlElement xml ("AUDIO");
    xml.setAttribute ("type", savedType);
    xml.setAttribute ("device", savedName);
    xml.setAttribute ("sampleRate", savedRate);
    xml.setAttribute ("bufferSize", savedBuffer);
    settingsFile.getParentDirectory().createDirectory();
    xml.writeTo (settingsFile);
}

void AudioDeviceController::handleLost (const juce::String& reason)
{
    if (status != Status::Running)
        return;

    juce::Logger::writeToLog ("Audio: DEVICE LOST (" + reason + "): " + currentType + " / " + currentName);
    if (device != nullptr)
    {
        stoppingOnPurpose = true;
        device->stop();
        device->close();
        device.reset();
        stoppingOnPurpose = false;
    }
    engine.release();
    currentType.clear();
    currentName.clear();
    setStatus (Status::Lost, reason);
}

//==============================================================================
void AudioDeviceController::audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs,
                                                              float* const* outputs, int numOutputs, int numSamples,
                                                              const juce::AudioIODeviceCallbackContext&)
{
    engine.process (inputs, numInputs, outputs, numOutputs, numSamples);
}

void AudioDeviceController::audioDeviceAboutToStart (juce::AudioIODevice* d)
{
    // Also called when an ASIO driver requests a reset (e.g. buffer size changed in its panel).
    engine.prepare (d->getCurrentSampleRate(), d->getCurrentBufferSizeSamples(),
                    d->getActiveOutputChannels().countNumberOfSetBits(),
                    d->getActiveInputChannels().countNumberOfSetBits());
}

void AudioDeviceController::audioDeviceStopped()
{
    if (! stoppingOnPurpose)
        unexpectedStop = true;
}

void AudioDeviceController::audioDeviceError (const juce::String&)
{
    errorFlag = true;   // handled on the message thread in timerCallback()
}

void AudioDeviceController::audioDeviceListChanged()
{
    if (status == Status::Running && device != nullptr && ! getDeviceNames (currentType).contains (currentName))
        handleLost ("device disconnected");

    if (status == Status::SavedDeviceMissing && canReconnect())
        reconnect();

    sendChangeMessage();
}

void AudioDeviceController::timerCallback()
{
    if (status == Status::Running)
    {
        if (device != nullptr && ! getDeviceNames (currentType).contains (currentName))
        {
            handleLost ("device disconnected");
            return;
        }

        // An ASIO reset request (e.g. buffer size changed in the driver panel) briefly
        // stops and restarts the device, so a single stop is only "suspect".
        const bool error = errorFlag.exchange (false);
        const bool stopped = unexpectedStop.exchange (false);
        if (error)
            juce::Logger::writeToLog ("Audio: driver reported an error");
        if (error || stopped || (device != nullptr && ! device->isPlaying()))
        {
            if (device != nullptr && device->isPlaying() && ! error)
                suspectTicks = 0;      // restarted by itself
            else if (++suspectTicks >= 2)
                handleLost (error ? "driver error" : "driver stopped");
        }
        else
        {
            suspectTicks = 0;
        }
        return;
    }

    // Some drivers (ASIO) never send hot-plug notifications: rescan for the lost device.
    if (status == Status::Lost || status == Status::SavedDeviceMissing)
    {
        if (auto* t = findType (savedType))
            t->scanForDevices();

        if (canReconnect())
        {
            if (autoReconnect)
                reconnect();
            else
                sendChangeMessage();   // UI shows "reconnect available"
        }
    }
}

} // namespace b4nd
