#include "ui/LiveModeView.h"
#include "core/TimeFormat.h"
#include "ui/Theme.h"

namespace b4nd
{

LiveModeView::LiveModeView (AppState& s)
    : state (s),
      mainMeterL ([this] { return state.getOutputLevel (0); }),
      mainMeterR ([this] { return state.getOutputLevel (1); }),
      clickMeter ([this] { return state.getOutputLevel (2); })
{
    for (auto* b : { &prevButton, &restartButton, &playButton, &stopButton, &nextButton, &loopButton, &emergencyButton, &exitButton })
    {
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (*b);
    }

    playButton.setColour (juce::TextButton::buttonColourId, theme::play.darker (0.5f));
    playButton.setColour (juce::TextButton::buttonOnColourId, theme::play.darker (0.1f));
    emergencyButton.setColour (juce::TextButton::buttonColourId, theme::danger.darker (0.2f));
    loopButton.setColour (juce::TextButton::buttonOnColourId, theme::accent);
    exitButton.setColour (juce::TextButton::buttonColourId, theme::panel);

    prevButton.onClick      = [this] { state.previousSong(); };
    restartButton.onClick   = [this] { state.restart(); };
    playButton.onClick      = [this] { state.togglePlayPause(); };
    stopButton.onClick      = [this] { state.stop(); };
    nextButton.onClick      = [this] { state.nextSong(); };
    loopButton.onClick      = [this] { state.toggleLoop(); };
    emergencyButton.onClick = [this] { state.emergencyStop(); };
    exitButton.onClick      = [this] { if (onExitClicked) onExitClicked(); };

    for (auto* m : { &mainMeterL, &mainMeterR, &clickMeter })
        addAndMakeVisible (*m);

    state.addChangeListener (this);
    rebuildCueButtons();
    changeListenerCallback (nullptr);
    startTimerHz (30);
}

LiveModeView::~LiveModeView()
{
    state.removeChangeListener (this);
}

void LiveModeView::rebuildCueButtons()
{
    cueButtons.clear();
    cuesBuiltForSong = state.getCurrentSongIndex();
    if (const auto* song = state.getCurrentSong())
        for (int i = 0; i < (int) song->cues.size(); ++i)
        {
            auto* b = cueButtons.add (new juce::TextButton (song->cues[(size_t) i].name.toUpperCase()));
            b->setWantsKeyboardFocus (false);
            b->onClick = [this, i] { state.jumpToCue (i); };
            addAndMakeVisible (b);
        }
    resized();
}

void LiveModeView::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (cuesBuiltForSong != state.getCurrentSongIndex())
        rebuildCueButtons();

    const bool playing = state.getTransportState() == TransportState::Playing;
    playButton.setButtonText (playing ? "PAUSE" : "PLAY");
    playButton.setToggleState (playing, juce::dontSendNotification);
    loopButton.setToggleState (state.isLooping(), juce::dontSendNotification);
    prevButton.setEnabled (state.getCurrentSongIndex() > 0);
    nextButton.setEnabled (state.getNextSong() != nullptr);

    // Highlight the current section.
    if (const auto* song = state.getCurrentSong())
    {
        int current = -1;
        for (size_t i = 0; i < song->cues.size(); ++i)
            if (song->cues[i].seconds <= state.getPositionSeconds() + 1e-3)
                current = (int) i;
        for (int i = 0; i < cueButtons.size(); ++i)
            cueButtons[i]->setColour (juce::TextButton::buttonColourId,
                                      i == current ? theme::accent.darker (0.3f) : theme::panelRaised);
    }
    repaint (headerArea);
    repaint (cellsArea);
    repaint (progressArea);
    repaint (statusArea);
}

void LiveModeView::timerCallback()
{
    mainMeterL.update();
    mainMeterR.update();
    clickMeter.update();
}

