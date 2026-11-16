#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>
#include <vector>

namespace b4nd
{

/**
    What we know about one plugin (docs/05 §2): its description, and whether it passed the
    Live Certified checks. A plugin file that crashed or hung the scanner is blacklisted.
*/
struct PluginRecord
{
    enum class Status { Certified, NotCertified, Blacklisted };

    juce::PluginDescription desc;      // empty name for a blacklisted file
    juce::String file;                 // plugin file / bundle path
    juce::Time fileTime;               // last modification: rescan when it changes
    Status status = Status::NotCertified;
    juce::String reason;               // why not certified / why blacklisted
    double load48 = 0.0, peak48 = 0.0; // processing time / block duration at 48 kHz (avg, max)
    double load96 = 0.0, peak96 = 0.0;
    juce::Time checkedAt;

    juce::String uid() const { return desc.createIdentifierString(); }
    static juce::String statusText (Status);
};

/** Result of checking one plugin instance (runs inside the scanner process). */
struct CertificationResult
{
    bool certified = false;
    juce::String reason;
    double load48 = 0.0, peak48 = 0.0, load96 = 0.0, peak96 = 0.0;
};

/**
    Live Certified checks on a loaded instance:
      - it has an audio output; instruments accept MIDI
      - prepareToPlay at 48 kHz and 96 kHz (256 samples) succeeds
      - 100 blocks (with notes) are processed in under 50 % of real time on average,
        with no NaN / Inf output
      - state survives getState -> setState -> getState unchanged
*/
CertificationResult certifyPlugin (juce::AudioPluginInstance&);

/** The scanner process protocol: one plugin file in, an XML report out. */
juce::XmlElement makeScanReport (const juce::String& file, const juce::OwnedArray<juce::PluginDescription>&,
                                 const std::vector<CertificationResult>&);

/**
    The plugin cache (%APPDATA%/B4ND LIVE MTR/plugin-cache.xml).
    Message thread only.
*/
class PluginCatalog
{
public:
    explicit PluginCatalog (juce::File cacheFile = defaultCacheFile());

    static juce::File defaultCacheFile();
    /** VST3 default folders plus the user's extra folders. */
    juce::FileSearchPath getSearchPath() const;
    void addSearchFolder (const juce::File&);
    const juce::StringArray& getExtraFolders() const noexcept { return extraFolders; }

    const std::vector<PluginRecord>& getRecords() const noexcept { return records; }
    const PluginRecord* findByUid (const juce::String& uid) const;
    /** Same plugin by name + manufacturer when the uid differs (another PC, another version). */
    const PluginRecord* findSimilar (const juce::PluginDescription&) const;

    /** True when the file was not scanned yet or changed since. */
    bool needsScan (const juce::File&) const;
    bool isBlacklisted (const juce::String& file) const;

    /** Replaces everything known about `file` with a scanner report (or a blacklist entry). */
    void applyScanReport (const juce::String& file, const juce::XmlElement& report);
    void blacklist (const juce::String& file, const juce::String& reason);
    void forget (const juce::String& file);
    void clearBlacklist();

    bool load();
    bool save() const;

private:
    juce::File cacheFile;
    std::vector<PluginRecord> records;
    juce::StringArray extraFolders;
};

/**
    Scans plugin files one by one in a separate process ("B4ND Plugin Scanner"), so a plugin that
    crashes or hangs while loading cannot take the app down: it is blacklisted instead.
    Runs on its own thread; results are applied on the message thread.
*/
class PluginScanner : private juce::Thread
{
public:
    explicit PluginScanner (PluginCatalog&);
    ~PluginScanner() override;

    /** Scanner executable next to the app (or $B4ND_PLUGIN_SCANNER). */
    static juce::File findScannerExecutable();

    /** Scans `files` (or every plugin found in the catalog's search path when empty).
        `onlyChanged`: skip files already scanned and unchanged. */
    void start (juce::StringArray files = {}, bool onlyChanged = true);
    void cancel();
    bool isScanning() const { return isThreadRunning(); }

    /** Progress for the UI. */
    juce::String getCurrentFile() const;
    int getDone() const noexcept { return done.load(); }
    int getTotal() const noexcept { return total.load(); }

    /** Called on the message thread after each file and at the end. */
    std::function<void()> onProgress, onFinished;

    int timeoutMs = 120000;   // per file

    /** Runs the scanner on one file and waits (any thread). Returns the report, or null + reason. */
    static std::unique_ptr<juce::XmlElement> scanOne (const juce::File& scanner, const juce::String& pluginFile,
                                                       int timeoutMs, juce::String& failure);

private:
    void run() override;

    PluginCatalog& catalog;
    juce::StringArray queue;
    juce::File scannerExe;
    mutable juce::CriticalSection lock;
    juce::String current;
    std::atomic<int> done { 0 }, total { 0 };
};

} // namespace b4nd
