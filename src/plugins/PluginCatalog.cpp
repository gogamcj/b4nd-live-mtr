#include "plugins/PluginCatalog.h"
#include "plugins/PluginInstrument.h"

#include <cmath>

namespace b4nd
{

juce::String PluginRecord::statusText (Status s)
{
    switch (s)
    {
        case Status::Certified:    return "LIVE CERTIFIED";
        case Status::NotCertified: return "NOT CERTIFIED";
        case Status::Blacklisted:  return "BLACKLISTED";
    }
    return {};
}

//==============================================================================
CertificationResult certifyPlugin (juce::AudioPluginInstance& p)
{
    CertificationResult r;
    juce::StringArray problems;
    constexpr int block = 256;

    auto layout = p.getBusesLayout();
    if (! layout.outputBuses.isEmpty())
    {
        layout.outputBuses.getReference (0) = juce::AudioChannelSet::stereo();
        for (auto& in : layout.inputBuses)
            in = juce::AudioChannelSet::disabled();
        if (p.checkBusesLayoutSupported (layout))
            p.setBusesLayout (layout);
    }
    if (p.getTotalNumOutputChannels() <= 0)
        problems.add ("no audio output");

    auto runAt = [&] (double sr, double& avg, double& peak)
    {
        p.setRateAndBufferSizeDetails (sr, block);
        p.prepareToPlay (sr, block);
        juce::AudioBuffer<float> buf (juce::jmax (2, p.getTotalNumInputChannels(), p.getTotalNumOutputChannels()), block);
        juce::MidiBuffer midi;
        const double blockSec = block / sr;
        double sum = 0.0, mx = 0.0;
        bool badOutput = false;
        int faults = 0;
        for (int i = 0; i < 120; ++i)
        {
            buf.clear();
            midi.clear();
            if (i == 20)
                for (int n : { 48, 60, 64, 67, 72 })
                    midi.addEvent (juce::MidiMessage::noteOn (1, n, (juce::uint8) 100), 0);
            if (i == 90)
                for (int n : { 48, 60, 64, 67, 72 })
                    midi.addEvent (juce::MidiMessage::noteOff (1, n), 0);
            const auto t0 = juce::Time::getHighResolutionTicks();
            faults += guardedProcess (p, buf, midi) != 0 ? 1 : 0;
            const auto t1 = juce::Time::getHighResolutionTicks();
            if (i >= 20)   // the first blocks may load samples / warm caches
            {
                const double used = juce::Time::highResolutionTicksToSeconds (t1 - t0) / blockSec;
                sum += used;
                mx = juce::jmax (mx, used);
            }
            for (int c = 0; c < buf.getNumChannels() && ! badOutput; ++c)
                for (int s = 0; s < block; ++s)
                    if (! std::isfinite (buf.getSample (c, s)))
                    {
                        badOutput = true;
                        break;
                    }
        }
        p.releaseResources();
        avg = sum / 100.0;
        peak = mx;
        const auto at = juce::String (juce::roundToInt (sr / 1000.0)) + " kHz";
        if (faults > 0)
            problems.add ("crashed or threw while processing at " + at);
        if (badOutput)
            problems.add ("NaN / Inf in the output at " + at);
        if (avg > 0.5)
            problems.add ("uses " + juce::String (juce::roundToInt (avg * 100.0)) + "% of real time at " + at + " (limit 50%)");
    };

    try
    {
        runAt (48000.0, r.load48, r.peak48);
        runAt (96000.0, r.load96, r.peak96);
    }
    catch (...)
    {
        problems.add ("exception while preparing or processing");
    }

    try
    {
        juce::MemoryBlock a, b;
        p.getStateInformation (a);
        p.setStateInformation (a.getData(), (int) a.getSize());
        p.getStateInformation (b);
        if (a != b)
            problems.add ("state changes after save and restore (" + juce::String ((int) a.getSize()) + " -> "
                          + juce::String ((int) b.getSize()) + " bytes)");
    }
    catch (...)
    {
        problems.add ("exception while saving or restoring its state");
    }

    r.certified = problems.isEmpty();
    r.reason = problems.joinIntoString ("; ");
    return r;
}

juce::XmlElement makeScanReport (const juce::String& file, const juce::OwnedArray<juce::PluginDescription>& descs,
                                 const std::vector<CertificationResult>& results)
{
    juce::XmlElement x ("B4NDSCAN");
    x.setAttribute ("file", file);
    for (int i = 0; i < descs.size(); ++i)
    {
        auto* e = x.createNewChildElement ("RESULT");
        const auto& r = results[(size_t) i];
        e->setAttribute ("certified", r.certified);
        e->setAttribute ("reason", r.reason);
        e->setAttribute ("load48", r.load48);
        e->setAttribute ("peak48", r.peak48);
        e->setAttribute ("load96", r.load96);
        e->setAttribute ("peak96", r.peak96);
        e->addChildElement (descs[i]->createXml().release());
    }
    x.setAttribute ("complete", 1);   // written last: a crashed scanner never produces this
    return x;
}

//==============================================================================
PluginCatalog::PluginCatalog (juce::File f) : cacheFile (std::move (f)) {}

juce::File PluginCatalog::defaultCacheFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("B4ND LIVE MTR").getChildFile ("plugin-cache.xml");
}

