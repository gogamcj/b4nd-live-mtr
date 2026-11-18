#include "ui/ModulePanel.h"
#include "ui/Theme.h"

namespace b4nd
{

namespace
{
juce::String u (const std::string& s) { return juce::String::fromUTF8 (s.c_str()); }

/** "" = do not send (-1), else 0-127. */
juce::String optionalText (int v) { return v < 0 ? juce::String() : juce::String (v); }
int optionalValue (const juce::TextEditor& e)
{
    const auto t = e.getText().trim();
    return t.isEmpty() ? -1 : juce::jlimit (0, 127, t.getIntValue());
}

juce::Colour statusColour (MidiOutputService::PortStatus s)
{
    switch (s)
    {
        case MidiOutputService::PortStatus::Ready:        return theme::ok;
        case MidiOutputService::PortStatus::Preparing:    return theme::warn;
        case MidiOutputService::PortStatus::NoPort:       return theme::textFaint;
        case MidiOutputService::PortStatus::Disconnected:
        case MidiOutputService::PortStatus::OpenFailed:   return theme::danger;
    }
    return theme::textFaint;
}

const char* const sendOrders[] = { "msb,lsb,pc", "lsb,msb,pc", "pc,msb,lsb", "msb,pc", "pc" };
const char* const sendOrderNames[] = { "Bank MSB > LSB > Program", "Bank LSB > MSB > Program", "Program > MSB > LSB",
                                       "Bank MSB > Program (no LSB)", "Program only" };
} // namespace

ModulePanel::ModulePanel (AppState& s) : state (s)
{
    list.setRowHeight (40);
    list.setColour (juce::ListBox::backgroundColourId, theme::panelInset);
    addAndMakeVisible (list);

    for (auto* e : { &nameEdit, &msbEdit, &lsbEdit, &programEdit, &volumeEdit, &panEdit, &delayEdit, &gapEdit, &settleEdit, &clockOffsetEdit })
    {
        addAndMakeVisible (*e);
        e->setFont (theme::font (13.0f));
        e->onReturnKey = [this] { applyEditor(); };
    }
    for (auto* e : { &msbEdit, &lsbEdit, &programEdit, &volumeEdit, &panEdit, &gapEdit, &settleEdit })
        e->setInputRestrictions (4, "0123456789");
    delayEdit.setInputRestrictions (6, "0123456789.");
    clockOffsetEdit.setInputRestrictions (6, "-0123456789.");
    clockToggle.setTooltip ("MIDI Clock (24 per beat) with Start / Stop / Continue / Song Position, following the song's tempo map. "
                            "For arpeggiators, drum machines and delays that sync to the song.");
    clockOffsetEdit.setTooltip ("Clock sent this many ms earlier (positive) or later (negative) than the music");
    msbEdit.setTextToShowWhenEmpty ("-", theme::textFaint);
    lsbEdit.setTextToShowWhenEmpty ("-", theme::textFaint);
    programEdit.setTextToShowWhenEmpty ("-", theme::textFaint);
    volumeEdit.setTextToShowWhenEmpty ("-", theme::textFaint);
    panEdit.setTextToShowWhenEmpty ("-", theme::textFaint);
    programEdit.setTooltip ("Program number 0-127 as sent on the wire (many manuals count from 1: subtract 1). Empty: not sent.");
    msbEdit.setTooltip ("Bank Select MSB (CC0). Empty: not sent.");
    lsbEdit.setTooltip ("Bank Select LSB (CC32). Empty: not sent.");
    delayEdit.setTooltip ("Module latency in ms: notes are sent this much earlier so the sound lines up with the MR");
    settleEdit.setTooltip ("Time the module needs after a program change before it plays reliably");

    for (int ch = 1; ch <= 16; ++ch)
        channelBox.addItem ("Channel " + juce::String (ch), ch);
    for (int i = 0; i < (int) std::size (sendOrderNames); ++i)
        orderBox.addItem (sendOrderNames[i], i + 1);
    for (auto* c : { &portBox, &channelBox, &orderBox })
        addAndMakeVisible (*c);
    addAndMakeVisible (sustainToggle);
    addAndMakeVisible (clockToggle);

    for (auto* b : { &addButton, &removeButton, &applyButton, &testButton, &recallButton, &refreshButton })
    {
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (*b);
    }
    applyButton.setColour (juce::TextButton::buttonColourId, theme::accent.darker (0.4f));
    testButton.setTooltip ("Plays middle C on the module's channel (works in Live Mode too)");
    recallButton.setTooltip ("Sends bank / program / volume / pan again (works in Live Mode too)");

    addButton.onClick = [this]
    {
        ModuleProfile p;
        p.deviceName = "Module " + std::to_string (state.getModules().size() + 1);
        if (! ports.empty())
        {
            p.portName = ports.front().name.toStdString();
            p.portIdentifier = ports.front().identifier.toStdString();
        }
        const auto err = state.addModule (p);
        message = err.isEmpty() ? juce::String ("Module added. Set its port, channel and preset, then APPLY.") : err;
        messageIsError = err.isNotEmpty();
        list.updateContent();
        selectModule ((int) state.getModules().size() - 1);
    };
    removeButton.onClick = [this]
    {
        if (editing < 0)
            return;
        const auto err = state.removeModule (editing);
        message = err.isEmpty() ? juce::String ("Module removed. Its tracks are now unassigned.") : err;
        messageIsError = err.isNotEmpty();
        list.updateContent();
        selectModule (juce::jmin (editing, (int) state.getModules().size() - 1));
    };
    applyButton.onClick = [this] { applyEditor(); };
    testButton.onClick = [this] { if (editing >= 0) state.sendModuleTestNote (editing); };
    recallButton.onClick = [this] { if (editing >= 0) state.recallModulePreset (editing); };
    refreshButton.onClick = [this] { refreshPorts(); state.getMidiOutput().pollPorts(); };

    state.addChangeListener (this);
    refreshPorts();
    list.updateContent();
    selectModule (state.getModules().empty() ? -1 : 0);
    rebuildTrackRows();
    startTimerHz (4);
    setSize (900, 640);
}

ModulePanel::~ModulePanel()
{
    state.removeChangeListener (this);
}

juce::String ModulePanel::lockReason() const
{
    if (state.isLiveMode())
        return "LIVE MODE: modules are locked. TEST NOTE and SEND PRESET still work.";
    if (state.getTransportState() == TransportState::Playing)
        return "Stop playback to change modules.";
    return {};
}

void ModulePanel::refreshEnablement()
{
    const bool canEdit = lockReason().isEmpty();
    const bool has = editing >= 0;
    addButton.setEnabled (canEdit && (int) state.getModules().size() < MidiOutputService::maxSlots);
    removeButton.setEnabled (canEdit && has);
    applyButton.setEnabled (canEdit && has);
    for (auto* c : std::initializer_list<juce::Component*> { &nameEdit, &msbEdit, &lsbEdit, &programEdit, &volumeEdit, &panEdit,
                                                             &delayEdit, &gapEdit, &settleEdit, &portBox, &channelBox, &orderBox,
                                                             &sustainToggle, &clockToggle, &clockOffsetEdit })
        c->setEnabled (canEdit && has);
    testButton.setEnabled (has);
    recallButton.setEnabled (has);
    for (auto& r : trackRows)
    {
        r.module->setEnabled (canEdit);
        r.channel->setEnabled (canEdit);
    }
}

void ModulePanel::refreshPorts()
{
    ports = state.listMidiOutputs();
    loadEditor();
}

void ModulePanel::selectModule (int index)
{
    if (juce::isPositiveAndBelow (index, (int) state.getModules().size()))
        list.selectRow (index);
    else
    {
        list.deselectAllRows();
        editing = -1;
        loadEditor();
    }
}

void ModulePanel::selectedRowsChanged (int row)
{
    editing = juce::isPositiveAndBelow (row, (int) state.getModules().size()) ? row : -1;
    loadEditor();
}

void ModulePanel::loadEditor()
{
    const juce::ScopedValueSetter<bool> svs (updating, true);
    portBox.clear (juce::dontSendNotification);
    if (editing < 0)
    {
        for (auto* e : { &nameEdit, &msbEdit, &lsbEdit, &programEdit, &volumeEdit, &panEdit, &delayEdit, &gapEdit, &settleEdit, &clockOffsetEdit })
            e->clear();
        refreshEnablement();
        repaint();
        return;
    }
    const auto& p = state.getModules()[(size_t) editing];
    nameEdit.setText (u (p.deviceName), false);
    msbEdit.setText (optionalText (p.bankMsb), false);
    lsbEdit.setText (optionalText (p.bankLsb), false);
    programEdit.setText (optionalText (p.program), false);
    volumeEdit.setText (optionalText (p.volume), false);
    panEdit.setText (optionalText (p.pan), false);
    delayEdit.setText (juce::String (p.delayOffsetMs, 1), false);
    gapEdit.setText (juce::String (p.gapMs), false);
    settleEdit.setText (juce::String (p.settleMs), false);
    channelBox.setSelectedId (juce::jlimit (1, 16, p.channel), juce::dontSendNotification);
    int order = 1;
    for (int i = 0; i < (int) std::size (sendOrders); ++i)
        if (p.sendOrder == sendOrders[i])
            order = i + 1;
    orderBox.setSelectedId (order, juce::dontSendNotification);
    sustainToggle.setToggleState (p.sustainEnabled, juce::dontSendNotification);
    clockToggle.setToggleState (p.clockOut, juce::dontSendNotification);
    clockOffsetEdit.setText (juce::String (p.clockOffsetMs, 1), false);

    portBox.addItem ("(no port)", 1);
    int selected = 1;
    bool present = false;
    for (int i = 0; i < (int) ports.size(); ++i)
    {
        portBox.addItem (ports[(size_t) i].name, i + 2);
        if ((! p.portIdentifier.empty() && ports[(size_t) i].identifier == u (p.portIdentifier))
            || (! present && ports[(size_t) i].name == u (p.portName)))
        {
            selected = i + 2;
            present = true;
        }
    }
    if (! present && ! p.portName.empty())
    {
        // Keep the saved port visible even when it is unplugged right now.
        portBox.addItem (u (p.portName) + "  (not connected)", 1000);
        selected = 1000;
    }
    portBox.setSelectedId (selected, juce::dontSendNotification);
    refreshEnablement();
    repaint();
}

void ModulePanel::applyEditor()
{
    if (editing < 0 || lockReason().isNotEmpty())
        return;
    auto p = state.getModules()[(size_t) editing];
    p.deviceName = nameEdit.getText().trim().toStdString();
    p.bankMsb = optionalValue (msbEdit);
    p.bankLsb = optionalValue (lsbEdit);
    p.program = optionalValue (programEdit);
    p.volume = optionalValue (volumeEdit);
    p.pan = optionalValue (panEdit);
    p.delayOffsetMs = delayEdit.getText().getDoubleValue();
    p.gapMs = gapEdit.getText().getIntValue();
    p.settleMs = settleEdit.getText().getIntValue();
    p.channel = channelBox.getSelectedId() > 0 ? channelBox.getSelectedId() : 1;
    p.sendOrder = sendOrders[juce::jlimit (0, (int) std::size (sendOrders) - 1, orderBox.getSelectedId() - 1)];
    p.sustainEnabled = sustainToggle.getToggleState();
    p.clockOut = clockToggle.getToggleState();
    p.clockOffsetMs = clockOffsetEdit.getText().getDoubleValue();
    const int portId = portBox.getSelectedId();
    if (portId == 1)
    {
        p.portName.clear();
        p.portIdentifier.clear();
    }
    else if (portId >= 2 && portId - 2 < (int) ports.size())
    {
        p.portName = ports[(size_t) (portId - 2)].name.toStdString();
        p.portIdentifier = ports[(size_t) (portId - 2)].identifier.toStdString();
    }
    const auto err = state.updateModule (editing, p);
    message = err.isEmpty() ? juce::String ("Applied. The preset is sent to the module now.") : err;
    messageIsError = err.isNotEmpty();
    list.repaint();
    loadEditor();
}

void ModulePanel::rebuildTrackRows()
{
    const auto* song = state.getCurrentSong();
    juce::String key = song != nullptr ? song->id : juce::String();
    for (const auto& m : state.getModules())
        key << "|" << u (m.id) << u (m.deviceName);
    if (song != nullptr)
        for (const auto& t : song->tracks)
            key << "|" << t.id << t.moduleId << t.midiChannel;
    if (key == trackRowsFor)
        return;
    trackRowsFor = key;

    trackRows.clear();
    if (song == nullptr)
        return;
    for (int i = 0; i < (int) song->tracks.size(); ++i)
    {
        const auto& t = song->tracks[(size_t) i];
        if (t.kind != TrackKind::MidiExternal)
            continue;
        TrackRow r;
        r.trackIndex = i;
        r.name = t.name;
        r.module = std::make_unique<juce::ComboBox>();
        r.channel = std::make_unique<juce::ComboBox>();
        r.module->addItem ("(no module)", 1);
        for (int m = 0; m < (int) state.getModules().size(); ++m)
            r.module->addItem (u (state.getModules()[(size_t) m].deviceName), m + 2);
        const int mi = state.findModule (t.moduleId);
        r.module->setSelectedId (mi >= 0 ? mi + 2 : 1, juce::dontSendNotification);
        r.channel->addItem ("Module's channel", 100);
        for (int ch = 1; ch <= 16; ++ch)
            r.channel->addItem ("Channel " + juce::String (ch), ch);
        r.channel->setSelectedId (t.midiChannel > 0 ? t.midiChannel : 100, juce::dontSendNotification);

        const int trackIndex = i;
        auto* mod = r.module.get();
        auto* chan = r.channel.get();
        auto apply = [this, trackIndex, mod, chan]
        {
            const int m = mod->getSelectedId() - 2;
            const auto id = juce::isPositiveAndBelow (m, (int) state.getModules().size()) ? u (state.getModules()[(size_t) m].id) : juce::String();
            const int ch = chan->getSelectedId() == 100 ? 0 : chan->getSelectedId();
            const auto err = state.setTrackModule (trackIndex, id, ch);
            if (err.isNotEmpty())
            {
                message = err;
                messageIsError = true;
                trackRowsFor.clear();   // restore the real assignment
                juce::Component::SafePointer<ModulePanel> safe (this);
                juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->rebuildTrackRows(); });
            }
            repaint();
        };
        r.module->onChange = apply;
        r.channel->onChange = apply;
        addAndMakeVisible (*r.module);
        addAndMakeVisible (*r.channel);
        trackRows.push_back (std::move (r));
    }
    resized();
    refreshEnablement();
}