void LiveModeView::paintBigCell (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& caption,
                                 const juce::String& value, juce::Colour colour) const
{
    g.setColour (theme::panel);
    g.fillRoundedRectangle (r.toFloat(), 8.0f);
    auto inner = r.reduced (14, 8);
    g.setColour (theme::textDim);
    g.setFont (theme::font (juce::jlimit (12.0f, 20.0f, r.getHeight() * 0.16f), true));
    g.drawText (caption, inner.removeFromTop ((int) (r.getHeight() * 0.24f)), juce::Justification::topLeft);
    g.setColour (colour);
    g.setFont (theme::mono (juce::jlimit (24.0f, 72.0f, inner.getHeight() * 0.78f), true));
    g.drawFittedText (value, inner, juce::Justification::centredLeft, 1, 0.6f);
}

void LiveModeView::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff0a0b0d));

    const auto* song = state.getCurrentSong();
    const auto* next = state.getNextSong();

    // Header: current / next song
    auto h = headerArea;
    auto nextArea = h.removeFromRight (h.getWidth() / 3);
    g.setColour (theme::textDim);
    g.setFont (theme::font (18.0f, true));
    g.drawText ("NOW  " + juce::String (state.getCurrentSongIndex() + 1) + " / " + juce::String ((int) state.getSongs().size()),
                h.removeFromTop (28), juce::Justification::centredLeft);
    g.setColour (theme::text);
    g.setFont (theme::font (juce::jlimit (28.0f, 64.0f, h.getHeight() * 0.75f), true));
    g.drawFittedText (song ? song->title : juce::String ("-"), h, juce::Justification::centredLeft, 1, 0.7f);

    nextArea.removeFromTop (28 + 38);   // leave room for the exit button
    g.setColour (theme::textDim);
    g.setFont (theme::font (16.0f, true));
    g.drawText ("NEXT", nextArea.removeFromTop (22), juce::Justification::centredRight);
    g.setColour (next ? theme::text.withAlpha (0.8f) : theme::textFaint);
    g.setFont (theme::font (26.0f, true));
    g.drawFittedText (next ? next->title : juce::String ("(end of setlist)"), nextArea, juce::Justification::topRight, 2);

    if (song == nullptr)
        return;

    const double pos = state.getPositionSeconds();
    const double beat = song->tempo.beatAtSeconds (pos);
    const auto bb = song->tempo.barBeatAtBeat (beat);

    // Big cells
    auto cells = cellsArea;
    const int gap = 10;
    const int cw = (cells.getWidth() - gap * 5) / 6;
    auto take = [&] { auto c = cells.removeFromLeft (cw); cells.removeFromLeft (gap); return c; };
    paintBigCell (g, take(), "BPM", juce::String (song->tempo.bpmAtBeat (beat), 1), theme::text);
    paintBigCell (g, take(), "KEY", song->key.replace (" minor", "m").replace (" major", ""), theme::text);
    paintBigCell (g, take(), "TIME SIG", juce::String (bb.numerator) + "/" + juce::String (bb.denominator), theme::text);
    paintBigCell (g, take(), "BAR", juce::String (bb.bar + 1) + "." + juce::String (bb.beatInBar + 1), theme::accent);
    paintBigCell (g, take(), "POSITION", formatSeconds (pos, false), theme::text);
    paintBigCell (g, cells, "REMAINING", "-" + formatSeconds (state.getRemainingSeconds(), false), theme::warn);

    // Progress with section colours
    auto p = progressArea.toFloat();
    g.setColour (theme::panel);
    g.fillRoundedRectangle (p, 6.0f);
    if (song->lengthSeconds > 0.0)
    {
        for (size_t i = 0; i < song->cues.size(); ++i)
        {
            const double a = song->cues[i].seconds / song->lengthSeconds;
            const double b = (i + 1 < song->cues.size() ? song->cues[i + 1].seconds : song->lengthSeconds) / song->lengthSeconds;
            auto seg = juce::Rectangle<float> (p.getX() + (float) a * p.getWidth(), p.getY(), (float) (b - a) * p.getWidth(), p.getHeight());
            g.setColour (theme::accent.withAlpha (i % 2 == 0 ? 0.10f : 0.05f));
            g.fillRect (seg);
            g.setColour (theme::outline);
            g.drawVerticalLine ((int) seg.getX(), seg.getY(), seg.getBottom());
        }
        const float fx = (float) (pos / song->lengthSeconds);
        g.setColour (theme::accent.withAlpha (0.55f));
        g.fillRoundedRectangle (p.withWidth (p.getWidth() * fx), 6.0f);
        g.setColour (juce::Colours::white);
        g.fillRect (p.getX() + p.getWidth() * fx - 1.5f, p.getY() - 4.0f, 3.0f, p.getHeight() + 8.0f);
    }

    // Status row
    auto st = statusArea;
    auto pill = [&] (const juce::String& text, juce::Colour c)
    {
        auto r = st.removeFromLeft (juce::jmin (300, st.getWidth() / 4)).reduced (4, 4);
        g.setColour (c.withAlpha (0.15f));
        g.fillRoundedRectangle (r.toFloat(), 6.0f);
        g.setColour (c);
        g.setFont (theme::font (16.0f, true));
        g.drawFittedText (text, r.reduced (10, 0), juce::Justification::centredLeft, 1);
    };
    pill ("AUDIO: NOT CONNECTED", theme::textDim);
    pill ("MIDI: NO PORTS", theme::textDim);
    pill ("VSTi: NONE", theme::textDim);
    if (state.isEmergencyStopped())
        pill ("EMERGENCY STOPPED", theme::danger);
    else
        pill (state.getTransportState() == TransportState::Playing ? "PLAYING" : "READY",
              state.getTransportState() == TransportState::Playing ? theme::play : theme::accent);

    // Meter captions
    g.setColour (theme::textDim);
    g.setFont (theme::font (12.0f, true));
    g.drawText ("MAIN", metersArea.getX(), metersArea.getBottom() + 2, 50, 16, juce::Justification::centredLeft);
    g.drawText ("CLICK", clickMeter.getX() - 6, metersArea.getBottom() + 2, 50, 16, juce::Justification::centredLeft);
}