juce::FileSearchPath PluginCatalog::getSearchPath() const
{
    juce::VST3PluginFormat fmt;
    auto path = fmt.getDefaultLocationsToSearch();
    for (const auto& f : extraFolders)
        path.addIfNotAlreadyThere (juce::File (f));
    return path;
}

void PluginCatalog::addSearchFolder (const juce::File& f)
{
    extraFolders.addIfNotAlreadyThere (f.getFullPathName());
}

const PluginRecord* PluginCatalog::findByUid (const juce::String& uid) const
{
    for (const auto& r : records)
        if (r.status != PluginRecord::Status::Blacklisted && r.uid() == uid)
            return &r;
    return nullptr;
}

const PluginRecord* PluginCatalog::findSimilar (const juce::PluginDescription& d) const
{
    for (const auto& r : records)
        if (r.status != PluginRecord::Status::Blacklisted && r.desc.name == d.name && r.desc.manufacturerName == d.manufacturerName)
            return &r;
    return nullptr;
}

static juce::Time modTime (const juce::File& f)
{
    // A VST3 bundle is a folder: use its binary when there is one.
    if (f.isDirectory())
    {
        const auto files = f.getChildFile ("Contents").findChildFiles (juce::File::findFiles, true, "*.so;*.vst3;*.dll");
        juce::Time t = f.getLastModificationTime();
        for (const auto& x : files)
            t = juce::jmax (t, x.getLastModificationTime());
        return t;
    }
    return f.getLastModificationTime();
}

bool PluginCatalog::needsScan (const juce::File& f) const
{
    for (const auto& r : records)
        if (r.file == f.getFullPathName())
            return r.fileTime != modTime (f);
    return true;
}

bool PluginCatalog::isBlacklisted (const juce::String& file) const
{
    for (const auto& r : records)
        if (r.file == file && r.status == PluginRecord::Status::Blacklisted)
            return true;
    return false;
}

void PluginCatalog::forget (const juce::String& file)
{
    records.erase (std::remove_if (records.begin(), records.end(), [&] (const PluginRecord& r) { return r.file == file; }),
                   records.end());
}

void PluginCatalog::blacklist (const juce::String& file, const juce::String& reason)
{
    forget (file);
    PluginRecord r;
    r.file = file;
    r.fileTime = modTime (juce::File (file));
    r.status = PluginRecord::Status::Blacklisted;
    r.reason = reason;
    r.checkedAt = juce::Time::getCurrentTime();
    r.desc.name = juce::File (file).getFileNameWithoutExtension();
    r.desc.fileOrIdentifier = file;
    records.push_back (r);
    juce::Logger::writeToLog ("Plugin BLACKLISTED: " + file + " (" + reason + ")");
}

