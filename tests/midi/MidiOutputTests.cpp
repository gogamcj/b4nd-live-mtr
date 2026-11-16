// MIDI output thread, port manager and SMF import (docs/04). No hardware: a fake port backend.

#include "TestHarness.h"
#include "midi/MidiFileImport.h"
#include "midi/MidiOutputService.h"

#include <memory>
#include <algorithm>
#include <mutex>
#include <thread>

using namespace b4nd;

namespace
{
struct Sent
{
    int slot;
    MidiBytes msg;
    std::int64_t atNs;
};

class FakeBackend final : public MidiPortBackend
{
public:
    std::vector<MidiPortInfo> listOutputs() override
    {
        std::lock_guard<std::mutex> l (m);
        return ports;
    }
    bool open (int slot, const MidiPortInfo& info) override
    {
        std::lock_guard<std::mutex> l (m);
        if (info.name == busyName)
            return false;
        opened.push_back (slot);
        openedIds[(size_t) slot] = info.identifier;
        return true;
    }
    void close (int slot) override
    {
        std::lock_guard<std::mutex> l (m);
        closed.push_back (slot);
    }
    void send (int slot, const MidiBytes& b) override
    {
        const auto t = AudioEngine::clockNs();
        std::lock_guard<std::mutex> l (m);
        sent.push_back ({ slot, b, t });
    }

    void setPorts (std::vector<MidiPortInfo> p) { std::lock_guard<std::mutex> l (m); ports = std::move (p); }
    std::vector<Sent> takeSent() { std::lock_guard<std::mutex> l (m); auto r = sent; sent.clear(); return r; }
    size_t sentCount() { std::lock_guard<std::mutex> l (m); return sent.size(); }

    std::mutex m;
    std::vector<MidiPortInfo> ports;
    std::vector<Sent> sent;
    std::vector<int> opened, closed;
    std::array<juce::String, 16> openedIds;
    juce::String busyName;
};

ModuleProfile module (const std::string& port, int ch = 1)
{
    ModuleProfile p;
    p.id = "m" + port;
    p.deviceName = "Module " + port;
    p.portName = port;
    p.channel = ch;
    p.bankMsb = 87;
    p.bankLsb = 64;
    p.program = 5;
    p.gapMs = 5;
    p.settleMs = 50;
    return p;
}

void waitFor (const std::function<bool()>& cond, int timeoutMs = 2000)
{
    const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
    while (! cond() && juce::Time::getMillisecondCounterHiRes() < end)
        juce::Thread::sleep (1);
}

bool isCc (const MidiBytes& m, int cc, int value) { return m.isController (cc) && m.data[2] == value; }

std::vector<std::uint8_t> smf (int tpq, const std::vector<std::vector<std::uint8_t>>& trackEvents)
{
    // Minimal SMF writer: header + one track of (delta, bytes...) entries.
    std::vector<std::uint8_t> trk;
    for (const auto& e : trackEvents)
        trk.insert (trk.end(), e.begin(), e.end());
    trk.insert (trk.end(), { 0x00, 0xff, 0x2f, 0x00 });
    std::vector<std::uint8_t> f { 'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, (std::uint8_t) (tpq >> 8), (std::uint8_t) tpq,
                                  'M', 'T', 'r', 'k' };
    const auto n = (std::uint32_t) trk.size();
    f.insert (f.end(), { (std::uint8_t) (n >> 24), (std::uint8_t) (n >> 16), (std::uint8_t) (n >> 8), (std::uint8_t) n });
    f.insert (f.end(), trk.begin(), trk.end());
    return f;
}
} // namespace

TEST_CASE ("SMF import: ticks rescaled to 960 PPQ, only channel messages kept, tempo read")
{
    // tpq 480: tempo 100 bpm, note on at 0, sysex, note off at 480 (1 beat), CC at 720, program at 960
    const auto data = smf (480, {
        { 0x00, 0xff, 0x51, 0x03, 0x09, 0x27, 0xc0 },          // 600000 us/qn = 100 bpm
        { 0x00, 0x92, 60, 100 },
        { 0x00, 0xf0, 0x03, 0x7e, 0x09, 0xf7 },                // sysex: not sent live
        { 0x83, 0x60, 0x82, 60, 0 },                           // delta 480
        { 0x81, 0x70, 0xb2, 64, 127 },                         // delta 240
        { 0x81, 0x70, 0xc2, 7 },                               // delta 240
    });
    const auto r = importMidiData (data.data(), data.size());
    CHECK (r.ok);
    CHECK (r.events.size() == 4);
    CHECK (r.noteCount == 1);
    CHECK (r.channelsUsed == (1u << 2));
    CHECK (std::abs (r.fileBpm - 100.0) < 0.01);
    if (r.events.size() == 4)
    {
        CHECK (r.events[0].tick == 0 && r.events[0].msg.isNoteOn());
        CHECK (r.events[1].tick == 960 && r.events[1].msg.isNoteOff());
        CHECK (r.events[2].tick == 1440 && r.events[2].msg.isController (64));
        CHECK (r.events[3].tick == 1920 && r.events[3].msg.size == 2 && r.events[3].msg.data[1] == 7);
    }
    CHECK (r.lastTick == 1920);

    const char junk[] = "this is not a midi file";
    CHECK (! importMidiData (junk, sizeof (junk)).ok);

    auto smpte = smf (480, { { 0x00, 0x90, 60, 100 } });
    smpte[12] = 0xe7; smpte[13] = 0x28;   // -25 fps, 40 ticks per frame
    const auto s = importMidiData (smpte.data(), smpte.size());
    CHECK (! s.ok && s.error.containsIgnoreCase ("SMPTE"));
}

