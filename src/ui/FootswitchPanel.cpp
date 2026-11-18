#include "ui/FootswitchPanel.h"
#include "ui/Theme.h"

namespace b4nd
{

namespace
{
juce::String describe (const FootswitchBinding& b)
{
    juce::String s = (b.note ? "Note " : "CC ") + juce::String (b.number);
    s << (b.channel < 0 ? ", any channel" : ", ch " + juce::String (b.channel + 1));
    s << (b.port.empty() ? ", any input" : ", " + juce::String (b.port));
    return s;
}
} // namespace

//==============================================================================
class FootswitchPanel::Row : public juce::Component
{
public:
    Row (FootswitchPanel& p, int i) : panel (p), index (i)
    {
        for (int a = 1; a < (int) LiveAction::count; ++a)
            action.addItem (liveActionLabel ((LiveAction) a), a);
        action.onChange = [this] { binding().action = (LiveAction) action.getSelectedId(); panel.pushBindings(); };
        learnButton.setTooltip ("Then press the pedal (or key / knob) for this action");
        learnButton.onClick = [this] { panel.learn (index); };
        twice.setTooltip ("Needs two presses within 0.5 s (against a stray step). Recommended for EMERGENCY STOP.");
        twice.onClick = [this] { binding().doublePress = twice.getToggleState(); panel.pushBindings(); };
        latch.setTooltip ("For pedals that send 127 on one press and 0 on the next (latching / toggle mode)");
        latch.onClick = [this] { binding().latching = latch.getToggleState(); panel.pushBindings(); };
        removeButton.onClick = [this]
        {
            panel.bindings.erase (panel.bindings.begin() + index);
            panel.pushBindings();
            juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<FootswitchPanel> (&panel)] {
                if (safe != nullptr) safe->rebuildRows();
            });
        };
        for (juce::Component* c : { (juce::Component*) &action, (juce::Component*) &learnButton, (juce::Component*) &twice,
                                    (juce::Component*) &latch, (juce::Component*) &removeButton })
        {
            c->setWantsKeyboardFocus (false);
            addAndMakeVisible (c);
        }
        refresh();
    }

    FootswitchBinding& binding() { return panel.bindings[(size_t) index]; }

    bool valid() const { return index < (int) panel.bindings.size(); }

    void refresh()
    {
        if (! valid())
            return;   // removed; the rows are rebuilt next
        const auto& b = binding();
        action.setSelectedId ((int) b.action, juce::dontSendNotification);
        twice.setToggleState (b.doublePress, juce::dontSendNotification);
        latch.setToggleState (b.latching, juce::dontSendNotification);
        latch.setEnabled (! b.note && ! panel.state.isLiveMode());
        const bool locked = panel.state.isLiveMode();
        for (juce::Component* c : { (juce::Component*) &action, (juce::Component*) &learnButton, (juce::Component*) &twice,
                                    (juce::Component*) &removeButton })
            c->setEnabled (! locked);
        learnButton.setButtonText (panel.learningRow == index ? "PRESS..." : "LEARN");
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        if (! valid())
            return;
        g.setColour (panel.learningRow == index ? theme::accent.withAlpha (0.18f) : theme::panelRaised);
        g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (0.0f, 1.0f), 4.0f);
        g.setColour (theme::text);
        g.setFont (theme::font (13.5f));
        g.drawText (describe (binding()), inputArea, juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (6, 3);
        action.setBounds (r.removeFromLeft (190));
        r.removeFromLeft (12);
        removeButton.setBounds (r.removeFromRight (30));
        r.removeFromRight (8);
        latch.setBounds (r.removeFromRight (70));
        twice.setBounds (r.removeFromRight (56));
        r.removeFromRight (8);
        learnButton.setBounds (r.removeFromRight (80));
        r.removeFromRight (8);
        inputArea = r;
    }

    FootswitchPanel& panel;
    int index;
    juce::ComboBox action;
    juce::TextButton learnButton { "LEARN" }, removeButton { "X" };
    juce::ToggleButton twice { "2x" }, latch { "LATCH" };
    juce::Rectangle<int> inputArea;
};

