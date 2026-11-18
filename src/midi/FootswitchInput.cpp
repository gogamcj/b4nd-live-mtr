#include "midi/FootswitchInput.h"

#include <map>

namespace b4nd
{

namespace
{
class JuceMidiInputBackend final : public MidiInputBackend,
                                   private juce::MidiInputCallback
{
public:
    ~JuceMidiInputBackend() override
    {
        for (auto& [id, p] : open_)
            if (p.input != nullptr)
                p.input->stop();
        open_.clear();
    }

    std::vector<MidiPortInfo> listInputs() override
    {
        std::vector<MidiPortInfo> r;
        for (const auto& d : juce::MidiInput::getAvailableDevices())
            r.push_back ({ d.name, d.identifier });
        return r;
    }

    bool open (const MidiPortInfo& info, Callback cb) override
    {
        close (info.identifier);
        auto in = juce::MidiInput::openDevice (info.identifier, this);
        if (in == nullptr)
            return false;
        {
            const juce::ScopedLock sl (cbLock);
            auto& p = open_[info.identifier];
            p.callback = std::move (cb);
            p.input = std::move (in);
            byInput[p.input.get()] = &p;
        }
        open_[info.identifier].input->start();
        return true;
    }

    void close (const juce::String& identifier) override
    {
        auto it = open_.find (identifier);
        if (it == open_.end())
            return;
        if (it->second.input != nullptr)
            it->second.input->stop();   // no callback after this returns
        const juce::ScopedLock sl (cbLock);
        byInput.erase (it->second.input.get());
        open_.erase (it);
    }

private:
    struct Port { std::unique_ptr<juce::MidiInput> input; Callback callback; };

    void handleIncomingMidiMessage (juce::MidiInput* source, const juce::MidiMessage& m) override
    {
        const auto* raw = m.getRawData();
        const int len = m.getRawDataSize();
        if (len < 2 || raw[0] < 0x80 || raw[0] >= 0xf0)
            return;   // only channel messages matter for footswitches
        const auto msg = len >= 3 ? MidiBytes::make (raw[0], raw[1], raw[2]) : MidiBytes::make (raw[0], raw[1]);
        const juce::ScopedLock sl (cbLock);
        if (auto it = byInput.find (source); it != byInput.end() && it->second->callback)
            it->second->callback (msg);
    }

    std::map<juce::String, Port> open_;
    std::map<juce::MidiInput*, Port*> byInput;
    juce::CriticalSection cbLock;
};
} // namespace

std::unique_ptr<MidiInputBackend> createJuceMidiInputBackend()
{
    return std::make_unique<JuceMidiInputBackend>();
}

//==============================================================================
FootswitchInput::FootswitchInput (std::unique_ptr<MidiInputBackend> b) : backend (std::move (b))
{
    mapper.setBindings (FootswitchMapper::defaultBindings());
}

FootswitchInput::~FootswitchInput()
{
    shutdown();
}

void FootswitchInput::start (bool withTimer)
{
    if (withTimer)
        startTimer (1000);
}

void FootswitchInput::shutdown()
{
    stopTimer();
    cancelPendingUpdate();
    for (const auto& p : ports)
        backend->close (p.port.identifier);
    ports.clear();
    const juce::SpinLock::ScopedLockType sl (lock);
    portNames.clear();
}

void FootswitchInput::setPorts (const std::vector<MidiPortInfo>& wanted)
{
    for (const auto& p : ports)
        backend->close (p.port.identifier);
    ports.clear();
    for (const auto& w : wanted)
        if (w.name.isNotEmpty() || w.identifier.isNotEmpty())
            ports.push_back ({ w, PortState::Status::Disconnected });
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        portNames.clear();
        for (const auto& p : ports)
            portNames.push_back (p.port.name.toStdString());
        mapper.reset();
    }
    checkPorts();
}

void FootswitchInput::setBindings (const std::vector<FootswitchBinding>& b)
{
    const juce::SpinLock::ScopedLockType sl (lock);
    mapper.setBindings (b);
}

