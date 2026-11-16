#include "ui/SidePanel.h"
#include "core/TimeFormat.h"
#include "ui/Theme.h"

namespace b4nd
{

SidePanel::SidePanel (AppState& s) : state (s)
{
    songList.setRowHeight (44);
    songList.setColour (juce::ListBox::backgroundColourId, theme::panel);
    songList.setWantsKeyboardFocus (false);
    addAndMakeVisible (songList);

    const char* items[] = { "Import Song (MP3/WAV)", "BPM / Beat / Key", "Chord Analysis", "Song Structure",
                            "Stem Separation", "Audio to MIDI" };
    for (auto* name : items)
    {
        auto* b = aiButtons.add (new juce::TextButton (name));
        b->setWantsKeyboardFocus (false);
        b->setEnabled (false);   // AI engine arrives in Phase 8-10
        b->setTooltip ("Available from Phase 8 (AI analyzer). Disabled in Live Mode.");
        addAndMakeVisible (b);
    }

    state.addChangeListener (this);
    songList.updateContent();
    songList.selectRow (state.getCurrentSongIndex(), true, true);
}

SidePanel::~SidePanel()
{
    state.removeChangeListener (this);
}

void SidePanel::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (songList.getSelectedRow() != state.getCurrentSongIndex())
        songList.selectRow (state.getCurrentSongIndex(), true, true);

    if (lastLive != state.isLiveMode())
    {
        lastLive = state.isLiveMode();
        repaint();
    }
}

int SidePanel::getNumRows()
{
    return (int) state.getSongs().size();
}

void SidePanel::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (! juce::isPositiveAndBelow (row, getNumRows()))
        return;

    const auto& song = state.getSongs()[(size_t) row];
    auto r = juce::Rectangle<int> (0, 0, width, height).reduced (6, 2);

    if (selected)
    {
        g.setColour (theme::accent.withAlpha (0.18f));
        g.fillRoundedRectangle (r.toFloat(), 4.0f);
        g.setColour (theme::accent);
        g.fillRect (r.getX(), r.getY() + 4, 3, r.getHeight() - 8);
    }

    auto text = r.reduced (10, 4);
    g.setColour (theme::textFaint);
    g.setFont (theme::mono (12.0f, true));
    g.drawText (juce::String (row + 1).paddedLeft ('0', 2), text.removeFromLeft (24), juce::Justification::centredLeft);

    auto top = text.removeFromTop (text.getHeight() / 2);
    g.setColour (selected ? theme::text : theme::text.withAlpha (0.85f));
    g.setFont (theme::font (13.0f, true));
    g.drawFittedText (song.title, top, juce::Justification::bottomLeft, 1);

    g.setColour (theme::textDim);
    g.setFont (theme::font (11.0f));
    g.drawText (juce::String (song.tempo.bpmAtBeat (0.0), 0) + " BPM  " + song.key + "  "
                    + formatSeconds (song.lengthSeconds, false),
                text, juce::Justification::topLeft);
}

void SidePanel::listBoxItemClicked (int row, const juce::MouseEvent&)
{
    state.selectSong (row);
}

void SidePanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    g.setColour (theme::outline);
    g.drawVerticalLine (getWidth() - 1, 0.0f, (float) getHeight());

    auto header = [&g] (juce::Rectangle<int> r, const juce::String& t)
    {
        g.setColour (theme::textDim);
        g.setFont (theme::font (11.0f, true));
        g.drawText (t, r.reduced (12, 0), juce::Justification::centredLeft);
        g.setColour (theme::outline);
        g.drawHorizontalLine (r.getBottom() - 1, 8.0f, (float) r.getRight() - 8.0f);
    };

    header (songHeader, "PROJECT  /  SETLIST");
    header (aiHeader, state.isLiveMode() ? "AI ANALYSIS  (locked in Live Mode)" : "AI ANALYSIS  (Phase 8+)");

    g.setColour (theme::textDim);
    g.setFont (theme::font (11.0f));
    g.drawText ("Setlist total  " + formatSeconds (state.getSetlistTotalSeconds(), false),
                totalArea.reduced (12, 0), juce::Justification::centredLeft);
}

void SidePanel::resized()
{
    auto r = getLocalBounds();
    songHeader = r.removeFromTop (30);

    const int n = aiButtons.size();
    const int buttonH = juce::jlimit (22, 30, (getHeight() * 2 / 5 - 40) / juce::jmax (1, n) - 4);
    auto ai = r.removeFromBottom (30 + n * (buttonH + 4) + 10);
    totalArea = r.removeFromBottom (26);
    songList.setBounds (r.reduced (0, 4));

    aiHeader = ai.removeFromTop (30);
    ai.reduce (10, 4);
    for (auto* b : aiButtons)
    {
        b->setBounds (ai.removeFromTop (buttonH));
        ai.removeFromTop (4);
    }
}

} // namespace b4nd
