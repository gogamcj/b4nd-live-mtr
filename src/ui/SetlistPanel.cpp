#include "ui/SetlistPanel.h"
#include "core/TimeFormat.h"
#include "ui/Theme.h"

namespace b4nd
{

namespace
{
juce::String mb (std::int64_t bytes) { return juce::String ((bytes + 524288) / 1048576) + " MB"; }

struct Columns
{
    juce::Rectangle<int> num, title, artist, bpm, key, length, start, media;
    explicit Columns (juce::Rectangle<int> r)
    {
        num = r.removeFromLeft (40);
        media = r.removeFromRight (170);
        start = r.removeFromRight (80);
        length = r.removeFromRight (70);
        key = r.removeFromRight (80);
        bpm = r.removeFromRight (60);
        title = r.removeFromLeft (r.getWidth() * 3 / 5);
        artist = r;
    }
};
} // namespace

SetlistPanel::SetlistPanel (AppState& s) : state (s)
{
    list.setRowHeight (34);
    list.setColour (juce::ListBox::backgroundColourId, theme::panelInset);
    addAndMakeVisible (list);
    for (auto* e : { &titleEditor, &artistEditor })
    {
        e->setFont (theme::font (14.0f));
        addAndMakeVisible (e);
    }
    titleEditor.setTextToShowWhenEmpty ("Title", theme::textFaint);
    artistEditor.setTextToShowWhenEmpty ("Artist", theme::textFaint);
    auto rename = [this] { report (state.setSongInfo (list.getSelectedRow(), titleEditor.getText(), artistEditor.getText())); };
    titleEditor.onReturnKey = rename;
    artistEditor.onReturnKey = rename;
    applyButton.onClick = rename;

    addButton.onClick = [this]
    {
        const int at = list.getSelectedRow() >= 0 ? list.getSelectedRow() + 1 : -1;
        const auto err = state.addSong ({}, at);
        report (err);
        if (err.isEmpty())
            list.selectRow (at >= 0 ? at : getNumRows() - 1);
    };
    removeButton.onClick = [this]
    {
        const int row = list.getSelectedRow();
        if (! juce::isPositiveAndBelow (row, getNumRows()))
            return;
        const auto title = state.getSongs()[(size_t) row].title;
        juce::Component::SafePointer<SetlistPanel> safe (this);
        juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::WarningIcon, "Delete song",
                                            "Delete \"" + title + "\" from the setlist? Its audio files stay on disk.", "Delete", "Cancel", nullptr,
                                            juce::ModalCallbackFunction::create ([safe, row] (int ok) {
                                                if (ok != 0 && safe != nullptr)
                                                    safe->report (safe->state.removeSong (row));
                                            }));
    };
    upButton.onClick = [this]
    {
        const int row = list.getSelectedRow();
        const auto err = state.moveSong (row, row - 1);
        report (err);
        if (err.isEmpty() && row > 0)
            list.selectRow (row - 1);
    };
    downButton.onClick = [this]
    {
        const int row = list.getSelectedRow();
        const auto err = state.moveSong (row, row + 1);
        report (err);
        if (err.isEmpty() && row + 1 < getNumRows())
            list.selectRow (row + 1);
    };
    for (auto* b : { &addButton, &removeButton, &upButton, &downButton, &applyButton })
    {
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (b);
    }
    setSize (980, 640);
    state.addChangeListener (this);
    list.selectRow (state.getCurrentSongIndex());
    refresh();
    startTimerHz (2);   // media loading progress
}

SetlistPanel::~SetlistPanel()
{
    state.removeChangeListener (this);
}

void SetlistPanel::report (const juce::String& error)
{
    message = error;
    messageIsError = error.isNotEmpty();
    refresh();
}

