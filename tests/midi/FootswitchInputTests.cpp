#include "TestHarness.h"
#include "midi/FootswitchInput.h"

#include <map>
#include <set>
#include <thread>

using namespace b4nd;

namespace
{
class FakeInputs final : public MidiInputBackend
{
public:
    std::vector<MidiPortInfo> present;
    std::map<juce::String, Callback> open_;
    std::set<juce::String> refuse;
    int opens = 0;

    std::vector<MidiPortInfo> listInputs() override { return present; }
    bool open (const MidiPortInfo& p, Callback cb) override
    {
        ++opens;
        if (refuse.count (p.name))
            return false;
        open_[p.identifier] = std::move (cb);
        return true;
    }
    void close (const juce::String& id) override { open_.erase (id); }
    void send (const juce::String& id, const MidiBytes& m)
    {
        if (auto it = open_.find (id); it != open_.end())
            it->second (m);
    }
};

MidiBytes cc (int n, int v) { return MidiBytes::make (0xb0, (std::uint8_t) n, (std::uint8_t) v); }
} // namespace

TEST_CASE ("Footswitch input: only enabled ports, actions on the message thread side, unplug and replug")
{
    auto fake = std::make_unique<FakeInputs>();
    auto* be = fake.get();
    be->present = { { "Keyboard", "kb" }, { "FS-1", "fs1" } };
    FootswitchInput in (std::move (fake));
    std::vector<LiveAction> got;
    in.onAction = [&] (LiveAction a) { got.push_back (a); };
    in.setPorts ({ { "FS-1", "fs1" } });
    CHECK (in.getPorts().size() == 1 && in.getPorts()[0].status == FootswitchInput::PortState::Status::Open);
    CHECK (be->open_.count ("fs1") == 1 && be->open_.count ("kb") == 0);   // the keyboard's sustain pedal is never heard

    be->send ("fs1", cc (64, 127));
    CHECK (got.empty());   // queued, not run on the MIDI thread
    in.deliverPending();
    CHECK (got.size() == 1 && got[0] == LiveAction::PlayPause);
    CHECK (in.getLastPress().count == 1 && in.getLastPress().input.number == 64 && in.getLastPress().input.port == "FS-1");

    // unplugged: closed and shown; same name with a new identifier comes back: reopened
    be->present = { { "Keyboard", "kb" } };
    in.setPorts ({ { "FS-1", "fs1" } });
    CHECK (in.getPorts()[0].status == FootswitchInput::PortState::Status::Disconnected);
    be->present = { { "Keyboard", "kb" }, { "FS-1", "fs1-new" } };
    in.setPorts (std::vector<MidiPortInfo> { { "FS-1", "fs1" } });
    CHECK (in.getPorts()[0].status == FootswitchInput::PortState::Status::Open);
    CHECK (in.getPorts()[0].port.identifier == "fs1-new");
    be->send ("fs1-new", cc (65, 127));
    in.deliverPending();
    CHECK (got.size() == 2 && got[1] == LiveAction::NextSong);

    // used by another program
    be->refuse.insert ("FS-1");
    in.setPorts ({ { "FS-1", "fs1-new" } });
    CHECK (in.getPorts()[0].status == FootswitchInput::PortState::Status::OpenFailed);
    in.shutdown();
    CHECK (be->open_.empty());
}

TEST_CASE ("Footswitch input: learn mode gives the next press, then acts normally again")
{
    auto fake = std::make_unique<FakeInputs>();
    auto* be = fake.get();
    be->present = { { "FS-1", "fs1" } };
    FootswitchInput in (std::move (fake));
    in.setPorts ({ { "FS-1", "fs1" } });
    std::vector<LiveAction> got;
    std::vector<FootswitchBinding> learned;
    in.onAction = [&] (LiveAction a) { got.push_back (a); };
    in.onLearned = [&] (const FootswitchBinding& b) { learned.push_back (b); };
    in.setLearning (true);
    in.receive (0, cc (64, 127), 1000.0);
    in.deliverPending();
    CHECK (got.empty() && learned.size() == 1 && learned[0].number == 64 && learned[0].port == "FS-1" && ! learned[0].note);
    CHECK (! in.isLearning());
    in.receive (0, cc (64, 0), 1100.0);
    in.receive (0, cc (64, 127), 1200.0);
    in.deliverPending();
    CHECK (got.size() == 1);
}

TEST_CASE ("Footswitch input: presses from several MIDI threads at once are all delivered")
{
    auto fake = std::make_unique<FakeInputs>();
    FootswitchInput in (std::move (fake));
    std::vector<FootswitchBinding> b;
    for (int n = 0; n < 4; ++n)
    {
        FootswitchBinding x;
        x.number = 20 + n;
        x.action = LiveAction::NextCue;
        b.push_back (x);
    }
    in.setBindings (b);
    int count = 0;
    in.onAction = [&] (LiveAction) { ++count; };
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back ([&in, t] {
            for (int k = 0; k < 20; ++k)
            {
                in.receive (0, cc (20 + t, 127), k * 100.0);
                in.receive (0, cc (20 + t, 0), k * 100.0 + 50.0);
            }
        });
    for (auto& th : threads)
        th.join();
    in.deliverPending();
    CHECK (count == 80);
    CHECK (in.getDroppedCount() == 0);
}
