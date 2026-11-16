#include "ui/PluginPanel.h"
#include "ui/Theme.h"

namespace b4nd
{

namespace
{
juce::Colour recordColour (PluginRecord::Status s)
{
    switch (s)
    {
        case PluginRecord::Status::Certified:    return theme::ok;
        case PluginRecord::Status::NotCertified: return theme::warn;
        case PluginRecord::Status::Blacklisted:  return theme::danger;
    }
    return theme::textFaint;
}

juce::Colour trackColour (const TrackInfo& t)
{
    if (t.pluginStatus == TrackInfo::PluginStatus::Ready)
        return t.pluginCertified ? theme::ok : theme::warn;
    return t.pluginUid.isEmpty() ? theme::textFaint : theme::danger;
}
} // namespace

PluginPanel::PluginPanel (AppState& s) : state (s)
{
    list.setRowHeight (40);
    list.setColour (juce::ListBox::backgroundColourId, theme::panelInset);
    addAndMakeVisible (list);
    for (auto* b : { &scanButton, &rescanButton, &folderButton, &clearButton, &addTrackButton })
    {
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (*b);
    }
    scanButton.setColour (juce::TextButton::buttonColourId, theme::accent.darker (0.4f));
    scanButton.setTooltip ("Checks plugins that are new or changed since the last scan. Each one is loaded in a separate "
                           "scanner program, so a plugin that crashes cannot take B4ND LIVE MTR down: it is blacklisted.");
    rescanButton.setTooltip ("Checks every plugin again (slow)");
    folderButton.setTooltip ("Adds a folder to search for VST3 plugins (besides the standard VST3 folders)");
    clearButton.setTooltip ("Forgets blacklisted plugins so the next scan tries them again");

    scanButton.onClick = [this] { startScan (true); };
    rescanButton.onClick = [this] { startScan (false); };
    clearButton.onClick = [this]
    {
        state.getPluginHost().getCatalog().clearBlacklist();
        state.getPluginHost().getCatalog().save();
        list.updateContent();
        setMessage ("Blacklist cleared. Scan again to retry those plugins.", false);
    };
    folderButton.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Add a VST3 plugin folder", juce::File(), "");
        juce::Component::SafePointer<PluginPanel> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [safe] (const juce::FileChooser& fc)
                              {
                                  if (safe == nullptr || fc.getResult() == juce::File())
                                      return;
                                  safe->state.getPluginHost().getCatalog().addSearchFolder (fc.getResult());
                                  safe->state.getPluginHost().getCatalog().save();
                                  safe->setMessage ("Folder added: " + fc.getResult().getFullPathName() + ". Press SCAN.", false);
                              });
    };
    addTrackButton.onClick = [this]
    {
        const int row = list.getSelectedRow();
        const auto& recs = state.getPluginHost().getCatalog().getRecords();
        if (! juce::isPositiveAndBelow (row, (int) recs.size()))
            return setMessage ("Select a plugin in the list first.", true);
        const auto err = state.setTrackPlugin (-1, recs[(size_t) row].uid());
        setMessage (err.isEmpty() ? "VSTi track added. Drop a .mid file on its lane to give it notes." : err, err.isNotEmpty());
    };

    auto& scanner = state.getPluginHost().getScanner();
    scanner.onProgress = [this] { list.updateContent(); repaint(); };
    scanner.onFinished = [this]
    {
        list.updateContent();
        const auto& recs = state.getPluginHost().getCatalog().getRecords();
        int cert = 0, notCert = 0, black = 0;
        for (const auto& r : recs)
            (r.status == PluginRecord::Status::Certified ? cert : r.status == PluginRecord::Status::Blacklisted ? black : notCert)++;
        setMessage ("Scan finished: " + juce::String (cert) + " Live Certified, " + juce::String (notCert) + " not certified, "
                    + juce::String (black) + " blacklisted.", false);
        state.reloadPlugins();   // missing / not scanned plugins of the project may be available now
    };

    state.addChangeListener (this);
    list.updateContent();
    rebuildTrackRows();
    startTimerHz (4);
    setSize (980, 660);
}