TEST_CASE ("MIDI out: messages leave at their due time, never early, in order")
{
    auto fake = std::make_unique<FakeBackend>();
    auto* be = fake.get();
    be->setPorts ({ { "UM-ONE", "id-1" } });
    MidiOutputService svc (nullptr, std::move (fake));
    svc.start (false);
    svc.setModules ({ module ("UM-ONE") });
    waitFor ([&] { return be->sentCount() >= 7; });   // reset + recall
    be->takeSent();
    svc.resetTimingStats();

    const auto t0 = AudioEngine::clockNs();
    std::vector<std::int64_t> due;
    for (int i = 0; i < 40; ++i)
    {
        const double ms = 3.0 + i * 2.5;
        svc.sendLater (0, MidiBytes::make (0x90, (std::uint8_t) (40 + i), 100), ms);
        due.push_back (t0 + (std::int64_t) (ms * 1.0e6));
    }
    waitFor ([&] { return be->sentCount() >= 40; });
    const auto got = be->takeSent();
    CHECK (got.size() == 40);
    std::vector<double> lateMs;
    bool ordered = true, early = false;
    for (size_t i = 0; i < got.size() && i < due.size(); ++i)
    {
        ordered = ordered && got[i].msg.data[1] == 40 + (int) i;
        const double l = (double) (got[i].atNs - due[i]) / 1.0e6;
        early = early || l < -0.25;   // the 0.1 ms send-ahead plus sendLater's own clock read
        lateMs.push_back (l);
    }
    CHECK (ordered);
    CHECK (! early);
    std::sort (lateMs.begin(), lateMs.end());
    const double median = lateMs.empty() ? 99.0 : lateMs[lateMs.size() / 2];
    std::printf ("    timing: median %.3f ms, max %.3f ms late\n", median, lateMs.empty() ? 0.0 : lateMs.back());
    CHECK (median < 1.5);   // a shared CI VM still manages this; a show PC is far better
    svc.shutdown();
}

TEST_CASE ("MIDI out: preset recall order and Ready status after the settle time")
{
    auto fake = std::make_unique<FakeBackend>();
    auto* be = fake.get();
    be->setPorts ({ { "UM-ONE", "id-1" } });
    MidiOutputService svc (nullptr, std::move (fake));
    svc.start (false);
    auto p = module ("UM-ONE", 3);
    p.sendOrder = "lsb,msb,pc";
    svc.setModules ({ p });
    CHECK (svc.getStatus (0) == MidiOutputService::PortStatus::Preparing);
    waitFor ([&] { return be->sentCount() >= 7; });
    const auto got = be->takeSent();
    CHECK (got.size() == 7);
    if (got.size() == 7)
    {
        CHECK (isCc (got[0].msg, 64, 0) && got[0].msg.channel() == 2);   // reset first
        CHECK (isCc (got[1].msg, 123, 0));
        CHECK (isCc (got[2].msg, 32, 64));                               // LSB before MSB as configured
        CHECK (isCc (got[3].msg, 0, 87));
        CHECK (got[4].msg.status() == 0xc0 && got[4].msg.data[1] == 5);
        CHECK (isCc (got[5].msg, 7, 100));
        CHECK (isCc (got[6].msg, 10, 64));
        CHECK (got[3].atNs - got[2].atNs >= 4'000'000);                  // gap respected
    }
    svc.updatePreparing();
    CHECK (svc.getStatus (0) == MidiOutputService::PortStatus::Preparing);   // settle time not over
    juce::Thread::sleep (80);
    svc.updatePreparing();
    CHECK (svc.getStatus (0) == MidiOutputService::PortStatus::Ready);

    // Changing only the program sends the preset again, without reopening the port.
    p.program = 9;
    svc.setModules ({ p });
    waitFor ([&] { return be->sentCount() >= 5; });
    const auto again = be->takeSent();
    CHECK (std::any_of (again.begin(), again.end(), [] (const Sent& s) { return s.msg.status() == 0xc0 && s.msg.data[1] == 9; }));
    CHECK (be->opened.size() == 1);
    svc.shutdown();
}