void PluginCatalog::applyScanReport (const juce::String& file, const juce::XmlElement& report)
{
    forget (file);
    int n = 0;
    for (auto* e : report.getChildWithTagNameIterator ("RESULT"))
    {
        PluginRecord r;
        if (auto* d = e->getChildByName ("PLUGIN"); d == nullptr || ! r.desc.loadFromXml (*d))
            continue;
        r.file = file;
        r.fileTime = modTime (juce::File (file));
        r.status = e->getBoolAttribute ("certified") ? PluginRecord::Status::Certified : PluginRecord::Status::NotCertified;
        r.reason = e->getStringAttribute ("reason");
        r.load48 = e->getDoubleAttribute ("load48");
        r.peak48 = e->getDoubleAttribute ("peak48");
        r.load96 = e->getDoubleAttribute ("load96");
        r.peak96 = e->getDoubleAttribute ("peak96");
        r.checkedAt = juce::Time::getCurrentTime();
        juce::Logger::writeToLog ("Plugin " + r.desc.name + " (" + r.desc.manufacturerName + "): " + PluginRecord::statusText (r.status)
                                  + (r.reason.isNotEmpty() ? " - " + r.reason : juce::String())
                                  + "  load " + juce::String (r.load48 * 100.0, 1) + "% @48k");
        records.push_back (r);
        ++n;
    }
    if (n == 0)
        blacklist (file, report.getStringAttribute ("error", "no loadable plugin in the file"));
}

void PluginCatalog::clearBlacklist()
{
    records.erase (std::remove_if (records.begin(), records.end(),
                                   [] (const PluginRecord& r) { return r.status == PluginRecord::Status::Blacklisted; }),
                   records.end());
}

bool PluginCatalog::load()
{
    records.clear();
    extraFolders.clear();
    auto xml = juce::XmlDocument::parse (cacheFile);
    if (xml == nullptr || ! xml->hasTagName ("B4NDPLUGINS"))
        return false;
    for (auto* f : xml->getChildWithTagNameIterator ("FOLDER"))
        extraFolders.addIfNotAlreadyThere (f->getStringAttribute ("path"));
    for (auto* e : xml->getChildWithTagNameIterator ("RECORD"))
    {
        PluginRecord r;
        if (auto* d = e->getChildByName ("PLUGIN"))
            r.desc.loadFromXml (*d);
        r.file = e->getStringAttribute ("file");
        r.fileTime = juce::Time (e->getStringAttribute ("fileTime").getLargeIntValue());
        const auto st = e->getStringAttribute ("status");
        r.status = st == "certified" ? PluginRecord::Status::Certified
                 : st == "blacklisted" ? PluginRecord::Status::Blacklisted
                                       : PluginRecord::Status::NotCertified;
        r.reason = e->getStringAttribute ("reason");
        r.load48 = e->getDoubleAttribute ("load48");
        r.peak48 = e->getDoubleAttribute ("peak48");
        r.load96 = e->getDoubleAttribute ("load96");
        r.peak96 = e->getDoubleAttribute ("peak96");
        r.checkedAt = juce::Time (e->getStringAttribute ("checkedAt").getLargeIntValue());
        records.push_back (r);
    }
    return true;
}

bool PluginCatalog::save() const
{
    juce::XmlElement x ("B4NDPLUGINS");
    for (const auto& f : extraFolders)
        x.createNewChildElement ("FOLDER")->setAttribute ("path", f);
    for (const auto& r : records)
    {
        auto* e = x.createNewChildElement ("RECORD");
        e->setAttribute ("file", r.file);
        e->setAttribute ("fileTime", juce::String (r.fileTime.toMilliseconds()));
        e->setAttribute ("status", r.status == PluginRecord::Status::Certified ? "certified"
                                  : r.status == PluginRecord::Status::Blacklisted ? "blacklisted" : "not-certified");
        e->setAttribute ("reason", r.reason);
        e->setAttribute ("load48", r.load48);
        e->setAttribute ("peak48", r.peak48);
        e->setAttribute ("load96", r.load96);
        e->setAttribute ("peak96", r.peak96);
        e->setAttribute ("checkedAt", juce::String (r.checkedAt.toMilliseconds()));
        if (r.status != PluginRecord::Status::Blacklisted)
            e->addChildElement (r.desc.createXml().release());
    }
    cacheFile.getParentDirectory().createDirectory();
    // Write a temp file and swap it in: a crash while saving never leaves a half-written cache.
    const auto tmp = cacheFile.getSiblingFile (cacheFile.getFileName() + ".tmp");
    return x.writeTo (tmp) && tmp.moveFileTo (cacheFile);
}