PluginPanel::~PluginPanel()
{
    auto& scanner = state.getPluginHost().getScanner();
    scanner.onProgress = nullptr;
    scanner.onFinished = nullptr;
    state.removeChangeListener (this);
}

juce::String PluginPanel::lockReason() const
{
    if (state.isLiveMode())
        return "LIVE MODE: plugins are locked and editors are closed.";
    if (state.getTransportState() == TransportState::Playing)
        return "Stop playback to change plugins.";
    return {};
}

void PluginPanel::setMessage (const juce::String& text, bool isError)
{
    message = text;
    messageIsError = isError;
    repaint();
}

void PluginPanel::startScan (bool onlyChanged)
{
    if (lockReason().isNotEmpty())
        return;
    auto& host = state.getPluginHost();
    if (! PluginScanner::findScannerExecutable().existsAsFile())
        return setMessage ("The scanner program (b4nd_plugin_scanner) is missing next to the app. Reinstall B4ND LIVE MTR.", true);
    host.getScanner().start ({}, onlyChanged);
    setMessage (host.getScanner().getTotal() == 0 ? juce::String ("Nothing new to scan.") : juce::String ("Scanning..."), false);
}

void PluginPanel::refreshEnablement()
{
    const bool canEdit = lockReason().isEmpty();
    const bool scanning = state.getPluginHost().getScanner().isScanning();
    scanButton.setEnabled (canEdit && ! scanning);
    rescanButton.setEnabled (canEdit && ! scanning);
    folderButton.setEnabled (canEdit && ! scanning);
    clearButton.setEnabled (canEdit && ! scanning);
    addTrackButton.setEnabled (canEdit && ! scanning);
    for (auto& r : trackRows)
    {
        r.plugin->setEnabled (canEdit && ! scanning);
        const auto* song = state.getCurrentSong();
        const bool loaded = song != nullptr && juce::isPositiveAndBelow (r.trackIndex, (int) song->tracks.size())
                            && song->tracks[(size_t) r.trackIndex].instrument != nullptr;
        r.edit->setEnabled (! state.isLiveMode() && loaded);
    }
}

void PluginPanel::rebuildTrackRows()
{
    const auto* song = state.getCurrentSong();
    const auto loadable = state.getPluginHost().getLoadableInstruments();
    juce::String key = song != nullptr ? song->id : juce::String();
    for (const auto* r : loadable)
        key << "|" << r->uid() << (int) r->status;
    if (song != nullptr)
        for (const auto& t : song->tracks)
            key << "|" << t.id << t.pluginUid << (int) t.pluginStatus;
    if (key == trackRowsFor)
        return;
    trackRowsFor = key;

    trackRows.clear();
    choiceUids.clear();
    for (const auto* r : loadable)
        choiceUids.push_back (r->uid());
    if (song == nullptr)
        return;
    for (int i = 0; i < (int) song->tracks.size(); ++i)
    {
        const auto& t = song->tracks[(size_t) i];
        if (t.kind != TrackKind::MidiVsti)
            continue;
        TrackRow row;
        row.trackIndex = i;
        row.plugin = std::make_unique<juce::ComboBox>();
        row.edit = std::make_unique<juce::TextButton> ("EDIT");
        row.plugin->addItem ("(no plugin)", 1);
        int selected = 1;
        for (int k = 0; k < (int) loadable.size(); ++k)
        {
            const auto* r = loadable[(size_t) k];
            row.plugin->addItem (r->desc.name + "  (" + r->desc.manufacturerName + ")"
                                 + (r->status == PluginRecord::Status::Certified ? "" : "  - not certified"), k + 2);
            if (r->uid() == t.pluginUid)
                selected = k + 2;
        }
        if (selected == 1 && t.pluginUid.isNotEmpty())
        {
            row.plugin->addItem (t.pluginName + "  (" + AppState::pluginStatusText (t) + ")", 1000);
            selected = 1000;
        }
        row.plugin->setSelectedId (selected, juce::dontSendNotification);

        const int trackIndex = i;
        auto* combo = row.plugin.get();
        combo->onChange = [this, trackIndex, combo]
        {
            const int id = combo->getSelectedId();
            if (id == 1000)
                return;
            const auto err = id == 1 ? state.clearTrackPlugin (trackIndex)
                                     : juce::isPositiveAndBelow (id - 2, (int) choiceUids.size())
                                           ? state.setTrackPlugin (trackIndex, choiceUids[(size_t) (id - 2)]) : juce::String();
            if (err.isNotEmpty())
                setMessage (err, true);
            trackRowsFor.clear();
            juce::Component::SafePointer<PluginPanel> safe (this);
            juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->rebuildTrackRows(); });
        };
        row.edit->setWantsKeyboardFocus (false);
        row.edit->onClick = [this, trackIndex]
        {
            if (const auto err = state.showPluginEditor (trackIndex); err.isNotEmpty())
                setMessage (err, true);
        };
        addAndMakeVisible (*row.plugin);
        addAndMakeVisible (*row.edit);
        trackRows.push_back (std::move (row));
    }
    resized();
    refreshEnablement();
}

