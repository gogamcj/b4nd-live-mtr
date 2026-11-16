#pragma once

#include "plugins/PluginCatalog.h"
#include "plugins/PluginInstrument.h"

#include <map>
#include <memory>

namespace b4nd
{

/**
    The app's VST3 host (docs/05): the plugin catalog, the out-of-process scanner, creating
    instrument instances from the catalog, and plugin editor windows. Message thread only.

    Safety rules:
      - only plugins that the scanner process already loaded successfully are ever loaded into
        the app (a blacklisted or never scanned plugin is refused);
      - instances are created with the device's rate and block size, and re-prepared when the
        device changes (the engine plays silence for that plugin meanwhile);
      - editors are closed before their instance goes away.
*/
class PluginHost
{
public:
    explicit PluginHost (juce::File cacheFile = PluginCatalog::defaultCacheFile());
    ~PluginHost();

    PluginCatalog& getCatalog() noexcept { return catalog; }
    PluginScanner& getScanner() noexcept { return scanner; }

    /** Instruments (and effects that accept MIDI) the catalog can load, best first. */
    std::vector<const PluginRecord*> getLoadableInstruments() const;

    /** Finds a saved plugin again: by uid, else the same name + vendor (another PC / version). */
    const PluginRecord* resolve (const juce::String& uid, const juce::String& descriptionXml) const;

    struct LoadResult
    {
        std::shared_ptr<PluginInstrument> instrument;
        juce::String error;
    };
    /** Creates and prepares an instance. Refuses blacklisted plugins. */
    LoadResult load (const PluginRecord&, double sampleRate, int blockSize);

    /** Scans one plugin file now (blocking, out of process) and adds it to the catalog. */
    juce::String scanFileNow (const juce::String& file);

    // --- editors ---
    void showEditor (const std::shared_ptr<PluginInstrument>&, const juce::String& title);
    void closeEditor (const PluginInstrument*);
    void closeAllEditors();
    bool hasEditorOpen (const PluginInstrument* p) const { return editors.count (p) > 0; }

private:
    class EditorWindow;

    PluginCatalog catalog;
    PluginScanner scanner { catalog };
    juce::AudioPluginFormatManager formats;
    std::map<const PluginInstrument*, std::unique_ptr<EditorWindow>> editors;
};

} // namespace b4nd