void ModulePanel::changeListenerCallback (juce::ChangeBroadcaster*)
{
    list.updateContent();
    list.repaint();
    rebuildTrackRows();
    refreshEnablement();
    repaint (statusArea);
}

void ModulePanel::timerCallback()
{
    list.repaint();
    repaint (statusArea);
    repaint (lockArea);
    refreshEnablement();
}

int ModulePanel::getNumRows()
{
    return (int) state.getModules().size();
}

void ModulePanel::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (! juce::isPositiveAndBelow (row, (int) state.getModules().size()))
        return;
    const auto& p = state.getModules()[(size_t) row];
    const auto st = state.getModuleStatus (row);
    if (selected)
        g.fillAll (theme::accent.withAlpha (0.18f));
    g.setColour (theme::outline);
    g.drawHorizontalLine (height - 1, 0.0f, (float) width);

    auto r = juce::Rectangle<int> (0, 0, width, height).reduced (10, 4);
    g.setColour (statusColour (st));
    g.fillEllipse (juce::Rectangle<float> (9.0f, 9.0f).withCentre ({ (float) r.getX() + 5.0f, (float) r.getCentreY() }));
    r.removeFromLeft (18);
    auto pill = r.removeFromRight (100);
    g.setFont (theme::font (10.0f, true));
    g.setColour (statusColour (st));
    g.drawText (MidiOutputService::statusText (st), pill, juce::Justification::centredRight);

    g.setColour (theme::text);
    g.setFont (theme::font (13.0f, true));
    g.drawFittedText (u (p.deviceName), r.removeFromTop (r.getHeight() / 2), juce::Justification::centredLeft, 1);
    g.setColour (theme::textDim);
    g.setFont (theme::font (11.0f));
    juce::String sub = (p.portName.empty() ? juce::String ("no port") : u (p.portName)) + "   ch " + juce::String (p.channel);
    if (p.program >= 0)
        sub << "   PC " << p.program;
    if (p.delayOffsetMs > 0.0)
        sub << "   -" << juce::String (p.delayOffsetMs, 1) << " ms";
    if (p.clockOut)
        sub << "   CLOCK";
    g.drawFittedText (sub, r, juce::Justification::centredLeft, 1);
}