void PluginPanel::changeListenerCallback (juce::ChangeBroadcaster*)
{
    rebuildTrackRows();
    refreshEnablement();
    repaint();
}

void PluginPanel::timerCallback()
{
    refreshEnablement();
    repaint (progressArea);
    repaint (tracksArea);
    repaint (lockArea);
}

int PluginPanel::getNumRows()
{
    return (int) state.getPluginHost().getCatalog().getRecords().size();
}

void PluginPanel::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    const auto& recs = state.getPluginHost().getCatalog().getRecords();
    if (! juce::isPositiveAndBelow (row, (int) recs.size()))
        return;
    const auto& r = recs[(size_t) row];
    if (selected)
        g.fillAll (theme::accent.withAlpha (0.18f));
    g.setColour (theme::outline);
    g.drawHorizontalLine (height - 1, 0.0f, (float) width);

    auto area = juce::Rectangle<int> (0, 0, width, height).reduced (10, 4);
    g.setColour (recordColour (r.status));
    g.fillEllipse (juce::Rectangle<float> (9.0f, 9.0f).withCentre ({ (float) area.getX() + 5.0f, (float) area.getCentreY() }));
    area.removeFromLeft (18);
    auto pill = area.removeFromRight (130);
    g.setFont (theme::font (10.0f, true));
    g.drawText (PluginRecord::statusText (r.status), pill, juce::Justification::centredRight);

    g.setColour (theme::text);
    g.setFont (theme::font (13.0f, true));
    g.drawFittedText (r.desc.name + (r.desc.isInstrument || r.status == PluginRecord::Status::Blacklisted ? "" : "  (effect)"), area.removeFromTop (area.getHeight() / 2),
                      juce::Justification::centredLeft, 1);
    g.setColour (theme::textDim);
    g.setFont (theme::font (11.0f));
    juce::String sub = r.status == PluginRecord::Status::Blacklisted
                           ? r.reason
                           : r.desc.manufacturerName + "  " + r.desc.version + "   load " + juce::String (r.load48 * 100.0, 1)
                                 + "% @48k / " + juce::String (r.load96 * 100.0, 1) + "% @96k";
    g.drawFittedText (sub, area, juce::Justification::centredLeft, 1);
}

void PluginPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    auto header = [&g] (juce::Rectangle<int> r, const juce::String& t)
    {
        g.setColour (theme::accent);
        g.setFont (theme::font (12.0f, true));
        g.drawText (t, r, juce::Justification::centredLeft);
        g.setColour (theme::outline);
        g.drawHorizontalLine (r.getBottom() - 1, (float) r.getX(), (float) r.getRight());
    };
    header (listHeader, "VST3 PLUGINS  (" + juce::String (getNumRows()) + ")");
    header (tracksHeader, "VSTi TRACKS IN THIS SONG");

    // Details of the selected plugin
    {
        auto d = detailArea;
        g.setColour (theme::panelInset);
        g.fillRoundedRectangle (d.toFloat(), 4.0f);
        d.reduce (12, 8);
        const auto& recs = state.getPluginHost().getCatalog().getRecords();
        const int row = list.getSelectedRow();
        g.setFont (theme::font (11.5f));
        if (juce::isPositiveAndBelow (row, (int) recs.size()))
        {
            const auto& r = recs[(size_t) row];
            g.setColour (recordColour (r.status));
            g.setFont (theme::font (14.0f, true));
            g.drawText (r.desc.name + "   " + PluginRecord::statusText (r.status), d.removeFromTop (22), juce::Justification::centredLeft);
            g.setFont (theme::font (11.5f));
            g.setColour (theme::textDim);
            auto line = [&] (const juce::String& t) { g.drawFittedText (t, d.removeFromTop (18), juce::Justification::centredLeft, 1); };
            line ("File: " + r.file);
            if (r.status != PluginRecord::Status::Blacklisted)
            {
                line ("Vendor: " + r.desc.manufacturerName + "   Version: " + r.desc.version + "   "
                      + (r.desc.isInstrument ? "Instrument" : "Effect"));
                line ("DSP load (256 samples): " + juce::String (r.load48 * 100.0, 1) + "% avg, " + juce::String (r.peak48 * 100.0, 0)
                      + "% peak @48k   " + juce::String (r.load96 * 100.0, 1) + "% avg, " + juce::String (r.peak96 * 100.0, 0) + "% peak @96k");
            }
            if (r.reason.isNotEmpty())
            {
                g.setColour (r.status == PluginRecord::Status::Certified ? theme::textDim : theme::warn);
                line ((r.status == PluginRecord::Status::Blacklisted ? "Blacklisted: " : "Not certified: ") + r.reason);
            }
            g.setColour (theme::textFaint);
            line ("Checked " + r.checkedAt.toString (true, true, false));
        }
        else
        {
            g.setColour (theme::textDim);
            g.drawFittedText ("LIVE CERTIFIED: loads in the scanner, prepares at 48 and 96 kHz, plays in under 50% of real time, "
                              "no invalid output, and its settings survive save/load.\n"
                              "NOT CERTIFIED: usable for rehearsal; Live Mode warns about it.\n"
                              "BLACKLISTED: crashed or hung the scanner. It is never loaded into B4ND LIVE MTR.",
                              d, juce::Justification::topLeft, 6);
        }
    }

    // Scan progress / messages
    {
        auto p = progressArea;
        auto& scanner = state.getPluginHost().getScanner();
        if (scanner.isScanning())
        {
            g.setColour (theme::accent);
            g.setFont (theme::font (12.0f, true));
            g.drawFittedText ("SCANNING " + juce::String (scanner.getDone() + 1) + " / " + juce::String (scanner.getTotal()) + ":  "
                              + juce::File (scanner.getCurrentFile()).getFileName(), p, juce::Justification::centredLeft, 1);
        }
        else if (message.isNotEmpty())
        {
            g.setColour (messageIsError ? theme::danger : theme::textDim);
            g.setFont (theme::font (12.0f, true));
            g.drawFittedText (message, p, juce::Justification::centredLeft, 1);
        }
    }

    // Track rows
    if (const auto* song = state.getCurrentSong())
    {
        for (const auto& r : trackRows)
        {
            const auto& t = song->tracks[(size_t) r.trackIndex];
            g.setFont (theme::font (12.0f, true));
            g.setColour (theme::trackVsti);
            g.drawFittedText (t.name, tracksArea.getX(), r.plugin->getY(), r.plugin->getX() - tracksArea.getX() - 10,
                              r.plugin->getHeight(), juce::Justification::centredLeft, 1);
            g.setFont (theme::font (11.0f, true));
            g.setColour (trackColour (t));
            juce::String st = AppState::pluginStatusText (t);
            if (t.instrument != nullptr && t.pluginStatus == TrackInfo::PluginStatus::Ready)
                st << "   DSP " << juce::String (t.instrument->getLoad() * 100.0f, 1) << "%";
            if (t.pluginError.isNotEmpty())
                st << "   " << t.pluginError;
            if (t.midiPath.isEmpty())
                st << "   (no .mid file)";
            g.drawFittedText (st, r.edit->getRight() + 12, r.edit->getY(), tracksArea.getRight() - r.edit->getRight() - 12,
                              r.edit->getHeight(), juce::Justification::centredLeft, 1);
        }
        if (trackRows.empty())
        {
            g.setColour (theme::textFaint);
            g.setFont (theme::font (12.0f));
            g.drawText ("No VSTi tracks in this song. Select a plugin above and press + ADD VSTi TRACK.", tracksArea.withHeight (26),
                        juce::Justification::centredLeft);
        }
    }

    if (const auto reason = lockReason(); reason.isNotEmpty())
    {
        g.setColour (theme::warn);
        g.setFont (theme::font (12.0f, true));
        g.drawText (reason, lockArea, juce::Justification::centredLeft);
    }
}