TEST_CASE ("MIDI out: unplug is detected, nothing is sent, replug reopens and recalls the preset")
{
    auto fake = std::make_unique<FakeBackend>();
    auto* be = fake.get();
    be->setPorts ({ { "UM-ONE", "id-1" }, { "INTEGRA-7", "id-2" } });
    MidiOutputService svc (nullptr, std::move (fake));
    svc.start (false);
    svc.setModules ({ module ("UM-ONE"), module ("INTEGRA-7", 2) });
    waitFor ([&] { return be->sentCount() >= 14; });
    be->takeSent();

    be->setPorts ({ { "UM-ONE", "id-1" } });   // INTEGRA-7 unplugged
    svc.pollPorts();
    CHECK (svc.getStatus (1) == MidiOutputService::PortStatus::Disconnected);
    CHECK (svc.getStatus (0) == MidiOutputService::PortStatus::Preparing);   // untouched
    CHECK (be->closed.size() == 1 && be->closed[0] == 1);

    const auto droppedBefore = svc.getDroppedCount();
    svc.sendLater (1, MidiBytes::make (0x91, 60, 100), 0.0);
    waitFor ([&] { return svc.getDroppedCount() > droppedBefore; });
    CHECK (svc.getDroppedCount() == droppedBefore + 1);
    CHECK (be->sentCount() == 0);

    // Back, on another USB socket: new identifier, same name.
    be->setPorts ({ { "UM-ONE", "id-1" }, { "INTEGRA-7", "id-2b" } });
    svc.pollPorts();
    CHECK (svc.getStatus (1) == MidiOutputService::PortStatus::Preparing);
    CHECK (be->openedIds[1] == "id-2b");
    waitFor ([&] { return be->sentCount() >= 7; });
    const auto got = be->takeSent();
    CHECK (got.size() == 7);
    CHECK (! got.empty() && got[0].slot == 1 && isCc (got[0].msg, 64, 0) && got[0].msg.channel() == 1);
    CHECK (std::any_of (got.begin(), got.end(), [] (const Sent& s) { return s.msg.status() == 0xc0; }));

    // A port another program holds open
    be->busyName = "Busy";
    be->setPorts ({ { "UM-ONE", "id-1" }, { "INTEGRA-7", "id-2b" }, { "Busy", "id-3" } });
    svc.setModules ({ module ("UM-ONE"), module ("INTEGRA-7", 2), module ("Busy") });
    CHECK (svc.getStatus (2) == MidiOutputService::PortStatus::OpenFailed);
    auto none = module ("");
    none.portName.clear();
    svc.setModules ({ module ("UM-ONE"), module ("INTEGRA-7", 2), none });
    CHECK (svc.getStatus (2) == MidiOutputService::PortStatus::NoPort);
    svc.shutdown();
}

TEST_CASE ("MIDI out: panic releases every sounding note and pedal, pending note-ons are dropped")
{
    auto fake = std::make_unique<FakeBackend>();
    auto* be = fake.get();
    be->setPorts ({ { "UM-ONE", "id-1" } });
    MidiOutputService svc (nullptr, std::move (fake));
    svc.start (false);
    svc.setModules ({ module ("UM-ONE", 1) });
    waitFor ([&] { return be->sentCount() >= 7; });
    be->takeSent();

    svc.sendLater (0, MidiBytes::make (0x90, 60, 100), 0.0);
    svc.sendLater (0, MidiBytes::make (0x93, 64, 100), 0.0);   // channel 4
    svc.sendLater (0, MidiBytes::make (0xb3, 64, 127), 0.0);   // pedal down
    waitFor ([&] { return be->sentCount() >= 3; });
    be->takeSent();
    svc.sendLater (0, MidiBytes::make (0x90, 67, 100), 500.0); // would start later: must never come
    juce::Thread::sleep (5);
    svc.panic();
    waitFor ([&] { return be->sentCount() >= 6; });
    juce::Thread::sleep (600);
    const auto got = be->takeSent();
    auto has = [&got] (std::uint8_t a, std::uint8_t b) {
        return std::any_of (got.begin(), got.end(), [=] (const Sent& s) { return s.msg.data[0] == a && s.msg.data[1] == b; });
    };
    CHECK (has (0x80, 60));
    CHECK (has (0x83, 64));
    CHECK (has (0xb3, 64) && has (0xb3, 123));
    CHECK (has (0xb0, 64) && has (0xb0, 123));
    CHECK (std::none_of (got.begin(), got.end(), [] (const Sent& s) { return s.msg.isNoteOn(); }));
    svc.shutdown();
}

