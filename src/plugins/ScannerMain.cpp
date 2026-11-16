// B4ND Plugin Scanner: loads and checks one plugin file in its own process (docs/05 §2).
//   "B4ND Plugin Scanner" --scan <plugin file> --out <report.xml>
// If a plugin crashes or hangs here, only this process dies; the app blacklists the file.

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include "plugins/PluginCatalog.h"

int main (int argc, char* argv[])
{
    juce::StringArray args;
    for (int i = 1; i < argc; ++i)
        args.add (juce::String::fromUTF8 (argv[i]));
    const int s = args.indexOf ("--scan"), o = args.indexOf ("--out");
    if (s < 0 || o < 0 || s + 1 >= args.size() || o + 1 >= args.size())
    {
        std::fprintf (stderr, "usage: --scan <plugin file> --out <report.xml>\n");
        return 2;
    }
    const auto pluginFile = args[s + 1];
    const juce::File out (args[o + 1]);
    out.deleteFile();

    const juce::ScopedJuceInitialiser_GUI juce;   // VST3 plugins expect a message thread (this one)

    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> descs;
    format.findAllTypesForFile (descs, pluginFile);

    std::vector<b4nd::CertificationResult> results;
    for (auto* d : descs)
    {
        juce::String error;
        b4nd::CertificationResult r;
        if (auto inst = format.createInstanceFromDescription (*d, 48000.0, 256, error))
            r = b4nd::certifyPlugin (*inst);
        else
            r.reason = "cannot be loaded: " + error;
        results.push_back (r);
        std::printf ("%s: %s %s\n", d->name.toRawUTF8(), r.certified ? "CERTIFIED" : "NOT CERTIFIED", r.reason.toRawUTF8());
    }

    auto report = b4nd::makeScanReport (pluginFile, descs, results);
    if (descs.isEmpty())
        report.setAttribute ("error", "no VST3 plugin found in the file");
    return report.writeTo (out) ? 0 : 3;
}