//==============================================================================
PluginScanner::PluginScanner (PluginCatalog& c) : juce::Thread ("B4ND plugin scan"), catalog (c) {}

PluginScanner::~PluginScanner()
{
    cancel();
}

juce::File PluginScanner::findScannerExecutable()
{
    if (const auto env = juce::SystemStats::getEnvironmentVariable ("B4ND_PLUGIN_SCANNER", {}); env.isNotEmpty())
        return juce::File (env);
    const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
   #if JUCE_WINDOWS
    return exe.getSiblingFile ("B4ND Plugin Scanner.exe");
   #else
    return exe.getSiblingFile ("B4ND Plugin Scanner");
   #endif
}

void PluginScanner::start (juce::StringArray files, bool onlyChanged)
{
    if (isThreadRunning())
        return;
    if (files.isEmpty())
    {
        juce::VST3PluginFormat fmt;
        files = fmt.searchPathsForPlugins (catalog.getSearchPath(), true, false);
    }
    queue.clear();
    for (const auto& f : files)
        if (! onlyChanged || catalog.needsScan (juce::File (f)))
            queue.add (f);
    scannerExe = findScannerExecutable();
    done = 0;
    total = queue.size();
    juce::Logger::writeToLog ("Plugin scan: " + juce::String (queue.size()) + " file(s) with " + scannerExe.getFullPathName());
    startThread();
}

void PluginScanner::cancel()
{
    stopThread (timeoutMs + 5000);
}

juce::String PluginScanner::getCurrentFile() const
{
    const juce::ScopedLock sl (lock);
    return current;
}

std::unique_ptr<juce::XmlElement> PluginScanner::scanOne (const juce::File& scanner, const juce::String& pluginFile,
                                                          int timeoutMs, juce::String& failure)
{
    if (! scanner.existsAsFile())
    {
        failure = "scanner program not found: " + scanner.getFullPathName();
        return nullptr;
    }
    const auto out = juce::File::createTempFile (".b4ndscan.xml");
    juce::ChildProcess cp;
    if (! cp.start (juce::StringArray { scanner.getFullPathName(), "--scan", pluginFile, "--out", out.getFullPathName() }, 0))
    {
        failure = "could not start the scanner";
        return nullptr;
    }
    if (! cp.waitForProcessToFinish (timeoutMs))
    {
        cp.kill();
        out.deleteFile();
        failure = "hung while loading (no answer in " + juce::String (timeoutMs / 1000) + " s)";
        return nullptr;
    }
    const auto code = cp.getExitCode();
    auto xml = juce::XmlDocument::parse (out);
    out.deleteFile();
    if (xml == nullptr || ! xml->hasTagName ("B4NDSCAN") || ! xml->getBoolAttribute ("complete"))
    {
        failure = "crashed while loading (scanner exit code " + juce::String ((juce::int64) code) + ")";
        return nullptr;
    }
    return xml;
}

void PluginScanner::run()
{
    for (const auto& file : queue)
    {
        if (threadShouldExit())
            break;
        {
            const juce::ScopedLock sl (lock);
            current = file;
        }
        juce::String failure;
        std::shared_ptr<juce::XmlElement> report (scanOne (scannerExe, file, timeoutMs, failure).release());
        juce::MessageManager::callAsync ([this, file, report, failure]
        {
            if (report != nullptr)
                catalog.applyScanReport (file, *report);
            else
                catalog.blacklist (file, failure);
            catalog.save();
            if (onProgress)
                onProgress();
        });
        ++done;
    }
    {
        const juce::ScopedLock sl (lock);
        current.clear();
    }
    juce::MessageManager::callAsync ([this]
    {
        juce::Logger::writeToLog ("Plugin scan finished");
        if (onFinished)
            onFinished();
    });
}

} // namespace b4nd
