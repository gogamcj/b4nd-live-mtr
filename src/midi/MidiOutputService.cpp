#include "midi/MidiOutputService.h"

#include <algorithm>
#include <map>
#include <thread>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <timeapi.h>
 #pragma comment(lib, "winmm.lib")
#endif

namespace b4nd
{

namespace
{
constexpr std::int64_t sendAheadNs = 100'000;      // send up to 0.1 ms before the due time
constexpr std::int64_t spinWindowNs = 2'000'000;   // closer than this: spin instead of sleeping
constexpr std::int64_t lateThresholdNs = 2'000'000;
constexpr size_t pendingCapacity = (size_t) AudioEngine::midiQueueSize * 2;

double nowMs() { return juce::Time::getMillisecondCounterHiRes(); }

//==============================================================================
class JuceMidiBackend final : public MidiPortBackend
{
public:
    std::vector<MidiPortInfo> listOutputs() override
    {
        std::vector<MidiPortInfo> r;
        for (const auto& d : juce::MidiOutput::getAvailableDevices())
            r.push_back ({ d.name, d.identifier });
        return r;
    }

    bool open (int slot, const MidiPortInfo& info) override
    {
        // Several modules often share one interface (different channels). Windows can open
        // a MIDI port only once, so one device object is shared by all slots on that port.
        std::shared_ptr<juce::MidiOutput> dev;
        for (size_t i = 0; i < outs.size(); ++i)
            if (outs[i] != nullptr && ids[i] == info.identifier)
                dev = outs[i];
        if (dev == nullptr)
            dev = std::shared_ptr<juce::MidiOutput> (juce::MidiOutput::openDevice (info.identifier).release());
        if (dev == nullptr)
            return false;
        const juce::ScopedLock sl (lock);
        outs[(size_t) slot] = dev;
        ids[(size_t) slot] = info.identifier;
        return true;
    }

    void close (int slot) override
    {
        std::shared_ptr<juce::MidiOutput> old;
        {
            const juce::ScopedLock sl (lock);
            old.swap (outs[(size_t) slot]);
            ids[(size_t) slot].clear();
        }
        // `old` is destroyed here, outside the lock (closing a WinMM port can take a moment).
    }

