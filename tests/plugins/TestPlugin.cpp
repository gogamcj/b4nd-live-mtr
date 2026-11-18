// Test fixture VST3 plugins for the B4ND plugin host. Not products.
//   B4ND_TEST_VARIANT 0: well-behaved 16-voice sine synth with one parameter ("level") and state
//   B4ND_TEST_VARIANT 1: crashes when it is created (scan must survive and blacklist it)
//   B4ND_TEST_VARIANT 2: far too slow (certification must fail on processing time)
//   B4ND_TEST_VARIANT 3: state does not round-trip (certification must fail on state)

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include <array>
#include <cmath>
#include <thread>

#ifndef B4ND_TEST_VARIANT
 #define B4ND_TEST_VARIANT 0
#endif

namespace
{
class TestSynth final : public juce::AudioProcessor
{
public:
    TestSynth()
        : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    {
       #if B4ND_TEST_VARIANT == 1
        volatile int* p = nullptr;
        *p = 42;   // access violation / SIGSEGV on purpose
       #endif
        addParameter (level = new juce::AudioParameterFloat (juce::ParameterID { "level", 1 }, "Level", 0.0f, 1.0f, 0.5f));
    }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}
    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override { return new juce::GenericAudioProcessorEditor (*this); }

    bool isBusesLayoutSupported (const BusesLayout& l) const override
    {
        return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
            || l.getMainOutputChannelSet() == juce::AudioChannelSet::mono();
    }

    void prepareToPlay (double sr, int) override
    {
        sampleRate = sr;
        for (auto& v : voices)
            v = {};
       #if B4ND_TEST_VARIANT == 4
        // A look-ahead instrument: its sound comes out 480 samples (10 ms at 48 kHz) after the MIDI.
        setLatencySamples (latency);
        delayLine.assign ((size_t) (2 * latency), 0.0f);
        delayPos = 0;
       #endif
    }
    void releaseResources() override {}

    using juce::AudioProcessor::processBlock;
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        buffer.clear();
        const int n = buffer.getNumSamples();
        int pos = 0;
        for (const auto meta : midi)
        {
            render (buffer, pos, meta.samplePosition);
            pos = meta.samplePosition;
            handle (meta.getMessage());
        }
        render (buffer, pos, n);
       #if B4ND_TEST_VARIANT == 4
        for (int i = 0; i < n; ++i)
        {
            for (int ch = 0; ch < 2 && ch < buffer.getNumChannels(); ++ch)
            {
                auto& slot = delayLine[(size_t) (2 * delayPos + ch)];
                const float in = buffer.getSample (ch, i);
                buffer.setSample (ch, i, slot);
                slot = in;
            }
            delayPos = (delayPos + 1) % latency;
        }
       #endif
       #if B4ND_TEST_VARIANT == 2
        // 1.5x the block duration: can never keep up in real time.
        std::this_thread::sleep_for (std::chrono::microseconds ((long long) (1.5e6 * n / sampleRate)));
       #endif
    }

    void getStateInformation (juce::MemoryBlock& dest) override
    {
        juce::XmlElement x ("B4NDTEST");
        x.setAttribute ("level", (double) level->get());
       #if B4ND_TEST_VARIANT == 3
        x.setAttribute ("nonce", juce::Random::getSystemRandom().nextInt());   // differs every time
       #endif
        copyXmlToBinary (x, dest);
    }

    void setStateInformation (const void* data, int size) override
    {
       #if B4ND_TEST_VARIANT == 3
        juce::ignoreUnused (data, size);   // forgets everything
       #else
        if (auto x = getXmlFromBinary (data, size); x != nullptr && x->hasTagName ("B4NDTEST"))
            *level = (float) x->getDoubleAttribute ("level", 0.5);
       #endif
    }

private:
   #if B4ND_TEST_VARIANT == 4
    static constexpr int latency = 480;
    std::vector<float> delayLine;
    int delayPos = 0;
   #endif
    struct Voice
    {
        int note = -1;
        double phase = 0.0, inc = 0.0;
        float amp = 0.0f, env = 0.0f;
        bool releasing = false;
    };

    void handle (const juce::MidiMessage& m)
    {
        if (m.isNoteOn())
        {
            Voice* v = &voices[0];
            for (auto& c : voices)
                if (c.note < 0 && c.env <= 0.0f) { v = &c; break; }
            v->note = m.getNoteNumber();
            v->inc = juce::MathConstants<double>::twoPi * juce::MidiMessage::getMidiNoteInHertz (v->note) / sampleRate;
            v->amp = m.getFloatVelocity();
            v->releasing = false;
        }
        else if (m.isNoteOff())
        {
            for (auto& v : voices)
                if (v.note == m.getNoteNumber() && ! v.releasing)
                    v.releasing = true;
        }
        else if (m.isAllNotesOff() || m.isAllSoundOff())
        {
            for (auto& v : voices)
                v.releasing = true;
        }
    }

    void render (juce::AudioBuffer<float>& b, int from, int to)
    {
        const float lv = level->get();
        const float step = (float) (1.0 / (0.005 * sampleRate));   // 5 ms attack / release
        for (auto& v : voices)
        {
            if (v.note < 0)
                continue;
            for (int i = from; i < to; ++i)
            {
                v.env = v.releasing ? std::max (0.0f, v.env - step) : std::min (1.0f, v.env + step);
                const float s = (float) std::sin (v.phase) * v.amp * v.env * lv * 0.25f;
                v.phase += v.inc;
                for (int c = 0; c < b.getNumChannels(); ++c)
                    b.addSample (c, i, s);
            }
            if (v.releasing && v.env <= 0.0f)
                v.note = -1;
        }
    }

    juce::AudioParameterFloat* level = nullptr;
    double sampleRate = 48000.0;
    std::array<Voice, 16> voices {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TestSynth)
};
} // namespace

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TestSynth();
}