void ModulePanel::paint (juce::Graphics& g)
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
    header (listHeader, "SOUND MODULES");
    header (editHeader, editing >= 0 ? "PROFILE" : "PROFILE  (add or select a module)");
    header (tracksHeader, "EXTERNAL MIDI TRACKS IN THIS SONG");

    auto label = [&g] (const juce::Component& c, const juce::String& t)
    {
        g.setColour (theme::textDim);
        g.setFont (theme::font (11.5f, true));
        g.drawText (t, c.getX() - 92, c.getY(), 88, c.getHeight(), juce::Justification::centredRight);
    };
    label (nameEdit, "Name");
    label (portBox, "MIDI port");
    label (channelBox, "Channel");
    label (msbEdit, "Bank MSB");
    label (lsbEdit, "Bank LSB");
    label (programEdit, "Program");
    label (volumeEdit, "Volume CC7");
    label (panEdit, "Pan CC10");
    label (delayEdit, "Delay ms");
    label (orderBox, "Send order");
    label (gapEdit, "Gap ms");
    label (settleEdit, "Settle ms");
    label (clockOffsetEdit, "Clock ms");

    // Status line
    auto st = statusArea;
    g.setColour (theme::panelInset);
    g.fillRoundedRectangle (st.toFloat(), 4.0f);
    st.reduce (10, 4);
    if (editing >= 0)
    {
        const auto s = state.getModuleStatus (editing);
        g.setColour (statusColour (s));
        g.setFont (theme::font (13.0f, true));
        g.drawText ("STATUS: " + MidiOutputService::statusText (s), st.removeFromTop (20), juce::Justification::centredLeft);
        g.setFont (theme::font (11.0f));
        g.setColour (theme::textDim);
        juce::String help;
        switch (s)
        {
            case MidiOutputService::PortStatus::NoPort:       help = "Choose the MIDI port the module is connected to."; break;
            case MidiOutputService::PortStatus::Disconnected: help = "The port is not present. Check the USB/MIDI cable; it reconnects by itself and the preset is sent again."; break;
            case MidiOutputService::PortStatus::OpenFailed:   help = "The port exists but cannot be opened. Close other programs that use it (Windows allows one program per port). Retried every second."; break;
            case MidiOutputService::PortStatus::Preparing:    help = "Preset sent, waiting for the settle time."; break;
            case MidiOutputService::PortStatus::Ready:        help = "Ready. Use TEST NOTE to hear it."; break;
        }
        g.drawFittedText (help, st.removeFromTop (18), juce::Justification::centredLeft, 1);
    }
    if (message.isNotEmpty())
    {
        g.setColour (messageIsError ? theme::danger : theme::textDim);
        g.setFont (theme::font (11.0f, true));
        g.drawFittedText (message, st, juce::Justification::centredLeft, 1);
    }

    // Track rows
    g.setFont (theme::font (12.0f));
    for (const auto& r : trackRows)
    {
        g.setColour (theme::trackExt);
        g.drawFittedText (r.name, tracksArea.getX(), r.module->getY(), r.module->getX() - tracksArea.getX() - 10, r.module->getHeight(),
                          juce::Justification::centredLeft, 1);
        const auto& t = state.getCurrentSong()->tracks[(size_t) r.trackIndex];
        g.setColour (t.midiStatus == TrackInfo::MidiStatus::Ready ? theme::textDim : theme::warn);
        const auto file = t.midiPath.isEmpty() ? juce::String ("no .mid file (drop one on the lane)")
                        : t.midiStatus == TrackInfo::MidiStatus::Ready ? juce::File (t.midiPath).getFileName() + "  (" + juce::String (t.midiNotes) + " notes)"
                        : t.midiStatus == TrackInfo::MidiStatus::Missing ? "MISSING " + juce::File (t.midiPath).getFileName()
                                                                         : "ERROR " + t.midiError;
        g.drawFittedText (file, r.channel->getRight() + 12, r.channel->getY(), tracksArea.getRight() - r.channel->getRight() - 12,
                          r.channel->getHeight(), juce::Justification::centredLeft, 1);
    }
    if (trackRows.empty())
    {
        g.setColour (theme::textFaint);
        g.drawText ("No external MIDI tracks in this song. Drop a .mid file on the timeline to add one.", tracksArea.withHeight (26),
                    juce::Justification::centredLeft);
    }

    const auto reason = lockReason();
    if (reason.isNotEmpty())
    {
        g.setColour (theme::warn);
        g.setFont (theme::font (12.0f, true));
        g.drawText (reason, lockArea, juce::Justification::centredLeft);
    }
}

