#include "ui/TransportBar.h"
#include "ui/SongTimingPanel.h"
#include "core/TimeFormat.h"
#include "ui/Theme.h"

namespace b4nd
{

TransportBar::TransportBar (AppState& s) : state (s)
{
    for (auto* b : { &restartButton, &playButton, &pauseButton, &stopButton, &loopButton, &clickButton, &emergencyButton, &liveButton })
    {
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (*b);
    }

    restartButton.setTooltip ("Restart song (Home)");
    playButton.setTooltip ("Play / Pause (Space)");
    stopButton.setTooltip ("Stop and return to start");
    emergencyButton.setTooltip ("Emergency stop: silence all outputs and MIDI (Esc)");
    liveButton.setTooltip ("Full-screen live performance mode (F11)");

    playButton.setColour (juce::TextButton::buttonColourId, theme::play.darker (0.55f));
    playButton.setColour (juce::TextButton::buttonOnColourId, theme::play);
    pauseButton.setColour (juce::TextButton::buttonOnColourId, theme::pause.darker (0.2f));
    emergencyButton.setColour (juce::TextButton::buttonColourId, theme::danger.darker (0.35f));
    loopButton.setClickingTogglesState (false);

    restartButton.onClick   = [this] { state.restart(); };
    playButton.onClick      = [this] { state.play(); };
    pauseButton.onClick     = [this] { state.pause(); };
    stopButton.onClick      = [this] { state.stop(); };
    loopButton.onClick      = [this] { state.toggleLoop(); };
    emergencyButton.onClick = [this] { state.emergencyStop(); };
    liveButton.onClick      = [this] { if (onLiveModeClicked) onLiveModeClicked(); };
    clickButton.onClick     = [this] { showClickMenu(); };
    clickButton.setTooltip ("Click of this song: count-in, 8ths / 16ths, accent, sound (locked in Live Mode)");

    state.addChangeListener (this);
    refresh();
}

TransportBar::~TransportBar()
{
    state.removeChangeListener (this);
}

void TransportBar::showClickMenu()
{
    const auto c = state.getClickSettings();
    const bool locked = state.isLiveMode();
    juce::PopupMenu m;
    m.addSectionHeader ("COUNT-IN (from stop)");
    for (int bars : { 0, 1, 2, 4 })
        m.addItem (100 + bars, bars == 0 ? juce::String ("Off") : juce::String (bars) + (bars == 1 ? " bar" : " bars"), ! locked,
                   c.countInBars == bars);
    m.addSectionHeader ("SUBDIVISION");
    m.addItem (201, "Quarter notes", ! locked, c.subdivision == 1);
    m.addItem (202, "8th notes", ! locked, c.subdivision == 2);
    m.addItem (204, "16th notes", ! locked, c.subdivision == 4);
    m.addSeparator();
    m.addItem (300, "Accent the first beat", ! locked, c.accentFirst);
    m.addSectionHeader ("SOUND");
    m.addItem (401, "Beep", ! locked, c.sound != "wood" && c.sound != "stick");
    m.addItem (402, "Wood", ! locked, c.sound == "wood");
    m.addItem (403, "Stick", ! locked, c.sound == "stick");
    if (locked)
    {
        m.addSeparator();
        m.addItem (-1, "Locked in Live Mode", false, false);
    }
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&clickButton), [this] (int id)
    {
        if (id <= 0)
            return;
        auto n = state.getClickSettings();
        if (id >= 100 && id < 200) { n.countInBars = id - 100; n.enabled = true; }
        else if (id >= 200 && id < 300) n.subdivision = id - 200;
        else if (id == 300) n.accentFirst = ! n.accentFirst;
        else if (id == 401) n.sound = "default";
        else if (id == 402) n.sound = "wood";
        else if (id == 403) n.sound = "stick";
        state.setClickSettings (n);
    });
}

void TransportBar::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refresh();
}

void TransportBar::refresh()
{
    const auto ts = state.getTransportState();
    playButton.setToggleState (ts == TransportState::Playing, juce::dontSendNotification);
    pauseButton.setToggleState (ts == TransportState::Paused, juce::dontSendNotification);
    loopButton.setToggleState (state.isLooping(), juce::dontSendNotification);
    repaint (cellsArea);
    repaint (infoArea);
}

void TransportBar::mouseDown (const juce::MouseEvent& e)
{
    if (! state.isLiveMode() && timingCells.contains (e.getPosition()))
        showSongTimingWindow (state);
}