//==============================================================================
FootswitchPanel::FootswitchPanel (AppState& s) : state (s)
{
    rowView.setViewedComponent (&rowHolder, false);
    rowView.setScrollBarsShown (true, false);
    addAndMakeVisible (rowView);
    rescanButton.onClick = [this] { rebuildPorts(); };
    addButton.onClick = [this]
    {
        if ((int) bindings.size() >= FootswitchMapper::maxBindings)
            return;
        FootswitchBinding b;
        b.action = LiveAction::NextCue;
        bindings.push_back (b);
        pushBindings();
        rebuildRows();
        learn ((int) bindings.size() - 1);
    };
    defaultsButton.setTooltip ("docs/04: CC 64 / Note 60 play-pause, CC 65 / Note 62 next song, CC 66 / Note 64 previous song, "
                               "CC 67 next cue, CC 68 twice emergency stop");
    defaultsButton.onClick = [this]
    {
        bindings = FootswitchMapper::defaultBindings();
        pushBindings();
        rebuildRows();
    };
    for (auto* b : { &rescanButton, &addButton, &defaultsButton })
    {
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (b);
    }

    state.getFootswitch().onLearned = [this] (const FootswitchBinding& in)
    {
        if (! juce::isPositiveAndBelow (learningRow, (int) bindings.size()))
            return;
        auto& b = bindings[(size_t) learningRow];
        b.note = in.note;
        b.number = in.number;
        b.channel = in.channel;
        b.port = in.port;
        if (b.note)
            b.latching = false;
        learningRow = -1;
        pushBindings();
        rebuildRows();
    };

    bindings = state.getFootswitch().getBindings();
    rebuildPorts();
    rebuildRows();
    setSize (860, 620);
    state.addChangeListener (this);
    startTimerHz (10);
}

FootswitchPanel::~FootswitchPanel()
{
    state.getFootswitch().setLearning (false);
    state.getFootswitch().onLearned = nullptr;
    state.removeChangeListener (this);
}

void FootswitchPanel::learn (int row)
{
    if (state.isLiveMode() || ! juce::isPositiveAndBelow (row, (int) bindings.size()))
        return;
    learningRow = learningRow == row ? -1 : row;
    state.getFootswitch().setLearning (learningRow >= 0);
    for (auto* r : rows)
        r->refresh();
    repaint (statusArea);
}

void FootswitchPanel::pushBindings()
{
    state.setFootswitchBindings (bindings);
}

void FootswitchPanel::rebuildPorts()
{
    portToggles.clear();
    portInfos = state.getFootswitch().listInputs();
    // enabled inputs that are not plugged in stay in the list (so they can be switched off)
    for (const auto& p : state.getFootswitch().getPorts())
    {
        bool found = false;
        for (const auto& x : portInfos)
            found = found || x.name == p.port.name;
        if (! found)
            portInfos.push_back (p.port);
    }
    for (int i = 0; i < (int) portInfos.size(); ++i)
    {
        auto* t = portToggles.add (new juce::ToggleButton (portInfos[(size_t) i].name));
        bool on = false;
        for (const auto& p : state.getFootswitch().getPorts())
            on = on || p.port.name == portInfos[(size_t) i].name;
        t->setToggleState (on, juce::dontSendNotification);
        t->setWantsKeyboardFocus (false);
        t->onClick = [this]
        {
            std::vector<MidiPortInfo> wanted;
            for (int k = 0; k < portToggles.size(); ++k)
                if (portToggles[k]->getToggleState())
                    wanted.push_back (portInfos[(size_t) k]);
            state.setFootswitchPorts (wanted);
            repaint();
        };
        t->setEnabled (! state.isLiveMode());
        addAndMakeVisible (t);
    }
    resized();
    repaint();
}

void FootswitchPanel::rebuildRows()
{
    rows.clear();
    for (int i = 0; i < (int) bindings.size(); ++i)
        rowHolder.addAndMakeVisible (rows.add (new Row (*this, i)));
    layoutRows();
}

void FootswitchPanel::layoutRows()
{
    const int w = rowView.getMaximumVisibleWidth();
    rowHolder.setSize (juce::jmax (100, w), (int) rows.size() * 36);
    for (int i = 0; i < rows.size(); ++i)
        rows[i]->setBounds (0, i * 36, rowHolder.getWidth(), 36);
}

void FootswitchPanel::changeListenerCallback (juce::ChangeBroadcaster*)
{
    for (auto* r : rows)
        r->refresh();
    for (auto* t : portToggles)
        t->setEnabled (! state.isLiveMode());
    for (auto* b : { &addButton, &defaultsButton })
        b->setEnabled (! state.isLiveMode());
    if (state.isLiveMode() && learningRow >= 0)
        learn (learningRow);   // stop learning
    repaint();
}

void FootswitchPanel::timerCallback()
{
    if (state.getFootswitch().getLastPress().count != lastSeenCount)
    {
        lastSeenCount = state.getFootswitch().getLastPress().count;
        repaint (statusArea);
    }
}

void FootswitchPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    g.setColour (theme::textDim);
    g.setFont (theme::font (13.0f, true));
    g.drawText ("MIDI INPUTS TO LISTEN TO (only these; a keyboard on another input is never heard)", portsHeader, juce::Justification::centredLeft);
    g.drawText ("PEDAL MAPPING", rowsHeader, juce::Justification::centredLeft);

    // port status next to each toggle
    const auto& open = state.getFootswitch().getPorts();
    for (int i = 0; i < portToggles.size(); ++i)
    {
        const auto r = portToggles[i]->getBounds();
        juce::String text;
        juce::Colour c = theme::textFaint;
        for (const auto& p : open)
            if (p.port.name == portInfos[(size_t) i].name)
            {
                if (p.status == FootswitchInput::PortState::Status::Open) { text = "OPEN"; c = theme::ok; }
                else if (p.status == FootswitchInput::PortState::Status::OpenFailed) { text = "CANNOT OPEN (used by another program?)"; c = theme::danger; }
                else { text = "NOT CONNECTED"; c = theme::danger; }
            }
        g.setColour (c);
        g.setFont (theme::font (12.5f, true));
        g.drawText (text, r.getRight() + 8, r.getY(), 300, r.getHeight(), juce::Justification::centredLeft);
    }
    if (portToggles.isEmpty())
    {
        g.setColour (theme::textFaint);
        g.setFont (theme::font (13.0f));
        g.drawText ("No MIDI inputs found. Plug in the footswitch (USB MIDI) and press RESCAN.", portsArea, juce::Justification::centredLeft);
    }

    auto st = statusArea.reduced (10, 0);
    g.setFont (theme::font (13.0f));
    const auto& last = state.getFootswitch().getLastPress();
    juce::String text;
    juce::Colour c = theme::textDim;
    if (state.isLiveMode())
    {
        text = "Live Mode: the mapping is locked (the pedals work).";
        c = theme::warn;
    }
    else if (learningRow >= 0)
    {
        text = "Press the pedal for " + juce::String (liveActionLabel (bindings[(size_t) learningRow].action)) + " now (LEARN again to cancel)";
        c = theme::accent;
    }
    else if (last.count > 0)
    {
        text = "Last press: " + describe (last.input) + "  ->  "
               + (last.waiting ? juce::String ("first of two presses")
                  : last.action == LiveAction::None ? juce::String ("not mapped") : juce::String (liveActionLabel (last.action)));
        if (state.getLastFootswitchText().contains ("refused"))
            text << "   (" << state.getLastFootswitchText().fromFirstOccurrenceOf ("refused", true, false) << ")";
    }
    else
        text = "Press a pedal to see what arrives. Pedals work in Edit and Live Mode; song changes and RESTART only when stopped.";
    g.setColour (c);
    g.drawText (text, st, juce::Justification::centredLeft);
}

void FootswitchPanel::resized()
{
    auto r = getLocalBounds().reduced (12);
    statusArea = r.removeFromBottom (30);
    auto top = r.removeFromTop (26);
    portsHeader = top;
    rescanButton.setBounds (top.removeFromRight (90).reduced (0, 1));
    const int portRows = juce::jmax (1, portToggles.size());
    portsArea = r.removeFromTop (juce::jmin (6, portRows) * 28);
    for (int i = 0; i < portToggles.size(); ++i)
        portToggles[i]->setBounds (portsArea.getX(), portsArea.getY() + i * 28, 260, 26);
    r.removeFromTop (12);
    auto rh = r.removeFromTop (28);
    rowsHeader = rh;
    defaultsButton.setBounds (rh.removeFromRight (100).reduced (0, 1));
    rh.removeFromRight (8);
    addButton.setBounds (rh.removeFromRight (80).reduced (0, 1));
    r.removeFromTop (4);
    rowView.setBounds (r);
    layoutRows();
}

//==============================================================================
namespace
{
class FootswitchWindow : public juce::DocumentWindow
{
public:
    explicit FootswitchWindow (AppState& s)
        : DocumentWindow ("Footswitch - B4ND LIVE MTR", theme::panel, DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new FootswitchPanel (s), true);
        setResizable (true, false);
        setResizeLimits (760, 460, 1600, 1200);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }
    void closeButtonPressed() override { setVisible (false); }
};

std::unique_ptr<FootswitchWindow> footswitchWindowOwner;
} // namespace

void showFootswitchWindow (AppState& state)
{
    if (footswitchWindowOwner == nullptr)
        footswitchWindowOwner = std::make_unique<FootswitchWindow> (state);
    footswitchWindowOwner->setVisible (true);
    footswitchWindowOwner->toFront (true);
}

FootswitchPanel* getFootswitchWindowContent()
{
    return footswitchWindowOwner != nullptr ? dynamic_cast<FootswitchPanel*> (footswitchWindowOwner->getContentComponent()) : nullptr;
}

void closeFootswitchWindow()
{
    footswitchWindowOwner.reset();
}

} // namespace b4nd