TEST_CASE ("MIDI out: engine events played in real time arrive on time, module delay sends earlier")
{
    constexpr double sr = 48000.0;
    constexpr int block = 128;
    // On the heap: the engine is ~600 kB and Windows threads have a 1 MB stack.
    std::unique_ptr<AudioEngine> enginePtr = std::make_unique<AudioEngine>();
    AudioEngine& engine = *enginePtr;
    engine.prepare (sr, block, 2, 0);
    engine.setRouting (OutputRouting::createForDevice (2));
    engine.setOutputLatencyMs (5.0);

    auto plan = std::make_unique<PlaybackPlan>();
    plan->lengthSamples = (std::int64_t) (sr * 10);
    for (int port = 0; port < 2; ++port)
    {
        MidiTrackRuntime t;
        t.slot = port;
        t.port = port;
        t.sendEarlyMs = port == 1 ? 3.0 : 0.0;   // module 2 is 3 ms slow
        for (int i = 0; i < 20; ++i)
        {
            const auto s = (std::int64_t) (sr * (0.05 + i * 0.025));
            t.events.push_back ({ s, MidiBytes::make ((std::uint8_t) (0x90 | port), (std::uint8_t) (50 + i), 100) });
            t.events.push_back ({ s + 600, MidiBytes::make ((std::uint8_t) (0x80 | port), (std::uint8_t) (50 + i), 0) });
        }
        plan->midiTracks.push_back (std::move (t));
    }
    engine.setPlan (std::move (plan), sr);

    auto fake = std::make_unique<FakeBackend>();
    auto* be = fake.get();
    be->setPorts ({ { "A", "a" }, { "B", "b" } });
    MidiOutputService svc (&engine, std::move (fake));
    svc.start (false);
    auto a = module ("A"); a.bankMsb = a.bankLsb = a.program = a.volume = a.pan = -1;
    auto b = module ("B", 2); b.bankMsb = b.bankLsb = b.program = b.volume = b.pan = -1;
    svc.setModules ({ a, b });
    waitFor ([&] { return be->sentCount() >= 4; });
    be->takeSent();

    // A fake audio callback thread: one block every block duration.
    std::vector<float> l (block), r (block);
    float* outs[2] { l.data(), r.data() };
    std::vector<std::pair<std::int64_t, std::int64_t>> blockStart;   // (sample, ns)
    EngineCommand play;
    play.type = EngineCommand::Type::Play;
    engine.postCommand (play);
    const auto t0 = AudioEngine::clockNs();
    const auto blockNs = (std::int64_t) (1.0e9 * block / sr);
    for (int i = 0; i < (int) (sr * 0.7 / block); ++i)
    {
        const auto target = t0 + i * blockNs;
        while (AudioEngine::clockNs() < target)
            std::this_thread::yield();
        blockStart.push_back ({ engine.getPosition(), AudioEngine::clockNs() });
        engine.process (nullptr, 0, outs, 2, block);
    }
    waitFor ([&] { return be->sentCount() >= 80; });
    const auto got = be->takeSent();
    CHECK (got.size() == 80);

    // Expected send time: the wall clock of its block + offset + 5 ms latency - module delay.
    auto expectedNs = [&] (std::int64_t sample, int port) {
        auto it = std::upper_bound (blockStart.begin(), blockStart.end(), std::make_pair (sample, std::numeric_limits<std::int64_t>::max()));
        --it;
        return it->second + (std::int64_t) ((double) (sample - it->first) / sr * 1.0e9) + 5'000'000 - (port == 1 ? 3'000'000 : 0);
    };
    double worst = 0.0;
    int checked = 0;
    for (const auto& s : got)
    {
        if (! s.msg.isNoteOn()) continue;
        const int i = s.msg.data[1] - 50;
        const auto sample = (std::int64_t) (sr * (0.05 + i * 0.025));
        const double errMs = (double) (s.atNs - expectedNs (sample, s.slot)) / 1.0e6;
        worst = std::max (worst, std::abs (errMs));
        CHECK (errMs > -0.3);
        ++checked;
    }
    std::printf ("    engine -> port: %d notes, worst %.3f ms from the target time\n", checked, worst);
    CHECK (checked == 40);
    CHECK (svc.getDroppedCount() == 0);
    svc.shutdown();
}