void TransportBar::paintCell (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& caption,
                              const juce::String& value, juce::Colour valueColour, bool monoValue) const
{
    g.setColour (theme::panelInset);
    g.fillRoundedRectangle (r.toFloat(), 4.0f);
    auto inner = r.reduced (8, 4);
    g.setColour (theme::textFaint);
    g.setFont (theme::font (10.0f, true));
    g.drawText (caption, inner.removeFromTop (13), juce::Justification::topLeft);
    g.setColour (valueColour);
    g.setFont (monoValue ? theme::mono (20.0f, true) : theme::font (19.0f, true));
    g.drawFittedText (value, inner, juce::Justification::centredLeft, 1);
}

void TransportBar::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    g.setColour (theme::outline);
    g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());

    // Project / song
    auto info = infoArea.reduced (12, 8);
    g.setColour (theme::accent);
    g.setFont (theme::font (11.0f, true));
    g.drawText ("B4ND LIVE MTR  |  " + state.getProjectName().toUpperCase() + (state.isDirty() ? " *" : ""), info.removeFromTop (16),
                juce::Justification::centredLeft);
    g.setColour (theme::text);
    g.setFont (theme::font (18.0f, true));
    const auto* song = state.getCurrentSong();
    g.drawFittedText (song ? song->title : juce::String ("(no song)"), info.removeFromTop (24),
                      juce::Justification::centredLeft, 1);
    g.setColour (theme::textDim);
    g.setFont (theme::font (12.0f));
    if (song)
        g.drawText (song->artist + "   |   Song " + juce::String (state.getCurrentSongIndex() + 1) + " / "
                        + juce::String ((int) state.getSongs().size()),
                    info.removeFromTop (16), juce::Justification::centredLeft);

    // Info cells
    if (song == nullptr)
        return;

    const double pos = state.getPositionSeconds();
    const double beat = song->tempo.beatAtSeconds (pos);
    const auto bb = song->tempo.barBeatAtBeat (beat);

    auto cells = cellsArea.reduced (0, 10);
    const int gap = 6;
    // Preferred widths; scaled down on narrow windows so every cell stays visible.
    const int preferred[] = { 92, 104, 66, 128, 128, 104 };
    int total = gap * 5;
    for (int w : preferred) total += w;
    const float scale = juce::jmin (1.0f, (float) cells.getWidth() / (float) total);
    int idx = 0;
    auto take = [&] { auto c = cells.removeFromLeft ((int) (preferred[idx++] * scale)); cells.removeFromLeft (gap); return c; };

    const auto bpmCell = take();
    paintCell (g, bpmCell, "BPM", juce::String (song->tempo.bpmAtBeat (beat), 1), theme::text, true);
    paintCell (g, take(), "KEY", song->key, theme::text, false);
    const auto sigCell = take();
    paintCell (g, sigCell, "TIME SIG", juce::String (bb.numerator) + "/" + juce::String (bb.denominator), theme::text, true);
    const_cast<TransportBar*> (this)->timingCells = bpmCell.getUnion (sigCell);
    if (state.isCountingIn())
        paintCell (g, take(), "BAR . BEAT", "COUNT-IN", theme::warn, false);
    else
        paintCell (g, take(), "BAR . BEAT", formatBarBeat (bb), theme::accent, true);
    paintCell (g, take(), "TIME", formatSeconds (pos), theme::text, true);
    paintCell (g, take(), "REMAIN", "-" + formatSeconds (state.getRemainingSeconds(), false), theme::textDim, true);

    if (state.isEmergencyStopped())
    {
        g.setColour (theme::danger);
        g.setFont (theme::font (12.0f, true));
        g.drawText ("EMERGENCY STOPPED", cells.reduced (4, 0), juce::Justification::centredLeft);
    }
}

void TransportBar::resized()
{
    auto r = getLocalBounds();
    infoArea = r.removeFromLeft (juce::jlimit (230, 330, r.getWidth() / 5));

    auto right = r.removeFromRight (juce::jlimit (560, 650, r.getWidth() / 3)).reduced (10, 14);
    liveButton.setBounds (right.removeFromRight (100));
    right.removeFromRight (8);
    emergencyButton.setBounds (right.removeFromRight (140));
    right.removeFromRight (14);
    const int w = juce::jmax (40, (right.getWidth() - 5 * 6) / 6);
    for (auto* b : { &restartButton, &playButton, &pauseButton, &stopButton, &loopButton, &clickButton })
    {
        b->setBounds (right.removeFromLeft (w));
        right.removeFromLeft (6);
    }

    cellsArea = r.reduced (8, 0);
}

} // namespace b4nd