    void send (int slot, const MidiBytes& m) override
    {
        std::shared_ptr<juce::MidiOutput> dev;
        {
            const juce::ScopedLock sl (lock);
            dev = outs[(size_t) slot];
        }
        if (dev != nullptr)
            dev->sendMessageNow (juce::MidiMessage (m.data, (int) m.size));
    }

private:
    juce::CriticalSection lock;
    std::array<std::shared_ptr<juce::MidiOutput>, MidiOutputService::maxSlots> outs;
    std::array<juce::String, MidiOutputService::maxSlots> ids;
};

bool samePreset (const ModuleProfile& a, const ModuleProfile& b)
{
    return a.channel == b.channel && a.bankMsb == b.bankMsb && a.bankLsb == b.bankLsb && a.program == b.program
        && a.volumeCc == b.volumeCc && a.volume == b.volume && a.panCc == b.panCc && a.pan == b.pan
        && a.sendOrder == b.sendOrder && a.gapMs == b.gapMs && a.settleMs == b.settleMs;
}
} // namespace

std::unique_ptr<MidiPortBackend> createJuceMidiBackend()
{
    return std::make_unique<JuceMidiBackend>();
}

//==============================================================================
MidiOutputService::MidiOutputService (AudioEngine* e, std::unique_ptr<MidiPortBackend> b)
    : juce::Thread ("B4ND MIDI out"), engine (e), backend (std::move (b))
{
    pending.reserve (pendingCapacity);
    incoming.reserve (1024);
    immediate.reserve (1024);
}

MidiOutputService::~MidiOutputService()
{
    shutdown();
}

void MidiOutputService::start (bool withTimer)
{
   #if JUCE_WINDOWS
    // 1 ms sleep resolution for the MIDI thread (the Windows default is ~15.6 ms).
    if (! timerPeriodRaised)
        timerPeriodRaised = timeBeginPeriod (1) == TIMERR_NOERROR;
   #endif
    if (! isThreadRunning())
        startRealtimeThread (juce::Thread::RealtimeOptions{}.withPriority (8));
    if (! isThreadRunning())
        startThread (juce::Thread::Priority::highest);
    if (withTimer)
        startTimer (100);
}

void MidiOutputService::shutdown()
{
    stopTimer();
    if (isThreadRunning())
    {
        panic();
        // Let the thread send the releases before it stops.
        for (int i = 0; i < 50 && panicRequested.load(); ++i)
            juce::Thread::sleep (2);
        stopThread (1000);
    }
    for (int i = 0; i < maxSlots; ++i)
        closeSlot (i);
   #if JUCE_WINDOWS
    if (timerPeriodRaised)
        timeEndPeriod (1);
    timerPeriodRaised = false;
   #endif
}

//==============================================================================
juce::String MidiOutputService::statusText (PortStatus s)
{
    switch (s)
    {
        case PortStatus::NoPort:       return "NO PORT";
        case PortStatus::Disconnected: return "DISCONNECTED";
        case PortStatus::OpenFailed:   return "CANNOT OPEN";
        case PortStatus::Preparing:    return "PREPARING";
        case PortStatus::Ready:        return "READY";
    }
    return {};
}

MidiOutputService::PortStatus MidiOutputService::getStatus (int slot) const noexcept
{
    if (slot < 0 || slot >= maxSlots)
        return PortStatus::NoPort;
    return (PortStatus) slots[(size_t) slot].status.load (std::memory_order_relaxed);
}

void MidiOutputService::setStatus (int slot, PortStatus s)
{
    auto& sl = slots[(size_t) slot];
    if (sl.status.exchange ((int) s) == (int) s)
        return;
    juce::Logger::writeToLog ("MIDI module " + juce::String (slot + 1) + " ("
                              + juce::String::fromUTF8 (sl.profile.deviceName.c_str()) + " @ "
                              + juce::String::fromUTF8 (sl.profile.portName.c_str()) + "): " + statusText (s));
    if (onStatusChanged)
        onStatusChanged();
}

//==============================================================================
void MidiOutputService::setModules (const std::vector<ModuleProfile>& modules)
{
    for (int i = 0; i < maxSlots; ++i)
    {
        auto& s = slots[(size_t) i];
        if (i >= (int) modules.size())
        {
            if (s.open)
                closeSlot (i);
            s.assigned = false;
            s.profile = {};
            setStatus (i, PortStatus::NoPort);
            continue;
        }
        const auto& p = modules[(size_t) i];
        const bool portChanged = ! s.assigned || s.profile.portName != p.portName || s.profile.portIdentifier != p.portIdentifier;
        const bool presetChanged = ! samePreset (s.profile, p);
        s.profile = p;
        s.assigned = true;
        s.channel.store (std::clamp (p.channel, 1, 16) - 1);
        if (portChanged && s.open)
            closeSlot (i);
        else if (presetChanged && s.open)
            queueRecall (i);
    }
    pollPorts();
}

void MidiOutputService::pollPorts()
{
    const auto available = backend->listOutputs();
    {
        juce::StringArray names;
        for (const auto& a : available)
            names.add (a.name);
        const auto list = names.isEmpty() ? juce::String ("(none)") : names.joinIntoString (", ");
        if (list != lastPortList)
        {
            lastPortList = list;
            juce::Logger::writeToLog ("MIDI outputs: " + list);
        }
    }
    auto find = [&available] (const ModuleProfile& p) -> const MidiPortInfo*
    {
        const auto id = juce::String::fromUTF8 (p.portIdentifier.c_str());
        const auto name = juce::String::fromUTF8 (p.portName.c_str());
        if (id.isNotEmpty())
            for (const auto& a : available)
                if (a.identifier == id)
                    return &a;
        // Identifiers can change when a USB device moves to another socket: fall back to the name.
        for (const auto& a : available)
            if (a.name == name)
                return &a;
        return nullptr;
    };

    for (int i = 0; i < maxSlots; ++i)
    {
        auto& s = slots[(size_t) i];
        if (! s.assigned)
            continue;
        if (s.profile.portName.empty() && s.profile.portIdentifier.empty())
        {
            if (s.open)
                closeSlot (i);
            setStatus (i, PortStatus::NoPort);
            continue;
        }
        const auto* found = find (s.profile);
        if (s.open)
        {
            if (found == nullptr || found->identifier != s.openedAs.identifier)
            {
                closeSlot (i);
                setStatus (i, PortStatus::Disconnected);
                if (found != nullptr)
                    openSlot (i, *found);
            }
        }
        else if (found != nullptr)
        {
            openSlot (i, *found);
        }
        else
        {
            setStatus (i, PortStatus::Disconnected);
        }
    }
}

void MidiOutputService::openSlot (int slot, const MidiPortInfo& info)
{
    auto& s = slots[(size_t) slot];
    bool ok;
    {
        const juce::ScopedLock sl (slotLocks[(size_t) slot]);
        ok = backend->open (slot, info);
        s.open = ok;
        s.openedAs = ok ? info : MidiPortInfo {};
    }
    if (! ok)
    {
        setStatus (slot, PortStatus::OpenFailed);
        return;
    }
    resetSounding[(size_t) slot] = true;
    // A module that was switched off or replugged may still hold notes or a pedal: reset first.
    const auto ch = (std::uint8_t) s.channel.load();
    sendLater (slot, MidiBytes::make ((std::uint8_t) (0xb0 | ch), 64, 0), 0.0);
    sendLater (slot, MidiBytes::make ((std::uint8_t) (0xb0 | ch), 123, 0), 0.0);
    queueRecall (slot);
}

void MidiOutputService::closeSlot (int slot)
{
    auto& s = slots[(size_t) slot];
    const juce::ScopedLock sl (slotLocks[(size_t) slot]);
    if (s.open)
        backend->close (slot);
    s.open = false;
    s.openedAs = {};
    resetSounding[(size_t) slot] = true;
}

void MidiOutputService::queueRecall (int slot)
{
    if (slot < 0 || slot >= maxSlots || ! slots[(size_t) slot].open)
        return;
    auto& s = slots[(size_t) slot];
    double readyAt = 0.0;
    const double start = s.profile.gapMs > 0 ? (double) s.profile.gapMs : 0.0;   // after the reset messages
    double prevAt = -start;   // the first one keeps its gap after the reset messages
    for (const auto& m : presetRecallSequence (s.profile, &readyAt))
    {
        // The gap is kept relative to the previous message actually sent, so a late wake-up of
        // the MIDI thread cannot squeeze bank MSB / LSB / program together.
        sendLater (slot, m.msg, start + m.atMs, (std::max) (0.0, m.atMs - prevAt));
        prevAt = m.atMs;
    }
    s.readyAtMs = nowMs() + start + readyAt;
    setStatus (slot, PortStatus::Preparing);
}

void MidiOutputService::recallPreset (int slot)
{
    queueRecall (slot);
}

void MidiOutputService::sendTestNote (int slot, int note, int velocity, int lengthMs)
{
    if (slot < 0 || slot >= maxSlots)
        return;
    const auto ch = (std::uint8_t) slots[(size_t) slot].channel.load();
    sendLater (slot, MidiBytes::make ((std::uint8_t) (0x90 | ch), (std::uint8_t) (note & 0x7f), (std::uint8_t) std::clamp (velocity, 1, 127)), 0.0);
    sendLater (slot, MidiBytes::make ((std::uint8_t) (0x80 | ch), (std::uint8_t) (note & 0x7f), 0), (double) (std::max) (10, lengthMs));
}

void MidiOutputService::sendLater (int slot, const MidiBytes& m, double delayMs, double minGapMs)
{
    Pending p;
    p.minGapNs = (std::int64_t) ((std::max) (0.0, minGapMs) * 1.0e6);
    p.dueNs = AudioEngine::clockNs() + (std::int64_t) ((std::max) (0.0, delayMs) * 1.0e6);
    p.slot = slot;
    p.msg = m;
    const juce::ScopedLock sl (immediateLock);
    immediate.push_back (p);
    notify();
}

void MidiOutputService::panic()
{
    panicRequested = true;
    notify();
}

void MidiOutputService::updatePreparing()
{
    const double now = nowMs();
    for (int i = 0; i < maxSlots; ++i)
        if (getStatus (i) == PortStatus::Preparing && now >= slots[(size_t) i].readyAtMs)
            setStatus (i, PortStatus::Ready);
}

void MidiOutputService::timerCallback()
{
    updatePreparing();
    if (--pollCountdown <= 0)
    {
        pollCountdown = 10;   // once a second
        pollPorts();
    }
}

//==============================================================================
void MidiOutputService::sendOnThread (const Pending& p)
{
    if (p.slot < 0 || p.slot >= maxSlots || p.msg.size == 0)
    {
        dropped.fetch_add (1, std::memory_order_relaxed);
        return;
    }
    {
        const juce::ScopedLock sl (slotLocks[(size_t) p.slot]);
        if (! slots[(size_t) p.slot].open)
        {
            dropped.fetch_add (1, std::memory_order_relaxed);   // disconnected: nothing to send to
            return;
        }
        backend->send (p.slot, p.msg);
    }
    sounding[(size_t) p.slot].observe (p.msg);
    sent.fetch_add (1, std::memory_order_relaxed);

    const auto lateNs = AudioEngine::clockNs() - p.dueNs;
    if (lateNs > lateThresholdNs)
        late.fetch_add (1, std::memory_order_relaxed);
    if (lateNs > maxLateNs.load (std::memory_order_relaxed))
        maxLateNs.store (lateNs, std::memory_order_relaxed);
}

void MidiOutputService::doPanicOnThread()
{
    // Nothing new may start; releases queued by the engine are covered by the releases below.
    pending.erase (std::remove_if (pending.begin(), pending.end(), [] (const Pending& p) { return p.msg.isNoteOn(); }),
                   pending.end());
    for (int i = 0; i < maxSlots; ++i)
    {
        const juce::ScopedLock sl (slotLocks[(size_t) i]);
        if (! slots[(size_t) i].open)
        {
            sounding[(size_t) i].reset();
            continue;
        }
        auto send = [this, i] (const MidiBytes& m) { backend->send (i, m); sent.fetch_add (1, std::memory_order_relaxed); };
        sounding[(size_t) i].releaseAll (send);
        // Also the module's own channel, even if this run never played on it.
        const auto ch = (std::uint8_t) slots[(size_t) i].channel.load();
        send (MidiBytes::make ((std::uint8_t) (0xb0 | ch), 64, 0));
        send (MidiBytes::make ((std::uint8_t) (0xb0 | ch), 123, 0));
    }
}

void MidiOutputService::run()
{
    while (! threadShouldExit())
    {
        for (int i = 0; i < maxSlots; ++i)
            if (resetSounding[(size_t) i].exchange (false))
                sounding[(size_t) i].reset();

        bool added = false;
        if (engine != nullptr)
        {
            MidiOutEvent e;
            while (engine->popMidi (e))
            {
                if (pending.size() >= pendingCapacity)
                {
                    dropped.fetch_add (1, std::memory_order_relaxed);
                    continue;
                }
                if (e.msg.isController (123))
                {
                    // The engine released this channel (stop, locate, song change). Notes it queued
                    // ahead of time must not start after the release: drop them.
                    const int port = (int) e.port, ch = e.msg.channel();
                    pending.erase (std::remove_if (pending.begin(), pending.end(), [port, ch] (const Pending& p)
                                   { return p.slot == port && p.msg.isNoteOn() && p.msg.channel() == ch; }),
                                   pending.end());
                }
                pending.push_back ({ e.dueNs, seqCounter++, (int) e.port, e.msg });
                added = true;
            }
        }
        {
            const juce::ScopedLock sl (immediateLock);
            incoming.swap (immediate);
        }
        for (auto& p : incoming)
        {
            p.seq = seqCounter++;
            pending.push_back (p);
            added = true;
        }
        incoming.clear();
        if (added)
            std::sort (pending.begin(), pending.end(), [] (const Pending& a, const Pending& b)
                       { return a.dueNs != b.dueNs ? a.dueNs < b.dueNs : a.seq < b.seq; });

        if (panicRequested.load())
        {
            doPanicOnThread();
            panicRequested = false;
        }

        const auto now = AudioEngine::clockNs();
        std::array<std::int64_t, maxSlots> holdUntil {};   // a slot held back for a gap keeps its order
        bool anySent = false, deferred = false;
        for (size_t i = 0; i < pending.size() && pending[i].dueNs <= now + sendAheadNs; ++i)
        {
            auto& p = pending[i];
            const bool validSlot = p.slot >= 0 && p.slot < maxSlots;
            if (validSlot)
            {
                const auto slot = (size_t) p.slot;
                std::int64_t notBefore = holdUntil[slot];
                if (p.minGapNs > 0 && lastSentNs[slot] > 0)
                    notBefore = (std::max) (notBefore, lastSentNs[slot] + p.minGapNs);
                if (notBefore > now + sendAheadNs)
                {
                    p.dueNs = (std::max) (p.dueNs, notBefore);
                    holdUntil[slot] = p.dueNs;
                    deferred = true;
                    continue;
                }
            }
            sendOnThread (p);
            if (validSlot)
                lastSentNs[(size_t) p.slot] = AudioEngine::clockNs();
            p.done = true;
            anySent = true;
        }
        if (anySent)
            pending.erase (std::remove_if (pending.begin(), pending.end(), [] (const Pending& p) { return p.done; }),
                           pending.end());
        if (deferred)
            std::sort (pending.begin(), pending.end(), [] (const Pending& a, const Pending& b)
                       { return a.dueNs != b.dueNs ? a.dueNs < b.dueNs : a.seq < b.seq; });

        if (pending.empty())
        {
            wait (1);
            continue;
        }
        const auto untilNext = pending.front().dueNs - AudioEngine::clockNs();
        if (untilNext > spinWindowNs)
            wait (1);
        else if (untilNext > sendAheadNs)
            std::this_thread::yield();
    }
}

} // namespace b4nd