std::vector<FootswitchBinding> FootswitchInput::getBindings() const
{
    const juce::SpinLock::ScopedLockType sl (lock);
    return mapper.getBindings();
}

void FootswitchInput::setLearning (bool on)
{
    const juce::SpinLock::ScopedLockType sl (lock);
    mapper.setLearning (on);
}

bool FootswitchInput::isLearning() const
{
    const juce::SpinLock::ScopedLockType sl (lock);
    return mapper.isLearning();
}

void FootswitchInput::openPort (int index)
{
    auto& p = ports[(size_t) index];
    const bool ok = backend->open (p.port, [this, index] (const MidiBytes& m) {
        receive (index, m, juce::Time::getMillisecondCounterHiRes());
    });
    p.status = ok ? PortState::Status::Open : PortState::Status::OpenFailed;
    juce::Logger::writeToLog ("Footswitch input " + p.port.name + (ok ? ": open" : ": CANNOT OPEN (used by another program?)"));
}

void FootswitchInput::checkPorts()
{
    if (ports.empty())
        return;
    const auto present = backend->listInputs();
    bool changed = false;
    for (int i = 0; i < (int) ports.size(); ++i)
    {
        auto& p = ports[(size_t) i];
        const MidiPortInfo* found = nullptr;
        for (const auto& x : present)
            if (p.port.identifier.isNotEmpty() && x.identifier == p.port.identifier)
                found = &x;
        if (found == nullptr)
            for (const auto& x : present)
                if (x.name == p.port.name)
                    found = &x;
        if (found == nullptr)
        {
            if (p.status != PortState::Status::Disconnected)
            {
                backend->close (p.port.identifier);
                p.status = PortState::Status::Disconnected;
                juce::Logger::writeToLog ("FOOTSWITCH INPUT DISCONNECTED: " + p.port.name);
                changed = true;
            }
            continue;
        }
        if (p.status == PortState::Status::Open && found->identifier == p.port.identifier)
            continue;
        if (p.status == PortState::Status::Open)
            backend->close (p.port.identifier);   // came back with another identifier
        p.port.identifier = found->identifier;
        const auto before = p.status;
        openPort (i);
        changed = changed || p.status != before;
    }
    if (changed && onPortsChanged)
        onPortsChanged();
}

void FootswitchInput::timerCallback()
{
    checkPorts();
}

void FootswitchInput::receive (int portIndex, const MidiBytes& msg, double nowMs)
{
    Event e {};
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        static const std::string noName;
        const auto& name = juce::isPositiveAndBelow (portIndex, (int) portNames.size()) ? portNames[(size_t) portIndex] : noName;
        const auto r = mapper.handle (msg, name, nowMs);
        if (! r.press)
            return;
        e.action = r.action;
        e.learned = r.learned;
        e.waiting = r.waitingForSecond;
        e.note = r.input.note;
        e.channel = (std::int8_t) r.input.channel;
        e.number = (std::uint8_t) r.input.number;
        e.port = (std::int16_t) portIndex;
        e.atMs = nowMs;
        if (! queue.push (e))
        {
            dropped.fetch_add (1);
            return;
        }
    }
    triggerAsyncUpdate();
}

void FootswitchInput::handleAsyncUpdate()
{
    Event e {};
    while (queue.pop (e))
    {
        FootswitchBinding in;
        in.port = juce::isPositiveAndBelow ((int) e.port, (int) ports.size()) ? ports[(size_t) e.port].port.name.toStdString() : std::string();
        in.note = e.note;
        in.channel = e.channel;
        in.number = e.number;
        in.action = e.action;
        last.input = in;
        last.action = e.action;
        last.waiting = e.waiting;
        last.atMs = e.atMs;
        ++last.count;
        if (e.learned)
        {
            setLearning (false);
            if (onLearned)
                onLearned (in);
        }
        else if (e.action != LiveAction::None && onAction)
        {
            onAction (e.action);
        }
        else if (onPortsChanged)
        {
            onPortsChanged();   // a press without action: refresh "last input" displays
        }
    }
}

} // namespace b4nd
