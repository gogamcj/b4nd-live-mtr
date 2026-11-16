#include "plugins/PluginHost.h"

namespace b4nd
{

class PluginHost::EditorWindow final : public juce::DocumentWindow
{
public:
    EditorWindow (PluginHost& h, std::shared_ptr<PluginInstrument> inst, const juce::String& title)
        : juce::DocumentWindow (title, juce::Colours::black, juce::DocumentWindow::closeButton), host (h), instrument (std::move (inst))
    {
        setUsingNativeTitleBar (true);
        auto& p = instrument->getPlugin();
        juce::AudioProcessorEditor* ed = nullptr;
        try
        {
            ed = p.hasEditor() ? p.createEditorIfNeeded() : nullptr;
        }
        catch (...)
        {
            ed = nullptr;
        }
        if (ed == nullptr)
            ed = new juce::GenericAudioProcessorEditor (p);
        setContentOwned (ed, true);
        setResizable (ed->isResizable(), false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    ~EditorWindow() override
    {
        // The editor must go before the instance (it belongs to the plugin).
        if (auto* ed = dynamic_cast<juce::AudioProcessorEditor*> (getContentComponent()))
            instrument->getPlugin().editorBeingDeleted (ed);
        clearContentComponent();
    }

    void closeButtonPressed() override
    {
        const auto* key = instrument.get();
        juce::MessageManager::callAsync ([h = &host, key, alive = alive] { if (*alive) h->closeEditor (key); });
    }

    std::shared_ptr<bool> alive = std::make_shared<bool> (true);

private:
    PluginHost& host;
    std::shared_ptr<PluginInstrument> instrument;
};

PluginHost::PluginHost (juce::File cacheFile) : catalog (std::move (cacheFile))
{
    formats.addFormat (std::make_unique<juce::VST3PluginFormat>());
    catalog.load();
}

PluginHost::~PluginHost()
{
    closeAllEditors();
    scanner.cancel();
}

std::vector<const PluginRecord*> PluginHost::getLoadableInstruments() const
{
    std::vector<const PluginRecord*> v;
    for (const auto& r : catalog.getRecords())
        if (r.status != PluginRecord::Status::Blacklisted && (r.desc.isInstrument || r.desc.name.isNotEmpty()))
            v.push_back (&r);
    std::stable_sort (v.begin(), v.end(), [] (const PluginRecord* a, const PluginRecord* b)
    {
        if (a->desc.isInstrument != b->desc.isInstrument)
            return a->desc.isInstrument;
        if ((a->status == PluginRecord::Status::Certified) != (b->status == PluginRecord::Status::Certified))
            return a->status == PluginRecord::Status::Certified;
        return a->desc.name.compareIgnoreCase (b->desc.name) < 0;
    });
    return v;
}

const PluginRecord* PluginHost::resolve (const juce::String& uid, const juce::String& descriptionXml) const
{
    if (auto* r = catalog.findByUid (uid))
        return r;
    if (auto xml = juce::parseXML (descriptionXml))
    {
        juce::PluginDescription d;
        if (d.loadFromXml (*xml))
            return catalog.findSimilar (d);
    }
    return nullptr;
}

PluginHost::LoadResult PluginHost::load (const PluginRecord& rec, double sampleRate, int blockSize)
{
    LoadResult res;
    if (rec.status == PluginRecord::Status::Blacklisted)
    {
        res.error = "blacklisted: " + rec.reason;
        return res;
    }
    if (! juce::File (rec.file).exists())
    {
        res.error = "plugin file not found: " + rec.file;
        return res;
    }
    juce::String error;
    std::unique_ptr<juce::AudioPluginInstance> inst;
    try
    {
        inst = formats.createPluginInstance (rec.desc, sampleRate, blockSize, error);
    }
    catch (...)
    {
        error = "exception while creating the plugin";
    }
    if (inst == nullptr)
    {
        res.error = error.isNotEmpty() ? error : juce::String ("the plugin could not be created");
        return res;
    }
    auto pi = std::make_shared<PluginInstrument> (std::move (inst));
    if (! pi->prepare (sampleRate, blockSize, error))
    {
        res.error = "prepare failed: " + error;
        return res;
    }
    res.instrument = std::move (pi);
    return res;
}

juce::String PluginHost::scanFileNow (const juce::String& file)
{
    juce::String failure;
    auto report = PluginScanner::scanOne (PluginScanner::findScannerExecutable(), file, scanner.timeoutMs, failure);
    if (report != nullptr)
        catalog.applyScanReport (file, *report);
    else if (! failure.startsWith ("scanner program not found") && ! failure.startsWith ("could not start"))
        catalog.blacklist (file, failure);
    catalog.save();
    return report != nullptr ? juce::String() : failure;
}

void PluginHost::showEditor (const std::shared_ptr<PluginInstrument>& inst, const juce::String& title)
{
    if (inst == nullptr)
        return;
    if (auto it = editors.find (inst.get()); it != editors.end())
    {
        it->second->toFront (true);
        return;
    }
    editors[inst.get()] = std::make_unique<EditorWindow> (*this, inst, title);
}

void PluginHost::closeEditor (const PluginInstrument* p)
{
    if (auto it = editors.find (p); it != editors.end())
    {
        *it->second->alive = false;
        editors.erase (it);
    }
}

void PluginHost::closeAllEditors()
{
    for (auto& e : editors)
        *e.second->alive = false;
    editors.clear();
}

} // namespace b4nd