void LiveModeView::resized()
{
    auto r = getLocalBounds().reduced (24, 18);

    exitButton.setBounds (r.getRight() - 170, r.getY(), 170, 34);
    headerArea = r.removeFromTop (juce::jmax (100, r.getHeight() / 6));
    r.removeFromTop (12);

    // Meters sit to the right of the info block (cells + progress + cues), never over the buttons.
    const int cellsH = juce::jmax (90, r.getHeight() / 4);
    const int infoH = cellsH + 16 + 24 + 16 + 54;
    metersArea = r.removeFromRight (90).withHeight (infoH - 20);
    {
        auto m = metersArea;
        mainMeterL.setBounds (m.removeFromLeft (22));
        m.removeFromLeft (6);
        mainMeterR.setBounds (m.removeFromLeft (22));
        m.removeFromLeft (16);
        clickMeter.setBounds (m.removeFromLeft (22));
    }
    r.removeFromRight (16);

    cellsArea = r.removeFromTop (cellsH);
    r.removeFromTop (16);
    progressArea = r.removeFromTop (24);
    r.removeFromTop (16);

    cuesArea = r.removeFromTop (54);
    if (! cueButtons.isEmpty())
    {
        auto c = cuesArea;
        const int w = juce::jmin (180, (c.getWidth() - (cueButtons.size() - 1) * 8) / cueButtons.size());
        for (auto* b : cueButtons)
        {
            b->setBounds (c.removeFromLeft (w));
            c.removeFromLeft (8);
        }
    }
    r.removeFromTop (16);

    statusArea = r.removeFromBottom (44);
    r.removeFromBottom (14);

    // Transport buttons: emergency stop is widest and isolated on the right.
    auto buttons = getLocalBounds().reduced (24, 0).withTop (r.getY()).withBottom (r.getBottom());
    statusArea = statusArea.withX (buttons.getX()).withRight (buttons.getRight());
    emergencyButton.setBounds (buttons.removeFromRight (juce::jmax (220, buttons.getWidth() / 5)));
    buttons.removeFromRight (30);
    const int bw = (buttons.getWidth() - 5 * 12) / 6;
    for (auto* b : { &prevButton, &restartButton, &playButton, &stopButton, &loopButton, &nextButton })
    {
        b->setBounds (buttons.removeFromLeft (bw));
        buttons.removeFromLeft (12);
    }
}

} // namespace b4nd
