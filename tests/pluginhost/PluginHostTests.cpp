// VST3 hosting with real plugins (tests/plugins): the out-of-process scanner, blacklist,
// Live Certified checks, the plugin cache, and a VST3 instrument rendering through PluginInstrument.

#include "TestHarness.h"
#include "../engine/AllocationGuard.h"
#include "plugins/PluginCatalog.h"
#include "plugins/PluginInstrument.h"

#include <juce_events/juce_events.h>

using namespace b4nd;

namespace
{
juce::File pluginDir() { return juce::File (B4ND_TEST_PLUGIN_DIR); }
juce::File scanner()   { return juce::File (B4ND_SCANNER_EXE); }
juce::String pluginFile (const char* name) { return pluginDir().getChildFile (juce::String (name) + ".vst3").getFullPathName(); }

std::unique_ptr<juce::XmlElement> scan (const char* name, juce::String& failure, int timeoutMs = 120000)
{
    return PluginScanner::scanOne (scanner(), pluginFile (name), timeoutMs, failure);
}

const juce::XmlElement* firstResult (const juce::XmlElement& report) { return report.getChildByName ("RESULT"); }
} // namespace

TEST_CASE ("Scanner: B4ND Test Synth is Live Certified")
{
    juce::String failure;
    auto report = scan ("B4ND Test Synth", failure);
    CHECK (report != nullptr);
    if (report == nullptr) { std::printf ("    %s\n", failure.toRawUTF8()); return; }
    auto* r = firstResult (*report);
    CHECK (r != nullptr);
    if (r == nullptr) return;
    CHECK (r->getBoolAttribute ("certified"));
    CHECK (r->getDoubleAttribute ("load48") < 0.5);
    CHECK (r->getChildByName ("PLUGIN") != nullptr);
}

TEST_CASE ("Scanner: a plugin that crashes while loading is reported, the host survives")
{
    juce::String failure;
    auto report = scan ("B4ND Test Crash", failure);
    CHECK (report == nullptr);
    CHECK (failure.startsWith ("crashed"));
}

TEST_CASE ("Scanner: a plugin that hangs past the timeout is reported as hung")
{
    juce::String failure;
    auto report = scan ("B4ND Test Slow", failure, 300);   // its certification takes seconds
    CHECK (report == nullptr);
    CHECK (failure.startsWith ("hung"));
}

TEST_CASE ("Scanner: too slow for real time is NOT CERTIFIED")
{
    juce::String failure;
    auto report = scan ("B4ND Test Slow", failure);
    CHECK (report != nullptr);
    if (report == nullptr) return;
    auto* r = firstResult (*report);
    CHECK (r != nullptr && ! r->getBoolAttribute ("certified"));
    CHECK (r != nullptr && r->getDoubleAttribute ("load48") > 0.5);
}

TEST_CASE ("Scanner: state that does not survive save/restore is NOT CERTIFIED")
{
    juce::String failure;
    auto report = scan ("B4ND Test BadState", failure);
    CHECK (report != nullptr);
    if (report == nullptr) return;
    auto* r = firstResult (*report);
    CHECK (r != nullptr && ! r->getBoolAttribute ("certified"));
    CHECK (r != nullptr && r->getStringAttribute ("reason").containsIgnoreCase ("state"));
}

TEST_CASE ("Catalog: scan results, blacklist and the cache survive save/load")
{
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("b4nd-pluginhost-test");
    dir.deleteRecursively();
    dir.createDirectory();
    const auto cache = dir.getChildFile ("plugin-cache.xml");

    juce::String uid;
    {
        PluginCatalog cat (cache);
        juce::String failure;
        auto synth = scan ("B4ND Test Synth", failure);
        CHECK (synth != nullptr);
        if (synth == nullptr) return;
        cat.applyScanReport (pluginFile ("B4ND Test Synth"), *synth);
        CHECK (scan ("B4ND Test Crash", failure) == nullptr);
        cat.blacklist (pluginFile ("B4ND Test Crash"), failure);
        cat.addSearchFolder (dir);

        CHECK (cat.getRecords().size() == 2);
        CHECK (cat.isBlacklisted (pluginFile ("B4ND Test Crash")));
        CHECK (! cat.needsScan (juce::File (pluginFile ("B4ND Test Synth"))));
        CHECK (cat.needsScan (juce::File (pluginFile ("B4ND Test BadState"))));
        for (const auto& r : cat.getRecords())
            if (r.status != PluginRecord::Status::Blacklisted)
                uid = r.uid();
        CHECK (uid.isNotEmpty());
        CHECK (cat.save());
    }
    {
        PluginCatalog cat (cache);
        CHECK (cat.load());
        CHECK (cat.getRecords().size() == 2);
        CHECK (cat.isBlacklisted (pluginFile ("B4ND Test Crash")));
        auto* r = cat.findByUid (uid);
        CHECK (r != nullptr);
        if (r != nullptr)
        {
            CHECK (r->status == PluginRecord::Status::Certified);
            CHECK (r->desc.name == "B4ND Test Synth");
            CHECK (r->desc.isInstrument);
            CHECK (cat.findSimilar (r->desc) != nullptr);
        }
        CHECK (cat.getExtraFolders().contains (dir.getFullPathName()));
        CHECK (! cat.needsScan (juce::File (pluginFile ("B4ND Test Synth"))));
        cat.clearBlacklist();
        CHECK (! cat.isBlacklisted (pluginFile ("B4ND Test Crash")));
    }
    {
        // A damaged cache file must not stop the app: it starts empty.
        cache.replaceWithText ("<not xml");
        PluginCatalog cat (cache);
        CHECK (! cat.load());
        CHECK (cat.getRecords().empty());
    }
    dir.deleteRecursively();
}

