#include "ui/SongTimingPanel.h"

#include "ui/AnalysisPanel.h"
#include "ui/Theme.h"

#include <cmath>

namespace b4nd
{

namespace
{
const juce::StringArray& keyNames()
{
    static const juce::StringArray k = []
    {
        juce::StringArray a;
        for (const auto* n : { "C", "C#", "Db", "D", "Eb", "E", "F", "F#", "Gb", "G", "Ab", "A", "Bb", "B" })
            for (const auto* m : { " major", " minor" })
                a.add (juce::String (n) + m);
        return a;
    }();
    return k;
}

juce::String confText (double c)
{
    return c < 0.0 ? juce::String ("set by hand") : "AI " + juce::String (juce::roundToInt (c * 100.0)) + "%";
}
} // namespace

SongTimingPanel::SongTimingPanel (AppState& s) : state (s)
{
    for (auto* b : { &setBpm, &halfButton, &doubleButton, &tapButton, &setBar, &beatEarlier, &beatLater, &nudgeEarlier,
                     &nudgeLater, &atPlayhead, &sig34, &sig44, &sig68, &undoButton, &analyseButton })
    {
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (*b);
    }
    for (auto* e : { &bpmEditor, &barEditor })
    {
        e->setFont (theme::mono (16.0f));
        e->setJustification (juce::Justification::centredLeft);
        e->setColour (juce::TextEditor::backgroundColourId, theme::panelInset);
        e->setColour (juce::TextEditor::outlineColourId, theme::outline);
        e->setInputRestrictions (8, "0123456789.");
        addAndMakeVisible (*e);
    }
    for (auto* c : { &numBox, &denBox, &keyBox })
        addAndMakeVisible (*c);
    for (int n = 1; n <= 16; ++n)
        numBox.addItem (juce::String (n), n);
    for (int d : { 2, 4, 8, 16 })
        denBox.addItem (juce::String (d), d);
    for (int i = 0; i < keyNames().size(); ++i)
        keyBox.addItem (keyNames()[i], i + 1);

    bpmEditor.setTooltip ("One constant tempo for the whole song (removes tempo changes). Enter or SET BPM applies it.");
    tapButton.setTooltip ("Tap along (or press T) at least 3 times: the tapped BPM goes into the field. SET BPM applies it.");
    halfButton.setTooltip ("The analysis found double the real tempo: halve the whole tempo map (tempo changes stay in place)");
    doubleButton.setTooltip ("The analysis found half the real tempo: double the whole tempo map");
    barEditor.setTooltip ("Where bar 1 beat 1 is, in seconds from the start of the song");
    atPlayhead.setTooltip ("Bar 1 beat 1 at the playhead: stop on the first downbeat, then press this");
    analyseButton.setTooltip ("Analyse again in the AI Analysis window, e.g. with a beats-per-bar hint (3 for a waltz)");
    numBox.setTooltip ("Beats per bar");
    denBox.setTooltip ("Note value of one beat (4: quarter, 8: eighth)");

    setBpm.onClick = [this]
    {
        const double b = bpmEditor.getText().getDoubleValue();
        AppState::SongTimingEdit e;
        e.bpm = b;
        run (e, "Tempo set to " + juce::String (b, 2) + " BPM.");
    };
    bpmEditor.onReturnKey = [this] { setBpm.triggerClick(); };
    halfButton.onClick = [this] { AppState::SongTimingEdit e; e.tempoScale = 0.5; run (e, "Tempo halved."); };
    doubleButton.onClick = [this] { AppState::SongTimingEdit e; e.tempoScale = 2.0; run (e, "Tempo doubled."); };
    tapButton.onClick = [this] { tapped (tap.tap (juce::Time::getMillisecondCounterHiRes() * 0.001)); };

    auto sig = [this] (int n, int d)
    {
        AppState::SongTimingEdit e;
        e.numerator = n;
        e.denominator = d;
        run (e, "Time signature " + juce::String (n) + "/" + juce::String (d) + ".");
    };
    sig34.onClick = [sig] { sig (3, 4); };
    sig44.onClick = [sig] { sig (4, 4); };
    sig68.onClick = [sig] { sig (6, 8); };
    numBox.onChange = [this, sig] { if (numBox.getSelectedId() > 0) sig (numBox.getSelectedId(), juce::jmax (2, denBox.getSelectedId())); };
    denBox.onChange = [this, sig] { if (denBox.getSelectedId() > 0) sig (juce::jmax (1, numBox.getSelectedId()), denBox.getSelectedId()); };

    auto shift = [this] (double beats, double seconds, const juce::String& what)
    {
        const auto* song = state.getCurrentSong();
        if (song == nullptr)
            return;
        AppState::SongTimingEdit e;
        if (std::abs (seconds) > 1e-9)
            e.barOneSec = juce::jmax (0.0, song->tempo.getOffsetSeconds() + seconds);
        e.shiftBeats = beats;
        run (e, what);
    };
    beatEarlier.onClick = [shift] { shift (-1.0, 0.0, "Bar 1 one beat earlier."); };
    beatLater.onClick = [shift] { shift (1.0, 0.0, "Bar 1 one beat later."); };
    nudgeEarlier.onClick = [shift] { shift (0.0, -0.010, "Bar 1 10 ms earlier."); };
    nudgeLater.onClick = [shift] { shift (0.0, 0.010, "Bar 1 10 ms later."); };
    setBar.onClick = [this]
    {
        AppState::SongTimingEdit e;
        e.barOneSec = juce::jmax (0.0, barEditor.getText().getDoubleValue());
        run (e, "Bar 1 at " + juce::String (e.barOneSec, 3) + " s.");
    };
    barEditor.onReturnKey = [this] { setBar.triggerClick(); };
    atPlayhead.onClick = [this]
    {
        AppState::SongTimingEdit e;
        e.barOneSec = juce::jmax (0.0, state.getPositionSeconds());
        run (e, "Bar 1 at the playhead (" + juce::String (e.barOneSec, 3) + " s).");
    };
    keyBox.onChange = [this]
    {
        if (keyBox.getSelectedId() <= 0)
            return;
        AppState::SongTimingEdit e;
        e.key = keyBox.getText();
        run (e, "Key " + e.key + ".");
    };
    undoButton.onClick = [this]
    {
        const auto err = state.undoSongTiming();
        message = err.isEmpty() ? "Undone." : err;
        messageIsError = err.isNotEmpty();
        shownFor.clear();
        refresh();
    };
    analyseButton.onClick = [this] { showAnalysisWindow (state); };

    setWantsKeyboardFocus (true);
    state.addChangeListener (this);
    setSize (720, 470);
    refresh();
}

SongTimingPanel::~SongTimingPanel()
{
    state.removeChangeListener (this);
}

juce::String SongTimingPanel::lockReason() const
{
    if (state.isLiveMode())
        return "LIVE MODE: tempo and time signature are locked during a show.";
    if (state.getTransportState() != TransportState::Stopped)
        return "Stop playback to change the tempo or time signature.";
    return {};
}

double SongTimingPanel::tapped (double bpm)
{
    if (bpm > 0.0)
    {
        bpmEditor.setText (juce::String (bpm, 1), false);
        message = "Tapped " + juce::String (bpm, 1) + " BPM (" + juce::String (tap.taps()) + " taps). SET BPM to use it.";
    }
    else
        message = "Keep tapping... (" + juce::String (tap.taps()) + ")";
    messageIsError = false;
    repaint();
    return bpm;
}

bool SongTimingPanel::keyPressed (const juce::KeyPress& k)
{
    if (k.getTextCharacter() == 't' || k.getTextCharacter() == 'T')
    {
        tapButton.triggerClick();
        return true;
    }
    return false;
}

void SongTimingPanel::run (const AppState::SongTimingEdit& e, const juce::String& what)
{
    const auto err = state.editSongTiming (e);
    message = err.isEmpty() ? what : err;
    messageIsError = err.isNotEmpty();
    shownFor.clear();   // show the new values in the fields
    refresh();
}

void SongTimingPanel::refresh()
{
    const auto* song = state.getCurrentSong();
    const bool locked = lockReason().isNotEmpty() || song == nullptr;
    for (auto* c : std::initializer_list<juce::Component*> { &setBpm, &halfButton, &doubleButton, &setBar, &beatEarlier, &beatLater,
                                                             &nudgeEarlier, &nudgeLater, &atPlayhead, &sig34, &sig44, &sig68,
                                                             &numBox, &denBox, &keyBox, &bpmEditor, &barEditor })
        c->setEnabled (! locked);
    tapButton.setEnabled (song != nullptr);
    analyseButton.setEnabled (! state.isLiveMode());
    undoButton.setEnabled (! locked && state.canUndoSongTiming());
    if (song != nullptr)
    {
        // Only fill the fields when the song or its values changed (not while the user types).
        const auto bb = song->tempo.barBeatAtBeat (0.0);
        const juce::String id = song->id + ":" + juce::String (song->tempo.bpmAtBeat (0.0), 3) + ":" + juce::String (bb.numerator)
                                + "/" + juce::String (bb.denominator) + ":" + juce::String (song->tempo.getOffsetSeconds(), 4) + ":" + song->key;
        if (id != shownFor)
        {
            shownFor = id;
            bpmEditor.setText (juce::String (song->tempo.bpmAtBeat (0.0), 2), false);
            barEditor.setText (juce::String (song->tempo.getOffsetSeconds(), 3), false);
            numBox.setSelectedId (bb.numerator, juce::dontSendNotification);
            denBox.setSelectedId (bb.denominator, juce::dontSendNotification);
            const int k = keyNames().indexOf (song->key);
            if (k >= 0)
                keyBox.setSelectedId (k + 1, juce::dontSendNotification);
            else
            {
                keyBox.setSelectedId (0, juce::dontSendNotification);
                keyBox.setTextWhenNothingSelected (song->key.isEmpty() ? juce::String ("(no key)") : song->key);
            }
        }
    }
    repaint();
}

void SongTimingPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    const auto* song = state.getCurrentSong();
    auto r = infoArea;
    g.setColour (theme::text);
    g.setFont (theme::font (18.0f, true));
    g.drawText (song == nullptr ? juce::String ("No song") : song->title, r.removeFromTop (26), juce::Justification::centredLeft);
    if (song != nullptr)
    {
        const auto& tp = song->tempo.getTempoPoints();
        const auto bb = song->tempo.barBeatAtBeat (0.0);
        g.setColour (theme::textDim);
        g.setFont (theme::font (13.0f));
        g.drawText (juce::String (song->tempo.bpmAtBeat (0.0), 2) + " BPM"
                        + (tp.size() > 1 ? " (+" + juce::String ((int) tp.size() - 1) + " tempo changes)" : juce::String())
                        + " [" + confText (song->bpmConfidence) + "]   " + juce::String (bb.numerator) + "/" + juce::String (bb.denominator)
                        + "   bar 1 at " + juce::String (song->tempo.getOffsetSeconds(), 3) + " s   "
                        + (song->key.isEmpty() ? juce::String ("no key") : song->key) + " [" + confText (song->keyConfidence) + "]",
                    r, juce::Justification::centredLeft);
    }
    g.setColour (theme::textDim);
    g.setFont (theme::font (12.0f, true));
    g.drawText ("TEMPO", tempoLabel, juce::Justification::centredLeft);
    g.drawText ("TIME SIG", sigLabel, juce::Justification::centredLeft);
    g.drawText ("BAR 1", barLabel, juce::Justification::centredLeft);
    g.drawText ("KEY", keyLabel, juce::Justification::centredLeft);