void PluginPanel::resized()
{
    auto r = getLocalBounds().reduced (16, 12);
    lockArea = r.removeFromBottom (22);
    auto tracks = r.removeFromBottom (juce::jmax (100, 34 + (int) trackRows.size() * 32));
    r.removeFromBottom (10);

    auto left = r.removeFromLeft (r.getWidth() * 11 / 20);
    r.removeFromLeft (16);
    listHeader = left.removeFromTop (26);
    left.removeFromTop (6);
    auto lb = left.removeFromBottom (30);
    const int bw = (lb.getWidth() - 18) / 4;
    for (auto* b : { &scanButton, &rescanButton, &folderButton })
    {
        b->setBounds (lb.removeFromLeft (bw));
        lb.removeFromLeft (6);
    }
    clearButton.setBounds (lb);
    left.removeFromBottom (6);
    list.setBounds (left);

    r.removeFromTop (32);
    addTrackButton.setBounds (r.removeFromBottom (30));
    r.removeFromBottom (8);
    progressArea = r.removeFromBottom (24);
    r.removeFromBottom (6);
    detailArea = r;

    tracksHeader = tracks.removeFromTop (26);
    tracks.removeFromTop (6);
    tracksArea = tracks;
    for (auto& tr : trackRows)
    {
        auto line = tracks.removeFromTop (26);
        tracks.removeFromTop (6);
        line.removeFromLeft (200);
        tr.plugin->setBounds (line.removeFromLeft (340));
        line.removeFromLeft (8);
        tr.edit->setBounds (line.removeFromLeft (70));
    }
}

//==============================================================================
namespace
{
class PluginWindow : public juce::DocumentWindow
{
public:
    explicit PluginWindow (AppState& s)
        : DocumentWindow ("Plugins - B4ND LIVE MTR", theme::panel, DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new PluginPanel (s), true);
        setResizable (true, false);
        setResizeLimits (860, 560, 1800, 1200);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }
    void closeButtonPressed() override { setVisible (false); }
};

std::unique_ptr<PluginWindow> pluginWindowOwner;
} // namespace

void showPluginWindow (AppState& state)
{
    if (pluginWindowOwner == nullptr)
        pluginWindowOwner = std::make_unique<PluginWindow> (state);
    pluginWindowOwner->setVisible (true);
    pluginWindowOwner->toFront (true);
}

void closePluginWindow()
{
    pluginWindowOwner.reset();
}

juce::Component* getPluginWindowContent()
{
    return pluginWindowOwner != nullptr ? pluginWindowOwner->getContentComponent() : nullptr;
}

} // namespace b4nd