TEST_CASE ("PluginInstrument: the real test synth plays notes, state round-trips, render does not allocate")
{
    const juce::ScopedJuceInitialiser_GUI juceInit;   // VST3 hosting needs a message thread (this one); outlives the instance
    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> descs;
    format.findAllTypesForFile (descs, pluginFile ("B4ND Test Synth"));
    CHECK (descs.size() == 1);
    if (descs.isEmpty()) return;
    juce::String error;
    auto inst = format.createInstanceFromDescription (*descs[0], 48000.0, 256, error);
    CHECK (inst != nullptr);
    if (inst == nullptr) { std::printf ("    %s\n", error.toRawUTF8()); return; }

    PluginInstrument pi (std::move (inst));
    CHECK (pi.prepare (48000.0, 256, error));
    CHECK (pi.isPreparedFor (48000.0, 256));
    CHECK (! pi.isPreparedFor (96000.0, 256));

    std::vector<float> l (256), r (256);
    const MidiBytes on { { 0x90, 60, 100 }, 3 }, off { { 0x80, 60, 0 }, 3 };
    const int at0 = 0;

    // Silence before any note.
    CHECK (pi.render (l.data(), r.data(), 256, nullptr, nullptr, 0));
    float peak = 0.0f;
    for (float v : l) peak = std::max (peak, std::abs (v));
    CHECK (peak <= 0.0f);

    pi.render (l.data(), r.data(), 256, &on, &at0, 1);   // warm-up outside the guard
    std::size_t allocs = 0;
    peak = 0.0f;
    {
        b4ndtest::AllocationGuard g;
        for (int b = 0; b < 50; ++b)
        {
            const bool ok = pi.render (l.data(), r.data(), 256, b == 0 ? &on : nullptr, b == 0 ? &at0 : nullptr, b == 0 ? 1 : 0);
            CHECK (ok);
            for (float v : l) peak = std::max (peak, std::abs (v));
        }
        allocs = g.count();
    }
    CHECK (peak > 0.05f);      // the sine is audible
    CHECK (allocs == 0);
    if (allocs != 0) std::printf ("    %d allocations in render\n", (int) allocs);

    pi.render (l.data(), r.data(), 256, &off, &at0, 1);
    for (int b = 0; b < 40; ++b)
        pi.render (l.data(), r.data(), 256, nullptr, nullptr, 0);
    peak = 0.0f;
    for (float v : l) peak = std::max (peak, std::abs (v));
    CHECK (peak < 1.0e-4f);    // note-off ends the voice
    CHECK (! pi.hasFailed());

    // State round trip and re-prepare for another rate.
    auto& proc = pi.getPlugin();
    CHECK (! proc.getParameters().isEmpty());
    proc.getParameters()[0]->setValueNotifyingHost (0.25f);
    const auto s1 = pi.getState();
    CHECK (! s1.isEmpty());
    proc.getParameters()[0]->setValueNotifyingHost (0.9f);
    CHECK (pi.setState (s1));
    CHECK_NEAR (proc.getParameters()[0]->getValue(), 0.25f, 1.0e-4f);
    CHECK (pi.prepare (96000.0, 512, error));
    CHECK (pi.isPreparedFor (96000.0, 512));

    // A block larger than prepared is refused safely (silence, no overrun).
    std::vector<float> bl (1024, 1.0f), br (1024, 1.0f);
    CHECK (pi.render (bl.data(), br.data(), 1024, nullptr, nullptr, 0));
    CHECK (std::abs (bl[1000]) <= 0.0f);
}

TEST_CASE ("Scanner: missing scanner program fails cleanly")
{
    juce::String failure;
    auto report = PluginScanner::scanOne (juce::File ("/nonexistent/b4nd_plugin_scanner"), pluginFile ("B4ND Test Synth"), 1000, failure);
    CHECK (report == nullptr);
    CHECK (failure.startsWith ("scanner program not found"));
}