void SetlistPanel::refresh()
{
    list.updateContent();
    const bool locked = state.isLiveMode() || state.getTransportState() == TransportState::Playing;
    const int row = list.getSelectedRow();
    const bool sel = juce::isPositiveAndBelow (row, getNumRows());
    addButton.setEnabled (! locked);
    removeButton.setEnabled (! locked && sel && getNumRows() > 1);
    upButton.setEnabled (! locked && sel && row > 0);
    downButton.setEnabled (! locked && sel && row + 1 < getNumRows());
    for (juce::Component* c : { (juce::Component*) &titleEditor, (juce::Component*) &artistEditor, (juce::Component*) &applyButton })
        c->setEnabled (! state.isLiveMode() && sel);
    if (locked && message.isEmpty())
        message = state.isLiveMode() ? "Live Mode: the setlist is locked" : "Playing: stop to change the order";
    else if (! locked && (message == "Live Mode: the setlist is locked" || message == "Playing: stop to change the order"))
        message.clear();
    list.repaint();
    repaint();
}

void SetlistPanel::selectedRowsChanged (int row)
{
    if (juce::isPositiveAndBelow (row, getNumRows()))
    {
        const auto& s = state.getSongs()[(size_t) row];
        titleEditor.setText (s.title, false);
        artistEditor.setText (s.artist, false);
    }
    refresh();
}

void SetlistPanel::listBoxItemDoubleClicked (int row, const juce::MouseEvent&)
{
    if (state.getTransportState() != TransportState::Playing)
        state.selectSong (row);   // load it on the main screen
}

int SetlistPanel::getNumRows()
{
    return (int) state.getSongs().size();
}

void SetlistPanel::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (! juce::isPositiveAndBelow (row, getNumRows()))
        return;
    const auto& s = state.getSongs()[(size_t) row];
    const auto starts = state.getSongStartTimes();
    const bool current = row == state.getCurrentSongIndex();
    auto r = juce::Rectangle<int> (0, 0, width, height);
    g.setColour (selected ? theme::accent.withAlpha (0.2f) : (row % 2 ? theme::panelInset : theme::panel.darker (0.2f)));
    g.fillRect (r);
    if (current)
    {
        g.setColour (theme::play);
        g.fillRect (0, 3, 4, height - 6);
    }
    Columns c (r.reduced (8, 0));
    g.setFont (theme::mono (13.0f, true));
    g.setColour (theme::textFaint);
    g.drawText (juce::String (row + 1).paddedLeft ('0', 2), c.num, juce::Justification::centredLeft);
    g.setFont (theme::font (14.5f, true));
    g.setColour (theme::text);
    g.drawFittedText (s.title, c.title.withTrimmedRight (8), juce::Justification::centredLeft, 1);
    g.setFont (theme::font (13.5f));
    g.setColour (theme::textDim);
    g.drawFittedText (s.artist, c.artist.withTrimmedRight (8), juce::Justification::centredLeft, 1);
    g.drawText (juce::String (s.tempo.bpmAtBeat (0.0), 0), c.bpm, juce::Justification::centredLeft);
    g.drawText (s.key, c.key, juce::Justification::centredLeft);
    g.setFont (theme::mono (13.0f));
    g.drawText (formatSeconds (s.lengthSeconds, false), c.length, juce::Justification::centredLeft);
    g.drawText (formatSeconds (starts[(size_t) row], false), c.start, juce::Justification::centredLeft);

    const auto m = state.getSongMedia (row);
    juce::String text;
    juce::Colour col = theme::ok;
    if (m.tracks == 0)          { text = "no audio"; col = theme::textFaint; }
    else if (m.missing + m.errors > 0) { text = juce::String (m.missing + m.errors) + " MISSING"; col = theme::danger; }
    else if (m.loading > 0)     { text = "LOADING " + juce::String (m.ready) + "/" + juce::String (m.tracks); col = theme::warn; }
    else if (m.unloaded > 0)    { text = "not in RAM"; col = theme::textDim; }
    else                        { text = "READY  " + mb (m.bytes); }
    g.setFont (theme::font (12.5f, true));
    g.setColour (col);
    g.drawText (text, c.media, juce::Justification::centredLeft);
}

void SetlistPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    g.setColour (theme::text);
    g.setFont (theme::font (17.0f, true));
    g.drawText ("SETLIST  " + state.getProjectName(), headerArea, juce::Justification::centredLeft);

    Columns c (columnsArea.reduced (8, 0));
    g.setFont (theme::font (11.5f, true));
    g.setColour (theme::textFaint);
    g.drawText ("#", c.num, juce::Justification::centredLeft);
    g.drawText ("TITLE", c.title, juce::Justification::centredLeft);
    g.drawText ("ARTIST", c.artist, juce::Justification::centredLeft);
    g.drawText ("BPM", c.bpm, juce::Justification::centredLeft);
    g.drawText ("KEY", c.key, juce::Justification::centredLeft);
    g.drawText ("LENGTH", c.length, juce::Justification::centredLeft);
    g.drawText ("STARTS", c.start, juce::Justification::centredLeft);
    g.drawText ("AUDIO", c.media, juce::Justification::centredLeft);

    const auto ms = state.getMediaSummary();
    auto sum = summaryArea;
    g.setFont (theme::font (15.0f, true));
    g.setColour (theme::text);
    g.drawText (juce::String ((int) state.getSongs().size()) + " songs, total " + formatSeconds (state.getSetlistTotalSeconds(), false),
                sum.removeFromLeft (sum.getWidth() / 2), juce::Justification::centredLeft);
    g.setFont (theme::font (13.0f));
    g.setColour (ms.windowed ? theme::warn : theme::textDim);
    g.drawText ("Audio " + mb (ms.setlistBytes) + " / RAM budget " + mb (ms.budgetBytes) + ": "
                    + (ms.windowed ? "only previous, current and next song in RAM" : "every song in RAM"),
                sum, juce::Justification::centredRight);

    g.setColour (messageIsError ? theme::danger : theme::textDim);
    g.setFont (theme::font (13.0f));
    g.drawText (message, messageArea, juce::Justification::centredLeft);
}

void SetlistPanel::resized()
{
    auto r = getLocalBounds().reduced (12);
    headerArea = r.removeFromTop (30);
    messageArea = r.removeFromBottom (24);
    summaryArea = r.removeFromBottom (30);
    editArea = r.removeFromBottom (44);
    {
        auto e = editArea.reduced (0, 6);
        addButton.setBounds (e.removeFromLeft (110));
        e.removeFromLeft (6);
        removeButton.setBounds (e.removeFromLeft (80));
        e.removeFromLeft (16);
        upButton.setBounds (e.removeFromLeft (90));
        e.removeFromLeft (6);
        downButton.setBounds (e.removeFromLeft (100));
        e.removeFromLeft (16);
        applyButton.setBounds (e.removeFromRight (80));
        e.removeFromRight (6);
        artistEditor.setBounds (e.removeFromRight (e.getWidth() * 2 / 5));
        e.removeFromRight (6);
        titleEditor.setBounds (e);
    }
    columnsArea = r.removeFromTop (22);
    list.setBounds (r);
}

void SetlistPanel::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (list.getSelectedRow() >= getNumRows())
        list.selectRow (getNumRows() - 1);
    refresh();
}

void SetlistPanel::timerCallback()
{
    if (state.getMediaSummary().loading > 0)
        list.repaint();
}

//==============================================================================
namespace
{
class SetlistWindow : public juce::DocumentWindow
{
public:
    explicit SetlistWindow (AppState& s)
        : DocumentWindow ("Setlist - B4ND LIVE MTR", theme::panel, DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new SetlistPanel (s), true);
        setResizable (true, false);
        setResizeLimits (860, 420, 2000, 1400);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }
    void closeButtonPressed() override { setVisible (false); }
};

std::unique_ptr<SetlistWindow> setlistWindowOwner;
} // namespace

void showSetlistWindow (AppState& state)
{
    if (setlistWindowOwner == nullptr)
        setlistWindowOwner = std::make_unique<SetlistWindow> (state);
    setlistWindowOwner->setVisible (true);
    setlistWindowOwner->toFront (true);
}

SetlistPanel* getSetlistWindowContent()
{
    return setlistWindowOwner != nullptr ? dynamic_cast<SetlistPanel*> (setlistWindowOwner->getContentComponent()) : nullptr;
}

void closeSetlistWindow()
{
    setlistWindowOwner.reset();
}

} // namespace b4nd