void ModulePanel::resized()
{
    auto r = getLocalBounds().reduced (16, 12);
    lockArea = r.removeFromBottom (22);
    auto tracks = r.removeFromBottom (juce::jmax (90, 34 + (int) trackRows.size() * 32));
    r.removeFromBottom (10);

    auto left = r.removeFromLeft (300);
    r.removeFromLeft (16);
    listHeader = left.removeFromTop (26);
    left.removeFromTop (6);
    auto lb = left.removeFromBottom (30);
    addButton.setBounds (lb.removeFromLeft (lb.getWidth() * 3 / 5).reduced (0, 0));
    lb.removeFromLeft (6);
    removeButton.setBounds (lb);
    left.removeFromBottom (6);
    list.setBounds (left);

    editHeader = r.removeFromTop (26);
    r.removeFromTop (8);
    statusArea = r.removeFromBottom (62);
    r.removeFromBottom (8);
    auto buttons = r.removeFromBottom (30);
    r.removeFromBottom (8);
    const int bw = (buttons.getWidth() - 12) / 3;
    applyButton.setBounds (buttons.removeFromLeft (bw));
    buttons.removeFromLeft (6);
    testButton.setBounds (buttons.removeFromLeft (bw));
    buttons.removeFromLeft (6);
    recallButton.setBounds (buttons);

    editArea = r;
    const int colW = r.getWidth() / 2;
    auto colA = r.removeFromLeft (colW).withTrimmedLeft (92);
    auto colB = r.withTrimmedLeft (100);
    auto row = [] (juce::Rectangle<int>& c) { auto x = c.removeFromTop (26); c.removeFromTop (7); return x; };

    nameEdit.setBounds (row (colA));
    {
        auto pr = row (colA);
        refreshButton.setBounds (pr.removeFromRight (64));
        pr.removeFromRight (4);
        portBox.setBounds (pr);
    }
    channelBox.setBounds (row (colA));
    msbEdit.setBounds (row (colA).withWidth (70));
    lsbEdit.setBounds (row (colA).withWidth (70));
    programEdit.setBounds (row (colA).withWidth (70));

    volumeEdit.setBounds (row (colB).withWidth (70));
    panEdit.setBounds (row (colB).withWidth (70));
    delayEdit.setBounds (row (colB).withWidth (70));
    orderBox.setBounds (row (colB));
    gapEdit.setBounds (row (colB).withWidth (70));
    settleEdit.setBounds (row (colB).withWidth (70));
    sustainToggle.setBounds (row (colA).withX (colA.getX() - 4).withWidth (220));
    {
        auto cr = row (colB);
        clockOffsetEdit.setBounds (cr.removeFromLeft (70));
        cr.removeFromLeft (10);
        clockToggle.setBounds (cr);
    }

    tracksHeader = tracks.removeFromTop (26);
    tracks.removeFromTop (6);
    tracksArea = tracks;
    for (auto& tr : trackRows)
    {
        auto line = tracks.removeFromTop (26);
        tracks.removeFromTop (6);
        line.removeFromLeft (220);
        tr.module->setBounds (line.removeFromLeft (240));
        line.removeFromLeft (8);
        tr.channel->setBounds (line.removeFromLeft (160));
    }
}

//==============================================================================
namespace
{
class ModuleWindow : public juce::DocumentWindow
{
public:
    explicit ModuleWindow (AppState& s)
        : DocumentWindow ("Sound Modules - B4ND LIVE MTR", theme::panel, DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new ModulePanel (s), true);
        setResizable (true, false);
        setResizeLimits (820, 560, 1600, 1200);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }
    void closeButtonPressed() override { setVisible (false); }
};

std::unique_ptr<ModuleWindow> moduleWindowOwner;
} // namespace

void showModuleWindow (AppState& state, int selectModule)
{
    if (moduleWindowOwner == nullptr)
        moduleWindowOwner = std::make_unique<ModuleWindow> (state);
    moduleWindowOwner->setVisible (true);
    moduleWindowOwner->toFront (true);
    if (selectModule >= 0)
        if (auto* p = dynamic_cast<ModulePanel*> (moduleWindowOwner->getContentComponent()))
            p->selectModule (selectModule);
}

void closeModuleWindow()
{
    moduleWindowOwner.reset();
}

juce::Component* getModuleWindowContent()
{
    return moduleWindowOwner != nullptr ? moduleWindowOwner->getContentComponent() : nullptr;
}

} // namespace b4nd