    g.setFont (theme::font (13.0f));
    const auto lock = lockReason();
    if (lock.isNotEmpty())
    {
        g.setColour (theme::warn);
        g.drawFittedText (lock, messageArea, juce::Justification::topLeft, 2);
    }
    else if (message.isNotEmpty())
    {
        g.setColour (messageIsError ? theme::danger : theme::ok);
        g.drawFittedText (message, messageArea, juce::Justification::topLeft, 2);
    }
    if (const auto w = state.songTimingWarning(); w.isNotEmpty())
    {
        g.setColour (theme::warn);
        g.drawFittedText (w, warnArea, juce::Justification::topLeft, 2);
    }
    g.setColour (theme::textFaint);
    g.setFont (theme::font (12.0f));
    g.drawFittedText ("Cues and chords stay where they are in the song. MIDI tracks follow the tempo. Saved with the project.",
                      getLocalBounds().reduced (16).removeFromBottom (20), juce::Justification::centredLeft, 1);
}

void SongTimingPanel::resized()
{
    auto r = getLocalBounds().reduced (16);
    infoArea = r.removeFromTop (48);
    r.removeFromTop (10);
    auto row = [&r] { auto x = r.removeFromTop (34); r.removeFromTop (10); return x; };
    auto place = [] (juce::Rectangle<int>& x, juce::Component& c, int w) { c.setBounds (x.removeFromLeft (w).reduced (0, 2)); x.removeFromLeft (6); };

    auto t = row();
    tempoLabel = t.removeFromLeft (80);
    place (t, bpmEditor, 90);
    place (t, setBpm, 80);
    place (t, halfButton, 44);
    place (t, doubleButton, 44);
    place (t, tapButton, 70);

    auto s = row();
    sigLabel = s.removeFromLeft (80);
    place (s, numBox, 60);
    place (s, denBox, 60);
    s.removeFromLeft (10);
    place (s, sig34, 50);
    place (s, sig44, 50);
    place (s, sig68, 50);

    auto b = row();
    barLabel = b.removeFromLeft (80);
    place (b, barEditor, 90);
    place (b, setBar, 50);
    place (b, beatEarlier, 70);
    place (b, beatLater, 70);
    place (b, nudgeEarlier, 60);
    place (b, nudgeLater, 60);
    place (b, atPlayhead, 100);

    auto k = row();
    keyLabel = k.removeFromLeft (80);
    place (k, keyBox, 200);

    r.removeFromBottom (24);
    auto bottom = r.removeFromBottom (34);
    undoButton.setBounds (bottom.removeFromLeft (90).reduced (0, 2));
    analyseButton.setBounds (bottom.removeFromRight (140).reduced (0, 2));
    r.removeFromBottom (6);
    messageArea = r.removeFromTop (40);
    warnArea = r.removeFromTop (40);
}

//==============================================================================
namespace
{
class SongTimingWindow : public juce::DocumentWindow
{
public:
    explicit SongTimingWindow (AppState& s)
        : DocumentWindow ("Tempo / Time Signature / Key - B4ND LIVE MTR", theme::panel, DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new SongTimingPanel (s), true);
        setResizable (true, false);
        setResizeLimits (700, 440, 1400, 900);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }
    void closeButtonPressed() override { setVisible (false); }
};

std::unique_ptr<SongTimingWindow> timingOwner;
} // namespace

void showSongTimingWindow (AppState& state)
{
    if (timingOwner == nullptr)
        timingOwner = std::make_unique<SongTimingWindow> (state);
    timingOwner->setVisible (true);
    timingOwner->toFront (true);
}

SongTimingPanel* getSongTimingWindowContent()
{
    return timingOwner != nullptr ? dynamic_cast<SongTimingPanel*> (timingOwner->getContentComponent()) : nullptr;
}

void closeSongTimingWindow()
{
    timingOwner.reset();
}

} // namespace b4nd
